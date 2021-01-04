#ifndef GULLIVERS_CHECKER_HPP
#define GULLIVERS_CHECKER_HPP

#include <clang/AST/ASTConsumer.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>

class GAction : public clang::ASTFrontendAction {
public:
  unsigned int diagId;

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI,
                                                        clang::StringRef InFile) override;
};

#endif // GULLIVERS_CHECKER_HPP
