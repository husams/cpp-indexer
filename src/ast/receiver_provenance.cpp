#include "ast/receiver_provenance.hpp"

#include "ast/usr.hpp"
#include "ast/value_provenance.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"

#include <string>

namespace cidx::ast {

namespace {

// The explicit receiver expression of a call site: the member callee's base
// (implicit-object argument), or the base of a recovered dependent member
// callee (cache.get<std::string>(...)). Null for free calls / implicit this.
const clang::Expr *receiver_expression(const clang::Expr *site) {
  if (const auto *mc = llvm::dyn_cast<clang::CXXMemberCallExpr>(site)) {
    const auto *me = llvm::dyn_cast_or_null<clang::MemberExpr>(
        normalize_value_expr(mc->getCallee()));
    if (me != nullptr && !me->isImplicitAccess()) {
      return mc->getImplicitObjectArgument();
    }
    return nullptr;
  }
  const auto *anycall = llvm::dyn_cast<clang::CallExpr>(site);
  if (anycall == nullptr) {
    return nullptr;
  }
  const clang::Expr *callee_expr = normalize_value_expr(anycall->getCallee());
  if (const auto *dep =
          llvm::dyn_cast_or_null<clang::CXXDependentScopeMemberExpr>(
              callee_expr)) {
    if (!dep->isImplicitAccess()) {
      return dep->getBase();
    }
  } else if (const auto *unres =
                 llvm::dyn_cast_or_null<clang::UnresolvedMemberExpr>(
                     callee_expr)) {
    if (!unres->isImplicitAccess()) {
      return unres->getBase();
    }
  }
  return nullptr;
}

// Provenance of an EXPLICIT receiver: value classification, parameter
// position for parameter receivers, and by-value dispatch for the categories
// devirtualization narrows on.
ReceiverProvenance
classify_explicit_receiver(const clang::ASTContext &context,
                           const clang::Expr *recv_expr,
                           const clang::FunctionDecl * /*callee*/) {
  ReceiverProvenance recv;
  const ValueSource rv = classify_value_source(context, recv_expr);
  recv.src_kind = rv.src_kind;
  recv.type_usr = rv.type_usr;
  recv.decl_usr = rv.decl_usr;
  if (recv.src_kind == "local" && !recv.decl_usr.empty()) {
    if (const auto *dre = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
            normalize_value_expr(recv_expr))) {
      if (const auto *parm =
              llvm::dyn_cast<clang::ParmVarDecl>(dre->getDecl())) {
        recv.param_pos = static_cast<int64_t>(parm->getFunctionScopeIndex());
      }
    }
  }
  if ((recv.src_kind == "member" || recv.src_kind == "global" ||
       recv.src_kind == "call_result" || recv.src_kind == "local") &&
      !recv.type_usr.empty()) {
    recv.type_is_value =
        type_is_value(decl_type_for_expr(normalize_value_expr(recv_expr)),
                      recv.type_usr)
            ? 1
            : 0;
  }
  return recv;
}

// Implicit this: no explicit receiver child and a method/ctor/dtor callee
// (applies to operator calls and statics alike).
ReceiverProvenance classify_implicit_this(const clang::FunctionDecl *callee) {
  ReceiverProvenance recv;
  const clang::CXXRecordDecl *owner =
      llvm::cast<clang::CXXMethodDecl>(callee)->getParent();
  if (owner == nullptr) {
    return recv;
  }
  const clang::NamedDecl *o = owner;
  if (const clang::ClassTemplateDecl *ct = owner->getDescribedClassTemplate()) {
    o = ct;
  }
  const std::string ou = usr_for_decl(o);
  recv.src_kind = "this";
  recv.type_usr = ou;
  recv.decl_usr = ou;
  return recv;
}

} // namespace

ReceiverProvenance classify_call_receiver(const clang::ASTContext &context,
                                          const clang::Expr *site,
                                          const clang::FunctionDecl *callee) {
  if (const clang::Expr *recv_expr = receiver_expression(site)) {
    return classify_explicit_receiver(context, recv_expr, callee);
  }
  if (llvm::isa<clang::CXXMethodDecl>(callee)) {
    return classify_implicit_this(callee);
  }
  return {};
}

} // namespace cidx::ast
