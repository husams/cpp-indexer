#include "ast/index_consumer.hpp"

#include "ast/body_pass_visitor.hpp"
#include "ast/edge_sink.hpp"
#include "ast/edge_visitor.hpp"
#include "ast/ns_uses_visitor.hpp"
#include "ast/storage_symbol_sink.hpp"
#include "ast/symbol_visitor.hpp"

#include "clang/AST/ASTContext.h"

namespace cidx::lt {

IndexConsumer::IndexConsumer(
    StorageSymbolSink &symbols, EdgeSink &edges,
    std::vector<std::pair<std::string, int64_t>> targets)
    : symbols_(symbols), edges_(edges), targets_(std::move(targets)) {}

void IndexConsumer::HandleTranslationUnit(clang::ASTContext &context) {
  if (targets_.empty())
    return;
  clang::Decl *tu = context.getTranslationUnitDecl();

  const auto run_symbols = [&](const std::pair<std::string, int64_t> &t) {
    symbols_.set_current_file_id(t.second);
    SymbolVisitor visitor(context, symbols_, t.first);
    visitor.TraverseDecl(tu);
  };
  const auto run_edges = [&](const std::pair<std::string, int64_t> &t) {
    // index_edges (ast.cpp): drop the file's stale edges/definitions, then
    // B1 declaration walk, B2 body walk, B3 namespace uses.
    edges_.delete_edges_for_file(t.second);
    edges_.delete_definitions_for_file(t.second);
    EdgeVisitor decls(context, edges_, t.first, t.second);
    decls.TraverseDecl(tu);
    BodyPassVisitor bodies(context, edges_, t.first, t.second);
    bodies.TraverseDecl(tu);
    NsUsesVisitor ns(context, edges_, t.first, t.second);
    ns.TraverseDecl(tu);
  };

  // commands.cpp sequence: symbols(main) -> index_headers (all header
  // symbols, then per-header edges) -> edges(main) LAST. The per-file edge
  // deletes make this idempotent AND collapse cross-file double emissions
  // (a header-walk edge whose src symbol belongs to the main file is deleted
  // before the main walk re-emits it once).
  run_symbols(targets_[0]);
  for (size_t i = 1; i < targets_.size(); ++i)
    run_symbols(targets_[i]);
  for (size_t i = 1; i < targets_.size(); ++i)
    run_edges(targets_[i]);
  run_edges(targets_[0]);
}

} // namespace cidx::lt
