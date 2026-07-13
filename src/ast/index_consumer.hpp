// ASTConsumer that runs the full interleaved indexing sequence over a TU,
// mirroring AstIndexer's ordering exactly (ast_symbols.cpp + ast_cursor.cpp
// index_headers):
//
//   symbols(main) -> edges(main)
//   -> symbols(header 1) -> symbols(header 2) -> ...   (pass 1)
//   -> edges(header 1)   -> edges(header 2)   -> ...   (pass 2)
//
// This per-file symbol availability is semantically significant: during the
// main file's edge walk, header-owned symbols do not exist yet, so lookups
// (e.g. method_of's owner) miss and those edges are emitted by the header
// walk instead — exactly once.
#pragma once

#include "clang/AST/ASTConsumer.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace clang {
class ASTContext;
}

namespace cidx::lt {

class EdgeSink;
class StorageSymbolSink;

class IndexConsumer : public clang::ASTConsumer {
public:
  // targets: (absolute path, file_id) — main file FIRST, then owned headers
  // in cidx's discovery order.
  IndexConsumer(StorageSymbolSink &symbols, EdgeSink &edges,
                std::vector<std::pair<std::string, int64_t>> targets);

  void HandleTranslationUnit(clang::ASTContext &context) override;

private:
  StorageSymbolSink &symbols_;
  EdgeSink &edges_;
  std::vector<std::pair<std::string, int64_t>> targets_;
};

} // namespace cidx::lt
