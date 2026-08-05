#include "util/durable_publish.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
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

auto publish_file_atomically(const std::string &source,
                             const std::string &destination) -> std::string {
  // Durability before visibility: the bytes must survive a crash before any
  // reader can reach them under the published name.
  if (!sync_file(source)) {
    return "cannot flush " + source + ": " + std::strerror(errno);
  }
  const std::string directory = parent_directory(destination);
  // A directory fsync can legitimately fail on some filesystems; it is a
  // durability hardening step, not a correctness precondition for the rename.
  (void)sync_directory(directory);

  if (std::rename(source.c_str(), destination.c_str()) != 0) {
    return "cannot publish " + source + " over " + destination + ": " +
           std::strerror(errno);
  }
  (void)sync_directory(directory);
  return {};
}

auto current_process_id() -> long { return static_cast<long>(::getpid()); }

} // namespace cidx::util
