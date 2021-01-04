#include <clang/Analysis/AnalysisDeclContext.h>
#include <clang/Analysis/CFGStmtMap.h>
#include <clang/Analysis/FlowSensitive/DataflowWorklist.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/DeclVisitor.h>
#include <clang/AST/StmtVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Lex/PPCallbacks.h>
#include <llvm/ADT/ImmutableMap.h>
#include <llvm/ADT/ImmutableSet.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>

#include <set>
#include <map>

#include "GChecker.hpp"

using namespace clang;

extern bool showColors;
extern bool verbose;

namespace {

class FindSafeRanges : public PPCallbacks {
  const Preprocessor &PP;
  std::set<SourceLocation> safeIf;

public:
  using SafeRangesImpl = llvm::SmallVector<SourceRange, 128>;

  FindSafeRanges(const Preprocessor &PP, std::shared_ptr<SafeRangesImpl> safeRange)
  : PP(PP), safeRange(safeRange) { }

  void If(SourceLocation ifLoc, SourceRange conditionRange, ConditionValueKind) {
    StringRef conditionStr =
      Lexer::getSourceText(CharSourceRange::getTokenRange(conditionRange),
                           PP.getSourceManager(), PP.getLangOpts());

    if (conditionStr.find_lower("endian") != StringRef::npos)
      safeIf.insert(ifLoc);
  }

  // TODO elseif else

  void Endif(SourceLocation endIfLoc, SourceLocation ifLoc) {
    if (safeIf.find(ifLoc) == safeIf.cend())
      return;
    safeRange->push_back(SourceRange(ifLoc, endIfLoc));
  }

private:
  std::shared_ptr<SafeRangesImpl> safeRange;
};



//===----------------------------------------------------------------------===//
// Dom
//===----------------------------------------------------------------------===//

using DomElem = std::pair<const ValueDecl *, bool>;
class DomElemTrait : public llvm::ImutContainerInfo<DomElem> {
public:
  static inline bool isLess(key_type_ref lhs, key_type_ref rhs) {
    return lhs.first < rhs.first ||
           (lhs.first == rhs.first && !lhs.first && rhs.second);
  }
  static void Profile(llvm::FoldingSetNodeID &ID, value_type_ref X) {
    ID.AddPointer(X.first);
    ID.AddBoolean(X.second);
  }
};
using DomImpl = llvm::ImmutableSet<DomElem, DomElemTrait>;

class Dom {
public:
  static DomImpl getTop();

  static bool isTop(DomImpl dom);

  static DomImpl singleton(DomImpl::value_type_ref x);

  LLVM_NODISCARD
  static DomImpl add(DomImpl dom, DomImpl::value_type_ref x);

  LLVM_NODISCARD
  static DomImpl merge(DomImpl lhs, DomImpl rhs);

  static void printToStream(llvm::raw_ostream &stream, const DomImpl &dom) {
    if (dom.isEmpty()) {
      stream << "<top>";
      return;
    }
    // TODO print decl location if verbose or in case of shadowing
    auto ppEntry = [&stream](const DomImpl::value_type &pair){
      if (showColors) {
        if (pair.second)
          stream.changeColor(raw_ostream::GREEN, true);
        else
          stream.changeColor(raw_ostream::MAGENTA, true);

        stream << pair.first->getName();

        stream.resetColor();
      } else {
        stream << "<" << pair.first->getName() << (pair.second ? ",1" : ",0") << ">";
      }
    };
    stream << "{";
    auto it = dom.begin();
    ppEntry(*it);
    ++it;
    std::for_each(it, dom.end(), [&stream, ppEntry](const DomImpl::value_type &entry){ stream << ", "; ppEntry(entry); });
    stream << "}";
  }

  static std::string getAsString(const DomImpl &dom) {
    std::string str;
    llvm::raw_string_ostream stream(str);
    printToStream(stream, dom);
    return str;
  }
};



//===----------------------------------------------------------------------===//
// State
//===----------------------------------------------------------------------===//

using StateImpl = llvm::ImmutableMap<const ValueDecl *, DomImpl>;

class State {
  StateImpl state;
  bool bot;
  State(StateImpl state, bool bot);

public:
  static State getBot();

  static State getTop();

  bool isBot() const;

  bool isTop() const;

  bool operator!=(const State &other) const;

  DomImpl get(StateImpl::key_type x) const;

  LLVM_NODISCARD
  State merge(const State &other) const;

  LLVM_NODISCARD
  State set(StateImpl::key_type x, StateImpl::data_type dom) const;

  void dump() const { printToStream(llvm::errs()); }

  std::string getAsString() const {
    std::string str;
    llvm::raw_string_ostream stream(str);
    printToStream(stream);
    return str;
  }

  void printToStream(llvm::raw_ostream &stream, unsigned indent=0) const {
    if (isBot()) {
      for (unsigned i = 0; i < indent; ++i)
        stream << " ";
      stream << "<bot>";
      return;
    }
    if (isTop()) {
      for (unsigned i = 0; i < indent; ++i)
        stream << " ";
      stream << "<top>";
      return;
    }
    if (state.isEmpty())
      llvm::report_fatal_error("whoopsy");
    auto ppEntry = [&stream, indent](const StateImpl::value_type &P) {
      for (unsigned i = 0; i < indent; ++i)
        stream << " ";
      if (showColors)
        stream.changeColor(raw_ostream::BLUE, true);
      stream << P.first->getName();
      if (showColors)
        stream.resetColor();
      stream << " -> ";
      Dom::printToStream(stream, P.second);
    };
    auto it = state.begin();
    ppEntry(*it);
    ++it;
    std::for_each(it, state.end(), [ppEntry, &stream](const auto &entry){ stream << "\n"; ppEntry(entry); });
  }
};



//===----------------------------------------------------------------------===//
// Block2State
//===----------------------------------------------------------------------===//

class Block2State {
  using B2SImpl = std::map<const CFGBlock *, State>;
  B2SImpl B2S;

  void printToStream(llvm::raw_ostream &stream, unsigned indent=0) const {
    if (B2S.size() == 0)
      llvm::report_fatal_error("whoopsy");
    auto ppEntry = [&stream, indent](const B2SImpl::value_type &P) {
      if (showColors)
        stream.changeColor(raw_ostream::YELLOW, true);
      for (unsigned i = 0; i < indent; ++i)
        stream << " ";
      stream << "[B" << P.first->getBlockID() << "]\n";
      if (showColors)
        stream.resetColor();
      P.second.printToStream(stream, indent+2);
    };
    auto it = B2S.cbegin();
    ppEntry(*it);
    ++it;
    for (auto ei = B2S.cend(); it != ei; ++it) {
      stream << "\n\n";
      ppEntry(*it);
    }
  }

public:
  State get(const CFGBlock *block) const {
    const auto i = B2S.find(block);
    if (i == B2S.cend())
      return State::getBot();
    else
      return (*i).second;
  }

  void set(const CFGBlock *block, const State &state) {
    B2S.erase(block);
    B2S.insert({ block, state });
  }

  void clear() { B2S.clear(); }

  void dump(unsigned indent=0) const {
    printToStream(llvm::errs(), indent);
    llvm::errs() << "\n";
  }

  std::string getAsString() const {
    std::string str;
    llvm::raw_string_ostream stream(str);
    printToStream(stream);
    return str;
  }
};



//===----------------------------------------------------------------------===//
// GulliversHelper
//===----------------------------------------------------------------------===//

class GulliversHelper {
public:
  unsigned int diagId;
  std::shared_ptr<FindSafeRanges::SafeRangesImpl> safeRanges;
  Block2State B2S;
  bool diagnose;
  DomImpl::Factory DF;
  StateImpl::Factory SF;

  ~GulliversHelper() { B2S.clear(); }
};

GulliversHelper *G;



//===----------------------------------------------------------------------===//
// Dom
//===----------------------------------------------------------------------===//

DomImpl Dom::getTop() { return G->DF.getEmptySet(); }

bool Dom::isTop(DomImpl dom) {
  return dom.isEmpty();
}

DomImpl Dom::singleton(DomImpl::value_type_ref x) {
  return G->DF.add(G->DF.getEmptySet(), x);
}

LLVM_NODISCARD
DomImpl Dom::add(DomImpl dom, DomImpl::value_type_ref x) {
  // FIXME
  // if (Dom::isTop(dom))
  //   return dom;
  return G->DF.add(dom, x);
}

LLVM_NODISCARD
DomImpl Dom::merge(DomImpl lhs, DomImpl rhs) {
  if (Dom::isTop(lhs) || Dom::isTop(rhs))
    return Dom::getTop();
  DomImpl dom = lhs;
  for (const DomImpl::value_type &x : rhs)
    dom = G->DF.add(dom, x);
  return dom;
}



//===----------------------------------------------------------------------===//
// State
//===----------------------------------------------------------------------===//

State::State(StateImpl state, bool bot) : state(state), bot(bot) { }

State State:: getBot() { return State(G->SF.getEmptyMap(), true); }

State State::getTop() { return State(G->SF.getEmptyMap(), false); }

bool State::isBot() const { return bot; }

bool State::isTop() const {
  return !bot && (state.isEmpty() ||
                  llvm::all_of(state, [](const auto &entry){ return Dom::isTop(entry.second); }));
}

bool State::operator!=(const State &other) const {
  return this->bot != other.bot || this->state != other.state;
}

LLVM_NODISCARD
State State::merge(const State &other) const {
  if (this->isBot())
    return other;
  if (other.isBot())
    return *this;
  if (this->isTop() || other.isTop())
    return State::getTop();

  StateImpl newState = G->SF.getEmptyMap();

  for (StateImpl::value_type_ref thisEntry : this->state) {
    auto x = thisEntry.first;
    auto thisDom = thisEntry.second;
    auto otherDom = other.get(x);
    auto mergedDom = Dom::merge(thisDom, otherDom);
    if (!Dom::isTop(mergedDom))
      newState = G->SF.add(newState, x, mergedDom);
  }
  for (StateImpl::value_type_ref otherEntry : other.state) {
    auto x = otherEntry.first;
    if (!this->state.contains(x))
      newState = G->SF.remove(newState, x);
  }
  assert(!newState.isEmpty());

  return State(newState, false);
}

LLVM_NODISCARD
State State::set(StateImpl::key_type x, StateImpl::data_type dom) const {
  if (Dom::isTop(dom))
    return State(G->SF.remove(state, x), false);
  else
    return State(G->SF.add(state, x, dom), false);
}

DomImpl State::get(StateImpl::key_type x) const {
  auto *dom = this->state.lookup(x);
  if (dom)
    return *dom;
  else
    return Dom::getTop();
}


using MapType = llvm::DenseMap<const Expr *, DomImpl>;

//===----------------------------------------------------------------------===//
// TransferFunctions
//===----------------------------------------------------------------------===//

class TransferFunctions : public ConstStmtVisitor<TransferFunctions> {

  AnalysisDeclContext &AC;
  State state;
  MapType &propagationMap;

  LLVM_NODISCARD
  DomImpl lookup(const Expr *E) {
    return propagationMap.find(E)->second;
  }

  void insert(const Expr *E, DomImpl dom) {
    propagationMap.erase(E);
    propagationMap.insert({ E, dom });
  }

  bool isInSafeRange(const SourceLocation &srcLoc) const {
    return llvm::any_of(*(G->safeRanges),
                        [&srcLoc](const SourceRange &range){ return range.fullyContains(srcLoc); });
  }

  bool isInSafeRange(const SourceRange &srcRange) const {
    return llvm::any_of(*(G->safeRanges),
                        [&srcRange](const SourceRange &range){ return range.fullyContains(srcRange); });
  }

public:
  TransferFunctions(AnalysisDeclContext &AC,
                    const State &state,
                    MapType &propagationMap)
  : AC(AC), state(state), propagationMap(propagationMap) { }

  State getState() const { return state; }

  void Visit(const Stmt *S) {
    if (state.isBot())
      return;

    if (dyn_cast<UnaryOperator>(S) || dyn_cast<BinaryOperator>(S))
      return ConstStmtVisitor<TransferFunctions>::Visit(S);

    switch (S->getStmtClass()) {
    default: S->dump(); llvm_unreachable("TransferFunction not yet implemented");
    case Stmt::ConditionalOperatorClass:
    case Stmt::IntegerLiteralClass:
    case Stmt::UnaryExprOrTypeTraitExprClass:
    case Stmt::DeclStmtClass:
    case Stmt::DeclRefExprClass:
    case Stmt::ParenExprClass:
    case Stmt::CStyleCastExprClass:
    case Stmt::ImplicitCastExprClass:
      return ConstStmtVisitor<TransferFunctions>::Visit(S);
    }
  }

  void VisitDeclRefExpr(const DeclRefExpr *E) {
    assert(E->isLValue());

    if (state.isBot())
      return;

    auto *decl = E->getDecl();
    auto value = Dom::singleton({ decl, isInSafeRange(decl->getSourceRange()) });
    insert(E, value);
  }

  void VisitDeclStmt(const DeclStmt *S) {
    if (state.isBot())
      return;

    for (const auto *decl : S->getDeclGroup()) {
      if (const auto *varDecl = dyn_cast<const VarDecl>(decl)) {
        auto cinit = varDecl->getInit();
        if (!cinit)
          continue;
        state = state.set(varDecl, lookup(cinit));
      }
    }
  }

  void VisitUnaryOperator(const UnaryOperator *E) {
    if (state.isBot())
      return;

    switch (E->getOpcode()) {
    case UnaryOperator::Opcode::UO_AddrOf: {
      auto value = lookup(E->getSubExpr()->IgnoreParens());
      assert(value.isSingleton());
      auto *valueDecl = value.begin()->first;
      bool safe = isInSafeRange(E->getSourceRange());
      value = Dom::singleton({ valueDecl, safe });
      insert(E, value);
      break;
    }
    case UnaryOperator::Opcode::UO_Deref: {
      assert(E->isLValue());
      auto value = lookup(E->getSubExpr()->IgnoreParens());
      insert(E, value);
      if (G->diagnose && !Dom::isTop(value) && !E->getType().getTypePtr()->isPointerType()) {
        const QualType type = E->getType();
        for (const DomImpl::value_type &candDeclPair : value) {
          QualType candType = candDeclPair.first->getType();
          if (candType.getTypePtr()->isPointerType() && type.getTypePtr()->isPointerType())
            continue;
          if (candType != type && !candDeclPair.second) {
            DiagnosticBuilder DB = AC.getASTContext().getDiagnostics().Report(E->getExprLoc(), G->diagId);
            DB.AddString(candDeclPair.first->getName());
            DB.AddString(candType.getAsString());
            DB.AddString(type.getAsString());
            DB.AddSourceRange(CharSourceRange::getCharRange(E->getSourceRange()));
          }
        }
      }
      break;
    }
    default:
      insert(E, Dom::getTop());
    }
  }

  void VisitBinaryOperator(const BinaryOperator *E) {
    if (state.isBot())
      return;

    switch (E->getOpcode()) {
    case BinaryOperator::Opcode::BO_Assign: {
      DomImpl lhs_val = lookup(E->getLHS()->IgnoreParens());
      DomImpl rhs_val = lookup(E->getRHS()->IgnoreParens());

      if (lhs_val.isSingleton()) {
        if (auto varDecl = dyn_cast<const VarDecl>((*lhs_val.begin()).first))
          if (varDecl->hasLocalStorage())
            state = state.set(varDecl, rhs_val);
      } else if (Dom::isTop(lhs_val)) {
        state = State::getTop();
      } else {
        for (const DomImpl::value_type &declPair : lhs_val) {
          if (auto varDecl = dyn_cast<const VarDecl>(declPair.first)) {
            if (varDecl->hasLocalStorage())
              state = state.set(declPair.first, Dom::merge(state.get(declPair.first), rhs_val));
          }
        }
      }

      insert(E, rhs_val);

      break;
    }
    case BinaryOperator::Opcode::BO_AddAssign:
    case BinaryOperator::Opcode::BO_SubAssign:
      break;
    case BinaryOperator::Opcode::BO_Add:
    case BinaryOperator::Opcode::BO_Sub: {
      DomImpl lhs_val = lookup(E->getLHS()->IgnoreParens());
      DomImpl rhs_val = lookup(E->getRHS()->IgnoreParens());

      if (E->getLHS()->getType().getTypePtr()->isPointerType() && !E->getRHS()->getType().getTypePtr()->isPointerType())
        insert(E, lhs_val);
      else if (!E->getLHS()->getType().getTypePtr()->isPointerType() && E->getRHS()->getType().getTypePtr()->isPointerType())
        insert(E, rhs_val);
      else
        insert(E, Dom::getTop());

      break;
    }
    case BinaryOperator::Opcode::BO_Comma:
      insert(E, lookup(E->getRHS()->IgnoreParens()));
      break;
    default:
      insert(E, Dom::getTop());
    }
  }

  void VisitConditionalOperator(const ConditionalOperator *E) {
    if (state.isBot())
      return;

    auto true_val = lookup(E->getTrueExpr()->IgnoreParens());
    auto false_val = lookup(E->getFalseExpr()->IgnoreParens());

    insert(E, Dom::merge(true_val, false_val));
  }

  void VisitImplicitCastExpr(const ImplicitCastExpr *E) {
    if (state.isBot())
      return;

    if (E->getCastKind() == CK_LValueToRValue) {
      auto value = lookup(E->getSubExpr()->IgnoreParens());
      if (!Dom::isTop(value)) {
        DomImpl newValue = G->DF.getEmptySet();
        for (const auto &declPair : value) {
          DomImpl tmp = state.get(declPair.first);
          if (Dom::isTop(tmp)) {
            newValue = Dom::getTop();
            break;
          }
          for (auto &x : tmp)
            newValue = Dom::add(newValue, x);
        }
        value = newValue;
      }
      insert(E, value);
    } else {
      insert(E, lookup(E->getSubExpr()->IgnoreParens()));
    }
  }

  // Just decend
  void VisitParenExpr(const ParenExpr *E) {
    if (state.isBot())
      return;
    insert(E, lookup(E->getSubExpr()->IgnoreParens()));
  }
  void VisitCStyleCastExpr(const CStyleCastExpr *E) {
    if (state.isBot())
      return;
    insert(E, lookup(E->getSubExpr()->IgnoreParens()));
  }

  // Return TOP
  void VisitIntegerLiteral(const IntegerLiteral *E) {
    if (state.isBot())
      return;
    insert(E, Dom::getTop());
  }
  void VisitUnaryExprOrTypeTraitExpr(const UnaryExprOrTypeTraitExpr *E) {
    if (state.isBot())
      return;
    insert(E, Dom::getTop());
  }
};



//===----------------------------------------------------------------------===//
// GASTVisitor
//===----------------------------------------------------------------------===//

class GASTVisitor : public DeclVisitor<GASTVisitor> {
  ASTContext &Context;
  std::shared_ptr<FindSafeRanges::SafeRangesImpl> safeRanges;
  unsigned int diagId;

  void getSubStmts(const Stmt *stmt, llvm::SmallPtrSetImpl<const Stmt *> &isSubStmt) const {
    for (const auto *subStmt : stmt->children()) {
      isSubStmt.insert(subStmt);
      getSubStmts(subStmt, isSubStmt);
    }
  }

  LLVM_NODISCARD
  State runOnBlock(const CFGBlock *block, AnalysisDeclContext &AC, const State &state, MapType &propagationMap) const {
    TransferFunctions TF(AC, state, propagationMap);

    for (const auto &I : *block) {
      if (Optional<CFGStmt> stmt = I.getAs<CFGStmt>())
        TF.Visit(stmt->getStmt());
    }

    return TF.getState();
  }

public:
  GASTVisitor(ASTContext &Context,
               std::shared_ptr<FindSafeRanges::SafeRangesImpl> safeRanges,
               unsigned int diagId)
  : Context(Context), safeRanges(safeRanges), diagId(diagId) { }

  void VisitFunctionDecl(FunctionDecl *FD) {
    if (!FD->hasBody())
      return;

    // llvm::errs() << "Analyzing function: " << FD->getName() << "\n";

    CFG::BuildOptions opts;
    opts.setAllAlwaysAdd();
    AnalysisDeclContext AC(nullptr, FD, opts);
    CFG *CFG = AC.getCFG();
    ForwardDataflowWorklist Worklist(*CFG, AC);
    G = new GulliversHelper();
    G->safeRanges = safeRanges;
    G->diagId = diagId;

    for (const CFGBlock *I : *CFG)
      Worklist.enqueueBlock(I);

    const CFGBlock *entry = &CFG->getEntry();
    MapType propagationMap;

    while (const CFGBlock *block = Worklist.dequeue()) {
      State state = block == entry ? State::getTop() : State::getBot();

      for (CFGBlock::const_pred_iterator it = block->pred_begin(),
                                         ei = block->pred_end(); it != ei; ++it) {
        if (const CFGBlock *pred = *it)
          state = state.merge(G->B2S.get(pred));
      }

      State old_state = G->B2S.get(block);
      State new_state = runOnBlock(block, AC, state, propagationMap);

      if (old_state != new_state) {
        G->B2S.set(block, new_state);
        Worklist.enqueueSuccessors(block);
      }
    }

    if (verbose) {
      llvm::errs() << "*** Fixed Point ***\n\n";
      G->B2S.dump(1);
      llvm::errs() << "\n\n*** CFG ***\n";
      CFG->dump(Context.getLangOpts(), showColors);
      CFG->viewCFG(Context.getLangOpts());
    }

    G->diagnose = true;

    for (const CFGBlock *block : *CFG) {
      State state = block == entry ? State::getTop() : State::getBot();

      // Least Upper Bound
      for (CFGBlock::const_pred_iterator it = block->pred_begin(),
                                         ei = block->pred_end(); it != ei; ++it) {
        if (const CFGBlock *pred = *it)
          state = state.merge(G->B2S.get(pred));
      }

      TransferFunctions TF(AC, state, propagationMap);

      for (const auto &I : *block) {
        if (Optional<CFGStmt> stmt = I.getAs<CFGStmt>())
          TF.Visit(stmt->getStmt());
      }
    }

    propagationMap.clear();
    delete G;
  }
};



//===----------------------------------------------------------------------===//
// GConsumer
//===----------------------------------------------------------------------===//

class GConsumer : public ASTConsumer {
  GASTVisitor Visitor;
public:
  GConsumer(ASTContext &Context,
            std::shared_ptr<FindSafeRanges::SafeRangesImpl> safeRanges,
            unsigned int diagId)
  : Visitor(Context, safeRanges, diagId) { }

  bool HandleTopLevelDecl(DeclGroupRef DR) override {
    for (auto *I : DR)
      Visitor.Visit(I);
    return true;
  }
};

} // end anonymous namespace



//===----------------------------------------------------------------------===//
// GAction
//===----------------------------------------------------------------------===//

std::unique_ptr<ASTConsumer> GAction::CreateASTConsumer(CompilerInstance &CI,
                                                        StringRef InFile) {
  Preprocessor &PP = CI.getPreprocessor();
  auto safeRanges = std::make_shared<FindSafeRanges::SafeRangesImpl>();
  PP.addPPCallbacks(std::make_unique<FindSafeRanges>(PP, safeRanges));

  diagId = CI.getDiagnostics().getCustomDiagID(
      clang::DiagnosticsEngine::Warning,
      "Possible endian unsafe access of object '%0' of type '%1' through lvalue of type '%2'");

  return std::make_unique<GConsumer>(CI.getASTContext(), safeRanges, diagId);
}
