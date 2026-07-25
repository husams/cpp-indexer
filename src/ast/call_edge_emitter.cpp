#include "ast/call_edge_emitter.hpp"

#include "ast/call_template_args.hpp"
#include "ast/decl_flags.hpp"
#include "ast/declaration_edge_visitor.hpp"
#include "ast/edge_emission_context.hpp"
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

namespace {

// Callable signatures reuse the declaration visitor through only its focused
// declaration services. The statement port already owns the pass budget.
class StatementDeclarationAdapter final : public DeclarationPassPorts {
public:
  explicit StatementDeclarationAdapter(StatementFactPorts &ports)
      : ports_(ports) {}

  auto lookup_symbol_id(const std::string &usr,
                        const std::optional<std::string> &source = std::nullopt)
      -> std::optional<std::int64_t> override {
    return ports_.lookup_symbol_id(usr, source);
  }
  auto mint_symbol(const MintRequest &request) -> std::int64_t override {
    return ports_.mint_symbol(request);
  }
  auto file_id_for_path(const std::string &path)
      -> std::optional<std::int64_t> override {
    return ports_.file_id_for_path(path);
  }
  auto type_arg_candidates(const std::string &name, bool qualified)
      -> std::vector<TypeArgCandidate> override {
    return ports_.type_arg_candidates(name, qualified);
  }
  auto symbol_ids_by_qual_name_kind(const std::string &qual_name,
                                    const std::string &kind_name)
      -> std::vector<std::int64_t> override {
    return ports_.symbol_ids_by_qual_name_kind(qual_name, kind_name);
  }
  auto add_edge(const EdgeRecord &edge) -> std::int64_t override {
    return ports_.add_edge(edge);
  }
  auto ensure_edge(const EdgeRecord &edge) -> std::int64_t override {
    return ports_.ensure_edge(edge);
  }
  void add_edge_site(const EdgeSiteRecord &site) override {
    ports_.add_edge_site(site);
  }
  void add_call_arg(const CallArgRecord &arg) override {
    ports_.add_call_arg(arg);
  }
  void add_template_param(const TemplateParamRecord &param) override {
    ports_.add_template_param(param);
  }
  void add_template_arg(const TemplateArgRecord &arg) override {
    ports_.add_template_arg(arg);
  }
  auto intern_type_node(const TypeNodeRecord &node) -> std::int64_t override {
    return ports_.intern_type_node(node);
  }
  void add_type_edge(std::int64_t src_id, std::int64_t kind,
                     std::int64_t position, std::int64_t dst_id) override {
    ports_.add_type_edge(src_id, kind, position, dst_id);
  }
  void
  replace_parameters(std::int64_t owner_id,
                     const std::vector<ParameterRecord> &parameters) override {
    ports_.replace_parameters(owner_id, parameters);
  }
  void add_symbol_type(std::int64_t symbol_id, std::int64_t kind,
                       std::int64_t type_id) override {
    ports_.add_symbol_type(symbol_id, kind, type_id);
  }

private:
  StatementFactPorts &ports_;
};

} // namespace

int64_t
CallEdgeEmitter::resolve_recovered_target(const clang::NamedDecl *keyed,
                                          const std::string &callee_usr) {
  int64_t dst_id = -1;
  if (const auto dst = ctx_.ports().lookup_symbol_id(
          callee_usr,
          expansion_loc(ctx_.context(), keyed->getLocation()).file)) {
    dst_id = *dst;
  }
  if (dst_id < 0) {
    const std::string qn = qualified_name(ctx_.context(), keyed);
    if (!qn.empty()) {
      const auto ids = ctx_.ports().symbol_ids_by_qual_name_kind(
          qn, cidx_stub_kind_name(keyed));
      if (ids.size() == 1) {
        dst_id = ids[0];
      }
    }
  }
  if (dst_id < 0 && !ctx_.context().getSourceManager().isInSystemHeader(
                        keyed->getLocation())) {
    if (auto req = ctx_.mint().build(keyed)) {
      dst_id = ctx_.ports().mint_symbol(*req);
    }
  }
  return dst_id;
}

// Non-recovered target: mint the callee (the instantiation flag comes from
// its TemplateSpecializationKind, so explicit specializations stay false) and
// re-assert its declaration-owned template identity — idempotent, so a target
// already handled by the declaration pass gains nothing twice. The call site
// contributes only the as-written `<...>` type spellings.
int64_t
CallEdgeEmitter::mint_resolved_target(const clang::Expr *site,
                                      const clang::FunctionDecl *callee) {
  const auto info = callable_template_info(callee);
  auto req = ctx_.mint().build(callee);
  if (!req) {
    return -1;
  }
  req->is_instantiation = info && info->is_instantiation;
  const int64_t dst_id = ctx_.ports().mint_symbol(*req);
  if (info) {
    emit_callable_template_identity(ctx_.ports(), ctx_.ports(), nullptr,
                                    ctx_.mint(), ctx_.targ_encoder(), dst_id,
                                    callee, *info, written_template_args(site));
  } else if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(callee);
             method != nullptr &&
             is_template_instantiation(method->getParent())) {
    emit_method_owner(ctx_.ports(), ctx_.ports(), ctx_.mint(),
                      ctx_.targ_encoder(), dst_id, method);
  }
  StatementDeclarationAdapter declaration_ports(ctx_.ports());
  DeclarationEdgeVisitor signature_visitor(ctx_.context(), declaration_ports,
                                           {}, ctx_.file_id());
  signature_visitor.emit_signature_types_for(callee, dst_id);
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
  const int64_t edge_id = ctx_.ports().add_edge(e);
  const ExpansionLoc loc = expansion_loc(ctx_.context(), site->getBeginLoc());
  EdgeSiteRecord siter;
  siter.edge_id = edge_id;
  siter.file_id = ctx_.file_id();
  siter.line = loc.line;
  siter.col = loc.col;
  siter.conditional = ctx_.in_conditional() ? 1 : 0;
  if (!recv.src_kind.empty()) {
    siter.recv_src_kind = recv.src_kind;
  }
  if (!recv.type_usr.empty()) {
    siter.recv_type_usr = recv.type_usr;
  }
  if (!recv.decl_usr.empty()) {
    siter.recv_decl_usr = recv.decl_usr;
  }
  siter.recv_param_pos = recv.param_pos;
  siter.recv_type_is_value = recv.type_is_value;
  ctx_.ports().add_edge_site(siter);
  return edge_id;
}

void CallEdgeEmitter::emit_resolved_call(const clang::Expr *site,
                                         const clang::FunctionDecl *callee,
                                         bool recovered,
                                         const clang::NamedDecl *mint_as) {
  const clang::NamedDecl *keyed =
      mint_as != nullptr ? mint_as : llvm::cast<clang::NamedDecl>(callee);
  const std::string callee_usr = usr_for_decl(keyed);
  if (callee_usr.empty()) {
    ctx_.record_unsupported("CallExpr", site->getBeginLoc(),
                            "callee has no canonical identity");
    return;
  }
  const int64_t dst_id = recovered ? resolve_recovered_target(keyed, callee_usr)
                                   : mint_resolved_target(site, callee);
  if (dst_id < 0) {
    ctx_.record_unsupported("CallExpr", site->getBeginLoc(),
                            "callee identity could not be resolved");
    return;
  }
  const int64_t edge_id = emit_call_site(site, dst_id, callee);
  emit_call_args(site, llvm::dyn_cast<clang::CallExpr>(site),
                 llvm::dyn_cast<clang::CXXConstructExpr>(site), edge_id);
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
    if (arg == nullptr) {
      continue;
    }
    // A defaulted argument (e.g. std::string's allocator on libstdc++) counts
    // in the argument list; its value comes from the callee's default, not
    // caller source, so CXXDefaultArgExpr classifies "unknown". (On libc++
    // the same call takes no such arg.)
    const ValueSource vs = classify_value_source(ctx_.context(), arg);
    if (vs.src_kind == "literal") {
      continue;
    }
    CallArgRecord ca;
    ca.edge_id = edge_id;
    ca.file_id = ctx_.file_id();
    ca.line = loc.line;
    ca.col = loc.col;
    ca.position = static_cast<int64_t>(pos);
    ca.src_kind = vs.src_kind;
    if (!vs.type_usr.empty()) {
      ca.type_usr = vs.type_usr;
    }
    if (!vs.decl_usr.empty()) {
      ca.decl_usr = vs.decl_usr;
    }
    if (!vs.callee_usr.empty()) {
      ca.callee_usr = vs.callee_usr;
    }
    if ((vs.src_kind == "member" || vs.src_kind == "global" ||
         vs.src_kind == "call_result" || vs.src_kind == "local") &&
        !vs.type_usr.empty()) {
      ca.type_is_value =
          type_is_value(decl_type_for_expr(normalize_value_expr(arg)),
                        vs.type_usr)
              ? 1
              : 0;
    }
    ctx_.ports().add_call_arg(ca);
  }
}

} // namespace cidx::ast
