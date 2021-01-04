#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <llvm/Support/MemoryBuffer.h>
#include "gtest/gtest.h"

#include "../src/GChecker.hpp"

using namespace clang;
using namespace clang::ast_matchers;

bool showColors = true;
bool verbose;

namespace {

class DiagConsumerSimple : public clang::DiagnosticConsumer {
  const unsigned *diagId;
  size_t *diagRaised;
  const std::string id;
  const std::string type;
  const std::string typeAccess;
public:
  DiagConsumerSimple(const unsigned *diagId, size_t *diagRaised, const char *id, const char *type, const char *typeAccess)
  : diagId(diagId), diagRaised(diagRaised), id(id), type(type), typeAccess(typeAccess) { }

  void HandleDiagnostic(DiagnosticsEngine::Level DiagLevel,
                        const Diagnostic &Info) override {
    if (Info.getID() != *diagId)
      return;
    ASSERT_TRUE(Info.getArgStdStr(0) == id);
    ASSERT_TRUE(Info.getArgStdStr(1) == type);
    ASSERT_TRUE(Info.getArgStdStr(2) == typeAccess);
    *diagRaised += 1;
  }
};

void trivialTest(const char *source, const char *id, const char *type, const char *typeAccess, size_t count=1) {
  auto invocation = std::make_shared<CompilerInvocation>();
  invocation->getPreprocessorOpts().addRemappedFile(
      "test.c",
      llvm::MemoryBuffer::getMemBuffer(source).release());
  invocation->getFrontendOpts().Inputs.push_back(
      FrontendInputFile("test.c", Language::C));
  invocation->getFrontendOpts().ProgramAction = frontend::ParseSyntaxOnly;
  invocation->getTargetOpts().Triple = "i386-unknown-linux-gnu";
  CompilerInstance compiler;
  compiler.setInvocation(std::move(invocation));
  GAction action;
  size_t diagRaised = 0;
  compiler.createDiagnostics(new DiagConsumerSimple(&action.diagId, &diagRaised, id, type, typeAccess));
  compiler.ExecuteAction(action);
  ASSERT_EQ(diagRaised, count);
  llvm::llvm_shutdown();
}

class DiagConsumerNoWarning : public clang::DiagnosticConsumer {
  const unsigned *diagId;
public:
  DiagConsumerNoWarning(const unsigned *diagId) : diagId(diagId) { }

  void HandleDiagnostic(DiagnosticsEngine::Level DiagLevel,
                        const Diagnostic &Info) override {
    if (Info.getID() != *diagId)
      return;
    FAIL() << "No warning expected";
  }
};

void noWarningTest(const char *source) {
  auto invocation = std::make_shared<CompilerInvocation>();
  invocation->getPreprocessorOpts().addRemappedFile(
      "test.c",
      llvm::MemoryBuffer::getMemBuffer(source).release());
  invocation->getFrontendOpts().Inputs.push_back(
      FrontendInputFile("test.c", Language::C));
  invocation->getFrontendOpts().ProgramAction = frontend::ParseSyntaxOnly;
  invocation->getTargetOpts().Triple = "i386-unknown-linux-gnu";
  CompilerInstance compiler;
  compiler.setInvocation(std::move(invocation));
  GAction action;
  compiler.createDiagnostics(new DiagConsumerNoWarning(&action.diagId));
  compiler.ExecuteAction(action);
  llvm::llvm_shutdown();
}

} // end anonymous namespace

TEST(Trivial, 1) {
  const char *source = R"cpp(
void fun() {
  int i;
  unsigned char *pc;
  pc = (unsigned char *) &i + sizeof(int) - 1;
  *pc = 1;
}
)cpp";
  trivialTest(source, "i", "int", "unsigned char");
}

TEST(Trivial, 2) {
  const char *source = R"cpp(
extern int unknown;
void fun() {
  int i;
  unsigned char c, *pc;
  if (unknown) pc = &c;
  else pc = (unsigned char *) &i;
  *pc = 1;
}
)cpp";
  trivialTest(source, "i", "int", "unsigned char");
}

TEST(Trivial, 3) {
  const char *source = R"cpp(
void fun() {
  int i;
  unsigned char c, *pc = &c;
  for (int j = 0; j < 2; ++j) {
      *pc = 1;
      pc = (unsigned char *) &i;
  }
}
)cpp";
  trivialTest(source, "i", "int", "unsigned char");
}

TEST(Trivial, 4) {
  const char *source = R"cpp(
extern int unknown;
void fun() {
  int i, *pi;
  unsigned char c, *pc, **ppc;
  pi = &i;
  pc = &c;
  if (unknown) ppc = &pc;
  else ppc = (unsigned char **) &pi;
  **ppc = 1;
}
)cpp";
  trivialTest(source, "i", "int", "unsigned char");
}

TEST(Trivial, 5) {
  const char *source = R"cpp(
extern int unknown;
void fun() {
  int i, *pi, **ppi;
  unsigned char c, *pc;
  pi = &i;
  ppi = &pi;
  pc = &c;
  if (unknown) {
    ppi = (int **) &pc;
    *ppi = pi;
  }
  *pc = 1;
}
)cpp";
  trivialTest(source, "i", "int", "unsigned char");
}

TEST(Trivial, 6) {
  const char *source = R"cpp(
extern int unknown;
void fun() {
  int i;
  unsigned char *pc;

  if (unknown)
#if __BYTE_ORDER == __BIG_ENDIAN
       pc = (unsigned char *) &i;
#endif
  else pc = (unsigned char *) &i;
  *pc = 1;
}
)cpp";
  trivialTest(source, "i", "int", "unsigned char");
}

TEST(Trivial, 7) {
  const char *source = R"cpp(
void fun() {
  int i = 0;
  unsigned char *pc = (unsigned char *) &i;
  *(pc + *pc) = 1;
}
)cpp";
  trivialTest(source, "i", "int", "unsigned char", 2);
}

TEST(Trivial, 8) {
  const char *source = R"cpp(
void fun() {
  int i = 0;
  unsigned char *pc = (unsigned char *) &i;
  i = *(pc + *pc);
}
)cpp";
  trivialTest(source, "i", "int", "unsigned char", 2);
}

TEST(Trivial, 9) {
  const char *source = R"cpp(
void fun() {
  int i;
  unsigned char *pc;
  *(pc=(unsigned char *)&i, pc) = 1;
}
)cpp";
  trivialTest(source, "i", "int", "unsigned char");
}

TEST(Trivial, 10) {
  const char *source = R"cpp(
extern int unknown;
void fun() {
  int i;
  unsigned char c;
  unsigned char *pc = unknown ? &c : (unsigned char *)&i;
  c = *pc;
}
)cpp";
  trivialTest(source, "i", "int", "unsigned char");
}

TEST(NoWarn, 1) {
  const char *source = R"cpp(
void fun() {
  int i, *pi;
  unsigned char *pc;
  pc = (unsigned char *) &i;
  pi = (int *) pc;
  *pi = 1;
}
)cpp";
  noWarningTest(source);
}

TEST(NoWarn, 2) {
  const char *source = R"cpp(
void fun() {
  int i, *pi;
  unsigned char *pc;
  pc = (unsigned char *) &i;
  *(pi=(int *)pc, pi) = 1;
}
)cpp";
  noWarningTest(source);
}

TEST(NoWarn, 3) {
  const char *source = R"cpp(
void fun() {
  int i;
  unsigned char *pc;
#if __BYTE_ORDER == __BIG_ENDIAN
  pc = ((unsigned char *) &i) + sizeof(int) - 1;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
  pc = (unsigned char *) &i;
#else
# error Esoteric byte-orders are not supported ;-)
#endif
  *pc = 1;
}
)cpp";
  noWarningTest(source);
}

TEST(NoWarn, 4) {
  const char *source = R"cpp(
extern int i;
extern unsigned char *pc;
extern unsigned char *unknown;
void fun() {
  pc = (unsigned char *) &i;
  *unknown = 42;
  *pc = 1;
}
)cpp";
  noWarningTest(source);
}
