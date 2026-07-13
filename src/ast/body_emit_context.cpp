#include "ast/body_emit_context.hpp"

#include "ast/edge_sink.hpp"
#include "ast/llvm_compat.hpp"
#include "ast/location.hpp"
#include "ast/type_use.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/TypeLoc.h"
#include "llvm/Config/llvm-config.h"

namespace cidx::lt {

namespace {

// Reference semantics locate TYPE_REF sites at the type NAME token (after any
// qualifier); TEMPLATE_REF at the template name.
clang::SourceLocation type_name_loc(clang::TypeLoc tl) {
#if LLVM_VERSION_MAJOR < 22
  // Pre-22 an elaborated (qualified) type wraps the tag/typedef loc; peel to
  // the named type so getNameLoc lands on the type NAME, not the qualifier.
  if (auto etl = tl.getAs<clang::ElaboratedTypeLoc>())
    tl = etl.getNamedTypeLoc();
#endif
  tl = tl.getUnqualifiedLoc();
  if (auto ts = tl.getAs<clang::TemplateSpecializationTypeLoc>())
    return ts.getTemplateNameLoc();
  // LLVM 22: tag/typedef TypeLocs embed their qualifier and carry NameLoc.
  if (auto tt = tl.getAs<clang::TagTypeLoc>())
    return tt.getNameLoc();
  if (auto td = tl.getAs<clang::TypedefTypeLoc>())
    return td.getNameLoc();
  if (auto spec = tl.getAs<clang::TypeSpecTypeLoc>())
    return spec.getNameLoc();
  return tl.getBeginLoc();
}

} // namespace

BodyEmitContext::BodyEmitContext(clang::ASTContext &context, EdgeSink &sink,
                                 int64_t src_id, int64_t file_id)
    : context_(context), sink_(sink), mint_(context, sink),
      resolver_(context, sink), minter_(context, sink, mint_, resolver_),
      src_id_(src_id), file_id_(file_id) {}

int64_t BodyEmitContext::emit_site_edge(const clang::Expr *site, int64_t dst_id,
                                        int kind) {
  return emit_site_edge_at(site->getBeginLoc(), dst_id, kind);
}

int64_t BodyEmitContext::emit_site_edge_at(clang::SourceLocation loc_in,
                                           int64_t dst_id, int kind) {
  EdgeRecord e;
  e.src_id = src_id_;
  e.dst_id = dst_id;
  e.kind = kind;
  const int64_t edge_id = sink_.add_edge(e);
  const ExpansionLoc loc = expansion_loc(context_, loc_in);
  EdgeSiteRecord siter;
  siter.edge_id = edge_id;
  siter.file_id = file_id_;
  siter.line = loc.line;
  siter.col = loc.col;
  siter.conditional = cond_depth_ > 0 ? 1 : 0;
  sink_.add_edge_site(siter);
  return edge_id;
}

void BodyEmitContext::emit_type_name_use(const clang::TypeSourceInfo *tsi,
                                         bool promote_described_template) {
  if (tsi == nullptr)
    return;
  const clang::NamedDecl *named = named_type_decl(tsi->getType());
  if (named == nullptr)
    return;
  if (promote_described_template) {
    if (const auto *rec = llvm::dyn_cast<clang::CXXRecordDecl>(named))
      if (const clang::ClassTemplateDecl *ct = rec->getDescribedClassTemplate())
        named = ct;
  }
  const std::string usr = usr_for_decl(named);
  if (usr.empty() || usr == owner_usr_)
    return;
  if (const auto dst = sink_.lookup_symbol_id(usr))
    if (*dst != src_id_)
      emit_site_edge_at(type_name_loc(tsi->getTypeLoc()), *dst, 7);
}

} // namespace cidx::lt
