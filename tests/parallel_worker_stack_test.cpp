// B-006 regression test: an extraction worker must get a stack large enough
// for the Clang front end's recursion depth, and a failure to create a worker
// must be reported rather than aborting the process.
//
// This test deliberately does NOT use Clang. `foundation.scheduling` is
// declared Clang-free and SQLite-free in the module manifest, and the defect is
// a property of the thread the runner creates, not of Clang: any callee that
// recurses past the worker's stack limit dies the same way. Driving it with a
// controlled recursion makes the test fast, hermetic, and exact about the
// threshold it asserts.
//
// Expected behaviour of this file:
//   * pre-fix, macOS (512 KiB default pthread stack): dies with SIGBUS inside
//     `deep_recursion` on a worker thread; ctest reports the crash, not a
//     failed CHECK.
//   * pre-fix, glibc/Linux (8 MiB default): the recursion probe PASSES. That
//     platform asymmetry is part of what this test documents -- it is why CI on
//     Linux would not have caught B-006, and why the fix must request the size
//     rather than inherit it. The stack-size case below is the part that fails
//     on Linux too before the fix, because nothing requests 8 MiB there either;
//     it merely happens to be the glibc default.
//   * post-fix: passes on both.
//
// Built next to parallel_extraction_test in tests/CMakeLists.txt with the
// `default` label only (no Clang, no database, no fixtures).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "index/parallel_runner.hpp"
#include "index/worker_thread.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <system_error>

#include <pthread.h>

using namespace cidx::index;

namespace {

// Clang's own budget for front-end recursion is `clang::DesiredStackSize`,
// 8 MiB, applied through llvm::CrashRecoveryContext::RunSafelyOnNewStack. A
// worker that runs the front end needs at least that. This probe goes well past
// the 512 KiB pthread default but stays comfortably inside 8 MiB, so the test
// proves the defect without becoming a brittle test of the exact ceiling.
//
// The probe is a BEHAVIOURAL backstop only. It cannot establish the budget --
// it establishes "more than 4 MiB" -- so the exact guarantee in AC 4239 is
// asserted separately, against the requested size and the worker's real stack.
constexpr std::size_t kFrameBytes = 4096;
constexpr std::size_t kProbeBytes = 4U << 20U; // 4 MiB of live stack
constexpr std::size_t kProbeFrames = kProbeBytes / kFrameBytes;

// `volatile` and the accumulated return value stop the optimiser from turning
// this into a loop or eliding the frame: the point of the test is that the
// frames are really on the stack.
auto deep_recursion(std::size_t depth) -> std::uint64_t {
  std::array<unsigned char, kFrameBytes> frame{};
  volatile unsigned char *const cells = frame.data();
  cells[0] = static_cast<unsigned char>(depth & 0xFFU);
  cells[kFrameBytes - 1] = static_cast<unsigned char>(depth & 0xFFU);
  if (depth == 0) {
    return cells[0];
  }
  return static_cast<std::uint64_t>(cells[kFrameBytes - 1]) +
         deep_recursion(depth - 1);
}

// The stack the CALLING thread actually has, as the platform reports it. Used
// from inside a worker to read that worker's own stack.
//
// macOS has `pthread_get_stacksize_np`; glibc reports it through the thread's
// attributes. Both are documented, non-intrusive queries -- neither changes the
// thread. If a future platform has neither, the case below degrades to the
// static assertion on the requested constant rather than silently passing.
auto self_stack_bytes() -> std::size_t {
#if defined(__APPLE__)
  return pthread_get_stacksize_np(pthread_self());
#elif defined(__GLIBC__)
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) != 0) {
    return 0;
  }
  std::size_t size = 0;
  const int rc = pthread_attr_getstacksize(&attr, &size);
  pthread_attr_destroy(&attr);
  return rc == 0 ? size : 0;
#else
  return 0;
#endif
}

auto probe_plan(std::size_t workers) -> ParallelPlan {
  ParallelPlan plan;
  plan.workers = workers;
  plan.max_queue_items = 4;
  plan.max_queue_bytes = 1U << 20U;
  plan.reserved_bytes_per_worker = 1024;
  return plan;
}

} // namespace

// The discriminating pair. The first case establishes that the recursion depth
// itself is tolerable -- the calling thread runs it fine, which is exactly why
// the serial indexer never hit this. The second runs the identical work inside
// `extract`, i.e. on a runner-created worker, which is the only thing that
// changes.
TEST_CASE("the calling thread tolerates front-end-scale recursion") {
  CHECK(deep_recursion(kProbeFrames) < UINT64_MAX);
}

TEST_CASE("a worker tolerates front-end-scale recursion") {
  constexpr std::size_t kUnits = 4;

  std::atomic<std::size_t> extracted{0};
  const std::function<ParallelResult<std::uint64_t>(std::size_t, std::size_t)>
      extract = [&extracted](std::size_t, std::size_t) {
        ParallelResult<std::uint64_t> result;
        result.payload = deep_recursion(kProbeFrames);
        result.bytes = 1;
        extracted.fetch_add(1, std::memory_order_relaxed);
        return result;
      };

  std::size_t published = 0;
  std::size_t expected_rank = 0;
  const std::function<bool(std::size_t, ParallelResult<std::uint64_t> &)>
      publish = [&](std::size_t rank, ParallelResult<std::uint64_t> &result) {
        CHECK(rank == expected_rank);
        CHECK(!result.error.has_value());
        ++expected_rank;
        ++published;
        return true;
      };

  ParallelRunMetrics metrics;
  const ParallelRunStatus status = run_parallel_extraction<std::uint64_t>(
      probe_plan(2), kUnits, extract, publish, [] { return false; },
      [](std::size_t) {}, metrics);

  CHECK(status == ParallelRunStatus::none);
  CHECK(published == kUnits);
  CHECK(extracted.load() == kUnits);
  // Proves the run really was parallel: a one-worker plan would take the
  // serial path and could not reproduce the defect.
  CHECK(metrics.workers_started == 2);
}

// AC 4239 is a statement about the SIZE, not about one probe depth. Asserting
// it needs the requested constant and the stack the platform actually handed
// the worker; a run that merely survives 4 MiB of frames would also be produced
// by a 5 MiB stack, which is not parity with `clang::DesiredStackSize`.
TEST_CASE("a worker's stack is the size the fix requests") {
  // The requested budget, checked at compile time so lowering the constant
  // fails the build rather than a run.
  static_assert(
      kWorkerStackBytes >= (8U << 20U),
      "workers must request at least clang::DesiredStackSize (8 MiB)");

  std::atomic<std::size_t> observed{0};
  const std::function<ParallelResult<std::uint64_t>(std::size_t, std::size_t)>
      extract = [&observed](std::size_t, std::size_t) {
        observed.store(self_stack_bytes(), std::memory_order_relaxed);
        ParallelResult<std::uint64_t> result;
        result.bytes = 1;
        return result;
      };
  const std::function<bool(std::size_t, ParallelResult<std::uint64_t> &)>
      publish =
          [](std::size_t, ParallelResult<std::uint64_t> &) { return true; };

  ParallelRunMetrics metrics;
  const ParallelRunStatus status = run_parallel_extraction<std::uint64_t>(
      probe_plan(2), 2, extract, publish, [] { return false; },
      [](std::size_t) {}, metrics);
  CHECK(status == ParallelRunStatus::none);

  const std::size_t worker_stack = observed.load(std::memory_order_relaxed);
  // 0 means the platform exposes no query. Skipping the runtime half is
  // honest; silently passing it would not be.
  if (worker_stack == 0) {
    MESSAGE("platform exposes no stack-size query; only the requested "
            "constant was verified");
  } else {
    // The exact guarantee: what the worker got is at least what Clang budgets,
    // and at least what this build asked for.
    CHECK(worker_stack >= (8U << 20U));
    CHECK(worker_stack >= kWorkerStackBytes);
  }
}

// Partial creation failure. `WorkerThread` reports a failed `pthread_create`
// as `std::system_error` instead of running silently short-handed -- but a
// throw from the Nth creation unwinds a pool that already holds N-1 joinable
// workers. Unless the runner's cleanup is established BEFORE the creation loop,
// that unwind destroys joinable threads and is `std::terminate`, i.e. the
// reported-failure claim is false in exactly the situation it exists for.
//
// `detail::before_worker_spawn` is a test-only seam: the runner calls it with
// the worker index immediately before each spawn, and it is empty in
// production. Provoking the failure through the real `pthread_create` is not
// deterministic (it depends on the host's thread limit and on how many workers
// happen to fit), and a non-deterministic test of a cleanup path is worth
// little.
TEST_CASE("a failed worker creation unwinds instead of terminating") {
  constexpr std::size_t kFailAt = 1; // worker 0 starts, worker 1 fails

  std::atomic<std::size_t> spawned{0};
  std::atomic<bool> extract_seen{false};

  detail::before_worker_spawn = [&spawned](std::size_t index) {
    if (index == kFailAt) {
      throw std::system_error(
          std::make_error_code(std::errc::resource_unavailable_try_again),
          "simulated pthread_create failure");
    }
    spawned.fetch_add(1, std::memory_order_relaxed);
  };
  // Reset even if a CHECK throws: the seam is process-global.
  struct Reset {
    ~Reset() { detail::before_worker_spawn = nullptr; }
  } reset;

  const std::function<ParallelResult<std::uint64_t>(std::size_t, std::size_t)>
      extract = [&extract_seen](std::size_t, std::size_t) {
        extract_seen.store(true, std::memory_order_relaxed);
        ParallelResult<std::uint64_t> result;
        result.bytes = 1;
        return result;
      };
  const std::function<bool(std::size_t, ParallelResult<std::uint64_t> &)>
      publish =
          [](std::size_t, ParallelResult<std::uint64_t> &) { return true; };

  ParallelRunMetrics metrics;

  // The observable requirement: the caller gets an exception it can report,
  // the process survives, and the already-started worker is stopped and
  // joined (if it were not, this test would deadlock or terminate rather than
  // fail).
  const auto run = [&] {
    // The status is deliberately discarded: this case is about the throw, and
    // `run_parallel_extraction` is [[nodiscard]].
    static_cast<void>(run_parallel_extraction<std::uint64_t>(
        probe_plan(4), 8, extract, publish, [] { return false; },
        [](std::size_t) {}, metrics));
  };
  CHECK_THROWS_AS(run(), std::system_error);

  CHECK(spawned.load() == kFailAt);
  // Only the workers that really started are counted; a plan of 4 that got 1
  // must not report 4.
  CHECK(metrics.workers_started == kFailAt);
}

// A worker that is handed a stack it cannot use must not take the whole process
// with it silently. Once the stack is requested explicitly this case documents
// the guarantee the fix provides: the depth Clang can reach on the main thread
// is reachable on a worker too, so parallel and serial indexing accept exactly
// the same inputs.
TEST_CASE("worker and calling-thread stack capacity agree") {
  std::uint64_t on_worker = 0;
  const std::function<ParallelResult<std::uint64_t>(std::size_t, std::size_t)>
      extract = [](std::size_t, std::size_t) {
        ParallelResult<std::uint64_t> result;
        result.payload = deep_recursion(kProbeFrames);
        result.bytes = 1;
        return result;
      };
  const std::function<bool(std::size_t, ParallelResult<std::uint64_t> &)>
      publish = [&on_worker](std::size_t, ParallelResult<std::uint64_t> &r) {
        on_worker = r.payload;
        return true;
      };

  ParallelRunMetrics metrics;
  const ParallelRunStatus status = run_parallel_extraction<std::uint64_t>(
      probe_plan(2), 2, extract, publish, [] { return false; },
      [](std::size_t) {}, metrics);

  CHECK(status == ParallelRunStatus::none);
  CHECK(on_worker == deep_recursion(kProbeFrames));
}
