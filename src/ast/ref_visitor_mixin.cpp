#include "ast/ref_visitor_mixin.hpp"

#include "ast/body_emit_context.hpp"
#include "ast/edge_sink.hpp"
#include "ast/llvm_compat.hpp"
#include "ast/type_use.hpp"
#include "ast/usr.hpp"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/TemplateBase.h"
#include "llvm/Config/llvm-config.h"

namespace cidx::lt {

namespace {

// Function-like reference kinds are covered by the call mixins, not uses(7).
bool is_function_like_decl(const clang::Decl *d) {
  return llvm::isa<clang::FunctionDecl>(d);
}

} // namespace

namespace detail {

void handle_decl_ref_expr(const clang::DeclRefExpr *dre, BodyEmitContext &ctx) {
  // uses(7): non-function references, lookup-only.
  const clang::ValueDecl *ref = dre->getDecl();
  if (ref != nullptr && !is_function_like_decl(ref)) {
    const std::string usr = usr_for_decl(ref);
    if (!usr.empty()) {
      if (const auto dst = ctx.sink().lookup_symbol_id(usr))
        // The site anchors at the NAME (after qualifiers).
        ctx.emit_site_edge_at(dre->getLocation(), *dst, 7);
    }
  }
  // Bare type name in the qualifier (Color::Red): a type use under an
  // expression parent. NNS is a value type with a Kind enum in LLVM 22, a
  // pointer with a SpecifierKind in LLVM 21.
  {
    const clang::Type *t = nullptr;
#if LLVM_VERSION_MAJOR >= 22
    const clang::NestedNameSpecifier nns = dre->getQualifier();
    if (nns.getKind() == clang::NestedNameSpecifier::Kind::Type)
      t = nns.getAsType();
#else
    if (const clang::NestedNameSpecifier *nns = dre->getQualifier())
      t = nns->getAsType();
#endif
    if (t != nullptr) {
      if (const clang::NamedDecl *td = named_type_decl(clang::QualType(t, 0))) {
        const std::string usr = usr_for_decl(td);
        if (!usr.empty() && usr != ctx.owner_usr()) {
          if (const auto dst = ctx.sink().lookup_symbol_id(usr))
            if (*dst != ctx.src_id())
              ctx.emit_site_edge(dre, *dst, 7);
        }
      }
    }
  }
  // Explicit template arguments (make_unique<Widget>(...)): each named
  // TYPE argument is spelled at the expression — emit uses.
  if (dre->hasExplicitTemplateArgs()) {
    for (const clang::TemplateArgumentLoc &tal : dre->template_arguments()) {
      if (tal.getArgument().getKind() != clang::TemplateArgument::Type)
        continue;
      ctx.emit_type_name_use(tal.getTypeSourceInfo(),
                             /*promote_described_template=*/false);
    }
  }
}

void handle_member_expr(const clang::MemberExpr *me, BodyEmitContext &ctx) {
  const clang::ValueDecl *ref = me->getMemberDecl();
  if (ref != nullptr && !is_function_like_decl(ref)) {
    const std::string usr = usr_for_decl(ref);
    if (!usr.empty()) {
      if (const auto dst = ctx.sink().lookup_symbol_id(usr))
        // The site anchors at the member NAME token.
        ctx.emit_site_edge_at(me->getMemberLoc(), *dst, 7);
    }
  }
}

} // namespace detail

} // namespace cidx::lt
