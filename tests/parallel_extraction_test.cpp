// S-074: the ordering, bounding and amortisation contracts of bounded parallel
// translation-unit extraction, proven without Clang or a database.
//
// The point of these tests is that they are hostile to the mechanism: worker
// completion order is deliberately randomised and inverted, budgets are set to
// pathological values, and workers are made to fail mid-run. What must survive
// is the legacy publication order, the exactly-once shared-header assignment,
// and the absence of a deadlock.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "index/header_claims.hpp"
#include "index/parallel_policy.hpp"
#include "index/parallel_runner.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace cidx::index;

namespace {

auto default_plan(std::size_t workers, std::size_t items = 64,
                  std::uint64_t bytes = 1U << 20U) -> ParallelPlan {
  ParallelPlan plan;
  plan.workers = workers;
  plan.max_queue_items = items;
  plan.max_queue_bytes = bytes;
  plan.reserved_bytes_per_worker = 1024;
  return plan;
}

auto always_running() -> std::function<bool()> {
  return [] { return false; };
}

auto ignore_abandon() -> std::function<void(std::size_t)> {
  return [](std::size_t) {};
}

} // namespace

TEST_CASE("publication follows dispatch order under randomised completion") {
  constexpr std::size_t kCount = 200;
  // A deterministic per-rank delay whose order is deliberately the REVERSE of
  // the dispatch order, so a runner that published on completion would produce
  // an almost exactly inverted sequence.
  std::vector<int> delay_microseconds(kCount);
  for (std::size_t i = 0; i < kCount; ++i) {
    delay_microseconds[i] = static_cast<int>((kCount - i) % 37) * 40;
  }

  std::vector<std::size_t> published;
  ParallelRunMetrics metrics;
  const ParallelRunStatus status = run_parallel_extraction<std::size_t>(
      default_plan(8), kCount,
      [&](std::size_t, std::size_t rank) {
        std::this_thread::sleep_for(
            std::chrono::microseconds(delay_microseconds[rank]));
        return ParallelResult<std::size_t>{.payload = rank, .bytes = 16};
      },
      [&](std::size_t rank, ParallelResult<std::size_t> &result) {
        CHECK(result.payload == rank);
        published.push_back(rank);
        return true;
      },
      always_running(), ignore_abandon(), metrics);

  CHECK(status == ParallelRunStatus::none);
  REQUIRE(published.size() == kCount);
  std::vector<std::size_t> expected(kCount);
  std::iota(expected.begin(), expected.end(), 0U);
  CHECK(published == expected);
  CHECK(metrics.items_published == kCount);
  CHECK(metrics.workers_started == 8);
}

TEST_CASE("randomised completion order over many seeds never reorders "
          "publication") {
  for (unsigned seed = 1; seed <= 12; ++seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> jitter(0, 800);
    std::vector<int> delays(80);
    for (int &value : delays) {
      value = jitter(rng);
    }
    std::vector<std::size_t> published;
    ParallelRunMetrics metrics;
    const ParallelRunStatus status = run_parallel_extraction<std::size_t>(
        default_plan(6), delays.size(),
        [&](std::size_t, std::size_t rank) {
          std::this_thread::sleep_for(std::chrono::microseconds(delays[rank]));
          return ParallelResult<std::size_t>{.payload = rank, .bytes = 8};
        },
        [&](std::size_t rank, ParallelResult<std::size_t> &) {
          published.push_back(rank);
          return true;
        },
        always_running(), ignore_abandon(), metrics);
    CHECK(status == ParallelRunStatus::none);
    REQUIRE(published.size() == delays.size());
    for (std::size_t i = 0; i < published.size(); ++i) {
      CHECK(published[i] == i);
    }
  }
}

TEST_CASE("a one-item byte budget still completes and bounds the buffer") {
  // Pathological budget: the buffer admits nothing beyond the rank the
  // publisher is waiting for. A runner that honoured the budget unconditionally
  // would deadlock here.
  ParallelPlan plan = default_plan(4);
  plan.max_queue_items = 1;
  plan.max_queue_bytes = 1;

  std::vector<std::size_t> published;
  ParallelRunMetrics metrics;
  const ParallelRunStatus status = run_parallel_extraction<std::size_t>(
      plan, 50,
      [&](std::size_t, std::size_t rank) {
        return ParallelResult<std::size_t>{.payload = rank, .bytes = 4096};
      },
      [&](std::size_t rank, ParallelResult<std::size_t> &) {
        published.push_back(rank);
        return true;
      },
      always_running(), ignore_abandon(), metrics);

  CHECK(status == ParallelRunStatus::none);
  REQUIRE(published.size() == 50);
  for (std::size_t i = 0; i < published.size(); ++i) {
    CHECK(published[i] == i);
  }
  // Only the publisher's own rank may exceed the budget.
  CHECK(metrics.peak_reorder_items <= 1);
}

TEST_CASE("a publication failure stops the run and abandons the remainder") {
  std::vector<std::size_t> published;
  std::mutex abandoned_mutex;
  std::vector<std::size_t> abandoned;
  ParallelRunMetrics metrics;

  const ParallelRunStatus status = run_parallel_extraction<std::size_t>(
      default_plan(4), 100,
      [&](std::size_t, std::size_t rank) {
        return ParallelResult<std::size_t>{.payload = rank, .bytes = 32};
      },
      [&](std::size_t rank, ParallelResult<std::size_t> &) {
        published.push_back(rank);
        return rank < 10; // writer failure at rank 10
      },
      always_running(),
      [&](std::size_t rank) {
        const std::scoped_lock lock(abandoned_mutex);
        abandoned.push_back(rank);
      },
      metrics);

  CHECK(status == ParallelRunStatus::publication_stopped);
  REQUIRE(published.size() == 11);
  CHECK(published.back() == 10);
  // Every dispatched-but-unpublished rank released its ordered gate, so nothing
  // downstream can stall waiting for it.
  const std::scoped_lock lock(abandoned_mutex);
  CHECK(abandoned.size() == metrics.items_dispatched - published.size());
}

TEST_CASE("an extraction that throws is reported, not swallowed") {
  std::vector<std::string> errors;
  ParallelRunMetrics metrics;
  const ParallelRunStatus status = run_parallel_extraction<std::size_t>(
      default_plan(3), 12,
      [&](std::size_t, std::size_t rank) -> ParallelResult<std::size_t> {
        if (rank == 7) {
          throw std::runtime_error("worker crashed on rank 7");
        }
        return ParallelResult<std::size_t>{.payload = rank, .bytes = 8};
      },
      [&](std::size_t, ParallelResult<std::size_t> &result) {
        if (result.error) {
          errors.push_back(*result.error);
        }
        return true;
      },
      always_running(), ignore_abandon(), metrics);

  CHECK(status == ParallelRunStatus::none);
  REQUIRE(errors.size() == 1);
  CHECK(errors.front() == "worker crashed on rank 7");
  CHECK(metrics.items_published == 12);
}

TEST_CASE("cancellation stops dispatch and publication") {
  std::atomic_bool cancel{false};
  std::vector<std::size_t> published;
  ParallelRunMetrics metrics;
  const ParallelRunStatus status = run_parallel_extraction<std::size_t>(
      default_plan(4), 500,
      [&](std::size_t, std::size_t rank) {
        return ParallelResult<std::size_t>{.payload = rank, .bytes = 8};
      },
      [&](std::size_t rank, ParallelResult<std::size_t> &) {
        published.push_back(rank);
        if (rank == 20) {
          cancel.store(true);
        }
        return true;
      },
      [&] { return cancel.load(); }, ignore_abandon(), metrics);

  CHECK(status == ParallelRunStatus::cancelled);
  CHECK(published.size() <= 22);
  CHECK(metrics.items_published < 500);
}

// -- header claim oracle -----------------------------------------------------

TEST_CASE("a shared header is granted to exactly one translation unit") {
  SequencedHeaderClaimOracle oracle;
  constexpr std::size_t kUnits = 200;
  std::atomic<int> owners{0};
  std::vector<std::thread> threads;
  threads.reserve(kUnits);
  // Every translation unit asks for the SAME header, and they ask from
  // arbitrary threads. Only the lowest rank may own it.
  std::atomic<std::size_t> owning_rank{kUnits + 1};
  for (std::size_t rank = 0; rank < kUnits; ++rank) {
    threads.emplace_back([&, rank] {
      const std::vector<HeaderClaimCandidate> candidates{
          {.path = "/repo/include/shared.hpp",
           .parsed_md5 = std::string("digest"),
           .already_indexed_in_database = false}};
      const std::vector<bool> owned = oracle.claim(rank, "config-a", candidates);
      if (owned.at(0)) {
        ++owners;
        owning_rank.store(rank);
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  CHECK(owners.load() == 1);
  CHECK(owning_rank.load() == 0); // legacy order, not completion order
  const HeaderClaimMetrics metrics = oracle.metrics();
  CHECK(metrics.granted == 1);
  CHECK(metrics.denied_in_flight_owner == kUnits - 1);
  CHECK(metrics.candidates == kUnits);
}

TEST_CASE("amortisation reproduces the serial indexed/already split") {
  // One translation unit introduces two headers; 999 more include them. The
  // serial engine reports "2 indexed, 999 already" -- the aggregate the
  // acceptance criterion names.
  SequencedHeaderClaimOracle oracle;
  constexpr std::size_t kUnits = 1000;
  std::atomic<int> indexed{0};
  std::atomic<int> already{0};
  std::vector<std::thread> threads;
  threads.reserve(kUnits);
  for (std::size_t rank = 0; rank < kUnits; ++rank) {
    threads.emplace_back([&, rank] {
      const std::vector<HeaderClaimCandidate> candidates{
          {.path = "/repo/include/a.hpp",
           .parsed_md5 = std::string("a"),
           .already_indexed_in_database = false},
          {.path = "/repo/include/b.hpp",
           .parsed_md5 = std::string("b"),
           .already_indexed_in_database = false}};
      const std::vector<bool> owned = oracle.claim(rank, "config-a", candidates);
      for (const bool own : owned) {
        if (own) {
          ++indexed;
        } else {
          ++already;
        }
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  CHECK(indexed.load() == 2);
  CHECK(already.load() == 2 * static_cast<int>(kUnits) - 2);
}

TEST_CASE("distinct configuration hashes each own the same header") {
  SequencedHeaderClaimOracle oracle;
  const std::vector<HeaderClaimCandidate> candidates{
      {.path = "/repo/include/shared.hpp",
       .parsed_md5 = std::string("digest"),
       .already_indexed_in_database = false}};
  CHECK(oracle.claim(0, "config-a", candidates).at(0));
  // A different normalized configuration is a different file_config row, so the
  // serial engine re-indexes; so must the oracle.
  CHECK(oracle.claim(1, "config-b", candidates).at(0));
  CHECK_FALSE(oracle.claim(2, "config-a", candidates).at(0));
}

TEST_CASE("a header already current in the database is never granted") {
  SequencedHeaderClaimOracle oracle;
  const std::vector<HeaderClaimCandidate> candidates{
      {.path = "/repo/include/warm.hpp",
       .parsed_md5 = std::string("digest"),
       .already_indexed_in_database = true}};
  CHECK_FALSE(oracle.claim(0, "config-a", candidates).at(0));
  const HeaderClaimMetrics metrics = oracle.metrics();
  CHECK(metrics.denied_already_indexed == 1);
  CHECK(metrics.granted == 0);
}

TEST_CASE("changed header content re-grants rather than reporting current") {
  SequencedHeaderClaimOracle oracle;
  CHECK(oracle
            .claim(0, "config-a",
                   {{.path = "/repo/h.hpp",
                     .parsed_md5 = std::string("old"),
                     .already_indexed_in_database = false}})
            .at(0));
  // A translation unit that parsed different bytes for the same path must not
  // be told the header is current.
  CHECK(oracle
            .claim(1, "config-a",
                   {{.path = "/repo/h.hpp",
                     .parsed_md5 = std::string("new"),
                     .already_indexed_in_database = false}})
            .at(0));
}

TEST_CASE("an abandoned rank never stalls its successors") {
  SequencedHeaderClaimOracle oracle;
  std::atomic_bool finished{false};
  std::thread later([&] {
    static_cast<void>(oracle.claim(
        3, "config-a",
        {{.path = "/repo/late.hpp",
          .parsed_md5 = std::string("d"),
          .already_indexed_in_database = false}}));
    finished.store(true);
  });
  // Ranks 0..2 never claim -- a failed parse, a crashed worker and a cache
  // replay. Releasing them must let rank 3 through.
  oracle.release_unclaimed(0);
  oracle.release_unclaimed(2); // out of order on purpose
  oracle.release_unclaimed(1);
  later.join();
  CHECK(finished.load());
}

TEST_CASE("cancellation releases every waiter without granting") {
  SequencedHeaderClaimOracle oracle;
  std::atomic_bool released{false};
  std::atomic_bool owned_anything{false};
  std::thread waiter([&] {
    const std::vector<bool> owned = oracle.claim(
        9, "config-a",
        {{.path = "/repo/x.hpp",
          .parsed_md5 = std::string("d"),
          .already_indexed_in_database = false}});
    owned_anything.store(owned.at(0));
    released.store(true);
  });
  oracle.cancel();
  waiter.join();
  CHECK(released.load());
  CHECK_FALSE(owned_anything.load());
}

// -- worker/budget policy ----------------------------------------------------

TEST_CASE("explicit --jobs is honoured but never exceeds the work available") {
  const HostResources host{.logical_cores = 16,
                           .total_memory_bytes = 64ULL << 30U,
                           .available_memory_bytes = 64ULL << 30U};
  CHECK(plan_parallel_extraction({.jobs = 4}, host, 100).workers == 4);
  CHECK(plan_parallel_extraction({.jobs = 32}, host, 100).workers == 32);
  CHECK(plan_parallel_extraction({.jobs = 32}, host, 3).workers == 3);
  CHECK(plan_parallel_extraction({.jobs = 1}, host, 100).workers == 1);
}

TEST_CASE("automatic selection is bounded by cores, work and memory") {
  const HostResources big{.logical_cores = 10,
                          .total_memory_bytes = 128ULL << 30U,
                          .available_memory_bytes = 128ULL << 30U};
  CHECK(plan_parallel_extraction({}, big, 1000).workers == 10);
  CHECK(plan_parallel_extraction({}, big, 3).workers == 3);

  // A small-memory host must not start one worker per core.
  const HostResources small{.logical_cores = 16,
                            .total_memory_bytes = 4ULL << 30U,
                            .available_memory_bytes = 4ULL << 30U};
  const ParallelPlan plan = plan_parallel_extraction({}, small, 1000);
  CHECK(plan.workers < 16);
  CHECK(plan.workers >= 1);
  CHECK(plan.selection_reason == "automatic (memory-bound)");
}

TEST_CASE("a hostile budget still yields a runnable plan") {
  const HostResources none{
      .logical_cores = 0, .total_memory_bytes = 0, .available_memory_bytes = 0};
  const ParallelPlan plan = plan_parallel_extraction({}, none, 0);
  CHECK(plan.workers >= 1);
  CHECK(plan.max_queue_items >= 1);
  CHECK(plan.max_queue_bytes > 0);

  const ParallelPlan explicit_zero_budget = plan_parallel_extraction(
      {.jobs = 8, .max_queue_bytes = 1, .max_queue_items = 1}, none, 10);
  CHECK(explicit_zero_budget.max_queue_items == 1);
  CHECK(explicit_zero_budget.max_queue_bytes == 1);
}

TEST_CASE("an explicit memory budget overrides the host probe") {
  const HostResources host{.logical_cores = 64,
                           .total_memory_bytes = 512ULL << 30U,
                           .available_memory_bytes = 512ULL << 30U};
  const ParallelPlan plan =
      plan_parallel_extraction({.memory_budget_bytes = 2ULL << 30U}, host, 1000);
  CHECK(plan.memory_budget_bytes == (2ULL << 30U));
  CHECK(plan.workers <= 3); // 2 GiB / 768 MiB
  CHECK(plan.workers >= 1);
}
