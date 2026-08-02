// Namespace-uses pass (B3): for each target-file namespace QUALIFIER
// reference (geo::..., using namespace ns, using ns::f) emit a uses(7) edge
// nearest-enclosing-indexed-symbol -> namespace, with one edge_site — the
// LibTooling analogue of emit_namespace_uses / ns_uses_descend
// (ast_cursor.cpp:285). Descends into bodies; lookup-only (bare std:: refs
// miss and are skipped).
#pragma once

#include "clang/AST/RecursiveASTVisitor.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace clang {
class ASTContext;
class Decl;
class NamedDecl;
class NestedNameSpecifierLoc;
class UsingDirectiveDecl;
} // namespace clang

namespace cidx::ast {

class NamespacePassPorts;
struct PassMetrics;

class NamespaceUseVisitor
    : public clang::RecursiveASTVisitor<NamespaceUseVisitor> {
public:
  using FileRouter =
      std::function<std::optional<std::int64_t>(const std::string &)>;

  NamespaceUseVisitor(clang::ASTContext &context, NamespacePassPorts &ports,
                      std::string target_file, int64_t file_id);
  NamespaceUseVisitor(clang::ASTContext &context, NamespacePassPorts &ports,
                      std::string target_file, int64_t file_id,
                      PassMetrics *metrics, FileRouter router = {});

  // Scope tracking: the nearest enclosing INDEXED symbol is the edge source.
  // TraverseDecl is the RAV adapter; begin_decl/end_decl are the same
  // non-recursive scope handler, so a recorded event stream can drive it.
  bool TraverseDecl(clang::Decl *decl);
  bool VisitDecl(clang::Decl *decl);

  std::optional<int64_t> begin_decl(clang::Decl *decl);
  void end_decl(std::optional<int64_t> scope_id);

  std::optional<int64_t> scope_symbol_id(const clang::Decl *decl);

  // The qualifier handler without the RAV descent, so replaying a recorded
  // qualifier does not re-traverse a subtree that was already recorded.
  void visit_nested_name_specifier(clang::NestedNameSpecifierLoc nns);
  bool TraverseNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc nns);
  bool VisitUsingDirectiveDecl(clang::UsingDirectiveDecl *decl);
  // LLVM 22 folds elaboration: tag/typedef TypeLocs embed their qualifier, so
  // RAV never hands it to TraverseNestedNameSpecifierLoc — visit explicitly.
  bool VisitTypeLoc(clang::TypeLoc tl);

private:
  bool in_target_file(const clang::Decl *decl);
  void emit_ns_use(const clang::NamedDecl *ns_decl, clang::SourceLocation loc);

  clang::ASTContext &context_;
  NamespacePassPorts &ports_;
  std::string target_file_;
  int64_t file_id_;
  std::vector<int64_t> scope_stack_; // enclosing indexed symbol ids
  // RAV can reach the same qualifier TypeLoc via two paths (function proto +
  // param decl); libclang visits each NAMESPACE_REF once — dedupe by site.
  std::set<std::tuple<int64_t, int64_t, int64_t, int64_t>> seen_;
  PassMetrics *metrics_ = nullptr;
  FileRouter router_;
};

} // namespace cidx::ast
