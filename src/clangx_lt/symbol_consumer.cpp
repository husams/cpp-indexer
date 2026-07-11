#include "clangx_lt/symbol_consumer.hpp"

#include "clangx_lt/symbol_visitor.hpp"

#include "clang/AST/ASTContext.h"

namespace cidx::lt {

SymbolConsumer::SymbolConsumer(SymbolEmitter &out) : out_(out) {}

void SymbolConsumer::HandleTranslationUnit(clang::ASTContext &context) {
  SymbolVisitor visitor(context, out_);
  visitor.TraverseDecl(context.getTranslationUnitDecl());
}

} // namespace cidx::lt
