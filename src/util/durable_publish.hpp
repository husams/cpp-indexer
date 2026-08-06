// Durable, atomic file publication.
//
// Making a freshly written file visible under its final name safely needs three
// things in order: the file's own bytes on stable storage, the directory entry
// that will name it on stable storage, and a rename that the kernel performs
// atomically. Callers either observe the whole previous file or the whole new
// one — never a partial file.
//
// The POSIX calls that do this (fsync, rename, getpid) are process-level
// facilities, so they live in the foundation utilities rather than in the
// product surfaces that consume them.
#pragma once

#include <functional>
#include <string>

namespace cidx::util {

// fsync a regular file. False when it cannot be opened or flushed.
[[nodiscard]] auto sync_file(const std::string &path) -> bool;

// fsync a directory, which is what makes a rename within it durable.
[[nodiscard]] auto sync_directory(const std::string &path) -> bool;

// Deterministic entry into the publication window. Null on every production
// path. A qualification suite uses it to act at the two instants that bracket
// the rename — the only instants at which an interruption could conceivably
// tear a published file — instead of racing them from a wall clock.
struct PublishHooks {
  // After the source and the directory are on stable storage, before rename(2).
  std::function<void()> before_rename;
  // After rename(2) returns, before the closing directory fsync.
  std::function<void()> after_rename;
};

// Deliver SIGINT to this process with the default disposition, i.e. terminate
// now. Lives here because raising a signal is a process-level facility, and it
// keeps the product surfaces free of signal handling.
[[noreturn]] void terminate_by_interrupt();

// Publish `source` as `destination`: fsync the source, fsync the destination's
// directory, rename, then fsync that directory again. Both paths must be on the
// same filesystem for the rename to be atomic.
//
// Returns an empty string on success, or a human-readable failure reason. On
// failure `source` is left where it is and `destination` is untouched.
[[nodiscard]] auto publish_file_atomically(const std::string &source,
                                           const std::string &destination,
                                           const PublishHooks &hooks = {})
    -> std::string;

// The current process id, for naming process-private temporary files.
[[nodiscard]] auto current_process_id() -> long;

} // namespace cidx::util
