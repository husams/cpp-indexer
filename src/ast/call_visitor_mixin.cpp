#include "ast/call_visitor_mixin.hpp"

#include "ast/body_emit_context.hpp"
#include "ast/call_emitter.hpp"
#include "ast/edge_sink.hpp"
#include "ast/kind_map.hpp"
#include "ast/names.hpp"
#include "ast/usr.hpp"
#include "ast/value_source.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/SourceManager.h"

#include <set>
#include <vector>

namespace cidx::lt {

namespace {

// The callee decl the way the reference resolves a call expression.
const clang::FunctionDecl *callee_decl(const clang::CallExpr *call) {
  return llvm::dyn_cast_or_null<clang::FunctionDecl>(call->getCalleeDecl());
}

// Candidate declarations of a dependent/overloaded callee: the OverloadExpr
// in callee position.
const clang::OverloadExpr *callee_overload_expr(const clang::CallExpr *call) {
  const clang::Expr *callee = call->getCallee();
  if (callee == nullptr)
    return nullptr;
  callee = peel_expr(callee);
  return llvm::dyn_cast_or_null<clang::OverloadExpr>(callee);
}

} // namespace

namespace detail {

void handle_call_expr(const clang::CallExpr *call, BodyEmitContext &ctx,
                      CallEmitter &emitter) {
  const clang::FunctionDecl *ref = callee_decl(call);
  bool recovered = false;
  if (ref == nullptr) {
    // Dependent/overloaded callee: single-candidate recovery, else link every
    // indexed overload.
    if (const clang::OverloadExpr *ovl = callee_overload_expr(call)) {
      std::vector<const clang::NamedDecl *> cands(ovl->decls_begin(),
                                                  ovl->decls_end());
      if (cands.size() == 1) {
        const clang::NamedDecl *cand = cands[0];
        const clang::NamedDecl *mint_as = nullptr;
        if (const auto *ftd =
                llvm::dyn_cast<clang::FunctionTemplateDecl>(cand)) {
          ref = ftd->getTemplatedDecl();
          mint_as = ftd;
        } else {
          ref = llvm::dyn_cast<clang::FunctionDecl>(cand);
        }
        if (ref != nullptr) {
          emitter.emit_resolved_call(call, ref, /*recovered=*/true, mint_as);
          return;
        }
      } else if (cands.size() >= 2) {
        std::set<int64_t> dst_ids;
        for (const clang::NamedDecl *cand : cands) {
          const std::string usr = usr_for_decl(cand);
          if (usr.empty())
            continue;
          if (const auto s = ctx.sink().lookup_symbol_id(usr)) {
            dst_ids.insert(*s);
            continue;
          }
          if (ctx.context().getSourceManager().isInSystemHeader(
                  cand->getLocation()))
            continue;
          if (auto req = ctx.mint().build(cand))
            dst_ids.insert(ctx.sink().mint_symbol(*req));
        }
        if (dst_ids.empty() && !cands.empty()) {
          const std::string qn = qualified_name(ctx.context(), cands[0]);
          if (!qn.empty())
            for (const int64_t id : ctx.sink().symbol_ids_by_qual_name_kind(
                     qn, cidx_stub_kind_name(cands[0])))
              dst_ids.insert(id);
        }
        for (const int64_t dst_id : dst_ids)
          ctx.emit_site_edge(call, dst_id, 1);
      }
    }
  }
  if (ref != nullptr)
    emitter.emit_resolved_call(call, ref, recovered);

  // Factory: make_unique<B> / make_shared<B> from system headers.
  if (ref != nullptr && !llvm::isa<clang::CXXMethodDecl>(ref)) {
    const std::string sp = spelling(ref);
    if ((sp == "make_unique" || sp == "make_shared") &&
        ctx.context().getSourceManager().isInSystemHeader(ref->getLocation())) {
      const clang::QualType result = call->getType().getCanonicalType();
      if (const auto *spec =
              llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
                  result->getAsCXXRecordDecl())) {
        const clang::TemplateArgumentList &args = spec->getTemplateArgs();
        if (args.size() > 0 &&
            args[0].getKind() == clang::TemplateArgument::Type) {
          const std::string fact_usr = record_usr_of_type(args[0].getAsType());
          if (!fact_usr.empty()) {
            if (const auto fact = ctx.sink().lookup_symbol_id(fact_usr)) {
              EdgeRecord fe;
              fe.src_id = ctx.src_id();
              fe.dst_id = *fact;
              fe.kind = 15; // factory-construct
              ctx.sink().add_edge(fe);
            }
          }
        }
      }
    }
  }
}

} // namespace detail

} // namespace cidx::lt
