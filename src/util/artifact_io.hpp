// Bounded byte artifact storage with memory/file-neutral immutable reads.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cidx::util {

enum class ArtifactIoErrorCode : std::uint8_t {
  missing,
  short_read,
  io_error,
  spill_failure,
  limit_exceeded,
};

class ArtifactIoError final : public std::runtime_error {
public:
  ArtifactIoError(ArtifactIoErrorCode code, const std::string &message);
  [[nodiscard]] auto code() const noexcept -> ArtifactIoErrorCode;

private:
  ArtifactIoErrorCode code_;
};

struct SpoolingByteSinkOptions {
  std::size_t spill_threshold_bytes = std::size_t{8} * 1024U * 1024U;
  std::string spill_directory;
  std::uint64_t max_bytes = 512ULL * 1024ULL * 1024ULL;
};

class ImmutableByteArtifact {
public:
  ImmutableByteArtifact() = default;

  [[nodiscard]] static auto from_bytes(std::vector<std::byte> bytes)
      -> ImmutableByteArtifact;
  [[nodiscard]] static auto from_file(std::string_view path)
      -> ImmutableByteArtifact;

  [[nodiscard]] auto valid() const -> bool;
  [[nodiscard]] auto size() const -> std::uint64_t;
  [[nodiscard]] auto spilled() const -> bool;
  [[nodiscard]] auto peak_memory_bytes() const -> std::size_t;
  void read_exact(std::uint64_t offset, std::span<std::byte> destination) const;
  [[nodiscard]] auto digest_prefix(std::uint64_t byte_count) const
      -> std::string;
  void write_to(std::ostream &output) const;
  void materialize(std::string_view path) const;

private:
  struct Storage;
  explicit ImmutableByteArtifact(std::shared_ptr<const Storage> storage);
  std::shared_ptr<const Storage> storage_;

  friend class SpoolingByteSink;
};

class SpoolingByteSink {
public:
  explicit SpoolingByteSink(const SpoolingByteSinkOptions &options);
  ~SpoolingByteSink();

  SpoolingByteSink(const SpoolingByteSink &) = delete;
  auto operator=(const SpoolingByteSink &) -> SpoolingByteSink & = delete;
  SpoolingByteSink(SpoolingByteSink &&) noexcept;
  auto operator=(SpoolingByteSink &&) noexcept -> SpoolingByteSink &;

  void append(std::span<const std::byte> bytes);
  [[nodiscard]] auto size() const -> std::uint64_t;
  [[nodiscard]] auto digest_prefix() -> std::string;
  [[nodiscard]] auto finish() -> ImmutableByteArtifact;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cidx::util
