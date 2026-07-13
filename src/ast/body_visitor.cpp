#include "ast/body_visitor.hpp"

#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"

namespace cidx::lt {

namespace {

// Conditional-context kinds: If/For/While/Do/Switch/Case/?: — range-for is
// NOT in the set.
bool is_cond_stmt(const clang::Stmt *s) {
  return llvm::isa<clang::IfStmt>(s) || llvm::isa<clang::ForStmt>(s) ||
         llvm::isa<clang::WhileStmt>(s) || llvm::isa<clang::DoStmt>(s) ||
         llvm::isa<clang::SwitchStmt>(s) || llvm::isa<clang::CaseStmt>(s) ||
         llvm::isa<clang::ConditionalOperator>(s);
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
  // Constructor member initializers carry calls/uses too.
  if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(fn)) {
    for (const clang::CXXCtorInitializer *init : ctor->inits()) {
      if (init->isWritten() && init->getInit() != nullptr)
        TraverseStmt(init->getInit());
    }
  }
  if (clang::Stmt *body = fn->getBody())
    TraverseStmt(body);
}

bool BodyVisitor::dataTraverseStmtPre(clang::Stmt *stmt) {
  ctx_.push_stmt(stmt);
  if (is_cond_stmt(stmt))
    ctx_.enter_cond();
  return true;
}

bool BodyVisitor::dataTraverseStmtPost(clang::Stmt *stmt) {
  if (is_cond_stmt(stmt))
    ctx_.exit_cond();
  ctx_.pop_stmt();
  return true;
}

// Reference visit surface and order for range-for: only the RANGE INIT, the
// loop VARIABLE, and the BODY — the desugared begin/end/cond/inc statements
// must not be visited (their operator calls are not emitted). RAV's default
// (!shouldVisitImplicitCode) surface matches, but visits the loop variable
// before the range init; keep the reference order. The C++20 init-statement
// is traversed first when present.
bool BodyVisitor::TraverseCXXForRangeStmt(clang::CXXForRangeStmt *stmt) {
  if (!WalkUpFromCXXForRangeStmt(stmt))
    return false;
  if (stmt->getInit() != nullptr && !TraverseStmt(stmt->getInit()))
    return false;
  if (!TraverseStmt(stmt->getRangeInit()))
    return false;
  if (!TraverseStmt(stmt->getLoopVarStmt()))
    return false;
  return TraverseStmt(stmt->getBody());
}

} // namespace cidx::lt
