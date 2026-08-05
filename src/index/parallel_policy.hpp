// Deterministic worker/budget selection for bounded parallel translation-unit
// extraction (S-074).
//
// Nothing here parses, allocates a worker, or touches the database: this header
// owns only the arithmetic that turns an operator request plus observed host
// facts into a bounded plan. Keeping the policy pure makes the automatic
// selection reproducible on a recorded host and unit-testable without Clang.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cidx::index {

// Operator-supplied contracts. Every field is an explicit bound; `jobs == 0`
// selects the documented automatic policy rather than "unbounded".
struct ParallelBudgets {
  // 0 selects the automatic worker count (see select_worker_count).
  int jobs = 0;
  // Bytes of extracted-but-unpublished FactBatch payload allowed to sit in the
  // reorder buffer. 0 selects the automatic budget.
  std::uint64_t max_queue_bytes = 0;
  // Completed-but-unpublished translation units allowed in the reorder buffer.
  // 0 selects the automatic budget.
  std::size_t max_queue_items = 0;
  // Whole-process resident-memory ceiling used to derive the worker count.
  // 0 selects the automatic budget from observed host memory.
  std::uint64_t memory_budget_bytes = 0;
};

// The bounded plan the runner executes. Every value is >= 1 so no path can
// derive a zero-capacity queue or a zero-worker pool from a hostile budget.
struct ParallelPlan {
  std::size_t workers = 1;
  std::size_t max_queue_items = 1;
  std::uint64_t max_queue_bytes = 0;
  std::uint64_t memory_budget_bytes = 0;
  // Bytes of resident memory the policy reserved per worker.
  std::uint64_t reserved_bytes_per_worker = 0;
  // Why this worker count was chosen, for the profile and the operator log.
  std::string selection_reason;
};

// Observed host facts. Injected so the policy is testable without probing the
// machine and so a benchmark can replay a recorded host.
struct HostResources {
  std::size_t logical_cores = 1;
  std::uint64_t total_memory_bytes = 0;
  std::uint64_t available_memory_bytes = 0;
};

// Reads the real machine. Falls back to conservative values when a probe fails;
// never reports zero cores.
[[nodiscard]] auto probe_host_resources() -> HostResources;

// Conservative resident-memory reservation for one in-flight Clang extraction.
// The dominant term is one live ASTContext plus its source buffers; the value
// is deliberately generous because over-reserving costs throughput while
// under-reserving costs the whole run.
inline constexpr std::uint64_t kDefaultBytesPerWorker =
    768ULL * 1024ULL * 1024ULL;

// Default share of *available* memory the indexer is willing to occupy when the
// operator gave no explicit ceiling.
inline constexpr double kDefaultMemoryShare = 0.60;

// Derives the bounded plan. `pending_translation_units` caps the pool: a run
// with three translation units never starts eight workers.
//
// Automatic worker selection, in order:
//   1. never more workers than pending translation units;
//   2. never more workers than logical cores;
//   3. never more workers than the memory budget divided by the measured or
//      reserved bytes per worker;
//   4. at least one.
// An explicit `--jobs N` skips 2 and 3 for the core/memory *derivation* but is
// still clamped by 1 so the pool cannot exceed the work available.
[[nodiscard]] auto
plan_parallel_extraction(const ParallelBudgets &budgets,
                         const HostResources &host,
                         std::size_t pending_translation_units) -> ParallelPlan;

} // namespace cidx::index
