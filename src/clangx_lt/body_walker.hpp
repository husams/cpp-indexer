// BodyWalker: the LibTooling analogue of body_descent (ast_body.cpp) — walks a
// function-like definition's body emitting:
//   calls(1)  resolved, dependent-recovered (single overload), or overloaded
//             (every candidate) — with receiver provenance on the edge_site
//             and call_arg rows per non-literal argument
//   uses(7)   DeclRefExpr/MemberExpr to non-function symbols; bare type names
//             in expression position; local VarDecl declared types
//   construct forms 10/11/13/14 (value/temp/copy/move), 15 (factory),
//             12 (heap new), 16 (delete)
//   instantiates(5) + template_arg rows for callee/local-var template
//             specializations, incl. the instantiation-member promotion
//
// Manual recursion (not RAV) so parent-kind guards and cond_depth mirror the
// libclang child walk exactly. Emission goes through EdgeSink only.
#pragma once

#include "clangx_lt/instance_minter.hpp"
#include "clangx_lt/mint_builder.hpp"
#include "clangx_lt/template_arg_resolver.hpp"

#include "clang/Basic/SourceLocation.h"

#include <cstdint>
#include <string>

namespace clang {
class ASTContext;
class CXXConstructExpr;
class CXXDeleteExpr;
class CXXNewExpr;
class CallExpr;
class Decl;
class Expr;
class FunctionDecl;
class Stmt;
class VarDecl;
} // namespace clang

namespace cidx::lt {

class EdgeSink;

class BodyWalker {
public:
  BodyWalker(clang::ASTContext &context, EdgeSink &sink, int64_t src_id,
             int64_t file_id);

  // Walk fn's body (children of the definition, ctor-inits included).
  void walk(const clang::FunctionDecl *fn);

private:
  void descend(const clang::Stmt *stmt, const clang::Stmt *parent);
  void handle_stmt(const clang::Stmt *stmt, const clang::Stmt *parent);

  void handle_call(const clang::CallExpr *call, const clang::Stmt *parent);
  void handle_construct(const clang::CXXConstructExpr *ctor,
                        const clang::Stmt *parent);
  void handle_new(const clang::CXXNewExpr *expr);
  void handle_delete(const clang::CXXDeleteExpr *expr);
  void handle_var_decl(const clang::VarDecl *var);

  // Shared tail for a resolved/recovered callee decl. mint_as overrides the
  // decl used for USR/minting (a recovered FunctionTemplateDecl candidate).
  void emit_resolved_call(const clang::Expr *site,
                          const clang::FunctionDecl *callee, bool recovered,
                          const clang::NamedDecl *mint_as = nullptr);

  int64_t emit_site_edge(const clang::Expr *site, int64_t dst_id, int kind);
  int64_t emit_site_edge_at(clang::SourceLocation loc, int64_t dst_id,
                            int kind);
  void emit_call_args(const clang::Expr *site, const clang::CallExpr *call,
                      const clang::CXXConstructExpr *ctor, int64_t edge_id);

  clang::ASTContext &context_;
  EdgeSink &sink_;
  MintBuilder mint_;
  TemplateArgResolver resolver_;
  InstanceMinter minter_;
  int64_t src_id_;
  int64_t file_id_;
  int cond_depth_ = 0;
  std::string owner_usr_; // enclosing method's owning record (self-use skip)
};

} // namespace cidx::lt
