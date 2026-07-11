// Namespace-uses pass (B3): for each target-file namespace QUALIFIER
// reference (geo::..., using namespace ns, using ns::f) emit a uses(7) edge
// nearest-enclosing-indexed-symbol -> namespace, with one edge_site — the
// LibTooling analogue of emit_namespace_uses / ns_uses_descend
// (ast_cursor.cpp:285). Descends into bodies; lookup-only (bare std:: refs
// miss and are skipped).
#pragma once

#include "clang/AST/RecursiveASTVisitor.h"

#include <cstdint>
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

namespace cidx::lt {

class EdgeSink;

class NsUsesVisitor : public clang::RecursiveASTVisitor<NsUsesVisitor> {
public:
  NsUsesVisitor(clang::ASTContext &context, EdgeSink &sink,
                std::string target_file, int64_t file_id);

  // Scope tracking: the nearest enclosing INDEXED symbol is the edge source.
  bool TraverseDecl(clang::Decl *decl);

  bool TraverseNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc nns);
  bool VisitUsingDirectiveDecl(clang::UsingDirectiveDecl *decl);
  // LLVM 22 folds elaboration: tag/typedef TypeLocs embed their qualifier, so
  // RAV never hands it to TraverseNestedNameSpecifierLoc — visit explicitly.
  bool VisitTypeLoc(clang::TypeLoc tl);

private:
  bool in_target_file(const clang::Decl *decl) const;
  void emit_ns_use(const clang::NamedDecl *ns_decl,
                   clang::SourceLocation loc);

  clang::ASTContext &context_;
  EdgeSink &sink_;
  std::string target_file_;
  int64_t file_id_;
  std::vector<int64_t> scope_stack_; // enclosing indexed symbol ids
  // RAV can reach the same qualifier TypeLoc via two paths (function proto +
  // param decl); libclang visits each NAMESPACE_REF once — dedupe by site.
  std::set<std::tuple<int64_t, int64_t, int64_t, int64_t>> seen_;
};

} // namespace cidx::lt
