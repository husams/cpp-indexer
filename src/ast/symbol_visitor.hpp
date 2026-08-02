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
//    VARIABLES and local function declarations are not symbols;
//  - class-template PARTIAL specializations are first-class symbols (they are
//    lexical decls, retained while the templated pattern stays suppressed);
//  - explicit function/method INSTANTIATIONS never appear as lexical decls,
//    so the Function/ClassTemplateDecl callbacks emit them from the
//    specialization lists, anchored at their point of instantiation.
//
// Edge/body extraction lives in separate visitors (later phases) — this file
// holds the symbol visitor only.
#pragma once

#include "ast/symbol_extractor.hpp"

#include "clang/AST/RecursiveASTVisitor.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace clang {
class ASTContext;
class Decl;
class ClassTemplateDecl;
class FunctionDecl;
class FunctionTemplateDecl;
class SourceManager;
class NamedDecl;
} // namespace clang

namespace cidx::ast {

class SymbolEmitter;
struct PassMetrics;

class SymbolVisitor : public clang::RecursiveASTVisitor<SymbolVisitor> {
public:
  using FileRouter = std::function<bool(const std::string &)>;

  // target_file empty: emit every non-system file's decls (the TSV probe's
  // whole-TU mode). Non-empty: emit ONLY decls of that file — the per-file
  // walk the interleaved indexer uses (index_file_notxn analogue).
  SymbolVisitor(clang::ASTContext &context, SymbolEmitter &out,
                std::string target_file = std::string(),
                PassMetrics *metrics = nullptr, FileRouter router = {});
  // Publishes the routing and identity spans this walk accumulated. The
  // visitor is constructed per pass, so the flush happens once per pass.
  ~SymbolVisitor();
  SymbolVisitor(const SymbolVisitor &) = delete;
  SymbolVisitor &operator=(const SymbolVisitor &) = delete;
  SymbolVisitor(SymbolVisitor &&) = delete;
  SymbolVisitor &operator=(SymbolVisitor &&) = delete;

  bool VisitDecl(clang::Decl *decl);
  bool VisitNamedDecl(clang::NamedDecl *decl);
  bool VisitFunctionTemplateDecl(clang::FunctionTemplateDecl *decl);
  bool VisitClassTemplateDecl(clang::ClassTemplateDecl *decl);

private:
  bool should_emit(const clang::NamedDecl *decl);
  void emit_explicit_instantiation(const clang::FunctionDecl *fd);
  // The normalized expansion path of a location, resolved once per FileID.
  // The answer depends only on the file, never on the position inside it, so
  // a TU that reaches one header through a thousand decls normalizes it once.
  auto normalized_file(clang::SourceLocation loc) -> const std::string &;

  clang::ASTContext &context_;
  clang::SourceManager &source_manager_;
  SymbolExtractor extractor_;
  SymbolEmitter &out_;
  std::string target_file_;
  PassMetrics *metrics_ = nullptr;
  FileRouter router_;
  // Bounded by the number of distinct files this translation unit reaches,
  // and destroyed with the visitor at the end of the pass.
  std::unordered_map<unsigned, std::string> normalized_files_;
  double routing_seconds_ = 0.0;
  double identity_seconds_ = 0.0;
};

} // namespace cidx::ast
