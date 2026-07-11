// FrontendAction wiring the edge consumer to an externally owned EdgeSink and
// target-file list (composition root for the edges tool).
#pragma once

#include "clang/Frontend/FrontendAction.h"

#include <memory>
#include <string>
#include <vector>

namespace cidx::lt {

class EdgeSink;

class EdgeAction : public clang::ASTFrontendAction {
public:
  EdgeAction(EdgeSink &sink, std::vector<std::string> target_files);

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &ci, llvm::StringRef file) override;

private:
  EdgeSink &sink_;
  std::vector<std::string> target_files_;
};

} // namespace cidx::lt
