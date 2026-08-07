// A worker thread with an explicitly requested stack size (B-006).
//
// `std::thread` offers no way to ask for a stack, so a worker gets the
// platform's pthread default: 512 KiB on macOS, 8 MiB on glibc. That is not a
// budget the extraction callback can live within. In production the callback
// runs a complete Clang front-end invocation, and the front end's recursion
// during template instantiation and C++20 constraint satisfaction is unbounded
// in the input: on macOS a template-heavy translation unit walks off the
// worker's guard page and the process dies with SIGBUS, with no diagnostic and
// no report.
//
// Clang never relies on the ambient stack for this either. `clang -cc1` and
// every LibTooling entry point run the front end through
// `clang::CompilerInstance::ExecuteAction` /`runWithSufficientStackSpace`,
// which switches to a stack of `clang::DesiredStackSize` before recursing. The
// serial indexer inherits an equivalent budget for free because it parses on
// the process main thread. A worker must ask.
//
// POSIX rather than `llvm::thread` (which also takes a stack size) is
// deliberate: `foundation.scheduling` is declared in the module manifest as
// Clang-free and SQLite-free, and its `allowedDependencies` list contains
// neither `llvm` nor `clang`. Pulling an `llvm/` include in here to save thirty
// lines would break the very contract this module exists to keep.
#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <system_error>

#include <pthread.h>

namespace cidx::index {

// The stack every extraction worker is created with.
//
// 8 MiB is `clang::DesiredStackSize` -- what the Clang front end budgets for
// its own recursion via llvm::CrashRecoveryContext::RunSafelyOnNewStack -- and
// it is also the main-thread stack the serial indexing path already relies on.
// Matching it makes the two paths equally capable by construction: any input
// the serial path can parse, a worker can parse too. The cost is address space,
// not resident memory; the pages are committed lazily.
inline constexpr std::size_t kWorkerStackBytes = 8U << 20U;

namespace detail {

// Test-only seam, empty in production. `WorkerThread` calls it with the worker
// index immediately before creating the thread, so a test can make a specific
// spawn fail deterministically. Provoking a real `pthread_create` failure
// depends on the host's thread limit and on how many workers happen to fit, and
// a non-deterministic test of a cleanup path proves little.
//
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline std::function<void(std::size_t)> before_worker_spawn;

// What the trampoline owns while the thread runs. Heap-allocated because
// `pthread_create` carries a single `void *` and the creating scope may exit
// before the worker starts.
struct WorkerEntry {
  std::function<void(std::size_t)> body;
  std::size_t index = 0;
};

extern "C" inline auto worker_trampoline(void *argument) -> void * {
  const std::unique_ptr<WorkerEntry> entry(
      static_cast<WorkerEntry *>(argument));
  try {
    entry->body(entry->index);
  } catch (...) {
    // `std::thread`'s contract, kept deliberately: an exception that escapes
    // the thread function terminates. Spelled out rather than left to the
    // unwinder, because unwinding out of a pthread entry point is not portable.
    std::terminate();
  }
  return nullptr;
}

// Destroys a `pthread_attr_t` however the constructor below leaves its scope.
class AttrGuard {
public:
  explicit AttrGuard(pthread_attr_t &attr) noexcept : attr_(&attr) {}
  AttrGuard(const AttrGuard &) = delete;
  AttrGuard(AttrGuard &&) = delete;
  auto operator=(const AttrGuard &) -> AttrGuard & = delete;
  auto operator=(AttrGuard &&) -> AttrGuard & = delete;
  ~AttrGuard() { pthread_attr_destroy(attr_); }

private:
  pthread_attr_t *attr_;
};

} // namespace detail

// A joinable thread created with `kWorkerStackBytes` of stack.
//
// The surface is exactly what the dispatch runner's pool needs -- move,
// `joinable()`, `join()` -- and the semantics follow `std::thread`: a joinable
// thread that is destroyed terminates, an exception that escapes the thread
// function terminates, and a creation failure is reported as
// `std::system_error` rather than silently leaving the caller short-handed.
// `join()` is the one deliberate departure; see the note on it.
class WorkerThread {
public:
  WorkerThread() noexcept = default;

  // Runs `body(index)` on a new thread. Throws `std::system_error` if the
  // thread cannot be created.
  WorkerThread(const std::function<void(std::size_t)> &body,
               std::size_t index) {
    if (detail::before_worker_spawn) {
      detail::before_worker_spawn(index);
    }

    pthread_attr_t attr{};
    if (const int rc = pthread_attr_init(&attr); rc != 0) {
      throw std::system_error(rc, std::generic_category(), "pthread_attr_init");
    }
    const detail::AttrGuard attr_guard(attr);
    if (const int rc = pthread_attr_setstacksize(&attr, kWorkerStackBytes);
        rc != 0) {
      throw std::system_error(rc, std::generic_category(),
                              "pthread_attr_setstacksize");
    }

    auto entry = std::make_unique<detail::WorkerEntry>(body, index);
    pthread_t handle{};
    if (const int rc = pthread_create(&handle, &attr,
                                      &detail::worker_trampoline, entry.get());
        rc != 0) {
      throw std::system_error(rc, std::generic_category(), "pthread_create");
    }
    // The thread owns the entry from here; the trampoline deletes it.
    [[maybe_unused]] const detail::WorkerEntry *owned_by_thread =
        entry.release();
    handle_ = handle;
    started_ = true;
  }

  WorkerThread(const WorkerThread &) = delete;
  auto operator=(const WorkerThread &) -> WorkerThread & = delete;

  WorkerThread(WorkerThread &&other) noexcept
      : handle_(other.handle_), started_(other.started_) {
    other.started_ = false;
  }

  auto operator=(WorkerThread &&other) noexcept -> WorkerThread & {
    if (this != &other) {
      if (started_) {
        std::terminate();
      }
      handle_ = other.handle_;
      started_ = other.started_;
      other.started_ = false;
    }
    return *this;
  }

  ~WorkerThread() {
    if (started_) {
      std::terminate();
    }
  }

  [[nodiscard]] auto joinable() const noexcept -> bool { return started_; }

  // Deliberately `noexcept`, which is where this departs from `std::thread`.
  // The only caller is the runner's scope-exit pool guard, and a destructor
  // that can throw is both a `bugprone-exception-escape` finding and a real
  // hazard during an unwind. There is nothing to recover to either: every
  // `pthread_join` failure (EINVAL, ESRCH, EDEADLK) means the worker was never
  // joined, so the run state it still references is about to go out of scope.
  // Terminating says that plainly instead of continuing over a live thread.
  void join() noexcept {
    if (!started_) {
      std::terminate();
    }
    started_ = false;
    if (pthread_join(handle_, nullptr) != 0) {
      std::terminate();
    }
  }

private:
  pthread_t handle_{};
  bool started_ = false;
};

} // namespace cidx::index
