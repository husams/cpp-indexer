#include "ast/value_source.hpp"

#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"

namespace cidx::lt {

namespace {

// libclang's UNEXPOSED_EXPR wraps implicit casts and the invisible C++
// temporary plumbing; peel those the way clang_visitChildren steps through
// them (first child).
const clang::Expr *unwrap_once(const clang::Expr *e) {
  if (const auto *pe = llvm::dyn_cast<clang::ParenExpr>(e))
    return pe->getSubExpr();
  if (const auto *uo = llvm::dyn_cast<clang::UnaryOperator>(e))
    return uo->getSubExpr();
  if (const auto *cc = llvm::dyn_cast<clang::CStyleCastExpr>(e))
    return cc->getSubExpr();
  if (const auto *ic = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    return ic->getSubExpr();
  if (const auto *ec = llvm::dyn_cast<clang::ExprWithCleanups>(e))
    return ec->getSubExpr();
  if (const auto *mt = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(e))
    return mt->getSubExpr();
  if (const auto *bt = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(e))
    return bt->getSubExpr();
  return nullptr;
}

const clang::FunctionDecl *callee_of(const clang::Expr *e) {
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
    const clang::Decl *callee = call->getCalleeDecl();
    return llvm::dyn_cast_or_null<clang::FunctionDecl>(callee);
  }
  if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructExpr>(e))
    return ctor->getConstructor();
  return nullptr;
}

} // namespace

const clang::Expr *peel_expr(const clang::Expr *expr) {
  for (int i = 0; i < 16 && expr != nullptr; ++i) {
    const clang::Expr *inner = unwrap_once(expr);
    if (inner == nullptr)
      break;
    expr = inner;
  }
  return expr;
}

std::string record_usr_of_type(clang::QualType type) {
  if (type.isNull())
    return {};
  clang::QualType canonical = type.getCanonicalType();
  for (int i = 0; i < 8; ++i) {
    if (canonical->isPointerType() || canonical->isReferenceType())
      canonical = canonical->getPointeeType().getCanonicalType();
    else
      break;
  }
  const clang::TagDecl *decl = canonical->getAsTagDecl();
  if (decl == nullptr)
    return {};
  return usr_for_decl(decl);
}

bool type_is_value(clang::QualType loc_type,
                   const std::string &dispatch_record_usr) {
  if (dispatch_record_usr.empty() || loc_type.isNull())
    return false;
  const clang::QualType c = loc_type.getCanonicalType();
  if (!c->isRecordType())
    return false;
  const clang::TagDecl *decl = c->getAsTagDecl();
  if (decl == nullptr)
    return false;
  return usr_for_decl(decl) == dispatch_record_usr;
}

clang::QualType decl_type_for_expr(const clang::Expr *peeled) {
  if (peeled == nullptr)
    return {};
  if (const auto *dre = llvm::dyn_cast<clang::DeclRefExpr>(peeled))
    return dre->getDecl()->getType();
  if (const auto *me = llvm::dyn_cast<clang::MemberExpr>(peeled))
    return me->getMemberDecl()->getType();
  if (const clang::FunctionDecl *fd = callee_of(peeled))
    return fd->getReturnType();
  return peeled->getType();
}

ValueSource classify_value_source(const clang::ASTContext & /*context*/,
                                  const clang::Expr *expr) {
  const clang::Expr *peeled = peel_expr(expr);
  if (peeled == nullptr)
    return {"unknown", "", "", ""};

  if (llvm::isa<clang::CXXThisExpr>(peeled)) {
    const std::string tu = record_usr_of_type(peeled->getType());
    return {"this", tu, tu, ""};
  }

  if (const auto *dre = llvm::dyn_cast<clang::DeclRefExpr>(peeled)) {
    const clang::ValueDecl *ref = dre->getDecl();
    const std::string decl_usr = usr_for_decl(ref);
    const std::string type_usr = record_usr_of_type(peeled->getType());
    if (llvm::isa<clang::ParmVarDecl>(ref))
      return {"local", type_usr, decl_usr, ""};
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref)) {
      // The cursor-kind check (FunctionDecl/CXXMethod/Ctor/Dtor/Lambda) does
      // NOT include FUNCTION_TEMPLATE: a local inside a function-template
      // pattern classifies as 'global' (bug-compatible with the reference).
      const clang::DeclContext *dc = var->getDeclContext();
      bool local_ctx = false;
      if (dc != nullptr) {
        if (const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(dc))
          local_ctx = fn->getDescribedFunctionTemplate() == nullptr;
        else if (const auto *rec = llvm::dyn_cast<clang::CXXRecordDecl>(dc))
          local_ctx = rec->isLambda();
      }
      if (local_ctx)
        return {"local", type_usr, decl_usr, ""};
      return {"global", type_usr, decl_usr, ""};
    }
    return {"unknown", type_usr, decl_usr, ""};
  }

  if (const auto *me = llvm::dyn_cast<clang::MemberExpr>(peeled)) {
    const std::string decl_usr = usr_for_decl(me->getMemberDecl());
    const std::string type_usr = record_usr_of_type(peeled->getType());
    return {"member", type_usr, decl_usr, ""};
  }

  // Single-arg functional cast `std::string("x")` is CURSOR kind 128 whose
  // libclang reference is null -> call_result with no callee. Temporary-object
  // syntax `Widget(7)` / construct exprs are CALL_EXPR cursors referencing the
  // ctor -> construct.
  if (llvm::isa<clang::CXXFunctionalCastExpr>(peeled)) {
    const std::string type_usr = record_usr_of_type(peeled->getType());
    return {"call_result", type_usr, "", ""};
  }
  if (llvm::isa<clang::CXXConstructExpr>(peeled)) {
    const std::string type_usr = record_usr_of_type(peeled->getType());
    return {"construct", type_usr, "", ""};
  }
  if (llvm::isa<clang::CallExpr>(peeled)) {
    if (const clang::FunctionDecl *fd = callee_of(peeled)) {
      if (llvm::isa<clang::CXXConstructorDecl>(fd) ||
          llvm::isa<clang::CXXConversionDecl>(fd)) {
        const std::string type_usr = record_usr_of_type(peeled->getType());
        return {"construct", type_usr, "", ""};
      }
      const std::string type_usr = record_usr_of_type(peeled->getType());
      return {"call_result", type_usr, "", usr_for_decl(fd)};
    }
    const std::string type_usr = record_usr_of_type(peeled->getType());
    return {"call_result", type_usr, "", ""};
  }

  // Dependent construct in a template pattern (any(value) materialized for
  // `store_[key] = value`): libclang shows a CALL_EXPR with a null reference.
  if (const auto *uc =
          llvm::dyn_cast<clang::CXXUnresolvedConstructExpr>(peeled)) {
    const std::string type_usr = record_usr_of_type(uc->getTypeAsWritten());
    return {"call_result", type_usr, "", ""};
  }
  if (llvm::isa<clang::CXXNewExpr>(peeled)) {
    const std::string type_usr = record_usr_of_type(peeled->getType());
    return {"construct", type_usr, "", ""};
  }

  if (llvm::isa<clang::IntegerLiteral>(peeled) ||
      llvm::isa<clang::FloatingLiteral>(peeled) ||
      llvm::isa<clang::StringLiteral>(peeled) ||
      llvm::isa<clang::CharacterLiteral>(peeled) ||
      llvm::isa<clang::CXXBoolLiteralExpr>(peeled) ||
      llvm::isa<clang::CXXNullPtrLiteralExpr>(peeled) ||
      llvm::isa<clang::GNUNullExpr>(peeled))
    return {"literal", "", "", ""};

  return {"unknown", "", "", ""};
}

} // namespace cidx::lt
