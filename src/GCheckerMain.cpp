#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>

#include "GChecker.hpp"

using namespace clang;
using namespace clang::tooling;

static llvm::cl::OptionCategory GCheckerToolCat("Gulliver's Checker");
static llvm::cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static llvm::cl::opt<bool> Verbose("v", llvm::cl::cat(GCheckerToolCat), llvm::cl::desc("Be verbose"));

bool showColors = true;
bool verbose;

int main(int argc, const char **argv) {
  CommonOptionsParser OptionsParser(argc, argv, GCheckerToolCat);
  verbose = Verbose.getValue();
  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());
  return Tool.run(newFrontendActionFactory<GAction>().get());
}
