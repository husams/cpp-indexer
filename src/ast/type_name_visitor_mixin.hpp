// TypeNameVisitorMixin: uses(7) for bare type names in expression position —
// sizeof(T)/alignof(T) argument types and explicit cast target types
// (static_cast<T>, (T)x, T(x) functional casts). A class template's pattern
// record is keyed as the template. CRTP mixin for BodyVisitor.
#pragma once

namespace clang {
class ExplicitCastExpr;
class UnaryExprOrTypeTraitExpr;
} // namespace clang

namespace cidx::lt {

class BodyEmitContext;

namespace detail {
void handle_sizeof_expr(const clang::UnaryExprOrTypeTraitExpr *expr,
                        BodyEmitContext &ctx);
void handle_explicit_cast_expr(const clang::ExplicitCastExpr *cast,
                               BodyEmitContext &ctx);
} // namespace detail

template <class Derived> class TypeNameVisitorMixin {
public:
  bool VisitUnaryExprOrTypeTraitExpr(clang::UnaryExprOrTypeTraitExpr *expr) {
    detail::handle_sizeof_expr(expr, static_cast<Derived &>(*this).body_ctx());
    return true;
  }

  bool VisitExplicitCastExpr(clang::ExplicitCastExpr *cast) {
    detail::handle_explicit_cast_expr(cast,
                                      static_cast<Derived &>(*this).body_ctx());
    return true;
  }

protected:
  ~TypeNameVisitorMixin() = default;
};

} // namespace cidx::lt
