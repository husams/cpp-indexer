#include "ast/type_name_visitor_mixin.hpp"

#include "ast/body_emit_context.hpp"

#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"

namespace cidx::lt {

namespace detail {

void handle_sizeof_expr(const clang::UnaryExprOrTypeTraitExpr *expr,
                        BodyEmitContext &ctx) {
  if (expr->isArgumentType())
    ctx.emit_type_name_use(expr->getArgumentTypeInfo(),
                           /*promote_described_template=*/true);
}

void handle_explicit_cast_expr(const clang::ExplicitCastExpr *cast,
                               BodyEmitContext &ctx) {
  ctx.emit_type_name_use(cast->getTypeInfoAsWritten(),
                         /*promote_described_template=*/true);
}

} // namespace detail

} // namespace cidx::lt
