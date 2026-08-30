#include "ast/spillable_fact_buffer.hpp"

#include "util/hashing.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

namespace cidx::ast {
namespace {

constexpr std::array<char, 8> kMagic = {'C', 'I', 'D', 'X', 'S', 'E', 'G', '1'};

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

void write_u64(std::ostream &out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out.put(static_cast<char>((value >> shift) & 0xffU));
  }
}

auto read_u64(std::istream &in) -> std::uint64_t {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64; shift += 8) {
    const int byte = in.get();
    if (byte == std::char_traits<char>::eof()) {
      throw std::runtime_error("fact spill segment is truncated");
    }
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(byte))
             << shift;
  }
  return value;
}

auto unique_directory(const std::filesystem::path &base)
    -> std::filesystem::path {
  const auto parent =
      base.empty() ? std::filesystem::temp_directory_path() : base;
  std::filesystem::create_directories(parent);
  for (std::uint64_t attempt = 0; attempt != 1000; ++attempt) {
    const auto candidate =
        parent /
        ("cidx-facts-" +
         std::to_string(static_cast<std::uint64_t>(
             std::chrono::steady_clock::now().time_since_epoch().count())) +
         "-" + std::to_string(attempt));
    std::error_code error;
    if (std::filesystem::create_directory(candidate, error)) {
      return candidate;
    }
  }
  throw std::runtime_error("unable to create a private fact spill directory");
}

template <typename Record>
void write_segment_records(
    std::ostream &output, const std::vector<Record> &records,
    const typename SpillableFactBuffer<Record>::Encoder &encoder) {
  for (const Record &record : records) {
    const std::vector<std::byte> encoded = encoder(record);
    write_u64(output, encoded.size());
    output.write(reinterpret_cast<const char *>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
  }
}

} // namespace

template <typename Record> struct SpillableFactBuffer<Record>::Impl {
  SpillableFactBufferOptions options;
  Encoder encoder;
  Decoder decoder;
  std::vector<Record> resident;
  std::vector<SpillSegment> segments;
  std::uint64_t next_index = 0;
  std::uint64_t resident_size = 0;
  std::uint64_t total_size = 0;
  std::filesystem::path directory;
  std::shared_ptr<void> cleanup;

  Impl(SpillableFactBufferOptions value, Encoder encode, Decoder decode)
      : options(std::move(value)), encoder(std::move(encode)),
        decoder(std::move(decode)) {}

  ~Impl() = default;

  void spill() {
    if (resident.empty()) {
      return;
    }
    if (!encoder || !decoder) {
      throw std::runtime_error("fact spill buffer has no record codec");
    }
    if (directory.empty()) {
      directory = unique_directory(options.spill_directory);
      cleanup = std::shared_ptr<void>(
          new std::filesystem::path(directory), [](void *value) noexcept {
            auto *path = static_cast<std::filesystem::path *>(value);
            std::error_code ignored;
            std::filesystem::remove_all(*path, ignored);
            delete path;
          });
    }
    const auto path =
        directory / ("segment-" + std::to_string(segments.size()) + ".bin");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("unable to create fact spill segment");
    }
    output.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    write_u64(output, static_cast<std::uint64_t>(options.family));
    write_u64(output, next_index - resident.size());
    write_u64(output, resident.size());
    write_segment_records(output, resident, encoder);
    output.close();
    const auto size = std::filesystem::file_size(path);
    const std::uint64_t resident_bytes = resident_size;
    const std::uint64_t prior_bytes = total_size - resident_bytes;
    if (size > options.max_total_bytes ||
        prior_bytes > options.max_total_bytes - size) {
      std::filesystem::remove(path);
      throw std::runtime_error("fact spill hard byte limit exceeded");
    }
    segments.push_back(
        {.path = path,
         .family = options.family,
         .first_index = next_index - resident.size(),
         .record_count = resident.size(),
         .byte_size = size,
         .sha256 = digest_bytes(cidx::sha256_of(path.string()).value_or(""))});
    resident.clear();
    resident.shrink_to_fit();
    resident_size = 0;
    total_size = prior_bytes + size;
  }
};

template <typename Record>
SpillableFactBuffer<Record>::SpillableFactBuffer(
    SpillableFactBufferOptions options, Encoder encoder, Decoder decoder)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(encoder),
                                   std::move(decoder))) {}

template <typename Record>
SpillableFactBuffer<Record>::~SpillableFactBuffer() = default;

template <typename Record>
SpillableFactBuffer<Record>::SpillableFactBuffer(
    SpillableFactBuffer &&) noexcept = default;

template <typename Record>
auto SpillableFactBuffer<Record>::operator=(SpillableFactBuffer &&) noexcept
    -> SpillableFactBuffer & = default;

template <typename Record>
auto SpillableFactBuffer<Record>::append(Record record) -> std::uint64_t {
  if (!impl_->encoder) {
    throw std::runtime_error("fact spill buffer has no record encoder");
  }
  const std::uint64_t encoded_size = impl_->encoder(record).size();
  constexpr std::uint64_t frame_size = sizeof(std::uint64_t);
  if (encoded_size > impl_->options.max_total_bytes ||
      encoded_size + frame_size > impl_->options.max_total_bytes ||
      impl_->total_size >
          impl_->options.max_total_bytes - encoded_size - frame_size) {
    throw std::runtime_error("fact payload hard byte limit exceeded");
  }
  const std::uint64_t index = impl_->next_index++;
  impl_->resident_size += encoded_size + frame_size;
  impl_->total_size += encoded_size + frame_size;
  impl_->resident.push_back(std::move(record));
  if (impl_->resident_size >= impl_->options.spill_threshold_bytes) {
    impl_->spill();
  }
  return index;
}

template <typename Record>
auto SpillableFactBuffer<Record>::size() const -> std::uint64_t {
  return impl_->next_index;
}

template <typename Record>
auto SpillableFactBuffer<Record>::resident_bytes() const -> std::uint64_t {
  return impl_->resident_size;
}

template <typename Record>
auto SpillableFactBuffer<Record>::spilled() const -> bool {
  return !impl_->segments.empty();
}

template <typename Record>
auto SpillableFactBuffer<Record>::segments() const
    -> const std::vector<SpillSegment> & {
  return impl_->segments;
}

template <typename Record>
void SpillableFactBuffer<Record>::for_each_in_order(
    const Consumer &consumer) const {
  for (const SpillSegment &segment : impl_->segments) {
    std::ifstream input(segment.path, std::ios::binary);
    if (!input) {
      throw std::runtime_error("fact spill segment is missing");
    }
    std::array<char, kMagic.size()> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (magic != kMagic) {
      throw std::runtime_error(
          "fact spill segment has an invalid wire version");
    }
    static_cast<void>(read_u64(input));
    const std::uint64_t first = read_u64(input);
    const std::uint64_t count = read_u64(input);
    for (std::uint64_t offset = 0; offset < count; ++offset) {
      const std::uint64_t size = read_u64(input);
      if (size > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("fact spill record is too large");
      }
      std::vector<std::byte> bytes(static_cast<std::size_t>(size));
      input.read(reinterpret_cast<char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
      if (!input) {
        throw std::runtime_error("fact spill segment is truncated");
      }
      consumer(first + offset, impl_->decoder(bytes));
    }
  }
  const std::uint64_t first = impl_->next_index - impl_->resident.size();
  for (std::size_t offset = 0; offset < impl_->resident.size(); ++offset) {
    consumer(first + offset, impl_->resident[offset]);
  }
}

template <typename Record>
auto SpillableFactBuffer<Record>::freeze() && -> FrozenFactRecords<Record> {
  if (impl_->segments.empty()) {
    return InMemoryRecords<Record>{.records = std::move(impl_->resident)};
  }
  SpilledRecords<Record> result{.records = std::move(impl_->resident),
                                .segments = std::move(impl_->segments),
                                .cleanup = std::move(impl_->cleanup)};
  return result;
}

template class SpillableFactBuffer<std::string>;

} // namespace cidx::ast
