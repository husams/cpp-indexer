#include "ast/call_template_args.hpp"

#include "ast/display_name_rewrite.hpp"
#include "ast/edge_records.hpp"
#include "ast/edge_sink.hpp"
#include "ast/template_argument_encoder.hpp"
#include "ast/value_provenance.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"

#include <string>
#include <vector>

namespace cidx::lt {

namespace {

// Free-function specialization: every argument from the full specialization
// arg list encodes through the canonical encoder; display uses the encoded
// literal with a '?' placeholder that suppresses the rewrite.
void emit_free_function_spec_args(EdgeSink &sink,
                                  const TemplateArgumentEncoder &targ_encoder,
                                  const clang::FunctionDecl *callee,
                                  int64_t dst_id) {
  std::vector<std::string> display_args;
  if (const clang::TemplateArgumentList *args =
          callee->getTemplateSpecializationArgs()) {
    for (unsigned ai = 0; ai < args->size(); ++ai) {
      const auto record =
          targ_encoder.emit(dst_id, static_cast<int64_t>(ai), args->get(ai));
      if (record)
        display_args.push_back(TemplateArgumentEncoder::display_text(*record));
    }
  }
  // update_callable_template_display_name: skip on empty or any '?'.
  if (const auto disp = sink.lookup_display_name(dst_id)) {
    if (const auto rewritten =
            rewrite_template_display_name(*disp, display_args))
      sink.update_display_name(dst_id, *rewritten);
  }
}

// Method specialization: the explicit `<...>` args written at the call site.
void emit_method_call_site_args(const TemplateArgumentEncoder &targ_encoder,
                                const clang::Expr *site, int64_t dst_id) {
  const auto *call = llvm::dyn_cast<clang::CallExpr>(site);
  if (call == nullptr)
    return;
  const auto *me = llvm::dyn_cast_or_null<clang::MemberExpr>(
      normalize_value_expr(call->getCallee()));
  if (me == nullptr || !me->hasExplicitTemplateArgs())
    return;
  int64_t pos = 0;
  for (const clang::TemplateArgumentLoc &tal : me->template_arguments()) {
    const clang::TypeSourceInfo *tsi = tal.getTypeSourceInfo();
    targ_encoder.emit(dst_id, pos++, tal.getArgument(),
                      tsi != nullptr ? tsi->getType() : clang::QualType());
  }
}

} // namespace

void emit_callable_template_args(clang::ASTContext & /*context*/,
                                 EdgeSink &sink,
                                 const TemplateArgumentEncoder &targ_encoder,
                                 const clang::FunctionDecl *callee,
                                 const clang::Expr *site, int64_t dst_id) {
  if (dst_id < 0)
    return;
  // Template args of a callable specialization: free-function specs read the
  // full specialization arg list; method specs fall back to the explicit
  // `<...>` args written at the call site.
  const bool is_inst_member = callee->getPrimaryTemplate() != nullptr ||
                              callee->getMemberSpecializationInfo() != nullptr;
  if (!is_inst_member)
    return;
  if (!llvm::isa<clang::CXXMethodDecl>(callee))
    emit_free_function_spec_args(sink, targ_encoder, callee, dst_id);
  else
    emit_method_call_site_args(targ_encoder, site, dst_id);
}

} // namespace cidx::lt
