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
    int64_t file_id, PassMetrics *metrics, FileRouter router)
    : context_(context), identity_(identity), definitions_(definitions),
      target_file_(std::move(target_file)), file_id_(file_id),
      metrics_(metrics), router_(std::move(router)) {}

bool FunctionDefinitionVisitor::VisitDecl(clang::Decl * /*decl*/) {
  if (metrics_ != nullptr) {
    metrics_->note_visited();
  }
  return true;
}

// An indexable function definition has an actual body and sits in a routed
// file. Nested methods and lambda call operators are intentionally included;
// the canonical pending-body key prevents duplicate statement walks.
bool FunctionDefinitionVisitor::is_indexable_definition(
    const clang::FunctionDecl *decl) {
  if (!decl->doesThisDeclarationHaveABody()) {
    return false;
  }
  const std::string file = expansion_loc(context_, decl->getLocation()).file;
  if (router_) {
    const auto file_id = router_(file);
    if (!file_id) {
      return false;
    }
    file_id_ = *file_id;
    target_file_ = file;
    return true;
  }
  return file == target_file_;
}

void FunctionDefinitionVisitor::run_statement_pass(
    StatementFactPorts &ports, PassMetrics *metrics,
    DefinitionScopeEmitter *statement_definitions) {
  DefinitionScopeEmitter &definitions =
      statement_definitions != nullptr ? *statement_definitions : definitions_;
  pending_bodies_.drain([&](const PendingBody &body) {
    if (router_) {
      router_(body.file);
    }
    StatementEdgeVisitor statement_body(context_, ports, body.symbol_id,
                                        body.file_id, body.file, metrics);
    statement_body.walk(body.definition);
    ++statement_body_count_;
    definitions.copy_body_edges_to_def_edge(body.definition_id, body.symbol_id);
  });
}

// Create the definition row. Statement facts are emitted by the separate
// statement pass above so the two stages can be independently registered.
void FunctionDefinitionVisitor::index_definition(
    clang::FunctionDecl *decl, const clang::NamedDecl *keyed, int64_t fn_sym,
    const clang::FunctionDecl *canonical_owner) {
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
  ++definition_count_;
  pending_bodies_.schedule({.canonical_owner = canonical_owner,
                            .definition = decl,
                            .symbol_id = fn_sym,
                            .definition_id = def_id,
                            .file_id = file_id_,
                            .file = target_file_});
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
  clang::FunctionDecl *definition = decl->getDefinition();
  if (definition == nullptr) {
    return true;
  }
  const clang::FunctionDecl *canonical_owner = definition->getCanonicalDecl();
  if (const clang::FunctionTemplateDecl *ft =
          decl->getDescribedFunctionTemplate()) {
    canonical_owner = ft->getTemplatedDecl()->getCanonicalDecl();
  }
  if (pending_bodies_.contains(canonical_owner)) {
    return true;
  }
  const std::string usr = usr_for_decl(keyed);
  if (usr.empty()) {
    return true;
  }
  if (const auto fn_sym = identity_.lookup_symbol_id(
          usr, expansion_loc(context_, keyed->getLocation()).file)) {
    index_definition(definition, keyed, *fn_sym, canonical_owner);
  }
  return true;
}

} // namespace cidx::ast
