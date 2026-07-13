// CallVisitorMixin: calls(1) from a CallExpr — resolved callee, dependent
// single-candidate recovery, overload-set fan-out (every candidate), and the
// make_unique/make_shared factory edge (15). CRTP mixin for BodyVisitor.
#pragma once

namespace clang {
class CallExpr;
} // namespace clang

namespace cidx::lt {

class BodyEmitContext;
class CallEmitter;

namespace detail {
void handle_call_expr(const clang::CallExpr *call, BodyEmitContext &ctx,
                      CallEmitter &emitter);
} // namespace detail

template <class Derived> class CallVisitorMixin {
public:
  bool VisitCallExpr(clang::CallExpr *call) {
    auto &d = static_cast<Derived &>(*this);
    detail::handle_call_expr(call, d.body_ctx(), d.call_emitter());
    return true;
  }

protected:
  ~CallVisitorMixin() = default;
};

} // namespace cidx::lt
