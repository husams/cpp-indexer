#include "ast/local_var_visitor_mixin.hpp"

#include "ast/body_emit_context.hpp"
#include "ast/edge_sink.hpp"
#include "ast/location.hpp"
#include "ast/type_use.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

namespace cidx::lt {

namespace detail {

bool var_listed_in_current_decl_stmt(const clang::VarDecl *var,
                                     const BodyEmitContext &ctx) {
  const auto *ds = llvm::dyn_cast_or_null<clang::DeclStmt>(ctx.current_stmt());
  return ds != nullptr && llvm::is_contained(ds->decls(), var);
}

void handle_local_var_decl(const clang::VarDecl *var, BodyEmitContext &ctx) {
  // Declared type -> uses edge + instance mint + class template
  // instantiates/template_arg rows.
  const ExpansionLoc loc = expansion_loc(ctx.context(), var->getLocation());
  emit_type_use(ctx.sink(), ctx.src_id(), var->getType(), ctx.file_id(), loc,
                ctx.in_conditional() ? 1 : 0);
  ctx.minter().mint_instance_from_type(var->getType());

  const auto *spec =
      llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
          var->getType()->getAsCXXRecordDecl());
  if (spec == nullptr)
    return;
  const clang::ClassTemplateDecl *primary = spec->getSpecializedTemplate();
  if (primary == nullptr)
    return;
  const std::string prim_usr = usr_for_decl(primary);
  if (prim_usr.empty())
    return;
  const auto prim = ctx.sink().lookup_symbol_id(prim_usr);
  if (!prim)
    return; // lookup-only: no stubs for stdlib templates
  EdgeRecord inst;
  inst.src_id = ctx.src_id();
  inst.dst_id = *prim;
  inst.kind = 5; // instantiates
  ctx.sink().add_edge(inst);

  // Args print AS WRITTEN (`Box<Color> bc;` inside geo stores 'Color', not
  // 'geo::Color'): prefer the sugared args off the declared type.
  llvm::SmallVector<clang::QualType, 4> arg_types;
  if (const auto *tst =
          var->getType()->getAs<clang::TemplateSpecializationType>()) {
    for (const clang::TemplateArgument &a : tst->template_arguments())
      arg_types.push_back(a.getKind() == clang::TemplateArgument::Type
                              ? a.getAsType()
                              : clang::QualType());
  } else {
    const clang::TemplateArgumentList &args = spec->getTemplateArgs();
    for (unsigned ai = 0; ai < args.size(); ++ai)
      arg_types.push_back(args[ai].getKind() == clang::TemplateArgument::Type
                              ? args[ai].getAsType()
                              : clang::QualType());
  }
  const clang::PrintingPolicy &policy = ctx.context().getPrintingPolicy();
  for (unsigned ai = 0; ai < arg_types.size(); ++ai) {
    const clang::QualType at = arg_types[ai];
    if (at.isNull())
      continue;
    TemplateArgRecord ta;
    ta.owner_id = ctx.src_id();
    ta.position = static_cast<int64_t>(ai);
    ta.arg_kind = 1;
    const std::string spelling = at.getAsString(policy);
    if (!spelling.empty())
      ta.literal = spelling;
    if (const clang::TagDecl *td = at->getAsTagDecl()) {
      const std::string ref_usr = usr_for_decl(td);
      if (!ref_usr.empty())
        ta.ref_id = ctx.sink().lookup_symbol_id(ref_usr);
    }
    if (!ta.ref_id)
      ta.ref_id = ctx.resolver().resolve(ta.literal, var);
    ctx.sink().add_template_arg(ta);
  }
}

} // namespace detail

} // namespace cidx::lt
