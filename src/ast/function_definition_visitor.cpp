#include "ast/function_definition_visitor.hpp"

#include "ast/fact_emitters.hpp"
#include "ast/location.hpp"
#include "ast/pass_registry.hpp"
#include "ast/statement_edge_visitor.hpp"
#include "ast/usr.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/SourceManager.h"

namespace cidx::ast {

FunctionDefinitionVisitor::FunctionDefinitionVisitor(
    clang::ASTContext &context, DeclarationIdentityResolver &identity,
    DefinitionScopeEmitter &definitions, std::string target_file,
    int64_t file_id, PassMetrics *metrics)
    : context_(context), identity_(identity), definitions_(definitions),
      target_file_(std::move(target_file)), file_id_(file_id),
      metrics_(metrics) {}

bool FunctionDefinitionVisitor::VisitDecl(clang::Decl * /*decl*/) {
  if (metrics_ != nullptr) {
    metrics_->note_visited();
  }
  return true;
}

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

void FunctionDefinitionVisitor::run_statement_pass(
    StatementFactPorts &ports, PassMetrics *metrics,
    DefinitionScopeEmitter *statement_definitions) {
  DefinitionScopeEmitter &definitions =
      statement_definitions != nullptr ? *statement_definitions : definitions_;
  for (const DefinitionFact &fact : definitions_found_) {
    StatementEdgeVisitor body(context_, ports, fact.symbol_id, file_id_,
                              target_file_, metrics);
    body.walk(fact.decl);
    definitions.copy_body_edges_to_def_edge(fact.definition_id, fact.symbol_id);
  }
}

// Create the definition row. Statement facts are emitted by the separate
// statement pass above so the two stages can be independently registered.
void FunctionDefinitionVisitor::index_definition(clang::FunctionDecl *decl,
                                                 const clang::NamedDecl *keyed,
                                                 int64_t fn_sym) {
  const clang::SourceRange range = keyed->getSourceRange();
  const ExpansionLoc start = extent_start(context_, range);
  const ExpansionLoc end = extent_end(context_, range);
  if (metrics_ != nullptr) {
    metrics_->note_emitted();
  }
  const int64_t def_id = definitions_.get_or_create_definition(
      fn_sym, file_id_, start.line, start.col, end.line, end.col, std::nullopt);
  if (metrics_ != nullptr) {
    metrics_->note_fact_family("definitions", 1, 1);
  }
  definitions_found_.push_back(
      {.decl = decl, .symbol_id = fn_sym, .definition_id = def_id});
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
  if (const auto fn_sym = identity_.lookup_symbol_id(
          usr, expansion_loc(context_, keyed->getLocation()).file)) {
    index_definition(decl, keyed, *fn_sym);
  }
  return true;
}

} // namespace cidx::ast
