// ConstructVisitorMixin: a CXXConstructExpr maps to a call edge (the
// reference maps EVERY construct expression to a call — including an implicit
// default ctor for `B b;`), a uses(7) for temporary-object syntax naming the
// type, and a construction-form edge (10 value / 11 temp / 13 copy / 14 move)
// chosen by the ctor signature and the walk-parent kind. CRTP mixin for
// BodyVisitor.
#pragma once

namespace clang {
class CXXConstructExpr;
} // namespace clang

namespace cidx::lt {

class BodyEmitContext;
class CallEmitter;

namespace detail {
void handle_construct_expr(const clang::CXXConstructExpr *ctor,
                           BodyEmitContext &ctx, CallEmitter &emitter);
} // namespace detail

template <class Derived> class ConstructVisitorMixin {
public:
  bool VisitCXXConstructExpr(clang::CXXConstructExpr *ctor) {
    auto &d = static_cast<Derived &>(*this);
    detail::handle_construct_expr(ctor, d.body_ctx(), d.call_emitter());
    return true;
  }

protected:
  ~ConstructVisitorMixin() = default;
};

} // namespace cidx::lt
