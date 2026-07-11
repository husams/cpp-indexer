// AST indexer orchestration; extraction lives in focused ast_*.cpp modules.
#include "clangx/ast.hpp"

namespace cidx {

void AstIndexer::index_edges(const ParsedTu &tu, const std::string &filename,
                             int64_t file_id) {
  if (!graph_enabled_) {
    return;
  }
  // Delete stale edges from a previous index of this file (idempotent
  // re-index).
  db_.delete_edges_for_file(file_id);
  db_.delete_definitions_for_file(file_id); // v27: cascades this file's def_edge

  Transaction txn = db_.transaction();
  index_edges_notxn(tu, filename, file_id);
  txn.commit();
}

} // namespace cidx
