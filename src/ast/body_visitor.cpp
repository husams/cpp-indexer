#include "ast/body_visitor.hpp"

#include "ast/edge_sink.hpp"
#include "ast/kind_map.hpp"
#include "ast/location.hpp"
#include "ast/names.hpp"
#include "ast/type_use.hpp"
#include "ast/usr.hpp"
#include "ast/value_source.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Config/llvm-config.h"

#include <set>
#include <string>
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

// Function-like reference kinds are covered by the call callbacks, not
// uses(7).
bool is_function_like_decl(const clang::Decl *d) {
  return llvm::isa<clang::FunctionDecl>(d);
}

} // namespace

BodyVisitor::BodyVisitor(clang::ASTContext &context, EdgeSink &sink,
                         int64_t src_id, int64_t file_id)
    : ctx_(context, sink, src_id, file_id), emitter_(ctx_) {}

void BodyVisitor::walk(const clang::FunctionDecl *fn) {
  // Owner USR for the self-owner skip in the type-name branches.
  if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn)) {
    if (const clang::CXXRecordDecl *owner = method->getParent()) {
      const clang::NamedDecl *o = owner;
      if (const clang::ClassTemplateDecl *ct =
              owner->getDescribedClassTemplate())
        o = ct;
      ctx_.set_owner_usr(usr_for_decl(o));
    }
  }
  // Constructor member initializers carry calls/uses too; their init
  // expression is a var-init position for the construction form.
  if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fn)) {
    for (const clang::CXXCtorInitializer *init : ctor->inits()) {
      if (init->isWritten() && init->getInit() != nullptr) {
        const InitScope scope(direct_init_, init->getInit());
        TraverseStmt(init->getInit());
      }
    }
  }
  if (clang::Stmt *body = fn->getBody())
    TraverseStmt(body);
}

// ---- narrow scoped traversal overrides -------------------------------------

bool BodyVisitor::TraverseIfStmt(clang::IfStmt *stmt) {
  const CondScope guard(ctx_);
  return RecursiveASTVisitor::TraverseIfStmt(stmt);
}

bool BodyVisitor::TraverseForStmt(clang::ForStmt *stmt) {
  const CondScope guard(ctx_);
  return RecursiveASTVisitor::TraverseForStmt(stmt);
}

bool BodyVisitor::TraverseWhileStmt(clang::WhileStmt *stmt) {
  const CondScope guard(ctx_);
  return RecursiveASTVisitor::TraverseWhileStmt(stmt);
}

bool BodyVisitor::TraverseDoStmt(clang::DoStmt *stmt) {
  const CondScope guard(ctx_);
  return RecursiveASTVisitor::TraverseDoStmt(stmt);
}

bool BodyVisitor::TraverseSwitchStmt(clang::SwitchStmt *stmt) {
  const CondScope guard(ctx_);
  return RecursiveASTVisitor::TraverseSwitchStmt(stmt);
}

bool BodyVisitor::TraverseConditionalOperator(
    clang::ConditionalOperator *stmt) {
  const CondScope guard(ctx_);
  return RecursiveASTVisitor::TraverseConditionalOperator(stmt);
}

bool BodyVisitor::TraverseVarDecl(clang::VarDecl *var) {
  const InitScope scope(direct_init_, var->getInit());
  return RecursiveASTVisitor::TraverseVarDecl(var);
}

bool BodyVisitor::TraverseCXXNewExpr(clang::CXXNewExpr *expr) {
  const InitScope scope(new_init_, expr->getInitializer());
  return RecursiveASTVisitor::TraverseCXXNewExpr(expr);
}

// ---- calls ------------------------------------------------------------------

bool BodyVisitor::VisitCallExpr(clang::CallExpr *call) {
  emit_call(call);
  return true;
}

void BodyVisitor::emit_call(const clang::CallExpr *call) {
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
          emitter_.emit_resolved_call(call, ref, /*recovered=*/true, mint_as);
          return;
        }
      } else if (cands.size() >= 2) {
        std::set<int64_t> dst_ids;
        for (const clang::NamedDecl *cand : cands) {
          const std::string usr = usr_for_decl(cand);
          if (usr.empty())
            continue;
          if (const auto s = ctx_.sink().lookup_symbol_id(usr)) {
            dst_ids.insert(*s);
            continue;
          }
          if (ctx_.context().getSourceManager().isInSystemHeader(
                  cand->getLocation()))
            continue;
          if (auto req = ctx_.mint().build(cand))
            dst_ids.insert(ctx_.sink().mint_symbol(*req));
        }
        if (dst_ids.empty() && !cands.empty()) {
          const std::string qn = qualified_name(ctx_.context(), cands[0]);
          if (!qn.empty())
            for (const int64_t id : ctx_.sink().symbol_ids_by_qual_name_kind(
                     qn, cidx_stub_kind_name(cands[0])))
              dst_ids.insert(id);
        }
        for (const int64_t dst_id : dst_ids)
          ctx_.emit_site_edge(call, dst_id, 1);
      }
    }
  }
  if (ref != nullptr)
    emitter_.emit_resolved_call(call, ref, recovered);

  // Factory: make_unique<B> / make_shared<B> from system headers.
  if (ref != nullptr && !llvm::isa<clang::CXXMethodDecl>(ref)) {
    const std::string sp = spelling(ref);
    if ((sp == "make_unique" || sp == "make_shared") &&
        ctx_.context().getSourceManager().isInSystemHeader(
            ref->getLocation())) {
      const clang::QualType result = call->getType().getCanonicalType();
      if (const auto *spec =
              llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
                  result->getAsCXXRecordDecl())) {
        const clang::TemplateArgumentList &args = spec->getTemplateArgs();
        if (args.size() > 0 &&
            args[0].getKind() == clang::TemplateArgument::Type) {
          const std::string fact_usr = record_usr_of_type(args[0].getAsType());
          if (!fact_usr.empty()) {
            if (const auto fact = ctx_.sink().lookup_symbol_id(fact_usr)) {
              EdgeRecord fe;
              fe.src_id = ctx_.src_id();
              fe.dst_id = *fact;
              fe.kind = 15; // factory-construct
              ctx_.sink().add_edge(fe);
            }
          }
        }
      }
    }
  }
}

// ---- construction -----------------------------------------------------------

bool BodyVisitor::VisitCXXConstructExpr(clang::CXXConstructExpr *ctor) {
  emit_construct(ctor);
  return true;
}

void BodyVisitor::emit_construct(const clang::CXXConstructExpr *ctor) {
  const clang::CXXConstructorDecl *ref = ctor->getConstructor();
  if (ref == nullptr)
    return;
  // Every CXXConstructExpr maps to a call edge — including an implicit
  // default ctor for `B b;` (which mints a B() stub).
  emitter_.emit_resolved_call(ctor, ref, /*recovered=*/false);
  // Temporary-object syntax (Widget{...} / Widget(x)) names the type -> uses.
  if (const auto *tmp = llvm::dyn_cast<clang::CXXTemporaryObjectExpr>(ctor))
    ctx_.emit_type_name_use(tmp->getTypeSourceInfo(),
                            /*promote_described_template=*/false);

  // Construction form (10/11/13/14). A ctor that is a new-expression's
  // initializer emits no form — construct-heap(12) covers it.
  if (ctor == new_init_)
    return;
  const std::string type_usr = record_usr_of_type(ctor->getType());
  if (type_usr.empty())
    return;
  const auto dst = ctx_.sink().lookup_symbol_id(type_usr);
  if (!dst)
    return;
  const auto ctor_form = [&]() -> int {
    if (ref->getNumParams() == 1) {
      const std::string pt = ref->getParamDecl(0)->getType().getAsString(
          ctx_.context().getPrintingPolicy());
      if (pt.find("&&") != std::string::npos)
        return 14; // construct-move
      if (pt.find('&') != std::string::npos)
        return 13; // construct-copy
    }
    return 10; // construct-value
  };
  // A construct expression that IS the direct initializer of a variable or of
  // a constructor member initializer sits in the var-init position.
  const bool var_init = ctor == direct_init_;
  int form;
  if (var_init) {
    form = ctor_form();
  } else {
    const int sig = ctor_form();
    form = (sig == 13 || sig == 14) ? sig : 11; // construct-temp
  }
  EdgeRecord fe;
  fe.src_id = ctx_.src_id();
  fe.dst_id = *dst;
  fe.kind = form;
  ctx_.sink().add_edge(fe);
}

// ---- heap -------------------------------------------------------------------

bool BodyVisitor::VisitCXXNewExpr(clang::CXXNewExpr *expr) {
  const std::string heap_usr = record_usr_of_type(expr->getType());
  if (!heap_usr.empty()) {
    if (const auto dst = ctx_.sink().lookup_symbol_id(heap_usr)) {
      EdgeRecord fe;
      fe.src_id = ctx_.src_id();
      fe.dst_id = *dst;
      fe.kind = 12; // construct-heap
      ctx_.sink().add_edge(fe);
    }
  }
  // The allocated type name is spelled at the new-expr -> uses.
  ctx_.emit_type_name_use(expr->getAllocatedTypeSourceInfo(),
                          /*promote_described_template=*/false);
  return true;
}

bool BodyVisitor::VisitCXXDeleteExpr(clang::CXXDeleteExpr *expr) {
  const clang::Expr *arg = expr->getArgument();
  if (arg == nullptr)
    return true;
  const std::string usr = record_usr_of_type(arg->getType());
  if (usr.empty())
    return true;
  if (const auto dst = ctx_.sink().lookup_symbol_id(usr)) {
    EdgeRecord fe;
    fe.src_id = ctx_.src_id();
    fe.dst_id = *dst;
    fe.kind = 16; // destroy
    ctx_.sink().add_edge(fe);
  }
  return true;
}

// ---- references -------------------------------------------------------------

bool BodyVisitor::VisitDeclRefExpr(clang::DeclRefExpr *dre) {
  // uses(7): non-function references, lookup-only.
  const clang::ValueDecl *ref = dre->getDecl();
  if (ref != nullptr && !is_function_like_decl(ref)) {
    const std::string usr = usr_for_decl(ref);
    if (!usr.empty()) {
      if (const auto dst = ctx_.sink().lookup_symbol_id(usr))
        // The site anchors at the NAME (after qualifiers).
        ctx_.emit_site_edge_at(dre->getLocation(), *dst, 7);
    }
  }
  // Bare type name in the qualifier (Color::Red): a type use under an
  // expression parent. NNS is a value type with a Kind enum in LLVM 22, a
  // pointer with a SpecifierKind in LLVM 21.
  {
    const clang::Type *t = nullptr;
#if LLVM_VERSION_MAJOR >= 22
    const clang::NestedNameSpecifier nns = dre->getQualifier();
    if (nns.getKind() == clang::NestedNameSpecifier::Kind::Type)
      t = nns.getAsType();
#else
    if (const clang::NestedNameSpecifier *nns = dre->getQualifier())
      t = nns->getAsType();
#endif
    if (t != nullptr) {
      if (const clang::NamedDecl *td = named_type_decl(clang::QualType(t, 0))) {
        const std::string usr = usr_for_decl(td);
        if (!usr.empty() && usr != ctx_.owner_usr()) {
          if (const auto dst = ctx_.sink().lookup_symbol_id(usr))
            if (*dst != ctx_.src_id())
              ctx_.emit_site_edge(dre, *dst, 7);
        }
      }
    }
  }
  // Explicit template arguments (make_unique<Widget>(...)): each named
  // TYPE argument is spelled at the expression — emit uses.
  if (dre->hasExplicitTemplateArgs()) {
    for (const clang::TemplateArgumentLoc &tal : dre->template_arguments()) {
      if (tal.getArgument().getKind() != clang::TemplateArgument::Type)
        continue;
      ctx_.emit_type_name_use(tal.getTypeSourceInfo(),
                              /*promote_described_template=*/false);
    }
  }
  return true;
}

bool BodyVisitor::VisitMemberExpr(clang::MemberExpr *me) {
  const clang::ValueDecl *ref = me->getMemberDecl();
  if (ref != nullptr && !is_function_like_decl(ref)) {
    const std::string usr = usr_for_decl(ref);
    if (!usr.empty()) {
      if (const auto dst = ctx_.sink().lookup_symbol_id(usr))
        // The site anchors at the member NAME token.
        ctx_.emit_site_edge_at(me->getMemberLoc(), *dst, 7);
    }
  }
  return true;
}

// ---- spelled type names -----------------------------------------------------

bool BodyVisitor::VisitUnaryExprOrTypeTraitExpr(
    clang::UnaryExprOrTypeTraitExpr *expr) {
  if (expr->isArgumentType())
    ctx_.emit_type_name_use(expr->getArgumentTypeInfo(),
                            /*promote_described_template=*/true);
  return true;
}

bool BodyVisitor::VisitExplicitCastExpr(clang::ExplicitCastExpr *cast) {
  ctx_.emit_type_name_use(cast->getTypeInfoAsWritten(),
                          /*promote_described_template=*/true);
  return true;
}

// ---- local variables ---------------------------------------------------------

bool BodyVisitor::VisitVarDecl(clang::VarDecl *var) {
  // Reference scope: variables a DeclStmt declares. Typed predicates replace
  // the retired walk-parent gate: a local variable (excludes local-class
  // static members) that is not a parameter, catch variable, lambda
  // init-capture, or compiler-generated.
  if (!var->isLocalVarDecl() || llvm::isa<clang::ParmVarDecl>(var) ||
      var->isExceptionVariable() || var->isInitCapture() || var->isImplicit())
    return true;
  emit_local_var(var);
  return true;
}

void BodyVisitor::emit_local_var(const clang::VarDecl *var) {
  // Declared type -> uses edge + instance mint + class template
  // instantiates/template_arg rows.
  const ExpansionLoc loc = expansion_loc(ctx_.context(), var->getLocation());
  emit_type_use(ctx_.sink(), ctx_.src_id(), var->getType(), ctx_.file_id(),
                loc, ctx_.in_conditional() ? 1 : 0);
  ctx_.minter().mint_instance_from_type(var->getType());

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
  const auto prim = ctx_.sink().lookup_symbol_id(prim_usr);
  if (!prim)
    return; // lookup-only: no stubs for stdlib templates
  EdgeRecord inst;
  inst.src_id = ctx_.src_id();
  inst.dst_id = *prim;
  inst.kind = 5; // instantiates
  ctx_.sink().add_edge(inst);

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
  const clang::PrintingPolicy &policy = ctx_.context().getPrintingPolicy();
  for (unsigned ai = 0; ai < arg_types.size(); ++ai) {
    const clang::QualType at = arg_types[ai];
    if (at.isNull())
      continue;
    TemplateArgRecord ta;
    ta.owner_id = ctx_.src_id();
    ta.position = static_cast<int64_t>(ai);
    ta.arg_kind = 1;
    const std::string spelling = at.getAsString(policy);
    if (!spelling.empty())
      ta.literal = spelling;
    if (const clang::TagDecl *td = at->getAsTagDecl()) {
      const std::string ref_usr = usr_for_decl(td);
      if (!ref_usr.empty())
        ta.ref_id = ctx_.sink().lookup_symbol_id(ref_usr);
    }
    if (!ta.ref_id)
      ta.ref_id = ctx_.resolver().resolve(ta.literal, var);
    ctx_.sink().add_template_arg(ta);
  }
}

} // namespace cidx::lt
