#include "clangx_lt/call_template_args.hpp"

#include "clangx_lt/display_name_rewrite.hpp"
#include "clangx_lt/edge_records.hpp"
#include "clangx_lt/edge_sink.hpp"
#include "clangx_lt/llvm_compat.hpp"
#include "clangx_lt/template_arg_resolver.hpp"
#include "clangx_lt/usr.hpp"
#include "clangx_lt/value_source.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"

#include <string>
#include <vector>

namespace cidx::lt {

void emit_callable_template_args(clang::ASTContext &context, EdgeSink &sink,
                                 const TemplateArgResolver &resolver,
                                 const clang::FunctionDecl *callee,
                                 const clang::Expr *site, int64_t dst_id) {
  if (dst_id < 0)
    return;
  // Template args of a callable specialization. libclang's cursor API exposes
  // them for FREE-FUNCTION specs only (methods return -1); METHOD specs fall
  // back to the explicit `<...>` args written at the call site.
  const bool is_inst_member = callee->getPrimaryTemplate() != nullptr ||
                              callee->getMemberSpecializationInfo() != nullptr;
  if (!is_inst_member)
    return;

  const clang::PrintingPolicy &policy = context.getPrintingPolicy();
  const auto emit_arg = [&](int64_t pos, const clang::TemplateArgument &arg,
                            clang::QualType written) {
    TemplateArgRecord ta;
    ta.owner_id = dst_id;
    ta.position = pos;
    if (arg.getKind() == clang::TemplateArgument::Type) {
      ta.arg_kind = 1;
      const clang::QualType t = written.isNull() ? arg.getAsType() : written;
      const std::string sp = t.getAsString(policy);
      if (!sp.empty())
        ta.literal = sp;
      if (const clang::TagDecl *td = t->getAsTagDecl()) {
        const std::string ref_usr = usr_for_decl(td);
        if (!ref_usr.empty())
          ta.ref_id = sink.lookup_symbol_id(ref_usr);
      }
      if (!ta.ref_id)
        ta.ref_id = resolver.resolve(ta.literal, callee);
    } else if (arg.getKind() == clang::TemplateArgument::Integral) {
      ta.arg_kind = 2;
      ta.literal = cidx::lt::compat::integral_to_string(arg.getAsIntegral());
    } else if (arg.getKind() == clang::TemplateArgument::Pack) {
      // Observed C-API behavior on LLVM 22: a pack argument reports raw kind 4
      // with no literal (empirically verified on make_shared / make_unique
      // specs); cidx stores that verbatim.
      ta.arg_kind = 4;
    } else {
      return;
    }
    sink.add_template_arg(ta);
  };

  if (!llvm::isa<clang::CXXMethodDecl>(callee)) {
    // Full cursor-API mirror (index_cursor_template_args): every arg gets a
    // row; non-Type/Integral kinds map to 2/3/4 with no literal and a '?'
    // display placeholder that suppresses the display rewrite.
    std::vector<std::string> display_args;
    if (const clang::TemplateArgumentList *args =
            callee->getTemplateSpecializationArgs()) {
      for (unsigned ai = 0; ai < args->size(); ++ai) {
        const clang::TemplateArgument &arg = args->get(ai);
        TemplateArgRecord ta;
        ta.owner_id = dst_id;
        ta.position = static_cast<int64_t>(ai);
        switch (arg.getKind()) {
        case clang::TemplateArgument::Type: {
          ta.arg_kind = 1;
          const std::string sp = arg.getAsType().getAsString(policy);
          if (!sp.empty())
            ta.literal = sp;
          if (const clang::TagDecl *td = arg.getAsType()->getAsTagDecl()) {
            const std::string ref_usr = usr_for_decl(td);
            if (!ref_usr.empty())
              ta.ref_id = sink.lookup_symbol_id(ref_usr);
          }
          if (!ta.ref_id)
            ta.ref_id = resolver.resolve(ta.literal, callee);
          display_args.push_back(ta.literal.value_or("?"));
          break;
        }
        case clang::TemplateArgument::Integral:
          ta.arg_kind = 2;
          ta.literal = cidx::lt::compat::integral_to_string(arg.getAsIntegral());
          display_args.push_back(*ta.literal);
          break;
        case clang::TemplateArgument::Declaration:
        case clang::TemplateArgument::NullPtr:
        case clang::TemplateArgument::Expression:
          ta.arg_kind = 2;
          display_args.push_back("?");
          break;
        case clang::TemplateArgument::Template:
        case clang::TemplateArgument::TemplateExpansion:
          ta.arg_kind = 3;
          display_args.push_back("?");
          break;
        case clang::TemplateArgument::Pack:
          ta.arg_kind = 4;
          display_args.push_back("?");
          break;
        default:
          continue;
        }
        sink.add_template_arg(ta);
      }
    }
    // update_callable_template_display_name: skip on empty or any '?'.
    if (const auto disp = sink.lookup_display_name(dst_id)) {
      if (const auto rewritten =
              rewrite_template_display_name(*disp, display_args))
        sink.update_display_name(dst_id, *rewritten);
    }
  } else if (const auto *call = llvm::dyn_cast<clang::CallExpr>(site)) {
    const clang::Expr *callee_expr = peel_expr(call->getCallee());
    if (const auto *me =
            llvm::dyn_cast_or_null<clang::MemberExpr>(callee_expr)) {
      if (me->hasExplicitTemplateArgs()) {
        int64_t pos = 0;
        for (const clang::TemplateArgumentLoc &tal : me->template_arguments()) {
          const clang::TypeSourceInfo *tsi = tal.getTypeSourceInfo();
          emit_arg(pos++, tal.getArgument(),
                   tsi != nullptr ? tsi->getType() : clang::QualType());
        }
      }
    }
  }
}

} // namespace cidx::lt
