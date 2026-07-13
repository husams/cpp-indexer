// BodyVisitor: the body-pass walker over a function-like definition's body —
// a RecursiveASTVisitor composed from one CRTP mixin per node family. Each
// mixin emits its own edge kind through the shared BodyEmitContext/CallEmitter;
// this class contributes only the traversal shape:
//
//   - dataTraverseStmtPre/Post bracket every statement's subtree, maintaining
//     the walk-parent stack (construct-form parent guard) and the conditional
//     depth (If/For/While/Do/Switch/Case/?:; range-for excluded) on the
//     context. RAV's data recursion also keeps deep expression chains off the
//     C stack.
//   - TypeLoc / nested-name-specifier / template-argument traversal is
//     suppressed: the reference surface never descends into spelled types —
//     every type-name use is emitted explicitly by a mixin.
//   - Range-for keeps the reference visit surface and order (range init, loop
//     variable, body; the desugared begin/end/cond/inc never appear).
//   - walk() roots the traversal at written ctor member initializers and the
//     body only (params and return type are not part of the surface).
#pragma once

#include "ast/body_emit_context.hpp"
#include "ast/call_emitter.hpp"
#include "ast/call_visitor_mixin.hpp"
#include "ast/construct_visitor_mixin.hpp"
#include "ast/heap_visitor_mixin.hpp"
#include "ast/local_var_visitor_mixin.hpp"
#include "ast/ref_visitor_mixin.hpp"
#include "ast/type_name_visitor_mixin.hpp"

#include "clang/AST/RecursiveASTVisitor.h"

#include <cstdint>

namespace cidx::lt {

class EdgeSink;

class BodyVisitor : public clang::RecursiveASTVisitor<BodyVisitor>,
                    public CallVisitorMixin<BodyVisitor>,
                    public ConstructVisitorMixin<BodyVisitor>,
                    public HeapVisitorMixin<BodyVisitor>,
                    public RefVisitorMixin<BodyVisitor>,
                    public TypeNameVisitorMixin<BodyVisitor>,
                    public LocalVarVisitorMixin<BodyVisitor> {
public:
  BodyVisitor(clang::ASTContext &context, EdgeSink &sink, int64_t src_id,
              int64_t file_id);

  // Resolve each Visit hook to its mixin (the RAV base declares no-op
  // defaults for every one of these names).
  using CallVisitorMixin<BodyVisitor>::VisitCallExpr;
  using ConstructVisitorMixin<BodyVisitor>::VisitCXXConstructExpr;
  using HeapVisitorMixin<BodyVisitor>::VisitCXXNewExpr;
  using HeapVisitorMixin<BodyVisitor>::VisitCXXDeleteExpr;
  using RefVisitorMixin<BodyVisitor>::VisitDeclRefExpr;
  using RefVisitorMixin<BodyVisitor>::VisitMemberExpr;
  using TypeNameVisitorMixin<BodyVisitor>::VisitUnaryExprOrTypeTraitExpr;
  using TypeNameVisitorMixin<BodyVisitor>::VisitExplicitCastExpr;
  using LocalVarVisitorMixin<BodyVisitor>::VisitVarDecl;

  // Walk fn's body (written ctor member initializers included).
  void walk(const clang::FunctionDecl *fn);

  // Mixin accessors.
  BodyEmitContext &body_ctx() { return ctx_; }
  CallEmitter &call_emitter() { return emitter_; }

  // Traversal shape (see file comment).
  bool dataTraverseStmtPre(clang::Stmt *stmt);
  bool dataTraverseStmtPost(clang::Stmt *stmt);
  bool TraverseCXXForRangeStmt(clang::CXXForRangeStmt *stmt);
  bool TraverseTypeLoc(clang::TypeLoc tl, bool traverse_qualifier = true) {
    return true;
  }
  bool TraverseNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc nns) {
    return true;
  }
  bool TraverseTemplateArgumentLoc(const clang::TemplateArgumentLoc &tal) {
    return true;
  }

private:
  BodyEmitContext ctx_;
  CallEmitter emitter_;
};

} // namespace cidx::lt
