// Body-pass driver (B2): finds each function-like DEFINITION in the target
// file, creates its per-backend `definition` row, runs the StatementEdgeVisitor over
// its body, then snapshots the just-emitted calls/uses into def_edge.
#pragma once

#include "clang/AST/RecursiveASTVisitor.h"

#include <cstdint>
#include <string>

namespace clang {
class ASTContext;
class FunctionDecl;
class NamedDecl;
} // namespace clang

namespace cidx::ast {

class EdgeSink;

class FunctionDefinitionVisitor : public clang::RecursiveASTVisitor<FunctionDefinitionVisitor> {
public:
  FunctionDefinitionVisitor(clang::ASTContext &context, EdgeSink &sink,
                  std::string target_file, int64_t file_id);

  bool VisitFunctionDecl(clang::FunctionDecl *decl);

private:
  bool is_indexable_definition(const clang::FunctionDecl *decl) const;
  void index_definition(clang::FunctionDecl *decl,
                        const clang::NamedDecl *keyed, int64_t fn_sym);

  clang::ASTContext &context_;
  EdgeSink &sink_;
  std::string target_file_;
  int64_t file_id_;
};

} // namespace cidx::ast
