// BodyEmitContext: shared per-definition state for the body-pass visitor
// mixins — the enclosing definition's identity (src_id/file_id/owner_usr),
// the mint/resolve utilities, the traversal parent stack and conditional
// depth (maintained by BodyVisitor), and the edge+site emission helpers
// every mixin uses. Emission goes through EdgeSink only.
#pragma once

#include "ast/instance_minter.hpp"
#include "ast/mint_builder.hpp"
#include "ast/template_arg_resolver.hpp"

#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <string>

namespace clang {
class ASTContext;
class Expr;
class Stmt;
class TypeSourceInfo;
} // namespace clang

namespace cidx::lt {

class EdgeSink;

class BodyEmitContext {
public:
  BodyEmitContext(clang::ASTContext &context, EdgeSink &sink, int64_t src_id,
                  int64_t file_id);

  clang::ASTContext &context() const { return context_; }
  EdgeSink &sink() const { return sink_; }
  MintBuilder &mint() { return mint_; }
  const TemplateArgResolver &resolver() const { return resolver_; }
  const InstanceMinter &minter() const { return minter_; }
  int64_t src_id() const { return src_id_; }
  int64_t file_id() const { return file_id_; }

  // Enclosing method's owning record (self-use skip in type-name branches).
  const std::string &owner_usr() const { return owner_usr_; }
  void set_owner_usr(std::string usr) { owner_usr_ = std::move(usr); }

  // Traversal bookkeeping — driven by BodyVisitor's dataTraverseStmtPre/Post,
  // which bracket each statement's whole subtree.
  void push_stmt(const clang::Stmt *stmt) { stmt_stack_.push_back(stmt); }
  void pop_stmt() { stmt_stack_.pop_back(); }
  void enter_cond() { ++cond_depth_; }
  void exit_cond() { --cond_depth_; }
  bool in_conditional() const { return cond_depth_ > 0; }

  // The walk-parent of the statement currently being visited (the stack top
  // is that statement itself). Null at a traversal root (function body or a
  // constructor member initializer).
  const clang::Stmt *parent() const {
    return stmt_stack_.size() >= 2 ? stmt_stack_[stmt_stack_.size() - 2]
                                   : nullptr;
  }
  const clang::Stmt *current_stmt() const {
    return stmt_stack_.empty() ? nullptr : stmt_stack_.back();
  }

  // Edge + edge_site pair. emit_site_edge anchors at the expression start;
  // emit_site_edge_at at an explicit token (reference semantics anchor
  // DECL_REF/MEMBER_REF/TYPE_REF sites at the NAME token).
  int64_t emit_site_edge(const clang::Expr *site, int64_t dst_id, int kind);
  int64_t emit_site_edge_at(clang::SourceLocation loc, int64_t dst_id,
                            int kind);

  // uses(7) for a spelled type name in expression position (temp-object
  // syntax, new-expr allocated type, sizeof/cast argument, explicit template
  // arguments) — anchored at the type NAME token, skipping the enclosing
  // method's own record and self-references. When promote_described_template
  // is set, a class template's pattern record is keyed as the template
  // (sizeof/cast branch semantics).
  void emit_type_name_use(const clang::TypeSourceInfo *tsi,
                          bool promote_described_template);

private:
  clang::ASTContext &context_;
  EdgeSink &sink_;
  MintBuilder mint_;
  TemplateArgResolver resolver_;
  InstanceMinter minter_;
  int64_t src_id_;
  int64_t file_id_;
  int cond_depth_ = 0;
  std::string owner_usr_;
  llvm::SmallVector<const clang::Stmt *, 32> stmt_stack_;
};

} // namespace cidx::lt
