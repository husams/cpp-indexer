#include "util/durable_publish.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace cidx::util {
namespace {

auto sync_path(const std::string &path) -> bool {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  const int rc = ::fsync(fd);
  ::close(fd);
  return rc == 0;
}

auto parent_directory(const std::string &path) -> std::string {
  const std::filesystem::path parent =
      std::filesystem::path(path).parent_path();
  return parent.empty() ? std::string(".") : parent.string();
}

} // namespace

auto sync_file(const std::string &path) -> bool { return sync_path(path); }

auto sync_directory(const std::string &path) -> bool { return sync_path(path); }

void terminate_by_interrupt() {
  // Restore the default disposition first: the point is to die here, not to be
  // observed by whatever a caller may have installed.
  std::signal(SIGINT, SIG_DFL);
  std::raise(SIGINT);
  // Unreachable under the default disposition; present so the contract holds
  // even if SIGINT is blocked by an inherited mask.
  std::_Exit(EXIT_FAILURE);
}

auto publish_file_atomically(const std::string &source,
                             const std::string &destination,
                             const PublishHooks &hooks) -> std::string {
  // Durability before visibility: the bytes must survive a crash before any
  // reader can reach them under the published name.
  if (!sync_file(source)) {
    return "cannot flush " + source + ": " + std::strerror(errno);
  }
  const std::string directory = parent_directory(destination);
  // A directory fsync can legitimately fail on some filesystems; it is a
  // durability hardening step, not a correctness precondition for the rename.
  (void)sync_directory(directory);

  if (hooks.before_rename) {
    hooks.before_rename();
  }
  if (std::rename(source.c_str(), destination.c_str()) != 0) {
    return "cannot publish " + source + " over " + destination + ": " +
           std::strerror(errno);
  }
  if (hooks.after_rename) {
    hooks.after_rename();
  }
  (void)sync_directory(directory);
  return {};
}

auto current_process_id() -> long { return static_cast<long>(::getpid()); }

} // namespace cidx::util
