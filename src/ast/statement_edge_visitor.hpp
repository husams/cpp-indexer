// StatementEdgeVisitor: the body-pass walker over a function-like definition's
// body — a direct RecursiveASTVisitor. Clang owns the traversal; this class
// owns the nine Visit callbacks that map expression/declaration facts to edge
// records through the shared EdgeEmissionContext/CallEdgeEmitter, plus a small
// set of narrow, base-delegating Traverse overrides that carry scoped cidx
// context:
//
//   - conditional depth: If/For/While/Do/Switch/?: subtrees mark their edge
//     sites conditional (CaseStmt adds no override — a case label is always
//     inside its SwitchStmt's guard, so the flag is already set);
//   - direct-initializer identity: TraverseVarDecl and TraverseCXXNewExpr
//     record which construct expression is a variable/new initializer, which
//     selects the construction-form edge (value vs temp) and suppresses the
//     form under `new` in favour of construct-heap.
//
// walk() roots the traversal at written ctor member initializers and the
// body only (params and return type are not part of the surface).
#pragma once

#include "ast/call_edge_emitter.hpp"
#include "ast/edge_emission_context.hpp"

#include "clang/AST/RecursiveASTVisitor.h"

#include <cstdint>
#include <set>
#include <vector>

namespace clang {
class Stmt;
}

namespace cidx::ast {

class StatementFactPorts;
struct PassMetrics;

class StatementEdgeVisitor
    : public clang::RecursiveASTVisitor<StatementEdgeVisitor> {
public:
  StatementEdgeVisitor(clang::ASTContext &context, StatementFactPorts &ports,
                       int64_t src_id, int64_t file_id, std::string file,
                       PassMetrics *metrics = nullptr);

  // Walk fn's body (written ctor member initializers included).
  void walk(const clang::FunctionDecl *fn);
  auto VisitStmt(clang::Stmt *stmt) -> bool;

  // calls(1) with dependent/overload recovery + the factory edge (15).
  bool VisitCallExpr(clang::CallExpr *call);
  // Every construct expression is a call edge; construction form 10/11/13/14.
  bool VisitCXXConstructExpr(clang::CXXConstructExpr *ctor);
  // construct-heap(12) + allocated-type uses(7).
  bool VisitCXXNewExpr(clang::CXXNewExpr *expr);
  // destroy(16).
  bool VisitCXXDeleteExpr(clang::CXXDeleteExpr *expr);
  // uses(7) for non-function references, qualifier type names, and explicit
  // template arguments.
  bool VisitDeclRefExpr(clang::DeclRefExpr *dre);
  bool VisitMemberExpr(clang::MemberExpr *me);
  // uses(7) for type names in sizeof/alignof and explicit casts.
  bool VisitUnaryExprOrTypeTraitExpr(clang::UnaryExprOrTypeTraitExpr *expr);
  bool VisitExplicitCastExpr(clang::ExplicitCastExpr *cast);
  // Local variables declared by a DeclStmt: declared-type uses, instance
  // mint, instantiates(5) + template_arg rows.
  bool VisitVarDecl(clang::VarDecl *var);

  // Narrow scoped overrides (single-argument on purpose: RAV then dispatches
  // without the data-recursion queue, so the RAII guard brackets the whole
  // subtree). Each delegates to the RAV base for child traversal.
  bool TraverseIfStmt(clang::IfStmt *stmt);
  bool TraverseForStmt(clang::ForStmt *stmt);
  bool TraverseWhileStmt(clang::WhileStmt *stmt);
  bool TraverseDoStmt(clang::DoStmt *stmt);
  bool TraverseSwitchStmt(clang::SwitchStmt *stmt);
  bool TraverseConditionalOperator(clang::ConditionalOperator *stmt);
  bool TraverseVarDecl(clang::VarDecl *var);
  bool TraverseCXXNewExpr(clang::CXXNewExpr *expr);

private:
  // Scoped conditional-depth guard for the Traverse overrides above.
  class CondScope {
  public:
    explicit CondScope(EdgeEmissionContext &ctx) : ctx_(ctx) {
      ctx_.enter_cond();
    }
    ~CondScope() { ctx_.exit_cond(); }
    CondScope(const CondScope &) = delete;
    CondScope &operator=(const CondScope &) = delete;

  private:
    EdgeEmissionContext &ctx_;
  };

  // Scoped save/restore for the direct-initializer pointers.
  class InitScope {
  public:
    InitScope(const clang::Expr *&slot, const clang::Expr *value)
        : slot_(slot), saved_(slot) {
      slot_ = value;
    }
    ~InitScope() { slot_ = saved_; }
    InitScope(const InitScope &) = delete;
    InitScope &operator=(const InitScope &) = delete;

  private:
    const clang::Expr *&slot_;
    const clang::Expr *saved_;
  };

  void record_owner_usr(const clang::FunctionDecl *fn);
  void emit_call(const clang::CallExpr *call);
  void emit_overloaded_call(const clang::CallExpr *call,
                            const clang::OverloadExpr *ovl);
  std::set<int64_t>
  overload_candidate_ids(const clang::CallExpr *call,
                         const std::vector<const clang::NamedDecl *> &cands);
  void emit_factory_edge(const clang::CallExpr *call,
                         const clang::FunctionDecl *ref);
  void emit_construct(const clang::CXXConstructExpr *ctor);
  void emit_construction_form(const clang::CXXConstructExpr *ctor,
                              const clang::CXXConstructorDecl *ref);
  void emit_qualifier_type_use(const clang::DeclRefExpr *dre);
  void emit_explicit_template_arg_uses(const clang::DeclRefExpr *dre);
  void emit_local_var(const clang::VarDecl *var);
  void emit_local_var_template_args(
      const clang::VarDecl *var,
      const clang::ClassTemplateSpecializationDecl *spec);

  EdgeEmissionContext ctx_;
  CallEdgeEmitter emitter_;
  // The expression that is the direct initializer of the variable / ctor
  // member initializer currently being traversed (construct-form var-init
  // position), and the initializer of the enclosing new-expression (form
  // suppressed in favour of construct-heap). Compared by identity.
  const clang::Expr *direct_init_ = nullptr;
  const clang::Expr *new_init_ = nullptr;
};

} // namespace cidx::ast
