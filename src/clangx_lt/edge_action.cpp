#include "clangx_lt/edge_action.hpp"

#include "clangx_lt/edge_consumer.hpp"

namespace cidx::lt {

EdgeAction::EdgeAction(EdgeSink &sink, std::vector<std::string> target_files)
    : sink_(sink), target_files_(std::move(target_files)) {}

std::unique_ptr<clang::ASTConsumer>
EdgeAction::CreateASTConsumer(clang::CompilerInstance & /*ci*/,
                              llvm::StringRef /*file*/) {
  return std::make_unique<EdgeConsumer>(sink_, target_files_);
}

} // namespace cidx::lt
