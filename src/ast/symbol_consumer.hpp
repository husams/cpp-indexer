// ASTConsumer that drives the symbol visitor over a translation unit.
#pragma once

#include "clang/AST/ASTConsumer.h"

namespace clang {
class ASTContext;
}

namespace cidx::lt {

class SymbolEmitter;

class SymbolConsumer : public clang::ASTConsumer {
public:
  explicit SymbolConsumer(SymbolEmitter &out);

  void HandleTranslationUnit(clang::ASTContext &context) override;

private:
  SymbolEmitter &out_;
};

} // namespace cidx::lt
