#include "ast/instance_minter.hpp"

#include "ast/decl_flags.hpp"
#include "ast/edge_sink.hpp"
#include "ast/instantiation_edges.hpp"
#include "ast/kind_map.hpp"
#include "ast/mint_builder.hpp"
#include "ast/names.hpp"
#include "ast/template_argument_encoder.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/Basic/SourceManager.h"

namespace cidx::ast {

InstanceMinter::InstanceMinter(const clang::ASTContext &context, EdgeSink &sink,
                               const MintBuilder &mint,
                               const TemplateArgumentEncoder &targ_encoder)
    : context_(context), sink_(sink), mint_(mint), targ_encoder_(targ_encoder) {
}

namespace {

// Peel pointer/reference/array wrappers so `X<B>*` mints the same instance
// as `X<B>`.
clang::QualType strip_indirections(clang::QualType type) {
  while (!type.isNull()) {
    if (type->isPointerType() || type->isReferenceType()) {
      type = type->getPointeeType();
    } else if (const clang::ArrayType *at = type->getAsArrayTypeUnsafe()) {
      type = at->getElementType();
    } else {
      break;
    }
  }
  return type;
}

} // namespace

// The template a specialization instance points at: the PARTIAL
// specialization when the instance was instantiated from one, else the
// primary template. Null for std:: / system templates (kept collapsed onto
// the primary).
const clang::NamedDecl *InstanceMinter::mintable_primary(
    const clang::ClassTemplateSpecializationDecl *spec) const {
  const clang::NamedDecl *primary = spec->getSpecializedTemplate();
  const auto from = spec->getInstantiatedFrom();
  if (const auto *partial =
          from.dyn_cast<clang::ClassTemplatePartialSpecializationDecl *>()) {
    primary = partial;
  }
  if (primary == nullptr) {
    return nullptr;
  }
  if (context_.getSourceManager().isInSystemHeader(primary->getLocation())) {
    return nullptr;
  }
  return primary;
}

void InstanceMinter::mint_instance_from_type(clang::QualType type) const {
  type = strip_indirections(type);
  if (type.isNull()) {
    return;
  }
  const auto *spec =
      llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
          type->getAsCXXRecordDecl());
  if (spec == nullptr) {
    return; // not a class-template specialization
  }
  const clang::NamedDecl *primary = mintable_primary(spec);
  if (primary == nullptr) {
    return;
  }
  const std::string inst_usr = usr_for_decl(spec);
  const std::string prim_usr = usr_for_decl(primary);
  if (inst_usr.empty() || prim_usr.empty() || inst_usr == prim_usr) {
    return;
  }
  const int64_t inst_id = mint_instance_pair(spec, primary, type);
  if (inst_id < 0) {
    return;
  }
  // template_arg rows on the instance through the one canonical encoder
  // (all argument kinds, per docs/improvements/template-arg-contract.md).
  const clang::TemplateArgumentList &args = spec->getTemplateArgs();
  for (unsigned ai = 0; ai < args.size(); ++ai) {
    targ_encoder_.emit(inst_id, static_cast<int64_t>(ai), args[ai]);
  }
}

// Instance node + primary stub + the structural edge between them:
// instantiates(5) for implicit/explicit instantiations, specializes(4) when
// the concrete type IS an explicit full specialization (using it instantiates
// nothing, so the flag stays false too). The instance's display_name is the
// WRITTEN type spelling ('X<B>'); the primary's kind defaults to
// "class-template" when unmapped. Returns the instance id, -1 when unmintable.
int64_t InstanceMinter::mint_instance_pair(
    const clang::ClassTemplateSpecializationDecl *spec,
    const clang::NamedDecl *primary, clang::QualType written_type) const {
  auto inst_req = mint_.build(spec);
  if (!inst_req) {
    return -1;
  }
  inst_req->display_name =
      clang::QualType(written_type).getAsString(printing_policy(context_));
  inst_req->is_instantiation = is_template_instantiation(spec);
  inst_req->is_named_instance = true;
  const int64_t inst_id = sink_.mint_symbol(*inst_req);

  auto prim_req = mint_.build(primary);
  if (!prim_req) {
    return -1;
  }
  if (cidx_symbol_kind_name(primary) == nullptr) {
    prim_req->kind_name = "class-template";
  }
  const int64_t prim_id = sink_.mint_symbol(*prim_req);

  EdgeRecord e;
  e.src_id = inst_id;
  e.dst_id = prim_id;
  e.kind = spec->getSpecializationKind() == clang::TSK_ExplicitSpecialization
               ? 4  // specializes: authored full specialization
               : 5; // instantiates: X<B> -> X (or its partial)
  sink_.ensure_edge(e);
  // The instantiation's fields (with substituted types) — never traversed, so
  // minted here with a field_of edge back to this instance.
  emit_instance_fields(sink_, mint_, spec, inst_id);
  return inst_id;
}

void InstanceMinter::mint_named_instance(
    const clang::TypedefNameDecl *alias) const {
  mint_instance_from_type(alias->getUnderlyingType());
}

} // namespace cidx::ast
