#include "ast/call_edge_emitter.hpp"

#include "ast/call_template_args.hpp"
#include "ast/decl_flags.hpp"
#include "ast/edge_emission_context.hpp"
#include "ast/edge_sink.hpp"
#include "ast/instantiation_edges.hpp"
#include "ast/kind_map.hpp"
#include "ast/location.hpp"
#include "ast/names.hpp"
#include "ast/receiver_provenance.hpp"
#include "ast/type_graph.hpp"
#include "ast/usr.hpp"
#include "ast/value_provenance.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

namespace cidx::ast {

namespace {

std::optional<std::string> source_tokens(const clang::SourceRange range,
                                         const clang::SourceManager &sm,
                                         const clang::LangOptions &opts) {
  if (range.isInvalid()) {
    return std::nullopt;
  }
  const llvm::StringRef text = clang::Lexer::getSourceText(
      clang::CharSourceRange::getTokenRange(range), sm, opts);
  if (text.empty()) {
    return std::nullopt;
  }
  std::string out = text.str();
  const auto first = out.find_first_not_of(" \t\r\n");
  const auto last = out.find_last_not_of(" \t\r\n");
  return first == std::string::npos
             ? std::nullopt
             : std::optional<std::string>(out.substr(first, last - first + 1));
}

void emit_minted_signature(EdgeEmissionContext &ctx, int64_t owner_id,
                           const clang::FunctionDecl *fn) {
  TypeInterner types(ctx.context(), ctx.sink());
  if (!llvm::isa<clang::CXXConstructorDecl>(fn) &&
      !llvm::isa<clang::CXXDestructorDecl>(fn)) {
    if (const auto ret = types.intern(fn->getReturnType())) {
      ctx.sink().add_symbol_type(owner_id, kSymTypeReturnsK, *ret);
    }
  }
  const auto *pattern = fn->getPrimaryTemplate() != nullptr
                            ? fn->getPrimaryTemplate()->getTemplatedDecl()
                            : fn->getTemplateInstantiationPattern();
  std::vector<ParameterRecord> params;
  params.reserve(fn->getNumParams());
  std::optional<unsigned> concrete_pack_position;
  unsigned concrete_pack_size = 0;
  if (pattern != nullptr && fn->getNumParams() > pattern->getNumParams()) {
    for (unsigned pi = 0; pi < pattern->getNumParams(); ++pi) {
      if (!llvm::isa<clang::PackExpansionType>(
              pattern->getParamDecl(pi)->getType()))
        continue;
      if (const auto *args = fn->getTemplateSpecializationArgs()) {
        for (unsigned ai = 0; ai < args->size(); ++ai) {
          if (args->get(ai).getKind() == clang::TemplateArgument::Pack) {
            concrete_pack_position = pi;
            concrete_pack_size = args->get(ai).pack_size();
            break;
          }
        }
      }
      break;
    }
  }
  for (unsigned i = 0; i < fn->getNumParams(); ++i) {
    const clang::ParmVarDecl *p = fn->getParamDecl(i);
    ParameterRecord row;
    row.position = i;
    if (!p->getName().empty()) {
      row.name = p->getNameAsString();
    }
    const clang::QualType declared = p->getTypeSourceInfo() != nullptr
                                         ? p->getTypeSourceInfo()->getType()
                                         : p->getType();
    clang::QualType adjusted = p->getType();
    if (!adjusted->isReferenceType()) {
      clang::Qualifiers quals = adjusted.getLocalQualifiers();
      quals.removeConst();
      quals.removeVolatile();
      adjusted =
          clang::QualType(adjusted.getTypePtr(), quals.getAsOpaqueValue());
    }
    row.type_id = types.intern(adjusted);
    row.adjusted_type_id = row.type_id;
    row.declared_type_id = types.intern(declared);
    if (concrete_pack_position && i >= *concrete_pack_position &&
        i < *concrete_pack_position + concrete_pack_size) {
      row.position = *concrete_pack_position;
      row.pack_index = static_cast<int64_t>(i - *concrete_pack_position);
      params.push_back(std::move(row));
      continue;
    }
    const clang::ParmVarDecl *source_param = p;
    if (pattern != nullptr && i < pattern->getNumParams()) {
      source_param = pattern->getParamDecl(i);
    }
    if (source_param->hasDefaultArg()) {
      row.default_text = source_tokens(source_param->getDefaultArgRange(),
                                       ctx.context().getSourceManager(),
                                       ctx.context().getLangOpts());
      if (row.default_text && pattern != nullptr) {
        row.default_origin = qualified_name(ctx.context(), pattern);
      }
    }
    const clang::ParmVarDecl *forwarding_source =
        pattern != nullptr && i < pattern->getNumParams()
            ? pattern->getParamDecl(i)
            : nullptr;
    const clang::QualType forwarding_type = forwarding_source != nullptr
                                                ? forwarding_source->getType()
                                                : p->getType();
    const auto *source_rvalue =
        forwarding_type->getAs<clang::RValueReferenceType>();
    const bool forwarding =
        source_rvalue != nullptr &&
        !source_rvalue->getPointeeTypeAsWritten().hasLocalQualifiers() &&
        llvm::isa<clang::TemplateTypeParmType>(
            source_rvalue->getPointeeTypeAsWritten().getTypePtr()) &&
        pattern != nullptr &&
        pattern->getDescribedFunctionTemplate() != nullptr;
    if (forwarding) {
      row.reference_semantics = "from-forwarding";
    } else if (p->getType()->isLValueReferenceType()) {
      row.reference_semantics = "ordinary";
    } else if (p->getType()->isRValueReferenceType()) {
      row.reference_semantics = "ordinary";
    }
    const ExpansionLoc loc = expansion_loc(ctx.context(), p->getLocation());
    row.file_id = ctx.file_id();
    row.line = loc.line;
    row.col = loc.col;
    const bool is_pack_parameter =
        llvm::isa<clang::PackExpansionType>(p->getType()) ||
        (pattern != nullptr && i < pattern->getNumParams() &&
         llvm::isa<clang::PackExpansionType>(
             pattern->getParamDecl(i)->getType()));
    if (is_pack_parameter && !concrete_pack_position) {
      if (const auto *args = fn->getTemplateSpecializationArgs()) {
        for (unsigned ai = 0; ai < args->size(); ++ai) {
          if (args->get(ai).getKind() != clang::TemplateArgument::Pack)
            continue;
          const auto &pack = args->get(ai).getPackAsArray();
          for (unsigned pi = 0; pi < pack.size(); ++pi) {
            if (pack[pi].getKind() != clang::TemplateArgument::Type)
              continue;
            ParameterRecord expanded = row;
            expanded.pack_index = static_cast<int64_t>(pi);
            expanded.declared_type_id = types.intern(pack[pi].getAsType());
            expanded.adjusted_type_id = expanded.declared_type_id;
            expanded.type_id = expanded.declared_type_id;
            params.push_back(std::move(expanded));
          }
          if (!pack.empty())
            goto expanded_pack_done;
        }
      }
    }
    params.push_back(std::move(row));
  expanded_pack_done:;
  }
  ctx.sink().replace_parameters(owner_id, params);
}

} // namespace

int64_t
CallEdgeEmitter::resolve_recovered_target(const clang::NamedDecl *keyed,
                                          const std::string &callee_usr) {
  int64_t dst_id = -1;
  if (const auto dst = ctx_.sink().lookup_symbol_id(callee_usr)) {
    dst_id = *dst;
  }
  if (dst_id < 0) {
    const std::string qn = qualified_name(ctx_.context(), keyed);
    if (!qn.empty()) {
      const auto ids = ctx_.sink().symbol_ids_by_qual_name_kind(
          qn, cidx_stub_kind_name(keyed));
      if (ids.size() == 1) {
        dst_id = ids[0];
      }
    }
  }
  if (dst_id < 0 && !ctx_.context().getSourceManager().isInSystemHeader(
                        keyed->getLocation())) {
    if (auto req = ctx_.mint().build(keyed)) {
      dst_id = ctx_.sink().mint_symbol(*req);
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
  const int64_t dst_id = ctx_.sink().mint_symbol(*req);
  if (info) {
    emit_callable_template_identity(ctx_.sink(), ctx_.mint(),
                                    ctx_.targ_encoder(), dst_id, callee, *info,
                                    written_template_args(site));
  } else if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(callee);
             method != nullptr &&
             is_template_instantiation(method->getParent())) {
    emit_method_owner(ctx_.sink(), ctx_.mint(), ctx_.targ_encoder(), dst_id,
                      method);
  }
  emit_minted_signature(ctx_, dst_id, callee);
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
  if (callee_usr.empty()) {
    return;
  }
  const int64_t dst_id = recovered ? resolve_recovered_target(keyed, callee_usr)
                                   : mint_resolved_target(site, callee);
  if (dst_id < 0) {
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
    ctx_.sink().add_call_arg(ca);
  }
}

} // namespace cidx::ast
