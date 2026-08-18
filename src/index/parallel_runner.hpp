// Bounded, deterministic parallel dispatch with legacy-order publication
// (S-074).
//
// The runner is deliberately generic over "what one unit of extraction is": it
// owns only scheduling, backpressure, reordering and failure propagation, so
// the ordering contract can be proven under randomised completion order without
// Clang, a database, or a parse. Production supplies extraction and publication
// callbacks that operate on translation units.
//
// The contract the rest of the system depends on:
//
//   * Work is DISPATCHED in the caller's order. The caller supplies items
//     already sorted by the legacy apply key
//     `(component.path, directory.path, file.name)`.
//   * Work is PUBLISHED in that same order, on the calling thread, one item at
//     a time. Worker completion order never becomes persistence order.
//   * At most `workers` extractions are in flight, and completed-but-
//     unpublished results are bounded by both an item count and a byte budget.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "index/parallel_policy.hpp"

namespace cidx::index {

// Why a run stopped early. `none` means every item was published.
enum class ParallelRunStatus : std::uint8_t {
  none,
  // A publish callback asked to stop (a writer or commit failure).
  publication_stopped,
  // The caller's cancellation predicate fired.
  cancelled,
};

struct ParallelRunMetrics {
  std::size_t items_dispatched = 0;
  std::size_t items_published = 0;
  std::size_t workers_started = 0;
  // Peak occupancy of the reorder buffer.
  std::size_t peak_reorder_items = 0;
  std::uint64_t peak_reorder_bytes = 0;
  // Wall time workers spent blocked on the byte/item budget.
  double total_backpressure_seconds = 0.0;
  // Wall time the publisher spent waiting for the next in-order result.
  double total_publish_wait_seconds = 0.0;
  // Aggregate worker busy time, for utilisation.
  double total_worker_active_seconds = 0.0;
  double total_worker_idle_seconds = 0.0;
  double wall_seconds = 0.0;
  // Bytes reserved per in-flight extraction times peak in-flight extractions.
  std::uint64_t peak_reserved_bytes = 0;
  std::size_t publication_windows = 0;
  std::size_t peak_publish_window_items = 0;
  std::uint64_t peak_publish_window_bytes = 0;
};

// One unit of work as seen by the runner. `bytes` is filled by the extraction
// callback so the buffer can be bounded by real payload size.
template <typename Payload> struct ParallelResult {
  Payload payload;
  std::uint64_t bytes = 0;
  // Set when extraction threw or failed; publication is skipped and the error
  // is reported through the publish callback's failure branch.
  std::optional<std::string> error = std::nullopt;
};

template <typename Payload> struct RankedParallelResult {
  std::size_t rank = 0;
  ParallelResult<Payload> result;
};

struct ParallelWindowPublication {
  std::size_t items_published = 0;
  ParallelRunStatus stop_status = ParallelRunStatus::none;
};

// Windowed form used by the production indexer. The runner collects consecutive
// ranks up to both `max_window_items` and the plan's queue-byte bound, then
// submits that bounded span to one publication callback.
template <typename Payload>
[[nodiscard]] auto run_parallel_extraction_windowed(
    const ParallelPlan &plan, std::size_t count, std::size_t max_window_items,
    const std::function<ParallelResult<Payload>(std::size_t worker_index,
                                                std::size_t rank)> &extract,
    const std::function<ParallelWindowPublication(
        std::span<RankedParallelResult<Payload>> window)> &publish,
    const std::function<bool()> &cancelled,
    const std::function<void(std::size_t rank)> &on_abandon,
    ParallelRunMetrics &metrics) -> ParallelRunStatus;

// Runs `count` items through `extract` on up to `plan.workers` threads and
// hands each result to `publish` on the calling thread in index order.
//
// `extract(worker_index, rank)` runs concurrently and must not touch the
// authoritative database.
// `publish(rank, result)` runs serially on the calling thread; returning false
// stops the run (remaining items are neither published nor dispatched).
// `cancelled()` is polled between dispatches and publications.
// `on_abandon(rank)` is invoked for every rank that is dispatched but whose
// result is never published, so an ordered gate held elsewhere can be released.
template <typename Payload>
[[nodiscard]] auto run_parallel_extraction(
    const ParallelPlan &plan, std::size_t count,
    const std::function<ParallelResult<Payload>(std::size_t worker_index,
                                                std::size_t rank)> &extract,
    const std::function<bool(std::size_t rank, ParallelResult<Payload> &result)>
        &publish,
    const std::function<bool()> &cancelled,
    const std::function<void(std::size_t rank)> &on_abandon,
    ParallelRunMetrics &metrics) -> ParallelRunStatus;

} // namespace cidx::index

#include "index/parallel_runner_impl.hpp" // IWYU pragma: export
