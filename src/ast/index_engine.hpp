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

#include <cstddef>
#include <cstdint>

#include "ast/fact_records.hpp"
#include "ast/header_stats.hpp" // HeaderStats
#include "ast/pass_registry.hpp"
#include "storage/records.hpp"
#include "util/logger.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cidx {
class Storage;
}

namespace cidx::ast {

enum class IndexFailurePoint : std::uint8_t;

struct SourceSnapshot {
  std::optional<double> mtime;
  std::optional<std::string> md5;

  static SourceSnapshot capture(const std::string &path);
  [[nodiscard]] bool matches(const std::string &path) const;
};

struct IndexPassMetrics {
  std::string id;
  std::vector<FrontendCapability> required_capabilities;
  std::vector<std::string> dependencies;
  std::vector<std::string> consumed_fact_families;
  std::vector<std::string> produced_fact_families;
  FactCompleteness completeness = FactCompleteness::complete;
  FactTrust trust = FactTrust::trusted;
  std::size_t visited_constructs = 0;
  std::size_t emitted_facts = 0;
  std::size_t unknown_constructs = 0;
  std::size_t duplicates = 0;
  std::size_t diagnostics = 0;
  std::size_t registered_whole_tu_traversal_budget = 0;
  std::size_t whole_tu_traversals = 0;
  std::map<std::string, PassMetrics::FactFamily, std::less<>> fact_families;
  std::int64_t elapsed_microseconds = 0;
  bool budget_exhausted = false;
};

enum class IndexInvalidationReason : std::uint8_t {
  manual,
  repository_switch,
  clone_change,
  component_update,
  compile_option_update,
  generated_input_change,
  source_mutation,
};

struct IndexSessionMetrics {
  std::size_t generation = 0;
  std::size_t snapshot_rebuilds = 0;
  std::size_t descriptor_hits = 0;
  std::size_t descriptor_misses = 0;
  std::size_t configuration_id_hits = 0;
  std::size_t configuration_id_misses = 0;
  std::size_t configuration_hits = 0;
  std::size_t configuration_misses = 0;
  std::size_t driver_subprocesses = 0;
  std::size_t cache_evictions = 0;
  std::size_t file_stat_reads = 0;
  std::size_t file_hash_reads = 0;
  std::size_t source_change_checks = 0;
  std::size_t component_scans = 0;
  std::size_t ownership_hits = 0;
  std::size_t ownership_misses = 0;
  std::size_t repository_invalidations = 0;
  std::size_t clone_invalidations = 0;
  std::size_t component_invalidations = 0;
  std::size_t configuration_invalidations = 0;
  std::size_t generated_input_invalidations = 0;
  std::size_t source_invalidations = 0;
};

struct IndexOneOutcome {
  int stored = 0;            // main-file symbols stored (index_symbols)
  cidx::HeaderStats headers; // header two-pass counters
  std::vector<cidx::Diagnostic> diagnostics;
  bool parse_failed = false;   // load failure or fatal diags (ClangParseError)
  bool source_changed = false; // bytes changed during the parse
  std::optional<double> source_mtime;
  std::optional<std::string> source_md5; // digest captured before the parse
  std::string error;
  std::vector<std::string> failed_flags; // final args, for the log dump
  std::vector<IndexPassMetrics> pass_metrics;
  std::size_t registered_whole_tu_traversal_budget = 0;
  std::size_t observed_whole_tu_traversals = 0;
  std::vector<EvidenceRecord> evidence;
  IndexSessionMetrics session_metrics;
};

class IndexSession final {
public:
  explicit IndexSession(cidx::Storage &db,
                        cidx::Logger &log = cidx::Logger::root());
  ~IndexSession();
  IndexSession(IndexSession &&) noexcept;
  IndexSession &operator=(IndexSession &&) noexcept;
  IndexSession(const IndexSession &) = delete;
  IndexSession &operator=(const IndexSession &) = delete;

  void
  invalidate(IndexInvalidationReason reason = IndexInvalidationReason::manual);
  [[nodiscard]] IndexSessionMetrics metrics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  friend IndexOneOutcome
  run_index_one(cidx::Storage &db, IndexSession &session, const cidx::File &rec,
                const std::string &path, bool graph_enabled,
                IndexFailurePoint failure, bool no_front_end_reuse);
};

// Deterministic fault points used by the production TU pipeline tests. The
// default path is none; callers cannot enable these accidentally through the
// CLI.
enum class IndexFailurePoint : std::uint8_t {
  none,
  begin,
  adapter,
  partial_transform,
  commit,
  // The seven phase boundaries of the fused two-root pipeline, in publication
  // order. See cidx::storage::FailurePoint for what each one brackets.
  symbol_capture_complete,
  declaration_replay,
  definition_replay,
  statement_body_replay,
  namespace_replay,
  header_association,
  main_association,
};

IndexOneOutcome
run_index_one(cidx::Storage &db, const cidx::File &rec, const std::string &path,
              bool graph_enabled,
              IndexFailurePoint failure = IndexFailurePoint::none,
              bool no_front_end_reuse = false);

IndexOneOutcome
run_index_one(cidx::Storage &db, IndexSession &session, const cidx::File &rec,
              const std::string &path, bool graph_enabled,
              IndexFailurePoint failure = IndexFailurePoint::none,
              bool no_front_end_reuse = false);

} // namespace cidx::ast
