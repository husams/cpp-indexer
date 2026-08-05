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

#include <string>

namespace cidx::util {

// fsync a regular file. False when it cannot be opened or flushed.
[[nodiscard]] auto sync_file(const std::string &path) -> bool;

// fsync a directory, which is what makes a rename within it durable.
[[nodiscard]] auto sync_directory(const std::string &path) -> bool;

// Publish `source` as `destination`: fsync the source, fsync the destination's
// directory, rename, then fsync that directory again. Both paths must be on the
// same filesystem for the rename to be atomic.
//
// Returns an empty string on success, or a human-readable failure reason. On
// failure `source` is left where it is and `destination` is untouched.
[[nodiscard]] auto publish_file_atomically(const std::string &source,
                                           const std::string &destination)
    -> std::string;

// The current process id, for naming process-private temporary files.
[[nodiscard]] auto current_process_id() -> long;

} // namespace cidx::util
