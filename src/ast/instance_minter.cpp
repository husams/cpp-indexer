#include "ast/instance_minter.hpp"

#include "ast/edge_sink.hpp"
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

namespace cidx::lt {

InstanceMinter::InstanceMinter(const clang::ASTContext &context,
                               EdgeSink &sink, const MintBuilder &mint,
                               const TemplateArgumentEncoder &targ_encoder)
    : context_(context), sink_(sink), mint_(mint),
      targ_encoder_(targ_encoder) {}

void InstanceMinter::mint_instance_from_type(clang::QualType type) const {
  // Peel pointer/reference/array wrappers so `X<B>*` mints the same instance
  // as `X<B>` (mint_instance_from_type).
  for (int i = 0; i < 32; ++i) {
    if (type.isNull())
      return;
    if (type->isPointerType() || type->isReferenceType()) {
      type = type->getPointeeType();
    } else if (const clang::ArrayType *at = type->getAsArrayTypeUnsafe()) {
      type = at->getElementType();
    } else {
      break;
    }
  }
  if (type.isNull())
    return;
  const auto *spec = llvm::dyn_cast_or_null<
      clang::ClassTemplateSpecializationDecl>(type->getAsCXXRecordDecl());
  if (spec == nullptr)
    return; // not a class-template specialization
  // clang_getSpecializedCursorTemplate returns the PARTIAL specialization
  // when the instance was instantiated from one, else the primary template.
  const clang::NamedDecl *primary = spec->getSpecializedTemplate();
  {
    const auto from = spec->getInstantiatedFrom();
    if (const auto *partial =
            from.dyn_cast<clang::ClassTemplatePartialSpecializationDecl *>())
      primary = partial;
  }
  if (primary == nullptr)
    return;
  // Skip std:: / system templates (kept collapsed onto the primary).
  if (context_.getSourceManager().isInSystemHeader(primary->getLocation()))
    return;
  const std::string inst_usr = usr_for_decl(spec);
  const std::string prim_usr = usr_for_decl(primary);
  if (inst_usr.empty() || prim_usr.empty() || inst_usr == prim_usr)
    return;

  // Instance node: display_name is the WRITTEN type spelling ('X<B>').
  auto inst_req = mint_.build(spec);
  if (!inst_req)
    return;
  inst_req->display_name =
      clang::QualType(type).getAsString(context_.getPrintingPolicy());
  inst_req->is_instantiation = true;
  inst_req->is_named_instance = true;
  const int64_t inst_id = sink_.mint_symbol(*inst_req);

  // Primary stub: kind defaults to "class-template" when unmapped (partial
  // specialization patterns).
  auto prim_req = mint_.build(primary);
  if (!prim_req)
    return;
  if (cidx_symbol_kind_name(primary) == nullptr)
    prim_req->kind_name = "class-template";
  const int64_t prim_id = sink_.mint_symbol(*prim_req);

  EdgeRecord e;
  e.src_id = inst_id;
  e.dst_id = prim_id;
  e.kind = 5; // instantiates: X<B> -> X
  sink_.add_edge(e);

  // template_arg rows on the instance through the one canonical encoder
  // (all argument kinds, per docs/improvements/template-arg-contract.md).
  const clang::TemplateArgumentList &args = spec->getTemplateArgs();
  for (unsigned ai = 0; ai < args.size(); ++ai)
    targ_encoder_.emit(inst_id, static_cast<int64_t>(ai), args[ai]);
}

void InstanceMinter::mint_named_instance(
    const clang::TypedefNameDecl *alias) const {
  mint_instance_from_type(alias->getUnderlyingType());
}

} // namespace cidx::lt
