#include "ast/heap_visitor_mixin.hpp"

#include "ast/body_emit_context.hpp"
#include "ast/edge_sink.hpp"
#include "ast/value_source.hpp"

#include "clang/AST/ExprCXX.h"

namespace cidx::lt {

namespace detail {

void handle_new_expr(const clang::CXXNewExpr *expr, BodyEmitContext &ctx) {
  const std::string heap_usr = record_usr_of_type(expr->getType());
  if (!heap_usr.empty()) {
    if (const auto dst = ctx.sink().lookup_symbol_id(heap_usr)) {
      EdgeRecord fe;
      fe.src_id = ctx.src_id();
      fe.dst_id = *dst;
      fe.kind = 12; // construct-heap
      ctx.sink().add_edge(fe);
    }
  }
  // The allocated type name is spelled at the new-expr -> uses.
  ctx.emit_type_name_use(expr->getAllocatedTypeSourceInfo(),
                         /*promote_described_template=*/false);
}

void handle_delete_expr(const clang::CXXDeleteExpr *expr,
                        BodyEmitContext &ctx) {
  const clang::Expr *arg = expr->getArgument();
  if (arg == nullptr)
    return;
  const std::string usr = record_usr_of_type(arg->getType());
  if (usr.empty())
    return;
  if (const auto dst = ctx.sink().lookup_symbol_id(usr)) {
    EdgeRecord fe;
    fe.src_id = ctx.src_id();
    fe.dst_id = *dst;
    fe.kind = 16; // destroy
    ctx.sink().add_edge(fe);
  }
}

} // namespace detail

} // namespace cidx::lt
