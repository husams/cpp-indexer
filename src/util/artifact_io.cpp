#include "util/artifact_io.hpp"

#include "util/hashing.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace cidx::util {

namespace {

[[noreturn]] void fail(ArtifactIoErrorCode code, const std::string &message) {
  throw ArtifactIoError(code, message);
}

void read_from_descriptor(int descriptor, std::uint64_t extent,
                          std::uint64_t offset,
                          std::span<std::byte> destination) {
  if (offset > extent || destination.size() > extent - offset) {
    fail(ArtifactIoErrorCode::short_read,
         "byte artifact read exceeds its captured extent");
  }
  std::size_t read = 0;
  while (read < destination.size()) {
    const auto count =
        ::pread(descriptor, destination.data() + read,
                destination.size() - read, static_cast<off_t>(offset + read));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      fail(ArtifactIoErrorCode::io_error,
           "failed while reading the byte artifact backing file");
    }
    if (count == 0) {
      fail(ArtifactIoErrorCode::short_read,
           "byte artifact backing file became shorter");
    }
    read += static_cast<std::size_t>(count);
  }
}

} // namespace

ArtifactIoError::ArtifactIoError(ArtifactIoErrorCode code,
                                 const std::string &message)
    : std::runtime_error(message), code_(code) {}

auto ArtifactIoError::code() const noexcept -> ArtifactIoErrorCode {
  return code_;
}

struct ImmutableByteArtifact::Storage {
  ~Storage() {
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    if (remove_file && !path.empty()) {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  }

  std::vector<std::byte> bytes;
  std::filesystem::path path;
  int descriptor = -1;
  std::uint64_t size = 0;
  std::size_t peak_memory = 0;
  bool remove_file = false;
  bool was_spilled = false;
};

struct SpoolingByteSink::Impl {
  explicit Impl(const SpoolingByteSinkOptions &options)
      : threshold(options.spill_threshold_bytes), max_size(options.max_bytes),
        directory(options.spill_directory.empty()
                      ? std::filesystem::temp_directory_path()
                      : std::filesystem::path(options.spill_directory)) {
    memory.reserve(std::min<std::uint64_t>(
        {threshold, max_size, std::uint64_t{64} * 1024U}));
    peak_memory = memory.capacity();
  }

  ~Impl() {
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    if (!transferred && !path.empty()) {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  }

  void write_all(std::span<const std::byte> bytes) const {
    std::size_t written = 0;
    while (written < bytes.size()) {
      const auto count =
          ::write(descriptor, bytes.data() + written, bytes.size() - written);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        fail(ArtifactIoErrorCode::spill_failure,
             "failed while writing the byte artifact spill file");
      }
      written += static_cast<std::size_t>(count);
    }
  }

  void flush_buffer() {
    if (!write_buffer.empty()) {
      write_all(write_buffer);
      write_buffer.clear();
    }
  }

  void append_spilled(std::span<const std::byte> bytes) {
    if (write_buffer.capacity() == 0) {
      write_all(bytes);
      return;
    }
    if (bytes.size() >= write_buffer.capacity()) {
      flush_buffer();
      write_all(bytes);
      return;
    }
    std::size_t consumed = 0;
    while (consumed < bytes.size()) {
      const auto available = write_buffer.capacity() - write_buffer.size();
      const auto count = std::min(available, bytes.size() - consumed);
      const auto chunk = bytes.subspan(consumed, count);
      write_buffer.insert(write_buffer.end(), chunk.begin(), chunk.end());
      consumed += count;
      if (write_buffer.size() == write_buffer.capacity()) {
        flush_buffer();
      }
    }
  }

  void ensure_spill() {
    if (descriptor >= 0) {
      return;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
      fail(ArtifactIoErrorCode::spill_failure,
           "byte artifact spill directory is unavailable: " +
               directory.string());
    }
    std::string pattern = (directory / "cidx-byte-artifact-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    descriptor = ::mkstemp(writable.data());
    if (descriptor < 0) {
      fail(ArtifactIoErrorCode::spill_failure,
           "cannot create a byte artifact spill file in " + directory.string());
    }
    path = writable.data();
    if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
      fail(ArtifactIoErrorCode::spill_failure,
           "cannot secure the byte artifact spill descriptor");
    }
    if (::unlink(path.c_str()) != 0) {
      fail(ArtifactIoErrorCode::spill_failure,
           "cannot unlink the byte artifact spill file");
    }
    path.clear();
    if (!memory.empty()) {
      write_all(memory);
    }
    std::vector<std::byte>().swap(memory);
    write_buffer.reserve(
        std::min<std::size_t>(threshold, std::size_t{64} * 1024U));
    peak_memory = std::max(peak_memory, write_buffer.capacity());
  }

  std::size_t threshold;
  std::uint64_t max_size;
  std::filesystem::path directory;
  std::vector<std::byte> memory;
  std::vector<std::byte> write_buffer;
  std::filesystem::path path;
  int descriptor = -1;
  std::uint64_t byte_size = 0;
  std::size_t peak_memory = 0;
  bool transferred = false;
};

ImmutableByteArtifact::ImmutableByteArtifact(
    std::shared_ptr<const Storage> storage)
    : storage_(std::move(storage)) {}

auto ImmutableByteArtifact::from_bytes(std::vector<std::byte> bytes)
    -> ImmutableByteArtifact {
  auto storage = std::make_shared<Storage>();
  storage->size = bytes.size();
  storage->peak_memory = bytes.size();
  storage->bytes = std::move(bytes);
  return ImmutableByteArtifact(std::move(storage));
}

auto ImmutableByteArtifact::from_file(std::string_view path_value)
    -> ImmutableByteArtifact {
  const std::filesystem::path path(path_value);
  auto storage = std::make_shared<Storage>();
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    const auto code = errno == ENOENT || errno == ENOTDIR
                          ? ArtifactIoErrorCode::missing
                          : ArtifactIoErrorCode::io_error;
    fail(code, "cannot open byte artifact file: " + path.string());
  }
  storage->descriptor = descriptor;
  struct stat file_stat{};
  if (::fstat(descriptor, &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
      file_stat.st_size < 0) {
    fail(ArtifactIoErrorCode::io_error,
         "cannot determine byte artifact size: " + path.string());
  }
  storage->path = path;
  storage->size = static_cast<std::uint64_t>(file_stat.st_size);
  return ImmutableByteArtifact(std::move(storage));
}

auto ImmutableByteArtifact::valid() const -> bool {
  return storage_ != nullptr;
}
auto ImmutableByteArtifact::size() const -> std::uint64_t {
  return storage_ ? storage_->size : 0;
}
auto ImmutableByteArtifact::spilled() const -> bool {
  return storage_ && storage_->was_spilled;
}
auto ImmutableByteArtifact::peak_memory_bytes() const -> std::size_t {
  return storage_ ? storage_->peak_memory : 0;
}

void ImmutableByteArtifact::read_exact(std::uint64_t offset,
                                       std::span<std::byte> destination) const {
  if (!storage_) {
    fail(ArtifactIoErrorCode::missing, "byte artifact handle is empty");
  }
  if (offset > storage_->size || destination.size() > storage_->size - offset) {
    fail(ArtifactIoErrorCode::short_read,
         "byte artifact read exceeds its captured extent");
  }
  if (storage_->descriptor >= 0) {
    read_from_descriptor(storage_->descriptor, storage_->size, offset,
                         destination);
  } else {
    std::memcpy(destination.data(),
                storage_->bytes.data() + static_cast<std::size_t>(offset),
                destination.size());
  }
}

auto ImmutableByteArtifact::digest_prefix(std::uint64_t byte_count) const
    -> std::string {
  if (!storage_ || byte_count > storage_->size) {
    fail(ArtifactIoErrorCode::short_read,
         "byte artifact digest exceeds its captured extent");
  }
  if (storage_->descriptor >= 0) {
    const auto digest = sha256_of_fd_prefix(storage_->descriptor, byte_count);
    if (!digest) {
      fail(ArtifactIoErrorCode::io_error,
           "cannot hash the byte artifact payload");
    }
    return *digest;
  }
  return sha256_hex(
      std::span<const std::byte>(storage_->bytes).first(byte_count));
}

void ImmutableByteArtifact::write_to(std::ostream &output) const {
  if (!storage_) {
    fail(ArtifactIoErrorCode::missing, "byte artifact handle is empty");
  }
  std::array<std::byte, std::size_t{64} * 1024U> buffer{};
  std::uint64_t offset = 0;
  while (offset < storage_->size) {
    const auto request = static_cast<std::size_t>(
        std::min<std::uint64_t>(storage_->size - offset, buffer.size()));
    read_exact(offset, std::span(buffer).first(request));
    output.write(reinterpret_cast<const char *>(buffer.data()),
                 static_cast<std::streamsize>(request));
    if (!output) {
      fail(ArtifactIoErrorCode::io_error,
           "failed while writing the byte artifact transfer");
    }
    offset += request;
  }
}

void ImmutableByteArtifact::materialize(std::string_view path_value) const {
  const std::filesystem::path path(path_value);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    fail(ArtifactIoErrorCode::io_error,
         "cannot open the byte artifact destination: " + path.string());
  }
  write_to(output);
  output.close();
  if (!output) {
    fail(ArtifactIoErrorCode::io_error,
         "cannot finalize the byte artifact destination: " + path.string());
  }
}

SpoolingByteSink::SpoolingByteSink(const SpoolingByteSinkOptions &options)
    : impl_(std::make_unique<Impl>(options)) {}
SpoolingByteSink::~SpoolingByteSink() = default;
SpoolingByteSink::SpoolingByteSink(SpoolingByteSink &&) noexcept = default;
auto SpoolingByteSink::operator=(SpoolingByteSink &&) noexcept
    -> SpoolingByteSink & = default;

void SpoolingByteSink::append(std::span<const std::byte> bytes) {
  if (bytes.size() >
          std::numeric_limits<std::uint64_t>::max() - impl_->byte_size ||
      impl_->byte_size + bytes.size() > impl_->max_size) {
    fail(ArtifactIoErrorCode::limit_exceeded,
         "byte artifact exceeds the configured encode limit");
  }
  if (impl_->descriptor < 0 &&
      bytes.size() <= impl_->threshold - impl_->memory.size()) {
    const auto required = impl_->memory.size() + bytes.size();
    if (required > impl_->memory.capacity()) {
      const auto memory_ceiling = static_cast<std::size_t>(
          std::min<std::uint64_t>(impl_->threshold, impl_->max_size));
      const auto current = impl_->memory.capacity();
      const auto doubled =
          current > memory_ceiling / 2U ? memory_ceiling : current * 2U;
      impl_->memory.reserve(
          std::min(memory_ceiling, std::max(required, doubled)));
    }
    impl_->memory.insert(impl_->memory.end(), bytes.begin(), bytes.end());
    impl_->peak_memory = std::max(impl_->peak_memory, impl_->memory.capacity());
  } else {
    impl_->ensure_spill();
    impl_->append_spilled(bytes);
  }
  impl_->byte_size += bytes.size();
}

auto SpoolingByteSink::size() const -> std::uint64_t {
  return impl_->byte_size;
}

auto SpoolingByteSink::digest_prefix() -> std::string {
  if (impl_->descriptor < 0) {
    return sha256_hex(std::span<const std::byte>(impl_->memory));
  }
  impl_->flush_buffer();
  const auto digest = sha256_of_fd_prefix(impl_->descriptor, impl_->byte_size);
  if (!digest) {
    fail(ArtifactIoErrorCode::spill_failure,
         "cannot hash the byte artifact spill file");
  }
  return *digest;
}

auto SpoolingByteSink::finish() -> ImmutableByteArtifact {
  auto storage = std::make_shared<ImmutableByteArtifact::Storage>();
  if (impl_->descriptor >= 0) {
    impl_->flush_buffer();
    storage->path = std::move(impl_->path);
    storage->descriptor = impl_->descriptor;
    storage->remove_file = true;
    storage->was_spilled = true;
    impl_->descriptor = -1;
  } else {
    impl_->memory.shrink_to_fit();
    storage->bytes = std::move(impl_->memory);
  }
  storage->size = impl_->byte_size;
  storage->peak_memory = impl_->peak_memory;
  impl_->transferred = true;
  return ImmutableByteArtifact(std::move(storage));
}

} // namespace cidx::util
