// Production orchestration of the optional TU FactBatch cache (ADR-016).
//
// This is the only place indexing decides whether a translation unit is
// replayed from the cache or extracted by Clang. Both product surfaces (the
// CLI index command and the application index service) route through it, so a
// cache decision, its telemetry, and its conservative fallback are identical
// on every path. `index.db` stays authoritative: every failure mode here ends
// in a real extraction, never in a translation unit that only looks current.
#pragma once

#include "ast/index_engine.hpp"
#include "storage/tu_dependency_planner.hpp"
#include "storage/tu_fact_cache.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cidx {
class Storage;
struct File;
} // namespace cidx

namespace cidx::application {

// Deterministic fault points for the conservative-fallback gate. Production
// callers never set these; the CLI cannot reach them.
enum class TuCacheFaultInjection : std::uint8_t {
  none,
  // A validated hit whose payload fails to decode.
  decode_failure,
  // A validated hit whose transactional replay fails.
  replay_failure,
  // A publication whose evidence is reported incomplete.
  incomplete_evidence,
};

struct TuFactCacheOptions {
  bool enabled = true;
  // Empty selects the workspace's default artifact root.
  std::string artifact_root;
  TuCacheFaultInjection fault = TuCacheFaultInjection::none;
};

enum class TuCacheAction : std::uint8_t {
  disabled,
  // Served from the cache: Clang was not invoked for this translation unit.
  replayed,
  // Extracted by Clang and published as a new cache entry.
  extracted_and_published,
  // Extracted by Clang; no entry was published (failed parse, incomplete or
  // unresolved evidence, or an unpublishable outcome).
  extracted,
};

struct TuCacheDecision {
  TuCacheAction action = TuCacheAction::disabled;
  storage::TuFactCacheStatus status = storage::TuFactCacheStatus::unavailable;
  storage::DependencyFallbackReason fallback =
      storage::DependencyFallbackReason::none;
  std::string reason;
  std::string cache_identity;
  bool parser_invoked = true;
};

// Reads `CIDX_TU_FACT_CACHE` (0/false/off disables) and
// `CIDX_TU_FACT_CACHE_ROOT`.
[[nodiscard]] auto tu_fact_cache_options_from_environment()
    -> TuFactCacheOptions;

class TuFactCacheIndexer final {
public:
  TuFactCacheIndexer(cidx::Storage &db, ast::IndexSession &session,
                     TuFactCacheOptions options = {});
  ~TuFactCacheIndexer();
  TuFactCacheIndexer(const TuFactCacheIndexer &) = delete;
  auto operator=(const TuFactCacheIndexer &) -> TuFactCacheIndexer & = delete;
  TuFactCacheIndexer(TuFactCacheIndexer &&) noexcept;
  auto operator=(TuFactCacheIndexer &&) noexcept -> TuFactCacheIndexer &;

  // Drop-in replacement for ast::run_index_one with the cache decision made
  // around it. A cached replay reports the same outcome fields the extraction
  // path reports for the counters the product surfaces print.
  [[nodiscard]] auto
  index_one(const cidx::File &rec, const std::string &path, bool graph_enabled,
            ast::IndexFailurePoint failure = ast::IndexFailurePoint::none,
            bool no_front_end_reuse = false) -> ast::IndexOneOutcome;

  [[nodiscard]] auto last_decision() const -> const TuCacheDecision &;

  // Reverse-dependency planning over the recorded evidence: which translation
  // unit/configuration pairs a changed input set affects. Incomplete evidence
  // is reported through the plan's fallback reason, never as an empty set.
  [[nodiscard]] auto
  plan_affected(const std::vector<std::string> &changed_dependencies)
      -> storage::TuDependencyPlan;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cidx::application
