#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cidx::ast {

enum class FactIdentityKind : std::uint8_t {
  symbol,
  relation,
  type,
  definition,
  file,
};

struct IdentityRun {
  std::filesystem::path path;
  std::uint64_t entry_count = 0;
  std::uint64_t byte_size = 0;
  std::string first_key;
  std::string last_key;
  std::array<std::byte, 32> sha256{};
};

enum class IdentityInsertResult : std::uint8_t {
  inserted,
  existing,
  conflict,
};

struct SpillableIdentityIndexOptions {
  std::uint64_t max_resident_identity_bytes = 32ULL * 1024ULL * 1024ULL;
  std::uint64_t max_identity_entries = 1'000'000ULL;
  std::size_t max_identity_runs = 8;
  std::uint64_t max_total_bytes = 512ULL * 1024ULL * 1024ULL;
  std::filesystem::path spill_directory;
};

class SpillableIdentityIndex final {
public:
  explicit SpillableIdentityIndex(SpillableIdentityIndexOptions options = {});
  ~SpillableIdentityIndex();

  SpillableIdentityIndex(const SpillableIdentityIndex &) = delete;
  auto operator=(const SpillableIdentityIndex &)
      -> SpillableIdentityIndex & = delete;
  SpillableIdentityIndex(SpillableIdentityIndex &&) noexcept;
  auto operator=(SpillableIdentityIndex &&) noexcept
      -> SpillableIdentityIndex &;

  [[nodiscard]] auto lookup(FactIdentityKind kind, std::string_view key) const
      -> std::optional<std::int64_t>;
  auto insert(FactIdentityKind kind, std::string key, std::int64_t handle)
      -> IdentityInsertResult;
  [[nodiscard]] auto resident_bytes() const -> std::uint64_t;
  [[nodiscard]] auto entry_count() const -> std::uint64_t;
  [[nodiscard]] auto spilled() const -> bool;
  [[nodiscard]] auto runs() const -> const std::vector<IdentityRun> &;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cidx::ast
