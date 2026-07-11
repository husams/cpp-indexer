// Symbol-extraction visitor.
//
// RecursiveASTVisitor that walks decls in the main file and emits one
// SymbolRecord per persisted decl through a SymbolEmitter. This is the LibTooling
// analogue of the current libclang symbol pass (ast_symbols.cpp). Edge/body
// extraction lives in separate visitors (added in later phases) — this file
// holds the symbol visitor only.
#pragma once

#include "clang/AST/RecursiveASTVisitor.h"

namespace clang {
class ASTContext;
class SourceManager;
class NamedDecl;
} // namespace clang

namespace cidx::lt {

class SymbolEmitter;

class SymbolVisitor : public clang::RecursiveASTVisitor<SymbolVisitor> {
public:
  SymbolVisitor(clang::ASTContext &context, SymbolEmitter &out);

  bool VisitNamedDecl(clang::NamedDecl *decl);

private:
  clang::ASTContext &context_;
  clang::SourceManager &source_manager_;
  SymbolEmitter &out_;
};

} // namespace cidx::lt
