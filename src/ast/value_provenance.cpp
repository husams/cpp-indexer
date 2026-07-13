#include "ast/value_provenance.hpp"

#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"

namespace cidx::ast {

namespace {

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

const clang::Expr *normalize_value_expr(const clang::Expr *expr) {
  const clang::Expr *e = expr;
  while (e != nullptr) {
    // Parentheses and implicit casts carry no provenance of their own.
    const clang::Expr *stripped = e->IgnoreParenImpCasts();
    // Invisible C++ temporary plumbing.
    if (const auto *ec = llvm::dyn_cast<clang::ExprWithCleanups>(stripped)) {
      e = ec->getSubExpr();
      continue;
    }
    if (const auto *mt =
            llvm::dyn_cast<clang::MaterializeTemporaryExpr>(stripped)) {
      e = mt->getSubExpr();
      continue;
    }
    if (const auto *bt =
            llvm::dyn_cast<clang::CXXBindTemporaryExpr>(stripped)) {
      e = bt->getSubExpr();
      continue;
    }
    // A C-style cast of a value still denotes the operand's storage.
    if (const auto *cc = llvm::dyn_cast<clang::CStyleCastExpr>(stripped)) {
      e = cc->getSubExpr();
      continue;
    }
    // Explicitly handled unary operators: &x and *p preserve the operand's
    // provenance. Every other unary operator produces a derived value and
    // stays visible (the classifier answers 'unknown').
    if (const auto *uo = llvm::dyn_cast<clang::UnaryOperator>(stripped)) {
      if (uo->getOpcode() == clang::UO_AddrOf ||
          uo->getOpcode() == clang::UO_Deref) {
        e = uo->getSubExpr();
        continue;
      }
    }
    return stripped;
  }
  return e;
}

std::string record_usr_of_type(clang::QualType type) {
  if (type.isNull())
    return {};
  clang::QualType canonical = type.getCanonicalType();
  while (canonical->isPointerType() || canonical->isReferenceType())
    canonical = canonical->getPointeeType().getCanonicalType();
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

clang::QualType decl_type_for_expr(const clang::Expr *normalized) {
  if (normalized == nullptr)
    return {};
  if (const auto *dre = llvm::dyn_cast<clang::DeclRefExpr>(normalized))
    return dre->getDecl()->getType();
  if (const auto *me = llvm::dyn_cast<clang::MemberExpr>(normalized))
    return me->getMemberDecl()->getType();
  if (const clang::FunctionDecl *fd = callee_of(normalized))
    return fd->getReturnType();
  return normalized->getType();
}

namespace {

// DeclRefExpr provenance: parameters and local variables — including static
// locals and locals inside function/method-template patterns — plus lambda
// captures (whose declaration context is the closure record) are 'local';
// every other variable is 'global'.
ValueSource classify_decl_ref(const clang::DeclRefExpr *dre) {
  const clang::ValueDecl *ref = dre->getDecl();
  const std::string decl_usr = usr_for_decl(ref);
  const std::string type_usr =
      record_usr_of_type(llvm::cast<clang::Expr>(dre)->getType());
  if (llvm::isa<clang::ParmVarDecl>(ref))
    return {"local", type_usr, decl_usr, ""};
  if (const auto *var = llvm::dyn_cast<clang::VarDecl>(ref)) {
    const auto *rec =
        llvm::dyn_cast_or_null<clang::CXXRecordDecl>(var->getDeclContext());
    const bool local_ctx =
        var->isLocalVarDecl() || (rec != nullptr && rec->isLambda());
    if (local_ctx)
      return {"local", type_usr, decl_usr, ""};
    return {"global", type_usr, decl_usr, ""};
  }
  return {"unknown", type_usr, decl_usr, ""};
}

// Call-shaped values. Single-arg functional cast `std::string("x")` is a
// converted value -> call_result with no callee. Temporary-object syntax
// `Widget(7)` / construct exprs (and calls resolving to a ctor/conversion)
// -> construct.
ValueSource classify_call_shaped(const clang::Expr *peeled) {
  const std::string type_usr = record_usr_of_type(peeled->getType());
  if (llvm::isa<clang::CXXFunctionalCastExpr>(peeled))
    return {"call_result", type_usr, "", ""};
  if (llvm::isa<clang::CXXConstructExpr>(peeled))
    return {"construct", type_usr, "", ""};
  if (const clang::FunctionDecl *fd = callee_of(peeled)) {
    if (llvm::isa<clang::CXXConstructorDecl>(fd) ||
        llvm::isa<clang::CXXConversionDecl>(fd))
      return {"construct", type_usr, "", ""};
    return {"call_result", type_usr, "", usr_for_decl(fd)};
  }
  return {"call_result", type_usr, "", ""};
}

} // namespace

ValueSource classify_value_source(const clang::ASTContext & /*context*/,
                                  const clang::Expr *expr) {
  const clang::Expr *peeled = normalize_value_expr(expr);
  if (peeled == nullptr)
    return {"unknown", "", "", ""};

  if (llvm::isa<clang::CXXThisExpr>(peeled)) {
    const std::string tu = record_usr_of_type(peeled->getType());
    return {"this", tu, tu, ""};
  }

  if (const auto *dre = llvm::dyn_cast<clang::DeclRefExpr>(peeled))
    return classify_decl_ref(dre);

  if (const auto *me = llvm::dyn_cast<clang::MemberExpr>(peeled)) {
    const std::string decl_usr = usr_for_decl(me->getMemberDecl());
    const std::string type_usr = record_usr_of_type(peeled->getType());
    return {"member", type_usr, decl_usr, ""};
  }

  if (llvm::isa<clang::CXXFunctionalCastExpr>(peeled) ||
      llvm::isa<clang::CXXConstructExpr>(peeled) ||
      llvm::isa<clang::CallExpr>(peeled))
    return classify_call_shaped(peeled);

  // Dependent construct in a template pattern (any(value) materialized for
  // `store_[key] = value`): a call-shaped value with no resolvable callee.
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

} // namespace cidx::ast
