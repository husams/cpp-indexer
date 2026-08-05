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

struct ParallelIndexReport {
  ParallelRunMetrics run;
  HeaderClaimMetrics claims;
  ParallelPlan plan;
  ParallelRunStatus status = ParallelRunStatus::none;
  // Translation units whose extraction produced a publishable batch AND whose
  // publication committed.
  std::size_t published = 0;
  std::size_t failed = 0;
};

// Invoked on the calling thread, in legacy apply order, once per translation
// unit -- successful or not. `published` is false for a failed parse, a source
// that changed under the parse, or a writer failure. Returning false stops the
// run.
using ParallelIndexObserver = std::function<bool(
    const ParallelIndexTarget &target, const ast::IndexOneOutcome &outcome,
    bool published)>;

// `index_path` is the authoritative database each worker opens read-only.
// `db` is the scheduler's read-write handle, and the only handle that writes.
[[nodiscard]] auto
run_parallel_index(cidx::Storage &db, const std::string &index_path,
                   const std::vector<ParallelIndexTarget> &targets,
                   bool graph_enabled, bool no_front_end_reuse,
                   const ParallelBudgets &budgets,
                   const std::function<bool()> &cancelled,
                   const ParallelIndexObserver &observer)
    -> ParallelIndexReport;

// Publishes the report through the opt-in profile session. Named separately so
// a caller can decide when telemetry is emitted.
void record_parallel_index_profile(const ParallelIndexReport &report);

} // namespace cidx::index
