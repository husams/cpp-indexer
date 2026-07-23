#include "ast/statement_edge_visitor.hpp"

#include "ast/edge_sink.hpp"
#include "ast/kind_map.hpp"
#include "ast/location.hpp"
#include "ast/names.hpp"
#include "ast/type_use.hpp"
#include "ast/usr.hpp"
#include "ast/value_provenance.hpp"

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

namespace cidx::ast {

namespace {

// The callee decl the way the reference resolves a call expression.
const clang::FunctionDecl *callee_decl(const clang::CallExpr *call) {
  return llvm::dyn_cast_or_null<clang::FunctionDecl>(call->getCalleeDecl());
}

// Candidate declarations of a dependent/overloaded callee: the OverloadExpr
// in callee position.
const clang::OverloadExpr *callee_overload_expr(const clang::CallExpr *call) {
  const clang::Expr *callee = call->getCallee();
  if (callee == nullptr) {
    return nullptr;
  }
  callee = normalize_value_expr(callee);
  return llvm::dyn_cast_or_null<clang::OverloadExpr>(callee);
}

// Function-like reference kinds are covered by the call callbacks, not
// uses(7).
bool is_function_like_decl(const clang::Decl *d) {
  return llvm::isa<clang::FunctionDecl>(d);
}

} // namespace

StatementEdgeVisitor::StatementEdgeVisitor(clang::ASTContext &context,
                                           EdgeSink &sink, int64_t src_id,
                                           int64_t file_id)
    : ctx_(context, sink, src_id, file_id), emitter_(ctx_) {}

// Owner USR for the self-owner skip in the type-name branches.
void StatementEdgeVisitor::record_owner_usr(const clang::FunctionDecl *fn) {
  const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(fn);
  if (method == nullptr) {
    return;
  }
  const clang::CXXRecordDecl *owner = method->getParent();
  if (owner == nullptr) {
    return;
  }
  const clang::NamedDecl *o = owner;
  if (const clang::ClassTemplateDecl *ct = owner->getDescribedClassTemplate()) {
    o = ct;
  }
  ctx_.set_owner_usr(usr_for_decl(o));
}

void StatementEdgeVisitor::walk(const clang::FunctionDecl *fn) {
  record_owner_usr(fn);
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
  if (clang::Stmt *body = fn->getBody()) {
    TraverseStmt(body);
  }
}

// ---- narrow scoped traversal overrides -------------------------------------

bool StatementEdgeVisitor::TraverseIfStmt(clang::IfStmt *stmt) {
  const CondScope guard(ctx_);
  return RecursiveASTVisitor::TraverseIfStmt(stmt);
}

bool StatementEdgeVisitor::TraverseForStmt(clang::ForStmt *stmt) {
  const CondScope guard(ctx_);
  return RecursiveASTVisitor::TraverseForStmt(stmt);
}

bool StatementEdgeVisitor::TraverseWhileStmt(clang::WhileStmt *stmt) {
  const CondScope guard(ctx_);
  return RecursiveASTVisitor::TraverseWhileStmt(stmt);
}

bool StatementEdgeVisitor::TraverseDoStmt(clang::DoStmt *stmt) {
  const CondScope guard(ctx_);
  return RecursiveASTVisitor::TraverseDoStmt(stmt);
}

bool StatementEdgeVisitor::TraverseSwitchStmt(clang::SwitchStmt *stmt) {
  const CondScope guard(ctx_);
  return RecursiveASTVisitor::TraverseSwitchStmt(stmt);
}

bool StatementEdgeVisitor::TraverseConditionalOperator(
    clang::ConditionalOperator *stmt) {
  const CondScope guard(ctx_);
  return RecursiveASTVisitor::TraverseConditionalOperator(stmt);
}

bool StatementEdgeVisitor::TraverseVarDecl(clang::VarDecl *var) {
  const InitScope scope(direct_init_, var->getInit());
  return RecursiveASTVisitor::TraverseVarDecl(var);
}

bool StatementEdgeVisitor::TraverseCXXNewExpr(clang::CXXNewExpr *expr) {
  const InitScope scope(new_init_, expr->getInitializer());
  return RecursiveASTVisitor::TraverseCXXNewExpr(expr);
}

// ---- calls ------------------------------------------------------------------

bool StatementEdgeVisitor::VisitCallExpr(clang::CallExpr *call) {
  emit_call(call);
  return true;
}

void StatementEdgeVisitor::emit_call(const clang::CallExpr *call) {
  const clang::FunctionDecl *ref = callee_decl(call);
  if (ref == nullptr) {
    // Dependent/overloaded callee: single-candidate recovery, else link every
    // indexed overload.
    if (const clang::OverloadExpr *ovl = callee_overload_expr(call)) {
      emit_overloaded_call(call, ovl);
    }
    return;
  }
  emitter_.emit_resolved_call(call, ref, /*recovered=*/false);
  emit_factory_edge(call, ref);
}

void StatementEdgeVisitor::emit_overloaded_call(
    const clang::CallExpr *call, const clang::OverloadExpr *ovl) {
  const std::vector<const clang::NamedDecl *> cands(ovl->decls_begin(),
                                                    ovl->decls_end());
  if (cands.size() == 1) {
    // Single candidate: recover the concrete callee (template pattern for a
    // FunctionTemplateDecl, minted under the template itself).
    const clang::NamedDecl *cand = cands[0];
    const clang::NamedDecl *mint_as = nullptr;
    const clang::FunctionDecl *ref = nullptr;
    if (const auto *ftd = llvm::dyn_cast<clang::FunctionTemplateDecl>(cand)) {
      ref = ftd->getTemplatedDecl();
      mint_as = ftd;
    } else {
      ref = llvm::dyn_cast<clang::FunctionDecl>(cand);
    }
    if (ref != nullptr) {
      emitter_.emit_resolved_call(call, ref, /*recovered=*/true, mint_as);
    }
    return;
  }
  if (cands.size() < 2) {
    return;
  }
  // Overload set: fan out to every indexed candidate (minting non-system
  // ones), falling back to qual-name lookup when none resolved.
  for (const int64_t dst_id : overload_candidate_ids(cands)) {
    ctx_.emit_site_edge(call, dst_id, 1);
  }
}

std::set<int64_t> StatementEdgeVisitor::overload_candidate_ids(
    const std::vector<const clang::NamedDecl *> &cands) {
  std::set<int64_t> dst_ids;
  for (const clang::NamedDecl *cand : cands) {
    const std::string usr = usr_for_decl(cand);
    if (usr.empty()) {
      continue;
    }
    const auto identity_source =
        expansion_loc(ctx_.context(), cand->getLocation()).file;
    if (const auto s = ctx_.sink().lookup_symbol_id(usr, identity_source)) {
      dst_ids.insert(*s);
      continue;
    }
    if (ctx_.context().getSourceManager().isInSystemHeader(
            cand->getLocation())) {
      continue;
    }
    if (auto req = ctx_.mint().build(cand)) {
      dst_ids.insert(ctx_.sink().mint_symbol(*req));
    }
  }
  if (dst_ids.empty() && !cands.empty()) {
    const std::string qn = qualified_name(ctx_.context(), cands[0]);
    if (!qn.empty()) {
      for (const int64_t id : ctx_.sink().symbol_ids_by_qual_name_kind(
               qn, cidx_stub_kind_name(cands[0]))) {
        dst_ids.insert(id);
      }
    }
  }
  return dst_ids;
}

// Factory: make_unique<B> / make_shared<B> from system headers emits a
// factory-construct(15) edge to B.
void StatementEdgeVisitor::emit_factory_edge(const clang::CallExpr *call,
                                             const clang::FunctionDecl *ref) {
  if (llvm::isa<clang::CXXMethodDecl>(ref)) {
    return;
  }
  const std::string sp = spelling(ref);
  if (sp != "make_unique" && sp != "make_shared") {
    return;
  }
  if (!ctx_.context().getSourceManager().isInSystemHeader(ref->getLocation())) {
    return;
  }
  const clang::QualType result = call->getType().getCanonicalType();
  const auto *spec =
      llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
          result->getAsCXXRecordDecl());
  if (spec == nullptr) {
    return;
  }
  const clang::TemplateArgumentList &args = spec->getTemplateArgs();
  if (args.size() == 0 || args[0].getKind() != clang::TemplateArgument::Type) {
    return;
  }
  const std::string fact_usr = record_usr_of_type(args[0].getAsType());
  if (fact_usr.empty()) {
    return;
  }
  const auto *fact_decl = args[0].getAsType()->getAsCXXRecordDecl();
  if (const auto fact = ctx_.sink().lookup_symbol_id(
          fact_usr,
          fact_decl != nullptr
              ? std::optional<std::string>(
                    expansion_loc(ctx_.context(), fact_decl->getLocation())
                        .file)
              : std::nullopt)) {
    EdgeRecord fe;
    fe.src_id = ctx_.src_id();
    fe.dst_id = *fact;
    fe.kind = 15; // factory-construct
    ctx_.sink().add_edge(fe);
  }
}

// ---- construction -----------------------------------------------------------

bool StatementEdgeVisitor::VisitCXXConstructExpr(
    clang::CXXConstructExpr *ctor) {
  emit_construct(ctor);
  return true;
}

void StatementEdgeVisitor::emit_construct(const clang::CXXConstructExpr *ctor) {
  const clang::CXXConstructorDecl *ref = ctor->getConstructor();
  if (ref == nullptr) {
    return;
  }
  // Every CXXConstructExpr maps to a call edge — including an implicit
  // default ctor for `B b;` (which mints a B() stub).
  emitter_.emit_resolved_call(ctor, ref, /*recovered=*/false);
  // Temporary-object syntax (Widget{...} / Widget(x)) names the type -> uses.
  if (const auto *tmp = llvm::dyn_cast<clang::CXXTemporaryObjectExpr>(ctor)) {
    ctx_.emit_type_name_use(tmp->getTypeSourceInfo(),
                            /*promote_described_template=*/false);
  }
  // A ctor that is a new-expression's initializer emits no form —
  // construct-heap(12) covers it.
  if (ctor != new_init_) {
    emit_construction_form(ctor, ref);
  }
}

// Construction form (10 value / 11 temp / 13 copy / 14 move), chosen by the
// ctor signature and the initializer position: a construct expression that IS
// the direct initializer of a variable or of a constructor member initializer
// sits in the var-init position.
void StatementEdgeVisitor::emit_construction_form(
    const clang::CXXConstructExpr *ctor, const clang::CXXConstructorDecl *ref) {
  const std::string type_usr = record_usr_of_type(ctor->getType());
  if (type_usr.empty()) {
    return;
  }
  const auto *type_decl = ctor->getType()->getAsCXXRecordDecl();
  const auto dst = ctx_.sink().lookup_symbol_id(
      type_usr,
      type_decl != nullptr
          ? std::optional<std::string>(
                expansion_loc(ctx_.context(), type_decl->getLocation()).file)
          : std::nullopt);
  if (!dst) {
    return;
  }
  // The constructor CATEGORY decides copy/move — not the parameter's printed
  // spelling, which can contain '&' inside nested types (Holder<void(int &)>).
  int form = 10; // construct-value
  if (ref->isMoveConstructor()) {
    form = 14; // construct-move
  } else if (ref->isCopyConstructor()) {
    form = 13; // construct-copy
  }
  const bool var_init = ctor == direct_init_;
  if (!var_init && form != 13 && form != 14) {
    form = 11; // construct-temp
  }
  EdgeRecord fe;
  fe.src_id = ctx_.src_id();
  fe.dst_id = *dst;
  fe.kind = form;
  ctx_.sink().add_edge(fe);
}

// ---- heap -------------------------------------------------------------------

bool StatementEdgeVisitor::VisitCXXNewExpr(clang::CXXNewExpr *expr) {
  const std::string heap_usr = record_usr_of_type(expr->getType());
  if (!heap_usr.empty()) {
    const auto *heap_decl = expr->getType()->getAsCXXRecordDecl();
    if (const auto dst = ctx_.sink().lookup_symbol_id(
            heap_usr,
            heap_decl != nullptr
                ? std::optional<std::string>(
                      expansion_loc(ctx_.context(), heap_decl->getLocation())
                          .file)
                : std::nullopt)) {
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

bool StatementEdgeVisitor::VisitCXXDeleteExpr(clang::CXXDeleteExpr *expr) {
  const clang::Expr *arg = expr->getArgument();
  if (arg == nullptr) {
    return true;
  }
  const std::string usr = record_usr_of_type(arg->getType());
  if (usr.empty()) {
    return true;
  }
  const auto *type_decl = arg->getType()->getAsCXXRecordDecl();
  if (const auto dst = ctx_.sink().lookup_symbol_id(
          usr, type_decl != nullptr
                   ? std::optional<std::string>(
                         expansion_loc(ctx_.context(), type_decl->getLocation())
                             .file)
                   : std::nullopt)) {
    EdgeRecord fe;
    fe.src_id = ctx_.src_id();
    fe.dst_id = *dst;
    fe.kind = 16; // destroy
    ctx_.sink().add_edge(fe);
  }
  return true;
}

// ---- references -------------------------------------------------------------

bool StatementEdgeVisitor::VisitDeclRefExpr(clang::DeclRefExpr *dre) {
  // uses(7): non-function references, lookup-only.
  const clang::ValueDecl *ref = dre->getDecl();
  if (ref != nullptr && !is_function_like_decl(ref)) {
    const std::string usr = usr_for_decl(ref);
    if (!usr.empty()) {
      if (const auto dst = ctx_.sink().lookup_symbol_id(
              usr, expansion_loc(ctx_.context(), ref->getLocation()).file)) {
        // The site anchors at the NAME (after qualifiers).
        ctx_.emit_site_edge_at(dre->getLocation(), *dst, 7);
      }
    }
  }
  emit_qualifier_type_use(dre);
  emit_explicit_template_arg_uses(dre);
  return true;
}

// Bare type name in the qualifier (Color::Red): a type use under an
// expression parent. NNS is a value type with a Kind enum in LLVM 22, a
// pointer with a SpecifierKind in LLVM 21.
void StatementEdgeVisitor::emit_qualifier_type_use(
    const clang::DeclRefExpr *dre) {
  const clang::Type *t = nullptr;
#if LLVM_VERSION_MAJOR >= 22
  const clang::NestedNameSpecifier nns = dre->getQualifier();
  if (nns.getKind() == clang::NestedNameSpecifier::Kind::Type) {
    t = nns.getAsType();
  }
#else
  if (const clang::NestedNameSpecifier *nns = dre->getQualifier())
    t = nns->getAsType();
#endif
  if (t == nullptr) {
    return;
  }
  const clang::NamedDecl *td = named_type_decl(clang::QualType(t, 0));
  if (td == nullptr) {
    return;
  }
  const std::string usr = usr_for_decl(td);
  if (usr.empty() || usr == ctx_.owner_usr()) {
    return;
  }
  if (const auto dst = ctx_.sink().lookup_symbol_id(
          usr, expansion_loc(ctx_.context(), td->getLocation()).file)) {
    if (*dst != ctx_.src_id()) {
      ctx_.emit_site_edge(dre, *dst, 7);
    }
  }
}

// Explicit template arguments (make_unique<Widget>(...)): each named TYPE
// argument is spelled at the expression — emit uses.
void StatementEdgeVisitor::emit_explicit_template_arg_uses(
    const clang::DeclRefExpr *dre) {
  if (!dre->hasExplicitTemplateArgs()) {
    return;
  }
  for (const clang::TemplateArgumentLoc &tal : dre->template_arguments()) {
    if (tal.getArgument().getKind() != clang::TemplateArgument::Type) {
      continue;
    }
    ctx_.emit_type_name_use(tal.getTypeSourceInfo(),
                            /*promote_described_template=*/false);
  }
}

bool StatementEdgeVisitor::VisitMemberExpr(clang::MemberExpr *me) {
  const clang::ValueDecl *ref = me->getMemberDecl();
  if (ref != nullptr && !is_function_like_decl(ref)) {
    const std::string usr = usr_for_decl(ref);
    if (!usr.empty()) {
      if (const auto dst = ctx_.sink().lookup_symbol_id(
              usr, expansion_loc(ctx_.context(), ref->getLocation()).file)) {
        // The site anchors at the member NAME token.
        ctx_.emit_site_edge_at(me->getMemberLoc(), *dst, 7);
      }
    }
  }
  return true;
}

// ---- spelled type names -----------------------------------------------------

bool StatementEdgeVisitor::VisitUnaryExprOrTypeTraitExpr(
    clang::UnaryExprOrTypeTraitExpr *expr) {
  if (expr->isArgumentType()) {
    ctx_.emit_type_name_use(expr->getArgumentTypeInfo(),
                            /*promote_described_template=*/true);
  }
  return true;
}

bool StatementEdgeVisitor::VisitExplicitCastExpr(
    clang::ExplicitCastExpr *cast) {
  ctx_.emit_type_name_use(cast->getTypeInfoAsWritten(),
                          /*promote_described_template=*/true);
  return true;
}

// ---- local variables
// ---------------------------------------------------------

bool StatementEdgeVisitor::VisitVarDecl(clang::VarDecl *var) {
  // Reference scope: variables a DeclStmt declares. Typed predicates replace
  // the retired walk-parent gate: a local variable (excludes local-class
  // static members) that is not a parameter, catch variable, lambda
  // init-capture, or compiler-generated.
  if (!var->isLocalVarDecl() || llvm::isa<clang::ParmVarDecl>(var) ||
      var->isExceptionVariable() || var->isInitCapture() || var->isImplicit()) {
    return true;
  }
  emit_local_var(var);
  return true;
}

void StatementEdgeVisitor::emit_local_var(const clang::VarDecl *var) {
  // Declared type -> uses edge + concrete class-template instance mint.
  const ExpansionLoc loc = expansion_loc(ctx_.context(), var->getLocation());
  emit_type_use(ctx_.sink(), ctx_.src_id(), var->getType(), ctx_.file_id(), loc,
                ctx_.in_conditional() ? 1 : 0);
  ctx_.minter().mint_instance_from_type(var->getType());
}

} // namespace cidx::ast
