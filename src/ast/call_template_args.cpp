#include "ast/call_template_args.hpp"

#include "ast/value_provenance.hpp"

#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/TemplateBase.h"

namespace cidx::ast {

std::vector<clang::QualType> written_template_args(const clang::Expr *site) {
  std::vector<clang::QualType> written;
  const auto *call = llvm::dyn_cast_or_null<clang::CallExpr>(site);
  if (call == nullptr) {
    return written;
  }
  const clang::Expr *callee = normalize_value_expr(call->getCallee());
  llvm::ArrayRef<clang::TemplateArgumentLoc> locs;
  if (const auto *me = llvm::dyn_cast_or_null<clang::MemberExpr>(callee)) {
    if (me->hasExplicitTemplateArgs()) {
      locs = me->template_arguments();
    }
  } else if (const auto *dre =
                 llvm::dyn_cast_or_null<clang::DeclRefExpr>(callee)) {
    if (dre->hasExplicitTemplateArgs()) {
      locs = dre->template_arguments();
    }
  }
  for (const clang::TemplateArgumentLoc &tal : locs) {
    clang::QualType t;
    // getTypeSourceInfo is only meaningful for TYPE arguments; other kinds
    // keep a null overlay slot so positions stay aligned.
    if (tal.getArgument().getKind() == clang::TemplateArgument::Type) {
      if (const clang::TypeSourceInfo *tsi = tal.getTypeSourceInfo()) {
        t = tsi->getType();
      }
    }
    written.push_back(t);
  }
  return written;
}

} // namespace cidx::ast
