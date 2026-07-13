// Body-pass driver (B2): finds each function-like DEFINITION in the target
// file, creates its per-backend `definition` row, runs the BodyVisitor over
// its body, then snapshots the just-emitted calls/uses into def_edge.
#pragma once

#include "clang/AST/RecursiveASTVisitor.h"

#include <cstdint>
#include <string>

namespace clang {
class ASTContext;
class FunctionDecl;
} // namespace clang

namespace cidx::lt {

class EdgeSink;

class BodyPassVisitor : public clang::RecursiveASTVisitor<BodyPassVisitor> {
public:
  BodyPassVisitor(clang::ASTContext &context, EdgeSink &sink,
                  std::string target_file, int64_t file_id);

  bool VisitFunctionDecl(clang::FunctionDecl *decl);

private:
  clang::ASTContext &context_;
  EdgeSink &sink_;
  std::string target_file_;
  int64_t file_id_;
};

} // namespace cidx::lt
