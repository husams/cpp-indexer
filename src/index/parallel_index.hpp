// Bounded parallel translation-unit indexing: the production binding of the
// generic runner, the ordered header-claim oracle and the controlled writer
// (S-074).
//
// The division of labour is the point of the whole story:
//
//   worker  -- owns an isolated IndexSession, Toolchain and READ-ONLY database
//              handle; parses one translation unit and stops at an immutable
//              FactBatch. It never mutates the authoritative database, and the
//              read-only handle is what proves it rather than asserting it.
//   oracle  -- decides owned-header ownership in the legacy apply order, so a
//              header shared by K translation units is extracted exactly once
//              and the indexed/already counters match the serial run.
//   scheduler (the calling thread) -- publishes each completed batch through
//              the single controlled FactBatchWriter, in the legacy apply
//              order. Worker completion order never becomes persistence order.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ast/index_engine.hpp"
#include "index/parallel_policy.hpp"
#include "index/parallel_runner.hpp"
#include "storage/records.hpp"

namespace cidx {
class Storage;
}

namespace cidx::index {

// One translation unit to index, in the caller's legacy apply order.
struct ParallelIndexTarget {
  cidx::File file;
  std::string path;
};

// Bounded retry policy. A translation unit whose source changed UNDER the parse
// is the one genuinely transient failure the engine reports: the remedy the
// serial path documents is to index it again. Everything else -- a parse
// failure, a worker crash, a writer or commit failure -- is deterministic, so
// retrying it would burn a full parse to reach the same answer and would hide
// the defect. Those are reported, never retried.
inline constexpr std::size_t kDefaultSourceChangeRetries = 2;

struct ParallelIndexReport {
  ParallelRunMetrics run;
  HeaderClaimMetrics claims;
  ParallelPlan plan;
  ParallelRunStatus status = ParallelRunStatus::none;
  // Translation units whose extraction produced a publishable batch AND whose
  // publication committed.
  std::size_t published = 0;
  std::size_t failed = 0;
  // Extractions re-run because the source changed under the parse.
  std::size_t retries = 0;
  // Translation units that exhausted the retry budget and still saw a changed
  // source; reported as failures, never as published.
  std::size_t retry_exhausted = 0;
};

// Invoked on the calling thread, in legacy apply order, once per translation
// unit -- successful or not. `published` is false for a failed parse, a source
// that changed under the parse, or a writer failure. Returning false stops the
// run.
using ParallelIndexObserver =
    std::function<bool(const ParallelIndexTarget &target,
                       const ast::IndexOneOutcome &outcome, bool published)>;

// Deterministic completion-order control, in the same spirit as the engine's
// IndexFailurePoint injectors: null on every production path, and the only way
// a test can force a completion order that is NOT the dispatch order. Invoked
// on the worker thread with the rank it just finished extracting, before the
// result reaches the reorder buffer.
using ParallelExtractionBarrier = std::function<void(std::size_t rank)>;

// `index_path` is the authoritative database. Workers never open it: the run
// takes one consistent snapshot of it and each worker reads that read-only.
// `db` is the scheduler's read-write handle, and the only handle that writes.
[[nodiscard]] auto run_parallel_index(
    cidx::Storage &db, const std::string &index_path,
    const std::vector<ParallelIndexTarget> &targets, bool graph_enabled,
    bool no_front_end_reuse, const ParallelBudgets &budgets,
    const std::function<bool()> &cancelled,
    const ParallelIndexObserver &observer,
    const ParallelExtractionBarrier &extracted = {}) -> ParallelIndexReport;

// Publishes the report through the opt-in profile session. Named separately so
// a caller can decide when telemetry is emitted.
void record_parallel_index_profile(const ParallelIndexReport &report);

} // namespace cidx::index
