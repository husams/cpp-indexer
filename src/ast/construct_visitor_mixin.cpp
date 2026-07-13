#include "ast/construct_visitor_mixin.hpp"

#include "ast/body_emit_context.hpp"
#include "ast/call_emitter.hpp"
#include "ast/edge_sink.hpp"
#include "ast/value_source.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/Stmt.h"

namespace cidx::lt {

namespace detail {

void handle_construct_expr(const clang::CXXConstructExpr *ctor,
                           BodyEmitContext &ctx, CallEmitter &emitter) {
  const clang::CXXConstructorDecl *ref = ctor->getConstructor();
  if (ref == nullptr)
    return;
  // Every CXXConstructExpr maps to a call edge — including an implicit
  // default ctor for `B b;` (which mints a B() stub).
  emitter.emit_resolved_call(ctor, ref, /*recovered=*/false);
  // Temporary-object syntax (Widget{...} / Widget(x)) names the type -> uses.
  if (const auto *tmp = llvm::dyn_cast<clang::CXXTemporaryObjectExpr>(ctor))
    ctx.emit_type_name_use(tmp->getTypeSourceInfo(),
                           /*promote_described_template=*/false);

  // Construction form (10/11/13/14). A ctor under a CXXNewExpr parent emits
  // no form — construct-heap(12) covers it (parent-kind guard).
  const clang::Stmt *parent = ctx.parent();
  if (llvm::isa_and_nonnull<clang::CXXNewExpr>(parent))
    return;
  // A null/DeclStmt walk-parent marks the var-init position (the initializer
  // of a local variable, or a constructor member initializer).
  const std::string type_usr = record_usr_of_type(ctor->getType());
  if (type_usr.empty())
    return;
  const auto dst = ctx.sink().lookup_symbol_id(type_usr);
  if (!dst)
    return;
  const auto ctor_form = [&]() -> int {
    if (ref->getNumParams() == 1) {
      const std::string pt = ref->getParamDecl(0)->getType().getAsString(
          ctx.context().getPrintingPolicy());
      if (pt.find("&&") != std::string::npos)
        return 14; // construct-move
      if (pt.find('&') != std::string::npos)
        return 13; // construct-copy
    }
    return 10; // construct-value
  };
  const bool var_init =
      parent == nullptr || llvm::isa_and_nonnull<clang::DeclStmt>(parent);
  int form;
  if (var_init) {
    form = ctor_form();
  } else {
    const int sig = ctor_form();
    form = (sig == 13 || sig == 14) ? sig : 11; // construct-temp
  }
  EdgeRecord fe;
  fe.src_id = ctx.src_id();
  fe.dst_id = *dst;
  fe.kind = form;
  ctx.sink().add_edge(fe);
}

} // namespace detail

} // namespace cidx::lt
