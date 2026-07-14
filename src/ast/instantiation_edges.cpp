#include "ast/instantiation_edges.hpp"

#include "ast/decl_flags.hpp"
#include "ast/display_name_rewrite.hpp"
#include "ast/edge_records.hpp"
#include "ast/edge_sink.hpp"
#include "ast/mint_builder.hpp"
#include "ast/template_argument_encoder.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"

#include <string>

namespace cidx::ast {

namespace {

// spec -> primary structural edge kind: authored specializations use
// specializes(4); implicit/explicit instantiations use instantiates(5).
int64_t structural_edge_kind(clang::TemplateSpecializationKind tsk) {
  return tsk == clang::TSK_ExplicitSpecialization ? 4 : 5;
}

// Class-template-specialization owner: mint the owner with its TSK-derived
// flag, its method_of(9) edge, the owner -> primary structural edge, and the
// owner's template_arg rows through the one canonical encoder. All structural
// emission is idempotent (ensure_edge / keyed REPLACE).
void emit_spec_owner(EdgeSink &sink, MintBuilder &mint,
                     const TemplateArgumentEncoder &targ_encoder,
                     int64_t dst_id,
                     const clang::ClassTemplateSpecializationDecl *ospec) {
  const clang::ClassTemplateDecl *cls_prim = ospec->getSpecializedTemplate();
  auto oreq = mint.build(ospec);
  if (!oreq || cls_prim == nullptr)
    return;
  oreq->is_instantiation = is_template_instantiation(ospec);
  const int64_t type_id = sink.mint_symbol(*oreq);
  EdgeRecord mo;
  mo.src_id = dst_id;
  mo.dst_id = type_id;
  mo.kind = 9;
  sink.ensure_edge(mo);
  const std::string cp = usr_for_decl(cls_prim);
  if (!cp.empty())
    if (const auto cpid = sink.lookup_symbol_id(cp)) {
      EdgeRecord ie;
      ie.src_id = type_id;
      ie.dst_id = *cpid;
      ie.kind = structural_edge_kind(ospec->getSpecializationKind());
      sink.ensure_edge(ie);
    }
  const clang::TemplateArgumentList &args = ospec->getTemplateArgs();
  for (unsigned ai = 0; ai < args.size(); ++ai)
    targ_encoder.emit(type_id, static_cast<int64_t>(ai), args[ai]);
}

// Owner-type promotion for a concrete method specialization/instantiation:
// method_of(9) to the owning record, and — when the owner is itself a
// class-template specialization — the minted owner with its own identity.
// A plain-record owner is MINTED, not merely looked up: a nested record
// inside an instantiation (`Outer<int>::Inner`) is never a lexical decl, so
// its concrete member would otherwise lose method_of. Indexed owners
// resolve to their existing row (mint upserts by USR).
void emit_owner_promotion(EdgeSink &sink, MintBuilder &mint,
                          const TemplateArgumentEncoder &targ_encoder,
                          int64_t dst_id, const clang::CXXMethodDecl *m) {
  const clang::CXXRecordDecl *owner = m->getParent();
  if (owner == nullptr)
    return;
  const auto *ospec =
      llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(owner);
  if (ospec == nullptr) {
    if (auto oreq = mint.build(owner)) {
      oreq->is_instantiation = is_template_instantiation(owner);
      const int64_t oid = sink.mint_symbol(*oreq);
      EdgeRecord mo;
      mo.src_id = dst_id;
      mo.dst_id = oid;
      mo.kind = 9;
      sink.ensure_edge(mo);
    }
    return;
  }
  emit_spec_owner(sink, mint, targ_encoder, dst_id, ospec);
}

// template_arg rows from the FULL specialization argument list (deduced and
// defaulted arguments included), overlaid with the as-written types where
// positions align. Returns the display literals for the name rewrite.
std::vector<std::string>
emit_specialization_args(const TemplateArgumentEncoder &targ_encoder,
                         int64_t dst_id, const clang::FunctionDecl *fd,
                         const std::vector<clang::QualType> &written) {
  std::vector<std::string> display_args;
  const clang::TemplateArgumentList *args = fd->getTemplateSpecializationArgs();
  if (args == nullptr)
    return display_args;
  for (unsigned ai = 0; ai < args->size(); ++ai) {
    const clang::QualType w =
        ai < written.size() ? written[ai] : clang::QualType();
    const auto record =
        targ_encoder.emit(dst_id, static_cast<int64_t>(ai), args->get(ai), w);
    if (record)
      display_args.push_back(TemplateArgumentEncoder::display_text(*record));
  }
  return display_args;
}

} // namespace

std::optional<CallableTemplateInfo>
callable_template_info(const clang::FunctionDecl *fd) {
  CallableTemplateInfo info;
  if (const clang::FunctionTemplateDecl *ft = fd->getPrimaryTemplate())
    info.primary = ft;
  else if (fd->getMemberSpecializationInfo() != nullptr)
    info.primary = llvm::dyn_cast_or_null<clang::NamedDecl>(
        fd->getMemberSpecializationInfo()->getInstantiatedFrom());
  if (info.primary == nullptr)
    return std::nullopt;
  info.tsk = fd->getTemplateSpecializationKind();
  info.is_instantiation = is_template_instantiation(fd);
  return info;
}

void emit_callable_template_identity(
    EdgeSink &sink, MintBuilder &mint,
    const TemplateArgumentEncoder &targ_encoder, int64_t dst_id,
    const clang::FunctionDecl *fd, const CallableTemplateInfo &info,
    const std::vector<clang::QualType> &written) {
  if (dst_id < 0)
    return;
  const std::string prim_usr = usr_for_decl(info.primary);
  const std::string fd_usr = usr_for_decl(fd);
  if (!prim_usr.empty() && prim_usr != fd_usr)
    if (const auto prim = sink.lookup_symbol_id(prim_usr)) {
      EdgeRecord e;
      e.src_id = dst_id;
      e.dst_id = *prim;
      e.kind = structural_edge_kind(info.tsk);
      sink.ensure_edge(e);
    }

  const std::vector<std::string> display_args =
      emit_specialization_args(targ_encoder, dst_id, fd, written);
  if (const auto disp = sink.lookup_display_name(dst_id))
    if (const auto rewritten =
            rewrite_template_display_name(*disp, display_args))
      sink.update_display_name(dst_id, *rewritten);

  if (const auto *m = llvm::dyn_cast<clang::CXXMethodDecl>(fd))
    emit_owner_promotion(sink, mint, targ_encoder, dst_id, m);
}

bool is_explicit_instantiation_kind(clang::TemplateSpecializationKind tsk) {
  return tsk == clang::TSK_ExplicitInstantiationDeclaration ||
         tsk == clang::TSK_ExplicitInstantiationDefinition;
}

void for_each_explicit_callable_instantiation(
    const clang::FunctionTemplateDecl *tmpl,
    llvm::function_ref<void(const clang::FunctionDecl *)> fn) {
  for (const clang::FunctionDecl *fd : tmpl->specializations())
    if (is_explicit_instantiation_kind(fd->getTemplateSpecializationKind()))
      fn(fd);
}

namespace {

// Recursive walk of one instantiated context: callable members at this
// level, member function templates, and NESTED record/template contexts
// (`template void Outer<int>::Inner::run();` puts run inside the Inner
// record inside the specialization). decls() holds just what the TU
// actually instantiated.
void walk_instantiated_context(
    const clang::DeclContext *dc,
    llvm::function_ref<void(const clang::FunctionDecl *)> fn) {
  for (const clang::Decl *d : dc->decls()) {
    if (const auto *m = llvm::dyn_cast<clang::FunctionDecl>(d)) {
      if (!m->isImplicit() &&
          is_explicit_instantiation_kind(m->getTemplateSpecializationKind()))
        fn(m);
    } else if (const auto *ft =
                   llvm::dyn_cast<clang::FunctionTemplateDecl>(d)) {
      for_each_explicit_callable_instantiation(ft, fn);
    } else if (const auto *ct = llvm::dyn_cast<clang::ClassTemplateDecl>(d)) {
      for_each_explicit_callable_instantiation(ct, fn);
    } else if (const auto *rec = llvm::dyn_cast<clang::CXXRecordDecl>(d)) {
      if (!rec->isImplicit()) // skip the injected-class-name
        walk_instantiated_context(rec, fn);
    }
  }
}

} // namespace

void for_each_explicit_callable_instantiation(
    const clang::ClassTemplateDecl *tmpl,
    llvm::function_ref<void(const clang::FunctionDecl *)> fn) {
  // `template void PlainMember<int>::run();` creates the member inside the
  // (implicit) ClassTemplateSpecializationDecl — reachable only through the
  // class template's specialization list.
  for (const clang::ClassTemplateSpecializationDecl *spec :
       tmpl->specializations())
    walk_instantiated_context(spec, fn);
}

void emit_caller_instantiates(EdgeSink &sink, int64_t src_id,
                              const CallableTemplateInfo &info,
                              const std::string &callee_usr) {
  const std::string prim_usr = usr_for_decl(info.primary);
  if (prim_usr.empty() || prim_usr == callee_usr)
    return;
  const auto prim = sink.lookup_symbol_id(prim_usr);
  if (!prim)
    return;
  EdgeRecord inst;
  inst.src_id = src_id;
  inst.dst_id = *prim;
  inst.kind = 5;
  sink.add_edge(inst);
}

} // namespace cidx::ast
