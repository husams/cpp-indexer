#include "ast/symbol_consumer.hpp"

#include "ast/symbol_visitor.hpp"

#include "clang/AST/ASTContext.h"

namespace cidx::lt {

SymbolConsumer::SymbolConsumer(SymbolEmitter &out) : out_(out) {}

void SymbolConsumer::HandleTranslationUnit(clang::ASTContext &context) {
  SymbolVisitor visitor(context, out_);
  visitor.TraverseDecl(context.getTranslationUnitDecl());
}

} // namespace cidx::lt
