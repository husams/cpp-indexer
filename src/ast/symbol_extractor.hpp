// SymbolExtractor: builds a SymbolRecord from a NamedDecl (the LibTooling
// analogue of AstIndexer::to_symbol in ast_symbols.cpp). Pure extraction — no
// walk policy (symbol_visitor.hpp) and no I/O (symbol_emitter.hpp) here.
#pragma once

#include "ast/symbol_record.hpp"

#include <optional>

namespace clang {
class ASTContext;
class NamedDecl;
} // namespace clang

namespace cidx::ast {

class SymbolExtractor {
public:
  explicit SymbolExtractor(const clang::ASTContext &context);

  // nullopt when the decl is not persisted (unmapped kind or no USR).
  std::optional<SymbolRecord> extract(const clang::NamedDecl *decl) const;

private:
  const clang::ASTContext &context_;
};

} // namespace cidx::ast
