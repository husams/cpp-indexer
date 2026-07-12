#include "clangx_lt/receiver_provenance.hpp"

#include "clangx_lt/usr.hpp"
#include "clangx_lt/value_source.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"

#include <string>

namespace cidx::lt {

ReceiverProvenance classify_call_receiver(const clang::ASTContext &context,
                                          const clang::Expr *site,
                                          const clang::FunctionDecl *callee) {
  ReceiverProvenance recv;

  const clang::Expr *recv_expr = nullptr;
  if (const auto *mc = llvm::dyn_cast<clang::CXXMemberCallExpr>(site)) {
    const auto *me =
        llvm::dyn_cast_or_null<clang::MemberExpr>(peel_expr(mc->getCallee()));
    if (me != nullptr && !me->isImplicitAccess())
      recv_expr = me->getBase();
  } else if (const auto *anycall = llvm::dyn_cast<clang::CallExpr>(site)) {
    // Recovered dependent member calls (cache.get<std::string>(...)): the
    // callee is a CXXDependentScopeMemberExpr / UnresolvedMemberExpr whose base
    // is the receiver (libclang MEMBER_REF_EXPR first child).
    const clang::Expr *callee_expr = peel_expr(anycall->getCallee());
    if (const auto *dep = llvm::dyn_cast_or_null<
            clang::CXXDependentScopeMemberExpr>(callee_expr)) {
      if (!dep->isImplicitAccess())
        recv_expr = dep->getBase();
    } else if (const auto *unres = llvm::dyn_cast_or_null<
                   clang::UnresolvedMemberExpr>(callee_expr)) {
      if (!unres->isImplicitAccess())
        recv_expr = unres->getBase();
    }
  }

  if (recv_expr != nullptr) {
    const ValueSource rv = classify_value_source(context, recv_expr);
    recv.src_kind = rv.src_kind;
    recv.type_usr = rv.type_usr;
    recv.decl_usr = rv.decl_usr;
    if (recv.src_kind == "local" && !recv.decl_usr.empty()) {
      if (const auto *dre = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
              peel_expr(recv_expr))) {
        if (const auto *parm =
                llvm::dyn_cast<clang::ParmVarDecl>(dre->getDecl())) {
          // The cursor API's param iteration returns -1 on FUNCTION_TEMPLATE
          // parents, so cidx never resolves the position there.
          const auto *pfn = llvm::dyn_cast_or_null<clang::FunctionDecl>(
              llvm::dyn_cast_or_null<clang::Decl>(parm->getDeclContext()));
          if (pfn != nullptr && pfn->getDescribedFunctionTemplate() == nullptr)
            recv.param_pos =
                static_cast<int64_t>(parm->getFunctionScopeIndex());
        }
      }
    }
    if (recv.src_kind == "member" || recv.src_kind == "global" ||
        recv.src_kind == "call_result") {
      std::string dispatch_usr;
      if (const auto *m = llvm::dyn_cast<clang::CXXMethodDecl>(callee)) {
        if (const clang::CXXRecordDecl *owner = m->getParent())
          dispatch_usr = usr_for_decl(owner);
      }
      recv.type_is_value =
          type_is_value(decl_type_for_expr(peel_expr(recv_expr)), dispatch_usr)
              ? 1
              : 0;
    }
  } else if (llvm::isa<clang::CXXMethodDecl>(callee)) {
    // Implicit this: no explicit receiver child and a method/ctor/dtor callee
    // (libclang applies this to operator calls and statics alike).
    if (const clang::CXXRecordDecl *owner =
            llvm::cast<clang::CXXMethodDecl>(callee)->getParent()) {
      const clang::NamedDecl *o = owner;
      if (const clang::ClassTemplateDecl *ct = owner->getDescribedClassTemplate())
        o = ct;
      const std::string ou = usr_for_decl(o);
      recv.src_kind = "this";
      recv.type_usr = ou;
      recv.decl_usr = ou;
    }
  }

  return recv;
}

} // namespace cidx::lt
