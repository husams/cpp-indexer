// LocalVarVisitorMixin: a local variable declared in a DeclStmt emits its
// declared-type uses edge, an instance mint, and — for class template
// specializations — an instantiates(5) edge with template_arg rows spelled AS
// WRITTEN. Fires before the initializer is traversed, so a spec instance
// minted by the init's ctor call is not yet visible here (reference preorder).
// CRTP mixin for BodyVisitor.
#pragma once

namespace clang {
class DeclStmt;
class VarDecl;
} // namespace clang

namespace cidx::lt {

class BodyEmitContext;

namespace detail {
bool var_listed_in_current_decl_stmt(const clang::VarDecl *var,
                                     const BodyEmitContext &ctx);
void handle_local_var_decl(const clang::VarDecl *var, BodyEmitContext &ctx);
} // namespace detail

template <class Derived> class LocalVarVisitorMixin {
public:
  bool VisitVarDecl(clang::VarDecl *var) {
    auto &ctx = static_cast<Derived &>(*this).body_ctx();
    // Only variables listed in the DeclStmt currently being traversed — the
    // scope the reference handled. Excludes catch-variables, lambda captures
    // and parameters, and local-class static members, which reach VisitVarDecl
    // through other traversal paths.
    if (detail::var_listed_in_current_decl_stmt(var, ctx))
      detail::handle_local_var_decl(var, ctx);
    return true;
  }

protected:
  ~LocalVarVisitorMixin() = default;
};

} // namespace cidx::lt
