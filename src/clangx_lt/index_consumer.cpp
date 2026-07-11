#include "clangx_lt/index_consumer.hpp"

#include "clangx_lt/edge_visitor.hpp"
#include "clangx_lt/storage_symbol_sink.hpp"
#include "clangx_lt/symbol_visitor.hpp"

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
    EdgeVisitor visitor(context, edges_, t.first);
    visitor.TraverseDecl(tu);
  };

  // Main file: symbols then edges immediately.
  run_symbols(targets_[0]);
  run_edges(targets_[0]);
  // Headers: all symbols first (pass 1), then all edges (pass 2).
  for (size_t i = 1; i < targets_.size(); ++i)
    run_symbols(targets_[i]);
  for (size_t i = 1; i < targets_.size(); ++i)
    run_edges(targets_[i]);
}

} // namespace cidx::lt
