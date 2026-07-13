// HeapVisitorMixin: construct-heap(12) for `new T(...)` plus a uses(7) for
// the allocated type name, and destroy(16) for `delete p`. The ctor call
// inside a new-expression is handled by the construct mixin via traversal;
// the parent-kind guard there suppresses its form edge in favour of 12.
// CRTP mixin for BodyVisitor.
#pragma once

namespace clang {
class CXXDeleteExpr;
class CXXNewExpr;
} // namespace clang

namespace cidx::lt {

class BodyEmitContext;

namespace detail {
void handle_new_expr(const clang::CXXNewExpr *expr, BodyEmitContext &ctx);
void handle_delete_expr(const clang::CXXDeleteExpr *expr, BodyEmitContext &ctx);
} // namespace detail

template <class Derived> class HeapVisitorMixin {
public:
  bool VisitCXXNewExpr(clang::CXXNewExpr *expr) {
    detail::handle_new_expr(expr, static_cast<Derived &>(*this).body_ctx());
    return true;
  }

  bool VisitCXXDeleteExpr(clang::CXXDeleteExpr *expr) {
    detail::handle_delete_expr(expr, static_cast<Derived &>(*this).body_ctx());
    return true;
  }

protected:
  ~HeapVisitorMixin() = default;
};

} // namespace cidx::lt
