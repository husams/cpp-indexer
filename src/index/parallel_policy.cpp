#include "index/parallel_policy.hpp"

#include <algorithm>
#include <thread>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <array>
#include <sys/sysctl.h>
#include <sys/types.h>
#elifdef __linux__
#include <unistd.h>
#endif

namespace cidx::index {
namespace {

// Total physical memory, or 0 when the platform probe is unavailable.
[[nodiscard]] auto probe_total_memory_bytes() -> std::uint64_t {
#if defined(__APPLE__) || defined(__FreeBSD__)
  std::uint64_t value = 0;
  std::size_t size = sizeof(value);
  std::array<int, 2> name{CTL_HW, HW_MEMSIZE};
  if (sysctl(name.data(), name.size(), &value, &size, nullptr, 0) == 0) {
    return value;
  }
  return 0;
#elifdef __linux__
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long page_size = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && page_size > 0) {
    return static_cast<std::uint64_t>(pages) *
           static_cast<std::uint64_t>(page_size);
  }
  return 0;
#else
  return 0;
#endif
}

} // namespace

auto probe_host_resources() -> HostResources {
  HostResources host;
  const unsigned int hardware = std::thread::hardware_concurrency();
  host.logical_cores = hardware == 0 ? 1U : static_cast<std::size_t>(hardware);
  host.total_memory_bytes = probe_total_memory_bytes();
  // Without a portable "currently available" probe the total is the only
  // defensible input; the memory *share* below keeps the derived budget well
  // inside it.
  host.available_memory_bytes = host.total_memory_bytes;
  return host;
}

auto plan_parallel_extraction(const ParallelBudgets &budgets,
                              const HostResources &host,
                              std::size_t pending_translation_units)
    -> ParallelPlan {
  ParallelPlan plan;
  const std::size_t work =
      pending_translation_units == 0 ? 1 : pending_translation_units;
  const std::size_t cores = host.logical_cores == 0 ? 1 : host.logical_cores;

  plan.reserved_bytes_per_worker = kDefaultBytesPerWorker;
  plan.memory_budget_bytes =
      budgets.memory_budget_bytes != 0
          ? budgets.memory_budget_bytes
          : static_cast<std::uint64_t>(
                static_cast<double>(host.available_memory_bytes) *
                kDefaultMemoryShare);

  if (budgets.jobs > 0) {
    plan.workers = std::min(static_cast<std::size_t>(budgets.jobs), work);
    plan.selection_reason = "explicit --jobs";
  } else {
    // Memory-derived ceiling. A zero or unknown memory budget must not collapse
    // the pool to zero, so the ceiling is at least one worker.
    std::size_t memory_workers = cores;
    if (plan.memory_budget_bytes != 0 && plan.reserved_bytes_per_worker != 0) {
      memory_workers = static_cast<std::size_t>(plan.memory_budget_bytes /
                                                plan.reserved_bytes_per_worker);
      memory_workers = std::max<std::size_t>(memory_workers, 1);
    }
    plan.workers = std::min({work, cores, memory_workers});
    if (memory_workers < cores && plan.workers == memory_workers) {
      plan.selection_reason = "automatic (memory-bound)";
    } else if (work < cores && plan.workers == work) {
      plan.selection_reason = "automatic (work-bound)";
    } else {
      plan.selection_reason = "automatic (core-bound)";
    }
  }
  plan.workers = std::max<std::size_t>(plan.workers, 1);

  // The reorder buffer holds completed-but-unpublished translation units. Two
  // per worker lets a worker start its next translation unit while its previous
  // result waits for its turn in the legacy publication order, without letting
  // an arbitrarily long tail accumulate.
  plan.max_queue_items = budgets.max_queue_items != 0
                             ? budgets.max_queue_items
                             : std::max<std::size_t>(plan.workers * 2, 2);

  // Byte budget for that same buffer. Derived from the memory budget so an
  // operator who lowers memory also lowers the buffer, with a floor that always
  // admits one batch (otherwise a single large translation unit deadlocks).
  plan.max_queue_bytes =
      budgets.max_queue_bytes != 0
          ? budgets.max_queue_bytes
          : std::max<std::uint64_t>(plan.memory_budget_bytes / 8,
                                    64ULL * 1024ULL * 1024ULL);
  return plan;
}

} // namespace cidx::index
