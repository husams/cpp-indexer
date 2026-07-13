// RefVisitorMixin: uses(7) from reference expressions — DeclRefExpr and
// MemberExpr to non-function symbols (anchored at the NAME token), bare type
// names in a DeclRefExpr's qualifier (Color::Red), and explicit template
// arguments spelled at the expression (make_unique<Widget>(...)). CRTP mixin
// for BodyVisitor.
#pragma once

namespace clang {
class DeclRefExpr;
class MemberExpr;
} // namespace clang

namespace cidx::lt {

class BodyEmitContext;

namespace detail {
void handle_decl_ref_expr(const clang::DeclRefExpr *dre, BodyEmitContext &ctx);
void handle_member_expr(const clang::MemberExpr *me, BodyEmitContext &ctx);
} // namespace detail

template <class Derived> class RefVisitorMixin {
public:
  bool VisitDeclRefExpr(clang::DeclRefExpr *dre) {
    detail::handle_decl_ref_expr(dre, static_cast<Derived &>(*this).body_ctx());
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr *me) {
    detail::handle_member_expr(me, static_cast<Derived &>(*this).body_ctx());
    return true;
  }

protected:
  ~RefVisitorMixin() = default;
};

} // namespace cidx::lt
