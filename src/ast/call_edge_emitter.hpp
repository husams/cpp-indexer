// CallEdgeEmitter: the shared resolved-call tail used by the call and construct
// visitor mixins — resolve the destination symbol id (mint, recovered lookup,
// or qual-name fallback), then delegate each concern to its own translation
// unit: call_template_args (spec arg rows + display name), receiver_provenance
// (the edge_site recv_* fields), and instantiation_edges (the B3 family).
#pragma once

#include <cstdint>
#include <string>

namespace clang {
class CallExpr;
class CXXConstructExpr;
class Expr;
class FunctionDecl;
class NamedDecl;
} // namespace clang

namespace cidx::ast {

class EdgeEmissionContext;

class CallEdgeEmitter {
public:
  explicit CallEdgeEmitter(EdgeEmissionContext &ctx) : ctx_(ctx) {}

  // Shared tail for a resolved/recovered callee decl. mint_as overrides the
  // decl used for USR/minting (a recovered FunctionTemplateDecl candidate).
  void emit_resolved_call(const clang::Expr *site,
                          const clang::FunctionDecl *callee, bool recovered,
                          const clang::NamedDecl *mint_as = nullptr);

  // Resolve a recovered (single-candidate dependent/overloaded) callee USR to a
  // symbol id (lookup, else qual_name+kind, else mint); -1 when unresolved.
  int64_t mint_resolved_target(const clang::Expr *site,
                               const clang::FunctionDecl *callee);
  int64_t emit_call_site(const clang::Expr *site, int64_t dst_id,
                         const clang::FunctionDecl *callee);
  int64_t resolve_recovered_target(const clang::NamedDecl *keyed,
                                   const std::string &callee_usr);

  void emit_call_args(const clang::Expr *site, const clang::CallExpr *call,
                      const clang::CXXConstructExpr *ctor, int64_t edge_id);

private:
  EdgeEmissionContext &ctx_;
};

} // namespace cidx::ast
