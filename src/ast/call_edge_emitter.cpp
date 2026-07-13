#include "ast/call_edge_emitter.hpp"

#include "ast/edge_emission_context.hpp"
#include "ast/call_template_args.hpp"
#include "ast/edge_sink.hpp"
#include "ast/instantiation_edges.hpp"
#include "ast/kind_map.hpp"
#include "ast/location.hpp"
#include "ast/names.hpp"
#include "ast/receiver_provenance.hpp"
#include "ast/usr.hpp"
#include "ast/value_provenance.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/SourceManager.h"

namespace cidx::ast {

int64_t CallEdgeEmitter::resolve_recovered_target(const clang::NamedDecl *keyed,
                                              const std::string &callee_usr) {
  int64_t dst_id = -1;
  if (const auto dst = ctx_.sink().lookup_symbol_id(callee_usr))
    dst_id = *dst;
  if (dst_id < 0) {
    const std::string qn = qualified_name(ctx_.context(), keyed);
    if (!qn.empty()) {
      const auto ids = ctx_.sink().symbol_ids_by_qual_name_kind(
          qn, cidx_stub_kind_name(keyed));
      if (ids.size() == 1)
        dst_id = ids[0];
    }
  }
  if (dst_id < 0 && !ctx_.context().getSourceManager().isInSystemHeader(
                        keyed->getLocation())) {
    if (auto req = ctx_.mint().build(keyed))
      dst_id = ctx_.sink().mint_symbol(*req);
  }
  return dst_id;
}

// Non-recovered target: mint the callee (flagging instantiation members) and
// emit its template-arg rows.
int64_t CallEdgeEmitter::mint_resolved_target(const clang::Expr *site,
                                          const clang::FunctionDecl *callee) {
  const bool is_inst_member =
      callee->getPrimaryTemplate() != nullptr ||
      callee->getMemberSpecializationInfo() != nullptr;
  auto req = ctx_.mint().build(callee);
  if (!req)
    return -1;
  req->is_instantiation = is_inst_member;
  const int64_t dst_id = ctx_.sink().mint_symbol(*req);
  emit_callable_template_args(ctx_.context(), ctx_.sink(),
                              ctx_.targ_encoder(), callee, site, dst_id);
  return dst_id;
}

// calls(1) edge + its edge_site row carrying the receiver provenance.
int64_t CallEdgeEmitter::emit_call_site(const clang::Expr *site, int64_t dst_id,
                                    const clang::FunctionDecl *callee) {
  const ReceiverProvenance recv =
      classify_call_receiver(ctx_.context(), site, callee);
  EdgeRecord e;
  e.src_id = ctx_.src_id();
  e.dst_id = dst_id;
  e.kind = 1;
  const int64_t edge_id = ctx_.sink().add_edge(e);
  const ExpansionLoc loc = expansion_loc(ctx_.context(), site->getBeginLoc());
  EdgeSiteRecord siter;
  siter.edge_id = edge_id;
  siter.file_id = ctx_.file_id();
  siter.line = loc.line;
  siter.col = loc.col;
  siter.conditional = ctx_.in_conditional() ? 1 : 0;
  if (!recv.src_kind.empty())
    siter.recv_src_kind = recv.src_kind;
  if (!recv.type_usr.empty())
    siter.recv_type_usr = recv.type_usr;
  if (!recv.decl_usr.empty())
    siter.recv_decl_usr = recv.decl_usr;
  siter.recv_param_pos = recv.param_pos;
  siter.recv_type_is_value = recv.type_is_value;
  ctx_.sink().add_edge_site(siter);
  return edge_id;
}

void CallEdgeEmitter::emit_resolved_call(const clang::Expr *site,
                                     const clang::FunctionDecl *callee,
                                     bool recovered,
                                     const clang::NamedDecl *mint_as) {
  const clang::NamedDecl *keyed =
      mint_as != nullptr ? mint_as : llvm::cast<clang::NamedDecl>(callee);
  const std::string callee_usr = usr_for_decl(keyed);
  if (callee_usr.empty())
    return;
  const int64_t dst_id = recovered
                             ? resolve_recovered_target(keyed, callee_usr)
                             : mint_resolved_target(site, callee);
  if (dst_id < 0)
    return;
  const int64_t edge_id = emit_call_site(site, dst_id, callee);
  emit_call_args(site, llvm::dyn_cast<clang::CallExpr>(site),
                 llvm::dyn_cast<clang::CXXConstructExpr>(site), edge_id);
  // B3 instantiates edges for a non-recovered template specialization.
  if (!recovered)
    emit_instantiation_edges(ctx_.context(), ctx_.sink(), ctx_.mint(),
                             ctx_.targ_encoder(), ctx_.src_id(), dst_id,
                             callee, callee_usr);
}

void CallEdgeEmitter::emit_call_args(const clang::Expr *site,
                                 const clang::CallExpr *call,
                                 const clang::CXXConstructExpr *ctor,
                                 int64_t edge_id) {
  const unsigned nargs = call != nullptr   ? call->getNumArgs()
                         : ctor != nullptr ? ctor->getNumArgs()
                                           : 0;
  const ExpansionLoc loc = expansion_loc(ctx_.context(), site->getBeginLoc());
  for (unsigned pos = 0; pos < nargs; ++pos) {
    const clang::Expr *arg =
        call != nullptr ? call->getArg(pos) : ctor->getArg(pos);
    if (arg == nullptr)
      continue;
    // A defaulted argument (e.g. std::string's allocator on libstdc++) counts
    // in the argument list; its value comes from the callee's default, not
    // caller source, so CXXDefaultArgExpr classifies "unknown". (On libc++
    // the same call takes no such arg.)
    const ValueSource vs = classify_value_source(ctx_.context(), arg);
    if (vs.src_kind == "literal")
      continue;
    CallArgRecord ca;
    ca.edge_id = edge_id;
    ca.file_id = ctx_.file_id();
    ca.line = loc.line;
    ca.col = loc.col;
    ca.position = static_cast<int64_t>(pos);
    ca.src_kind = vs.src_kind;
    if (!vs.type_usr.empty())
      ca.type_usr = vs.type_usr;
    if (!vs.decl_usr.empty())
      ca.decl_usr = vs.decl_usr;
    if (!vs.callee_usr.empty())
      ca.callee_usr = vs.callee_usr;
    if ((vs.src_kind == "member" || vs.src_kind == "global" ||
         vs.src_kind == "call_result" || vs.src_kind == "local") &&
        !vs.type_usr.empty()) {
      ca.type_is_value =
          type_is_value(decl_type_for_expr(normalize_value_expr(arg)),
                        vs.type_usr)
              ? 1
              : 0;
    }
    ctx_.sink().add_call_arg(ca);
  }
}

} // namespace cidx::ast
