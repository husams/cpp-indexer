#include "ast/function_definition_visitor.hpp"

#include "ast/statement_edge_visitor.hpp"
#include "ast/edge_sink.hpp"
#include "ast/location.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/SourceManager.h"

namespace cidx::ast {

FunctionDefinitionVisitor::FunctionDefinitionVisitor(clang::ASTContext &context, EdgeSink &sink,
                                 std::string target_file, int64_t file_id)
    : context_(context), sink_(sink), target_file_(std::move(target_file)),
      file_id_(file_id) {}

// An indexable function definition: has an actual body, is not nested in
// another function (a LOCAL class's methods are covered by the enclosing
// function's descent), and sits in the target file.
bool FunctionDefinitionVisitor::is_indexable_definition(
    const clang::FunctionDecl *decl) const {
  if (!decl->doesThisDeclarationHaveABody()) {
    return false;
  }
  if (decl->getParentFunctionOrMethod() != nullptr) {
    return false;
  }
  return expansion_loc(context_, decl->getLocation()).file == target_file_;
}

// The definition row over keyed's extent, then a StatementEdgeVisitor walk of
// the body, snapshotted into def_edge rows.
void FunctionDefinitionVisitor::index_definition(clang::FunctionDecl *decl,
                                                 const clang::NamedDecl *keyed,
                                                 int64_t fn_sym) {
  const clang::SourceRange range = keyed->getSourceRange();
  const ExpansionLoc start = extent_start(context_, range);
  const ExpansionLoc end = extent_end(context_, range);
  const int64_t def_id = sink_.get_or_create_definition(
      fn_sym, file_id_, start.line, start.col, end.line, end.col,
      std::nullopt);
  StatementEdgeVisitor body(context_, sink_, fn_sym, file_id_);
  body.walk(decl);
  sink_.copy_body_edges_to_def_edge(def_id, fn_sym);
}

bool FunctionDefinitionVisitor::VisitFunctionDecl(clang::FunctionDecl *decl) {
  if (!is_indexable_definition(decl)) {
    return true;
  }
  // The symbol row keyed by this decl's USR: a templated pattern's body
  // belongs to its function template (same USR either way).
  const clang::NamedDecl *keyed = decl;
  if (const clang::FunctionTemplateDecl *ft =
          decl->getDescribedFunctionTemplate()) {
    keyed = ft;
  }
  const std::string usr = usr_for_decl(keyed);
  if (usr.empty()) {
    return true;
  }
  if (const auto fn_sym = sink_.lookup_symbol_id(usr)) {
    index_definition(decl, keyed, *fn_sym);
  }
  return true;
}

} // namespace cidx::ast
