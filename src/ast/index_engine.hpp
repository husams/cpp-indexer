// LT indexing engine: drop-in replacement for index_one's parse+index block
// (commands.cpp) running the Phase 0-2 parity-proven LibTooling visitors over
// one translation unit, with AstIndexer's exact sequencing:
//
//   symbols(main) -> header registration + header symbols (pass 1)
//   -> header edges (pass 2, with per-file delete) -> edges(main) (delete +
//   B1/B2/B3)
//
// Selected at runtime by CIDX_INDEX_ENGINE=lt. This header is clang-free so
// commands.cpp can include it without the Clang C++ API.
#pragma once

#include "ast/header_stats.hpp" // HeaderStats
#include "storage/records.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cidx {
class Storage;
}

namespace cidx::ast {

struct SourceSnapshot {
  std::optional<std::string> md5;

  static SourceSnapshot capture(const std::string &path);
  [[nodiscard]] bool matches(const std::string &path) const;
};

struct IndexOneOutcome {
  int stored = 0;            // main-file symbols stored (index_symbols)
  cidx::HeaderStats headers; // header two-pass counters
  std::vector<cidx::Diagnostic> diagnostics;
  bool parse_failed = false;   // load failure or fatal diags (ClangParseError)
  bool source_changed = false; // bytes changed during the parse
  std::optional<std::string> source_md5; // digest captured before the parse
  std::string error;
  std::vector<std::string> failed_flags; // final args, for the log dump
};

IndexOneOutcome run_index_one(cidx::Storage &db, const cidx::File &rec,
                              const std::string &path, bool graph_enabled);

} // namespace cidx::ast
