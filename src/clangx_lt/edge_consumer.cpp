#include "clangx_lt/edge_consumer.hpp"

#include "clangx_lt/edge_visitor.hpp"

#include "clang/AST/ASTContext.h"

namespace cidx::lt {

EdgeConsumer::EdgeConsumer(EdgeSink &sink,
                           std::vector<std::string> target_files)
    : sink_(sink), target_files_(std::move(target_files)) {}

void EdgeConsumer::HandleTranslationUnit(clang::ASTContext &context) {
  for (const std::string &file : target_files_) {
    EdgeVisitor visitor(context, sink_, file);
    visitor.TraverseDecl(context.getTranslationUnitDecl());
  }
}

} // namespace cidx::lt
