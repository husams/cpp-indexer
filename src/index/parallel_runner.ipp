// Implementation of run_parallel_extraction. Included by parallel_runner.hpp.
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace cidx::index {

template <typename Payload>
auto run_parallel_extraction(
    const ParallelPlan &plan, std::size_t count,
    const std::function<ParallelResult<Payload>(std::size_t, std::size_t)>
        &extract,
    const std::function<bool(std::size_t, ParallelResult<Payload> &)> &publish,
    const std::function<bool()> &cancelled,
    const std::function<void(std::size_t)> &on_abandon,
    ParallelRunMetrics &metrics) -> ParallelRunStatus {
  using Clock = std::chrono::steady_clock;
  const auto run_started = Clock::now();

  std::mutex mutex;
  std::condition_variable admit;   // a slot freed in the reorder buffer
  std::condition_variable arrived; // a result landed
  std::map<std::size_t, ParallelResult<Payload>> ready;
  std::size_t next_dispatch = 0;
  std::size_t next_publish = 0;
  std::uint64_t buffered_bytes = 0;
  std::size_t in_flight = 0;
  bool stopping = false;
  ParallelRunStatus status = ParallelRunStatus::none;

  const std::size_t workers =
      std::max<std::size_t>(std::min(plan.workers, count == 0 ? 1 : count), 1);

  auto worker_body = [&](std::size_t worker_index) {
    double idle = 0.0;
    double active = 0.0;
    while (true) {
      std::size_t rank = 0;
      {
        std::unique_lock lock(mutex);
        if (stopping || next_dispatch >= count) {
          break;
        }
        rank = next_dispatch++;
        ++in_flight;
        metrics.items_dispatched = std::max(metrics.items_dispatched, rank + 1);
        metrics.peak_reserved_bytes = std::max(
            metrics.peak_reserved_bytes, static_cast<std::uint64_t>(in_flight) *
                                             plan.reserved_bytes_per_worker);
      }

      const auto extract_started = Clock::now();
      ParallelResult<Payload> result;
      try {
        result = extract(worker_index, rank);
      } catch (const std::exception &error) {
        result = ParallelResult<Payload>{};
        result.error = error.what();
      } catch (...) {
        result = ParallelResult<Payload>{};
        result.error = "unknown extraction failure";
      }
      active +=
          std::chrono::duration<double>(Clock::now() - extract_started).count();

      const auto wait_started = Clock::now();
      {
        std::unique_lock lock(mutex);
        // Backpressure. The rank the publisher is waiting for is ALWAYS
        // admitted: holding it back to respect a byte budget would deadlock the
        // run, because nothing can drain until it lands.
        admit.wait(lock, [&] {
          if (stopping || rank == next_publish) {
            return true;
          }
          return ready.size() < plan.max_queue_items &&
                 buffered_bytes + result.bytes <= plan.max_queue_bytes;
        });
        const double waited =
            std::chrono::duration<double>(Clock::now() - wait_started).count();
        metrics.total_backpressure_seconds += waited;
        idle += waited;
        --in_flight;
        if (stopping) {
          // The run is unwinding; release any ordered gate this rank holds.
          lock.unlock();
          on_abandon(rank);
          break;
        }
        buffered_bytes += result.bytes;
        ready.emplace(rank, std::move(result));
        metrics.peak_reorder_items =
            std::max(metrics.peak_reorder_items, ready.size());
        metrics.peak_reorder_bytes =
            std::max(metrics.peak_reorder_bytes, buffered_bytes);
      }
      arrived.notify_all();
    }
    const std::scoped_lock lock(mutex);
    metrics.total_worker_active_seconds += active;
    metrics.total_worker_idle_seconds += idle;
  };

  std::vector<std::thread> pool;
  pool.reserve(workers);
  for (std::size_t i = 0; i < workers; ++i) {
    pool.emplace_back(worker_body, i);
  }
  metrics.workers_started = workers;

  // Stops and joins the pool on EVERY exit from the publisher scope below,
  // including an exception thrown by the caller-supplied publish callback.
  // Without it that unwind would cross joinable std::threads, which is
  // std::terminate rather than a propagated exception.
  struct PoolGuard {
    std::vector<std::thread> &pool;
    std::mutex &mutex;
    std::condition_variable &admit;
    std::condition_variable &arrived;
    bool &stopping;

    // Deliberately an aggregate holding only references: it is a scope-exit
    // action, never copied or moved.
    ~PoolGuard() {
      {
        const std::scoped_lock lock(mutex);
        stopping = true;
      }
      admit.notify_all();
      arrived.notify_all();
      for (std::thread &worker : pool) {
        if (worker.joinable()) {
          worker.join();
        }
      }
    }
  };

  {
    const PoolGuard pool_guard{pool, mutex, admit, arrived, stopping};

    // The calling thread is the publisher: exactly one writer, always in rank
    // order, never on a worker.
    //
    // `next_publish` is shared state -- a worker's admission test reads it to
    // decide whether it is the rank the publisher is blocked on -- so every
    // mutation happens under the mutex and is followed by a notify. Advancing
    // it outside the lock, or notifying before advancing, loses the wakeup and
    // deadlocks a full buffer.
    while (true) {
      ParallelResult<Payload> result;
      std::size_t rank = 0;
      {
        std::unique_lock lock(mutex);
        if (next_publish >= count) {
          break;
        }
        rank = next_publish;
        const auto wait_started = Clock::now();
        arrived.wait(lock, [&] { return stopping || ready.contains(rank); });
        metrics.total_publish_wait_seconds +=
            std::chrono::duration<double>(Clock::now() - wait_started).count();
        if (stopping) {
          break;
        }
        auto entry = ready.find(rank);
        result = std::move(entry->second);
        ready.erase(entry);
        buffered_bytes -= result.bytes;
      }
      admit.notify_all();

      if (cancelled()) {
        {
          const std::scoped_lock lock(mutex);
          stopping = true;
        }
        status = ParallelRunStatus::cancelled;
        on_abandon(rank);
        break;
      }
      const bool keep_going = publish(rank, result);
      if (keep_going) {
        // A publication that STOPS the run is not a published item: counting it
        // would overstate progress in exactly the failure report an operator
        // reads to find out how far a run got.
        ++metrics.items_published;
      }
      {
        const std::scoped_lock lock(mutex);
        next_publish = rank + 1;
        if (!keep_going) {
          stopping = true;
          status = ParallelRunStatus::publication_stopped;
        }
      }
      admit.notify_all();
      if (!keep_going) {
        break;
      }
    }

  } // PoolGuard: every worker has stopped and joined past this point.

  // Anything extracted but never published must release its ordered gate too.
  for (auto &[rank, unpublished] : ready) {
    static_cast<void>(unpublished);
    on_abandon(rank);
  }
  ready.clear();

  metrics.wall_seconds =
      std::chrono::duration<double>(Clock::now() - run_started).count();
  return status;
}

} // namespace cidx::index
