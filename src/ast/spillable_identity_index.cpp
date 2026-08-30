#include "ast/spillable_identity_index.hpp"

#include "util/hashing.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace cidx::ast {
namespace {

constexpr std::array<char, 8> kMagic = {'C', 'I', 'D', 'X', 'I', 'D', 'X', '1'};

auto digest_bytes(const std::string &digest) -> std::array<std::byte, 32> {
  std::array<std::byte, 32> result{};
  if (!digest.starts_with("sha256:") || digest.size() != 71) {
    return result;
  }
  for (std::size_t index = 0; index < result.size(); ++index) {
    const auto nibble = [](char value) -> unsigned {
      if (value >= '0' && value <= '9') {
        return static_cast<unsigned>(value - '0');
      }
      if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned>(value - 'a' + 10);
      }
      return 0;
    };
    result[index] =
        static_cast<std::byte>((nibble(digest[7 + (index * 2)]) << 4U) |
                               nibble(digest[8 + (index * 2)]));
  }
  return result;
}

auto encoded_key(FactIdentityKind kind, std::string_view key) -> std::string {
  std::string result;
  result.reserve(key.size() + 2);
  result.push_back(static_cast<char>(kind));
  result.push_back('\0');
  result.append(key);
  return result;
}

void write_u64(std::ostream &out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out.put(static_cast<char>((value >> shift) & 0xffU));
  }
}

void write_i64(std::ostream &out, std::int64_t value) {
  write_u64(out, static_cast<std::uint64_t>(value));
}

auto read_u64(std::istream &in) -> std::uint64_t {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64; shift += 8) {
    const int byte = in.get();
    if (byte == std::char_traits<char>::eof()) {
      throw std::runtime_error("identity run is truncated");
    }
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(byte))
             << shift;
  }
  return value;
}

auto read_i64(std::istream &in) -> std::int64_t {
  return static_cast<std::int64_t>(read_u64(in));
}

auto private_directory(const std::filesystem::path &base)
    -> std::filesystem::path {
  const auto parent =
      base.empty() ? std::filesystem::temp_directory_path() : base;
  std::filesystem::create_directories(parent);
  const auto stamp = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  for (std::uint64_t attempt = 0; attempt < 1000; ++attempt) {
    const auto result = parent / ("cidx-identities-" + std::to_string(stamp) +
                                  "-" + std::to_string(attempt));
    std::error_code error;
    if (std::filesystem::create_directory(result, error)) {
      return result;
    }
  }
  throw std::runtime_error("unable to create a private identity run directory");
}

using IdentityEntries =
    std::map<std::pair<FactIdentityKind, std::string>, std::int64_t>;

void write_identity_entries(std::ostream &output,
                            const IdentityEntries &entries, std::string &first,
                            std::string &last) {
  for (const auto &[identity, handle] : entries) {
    const auto key = encoded_key(identity.first, identity.second);
    if (first.empty()) {
      first = key;
    }
    last = key;
    write_u64(output, static_cast<std::uint64_t>(identity.first));
    write_u64(output, identity.second.size());
    output.write(identity.second.data(),
                 static_cast<std::streamsize>(identity.second.size()));
    write_i64(output, handle);
  }
}

void read_identity_entries(
    const IdentityRun &run,
    const std::function<void(FactIdentityKind, std::string, std::int64_t)>
        &consumer) {
  std::ifstream input(run.path, std::ios::binary);
  std::array<char, kMagic.size()> magic{};
  input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  if (!input || magic != kMagic) {
    throw std::runtime_error("identity run is corrupt during compaction");
  }
  const auto count = read_u64(input);
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto kind = static_cast<FactIdentityKind>(read_u64(input));
    const auto length = read_u64(input);
    std::string key(static_cast<std::size_t>(length), '\0');
    input.read(key.data(), static_cast<std::streamsize>(length));
    consumer(kind, std::move(key), read_i64(input));
  }
}

} // namespace

struct SpillableIdentityIndex::Impl {
  SpillableIdentityIndexOptions options;
  std::map<std::pair<FactIdentityKind, std::string>, std::int64_t> resident;
  std::vector<IdentityRun> runs;
  std::filesystem::path directory;
  std::uint64_t resident_size = 0;
  std::uint64_t total_size = 0;
  std::uint64_t total_entries = 0;
  std::uint64_t compaction_serial = 0;

  explicit Impl(SpillableIdentityIndexOptions value)
      : options(std::move(value)) {}

  ~Impl() {
    if (!directory.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }
  }

  [[nodiscard]] static auto find_in_run(const IdentityRun &run,
                                        FactIdentityKind kind,
                                        std::string_view key)
      -> std::optional<std::int64_t> {
    const std::string target = encoded_key(kind, key);
    if ((!run.first_key.empty() && target < run.first_key) ||
        (!run.last_key.empty() && target > run.last_key)) {
      return std::nullopt;
    }
    std::ifstream input(run.path, std::ios::binary);
    if (!input) {
      throw std::runtime_error("identity run is missing");
    }
    std::array<char, kMagic.size()> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (magic != kMagic) {
      throw std::runtime_error("identity run has an invalid wire version");
    }
    const auto count = read_u64(input);
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto stored_kind = static_cast<FactIdentityKind>(read_u64(input));
      const auto length = read_u64(input);
      std::string stored(static_cast<std::size_t>(length), '\0');
      input.read(stored.data(), static_cast<std::streamsize>(length));
      const auto handle = read_i64(input);
      if (stored_kind == kind && stored == key) {
        return handle;
      }
    }
    return std::nullopt;
  }

  void flush() {
    if (resident.empty()) {
      return;
    }
    if (directory.empty()) {
      directory =
          private_directory(std::filesystem::path(options.spill_directory));
    }
    const auto path =
        directory / ("run-" + std::to_string(runs.size()) + ".bin");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("unable to create identity run");
    }
    output.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    write_u64(output, resident.size());
    std::string first;
    std::string last;
    write_identity_entries(output, resident, first, last);
    output.close();
    const auto bytes = std::filesystem::file_size(path);
    if (total_size > options.max_total_bytes - bytes) {
      std::filesystem::remove(path);
      throw std::runtime_error("identity spill hard byte limit exceeded");
    }
    IdentityRun run{.path = path.string(),
                    .entry_count = resident.size(),
                    .byte_size = bytes,
                    .first_key = std::move(first),
                    .last_key = std::move(last)};
    const auto digest = cidx::sha256_of(path.string());
    if (!digest) {
      std::filesystem::remove(path);
      throw std::runtime_error("unable to checksum identity run");
    }
    run.sha256 = digest_bytes(*digest);
    runs.push_back(std::move(run));
    total_size += bytes;
    resident.clear();
    resident_size = 0;
    if (runs.size() > options.max_identity_runs) {
      compact();
    }
  }

  void compact() {
    std::map<std::pair<FactIdentityKind, std::string>, std::int64_t> merged;
    for (const IdentityRun &run : runs) {
      read_identity_entries(run,
                            [&merged](FactIdentityKind kind, std::string key,
                                      std::int64_t handle) {
                              merged[{kind, std::move(key)}] = handle;
                            });
    }
    // Do not reuse the current compacted path: it may already be one of the
    // runs being replaced, and removing the old runs would remove the new
    // file as well.
    const auto path =
        directory /
        ("run-compacted-" + std::to_string(compaction_serial++) + ".bin");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    write_u64(output, merged.size());
    std::string first;
    std::string last;
    write_identity_entries(output, merged, first, last);
    output.close();
    const auto bytes = std::filesystem::file_size(path);
    const auto digest = cidx::sha256_of(path.string());
    if (!digest || bytes > options.max_total_bytes) {
      std::filesystem::remove(path);
      throw std::runtime_error("identity compaction exceeded its hard limit");
    }
    for (const IdentityRun &run : runs) {
      std::error_code ignored;
      std::filesystem::remove(run.path, ignored);
    }
    total_size = bytes;
    runs.clear();
    runs.push_back({.path = path.string(),
                    .entry_count = merged.size(),
                    .byte_size = bytes,
                    .first_key = std::move(first),
                    .last_key = std::move(last),
                    .sha256 = digest_bytes(*digest)});
  }
};

SpillableIdentityIndex::SpillableIdentityIndex(
    SpillableIdentityIndexOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

SpillableIdentityIndex::~SpillableIdentityIndex() = default;
SpillableIdentityIndex::SpillableIdentityIndex(
    SpillableIdentityIndex &&) noexcept = default;
auto SpillableIdentityIndex::operator=(SpillableIdentityIndex &&) noexcept
    -> SpillableIdentityIndex & = default;

auto SpillableIdentityIndex::lookup(FactIdentityKind kind,
                                    std::string_view key) const
    -> std::optional<std::int64_t> {
  const auto found = impl_->resident.find({kind, std::string(key)});
  if (found != impl_->resident.end()) {
    return found->second;
  }
  for (const IdentityRun &run : std::ranges::reverse_view(impl_->runs)) {
    if (const auto result = Impl::find_in_run(run, kind, key)) {
      return result;
    }
  }
  return std::nullopt;
}

auto SpillableIdentityIndex::insert(FactIdentityKind kind, std::string key,
                                    std::int64_t handle)
    -> IdentityInsertResult {
  if (const auto existing = lookup(kind, key)) {
    return *existing == handle ? IdentityInsertResult::existing
                               : IdentityInsertResult::conflict;
  }
  const std::uint64_t cost = key.size() + sizeof(handle) + 32;
  if (cost > impl_->options.max_resident_identity_bytes ||
      impl_->resident_size >
          impl_->options.max_resident_identity_bytes - cost ||
      impl_->resident.size() >= impl_->options.max_identity_entries) {
    impl_->flush();
  }
  if (cost > impl_->options.max_resident_identity_bytes ||
      impl_->resident_size >
          impl_->options.max_resident_identity_bytes - cost ||
      impl_->resident.size() >= impl_->options.max_identity_entries) {
    throw std::runtime_error(
        "identity resident limit is too small for one entry");
  }
  impl_->resident.emplace(std::make_pair(kind, std::move(key)), handle);
  impl_->resident_size += cost;
  ++impl_->total_entries;
  return IdentityInsertResult::inserted;
}

auto SpillableIdentityIndex::resident_bytes() const -> std::uint64_t {
  return impl_->resident_size;
}

auto SpillableIdentityIndex::entry_count() const -> std::uint64_t {
  return impl_->total_entries;
}

auto SpillableIdentityIndex::spilled() const -> bool {
  return !impl_->runs.empty();
}

auto SpillableIdentityIndex::runs() const -> const std::vector<IdentityRun> & {
  return impl_->runs;
}

auto SpillableIdentityIndex::entries() const
    -> std::map<std::pair<FactIdentityKind, std::string>, std::int64_t> {
  std::map<std::pair<FactIdentityKind, std::string>, std::int64_t> result;
  for (const IdentityRun &run : impl_->runs) {
    read_identity_entries(run, [&result](FactIdentityKind kind, std::string key,
                                         std::int64_t handle) {
      result.insert_or_assign({kind, std::move(key)}, handle);
    });
  }
  for (const auto &[identity, handle] : impl_->resident) {
    result.insert_or_assign(identity, handle);
  }
  return result;
}

} // namespace cidx::ast
