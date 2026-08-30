#pragma once

#include "ast/fact_batch.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace cidx::ast {

struct SpillSegment {
  std::string path;
  FactFamily family = FactFamily::symbols;
  std::uint64_t first_index = 0;
  std::uint64_t record_count = 0;
  std::uint64_t byte_size = 0;
  std::array<std::byte, 32> sha256{};
};

struct SpillableFactBufferOptions {
  FactFamily family = FactFamily::symbols;
  std::uint64_t spill_threshold_bytes = 64ULL * 1024ULL * 1024ULL;
  std::uint64_t max_total_bytes = 512ULL * 1024ULL * 1024ULL;
  std::string spill_directory;
};

template <typename Record> struct InMemoryRecords {
  std::vector<Record> records;
};

template <typename Record> struct SpilledRecords {
  // Only records that remained resident at freeze time are materialized here;
  // spilled records stay in their segments and are consumed by iteration.
  std::vector<Record> records;
  std::vector<SpillSegment> segments;
  std::shared_ptr<void> cleanup;
};

template <typename Record>
using FrozenFactRecords =
    std::variant<InMemoryRecords<Record>, SpilledRecords<Record>>;

// Append-only payload storage. The codec is deliberately supplied by the
// owner of a record family: extraction.ast does not impose a wire format on
// FactRecords and can therefore keep this utility free of persistence code.
template <typename Record> class SpillableFactBuffer {
public:
  using Encoder = std::function<std::vector<std::byte>(const Record &)>;
  using Decoder = std::function<Record(std::span<const std::byte>)>;
  using Consumer = std::function<void(std::uint64_t, const Record &)>;

  SpillableFactBuffer(SpillableFactBufferOptions options, Encoder encoder,
                      Decoder decoder);
  ~SpillableFactBuffer();

  SpillableFactBuffer(const SpillableFactBuffer &) = delete;
  auto operator=(const SpillableFactBuffer &) -> SpillableFactBuffer & = delete;
  SpillableFactBuffer(SpillableFactBuffer &&) noexcept;
  auto operator=(SpillableFactBuffer &&) noexcept -> SpillableFactBuffer &;

  auto append(Record record) -> std::uint64_t;
  [[nodiscard]] auto size() const -> std::uint64_t;
  [[nodiscard]] auto resident_bytes() const -> std::uint64_t;
  [[nodiscard]] auto spilled() const -> bool;
  [[nodiscard]] auto segments() const -> const std::vector<SpillSegment> &;
  void for_each_in_order(const Consumer &consumer) const;
  [[nodiscard]] auto freeze() && -> FrozenFactRecords<Record>;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cidx::ast
