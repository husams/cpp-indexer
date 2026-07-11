// Symbol-extraction visitor.
//
// RecursiveASTVisitor that applies cidx's symbol-pass WALK POLICY and hands
// qualifying decls to the SymbolExtractor, emitting through a SymbolEmitter.
// This is the LibTooling analogue of the libclang symbol pass
// (ast_symbols.cpp index_file_notxn + for_file_cursors + body-local descent):
//
//  - only decls whose expansion location is in the main file;
//  - templates appear once (the template decl; the templated pattern decl is
//    skipped, mirroring libclang's single FunctionTemplate/ClassTemplate
//    cursor);
//  - inside function/method bodies only named-type locals are emitted
//    (typedef/alias/enum/enum-constant/record/field/method/ctor/dtor) — local
//    VARIABLES and local function declarations are not symbols.
//
// Edge/body extraction lives in separate visitors (later phases) — this file
// holds the symbol visitor only.
#pragma once

#include "clangx_lt/symbol_extractor.hpp"

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
  bool should_emit(const clang::NamedDecl *decl) const;

  clang::ASTContext &context_;
  clang::SourceManager &source_manager_;
  SymbolExtractor extractor_;
  SymbolEmitter &out_;
};

} // namespace cidx::lt
