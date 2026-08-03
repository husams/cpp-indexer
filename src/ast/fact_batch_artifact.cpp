#include "ast/fact_batch_artifact.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <ranges>
#include <utility>

namespace cidx::ast {

namespace {

constexpr std::array<std::byte, 8> kMagic = {
    std::byte{'C'}, std::byte{'I'}, std::byte{'D'}, std::byte{'X'},
    std::byte{'F'}, std::byte{'B'}, std::byte{'0'}, std::byte{'1'}};
constexpr std::array<std::byte, 8> kDigestMagic = {
    std::byte{'F'}, std::byte{'B'}, std::byte{'D'}, std::byte{'I'},
    std::byte{'G'}, std::byte{'0'}, std::byte{'0'}, std::byte{'1'}};
constexpr std::size_t kDigestTextSize = 71;
constexpr std::size_t kFooterSize = kDigestMagic.size() + kDigestTextSize;

[[noreturn]] void fail(FactBatchArtifactErrorCode code,
                       const std::string &message) {
  throw FactBatchArtifactError(code, message);
}

auto canonical_dependencies(std::vector<std::string> dependencies)
    -> std::vector<std::string> {
  std::ranges::sort(dependencies);
  const auto duplicates = std::ranges::unique(dependencies);
  dependencies.erase(duplicates.begin(), duplicates.end());
  return dependencies;
}

class BinaryWriter {
public:
  explicit BinaryWriter(util::SpoolingByteSink &output) : output_(output) {}

  void bytes(std::span<const std::byte> value) { output_.append(value); }

  void u8(std::uint8_t value) {
    const std::array bytes = {static_cast<std::byte>(value)};
    output_.append(bytes);
  }

  void u32(std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
    output_.append(bytes);
  }

  void u64(std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
    output_.append(bytes);
  }

  void i64(std::int64_t value) { u64(std::bit_cast<std::uint64_t>(value)); }
  void boolean(bool value) { u8(value ? 1U : 0U); }

  void string(std::string_view value) {
    u64(value.size());
    output_.append(std::as_bytes(std::span(value.data(), value.size())));
  }

  template <typename T, typename Write>
  void optional(const std::optional<T> &value, Write write) {
    boolean(value.has_value());
    if (value) {
      write(*value);
    }
  }

  template <typename T, typename Write>
  void vector(const std::vector<T> &values, Write write) {
    u64(values.size());
    for (const T &value : values) {
      write(value);
    }
  }

private:
  util::SpoolingByteSink &output_;
};

} // namespace

auto to_string(FactBatchArtifactErrorCode code) -> std::string_view {
  switch (code) {
  case FactBatchArtifactErrorCode::missing:
    return "missing";
  case FactBatchArtifactErrorCode::stale:
    return "stale";
  case FactBatchArtifactErrorCode::corrupt:
    return "corrupt";
  case FactBatchArtifactErrorCode::incompatible:
    return "incompatible";
  case FactBatchArtifactErrorCode::partial:
    return "partial";
  case FactBatchArtifactErrorCode::truncated:
    return "truncated";
  case FactBatchArtifactErrorCode::limit_exceeded:
    return "limit_exceeded";
  case FactBatchArtifactErrorCode::io_error:
    return "io_error";
  case FactBatchArtifactErrorCode::spill_failure:
    return "spill_failure";
  case FactBatchArtifactErrorCode::noncanonical:
    return "noncanonical";
  }
  return "unknown";
}

FactBatchArtifactError::FactBatchArtifactError(FactBatchArtifactErrorCode code,
                                               const std::string &message)
    : std::runtime_error(message), code_(code) {}

auto FactBatchArtifactError::code() const noexcept
    -> FactBatchArtifactErrorCode {
  return code_;
}

class FactBatchArtifactInput {
public:
  FactBatchArtifactInput(const FactBatchArtifact &artifact,
                         std::uint64_t payload_size,
                         const FactBatchArtifactDecodeLimits &limits)
      : storage_(artifact.storage_), payload_size_(payload_size),
        limits_(limits) {}

  void read_exact(std::span<std::byte> destination) {
    if (destination.size() > payload_size_ - offset_) {
      fail(FactBatchArtifactErrorCode::truncated,
           "FactBatch artifact ended inside a declared field");
    }
    try {
      storage_.read_exact(offset_, destination);
    } catch (const util::ArtifactIoError &error) {
      if (error.code() == util::ArtifactIoErrorCode::io_error) {
        fail(FactBatchArtifactErrorCode::io_error, error.what());
      }
      fail(FactBatchArtifactErrorCode::truncated, error.what());
    }
    offset_ += destination.size();
  }

  [[nodiscard]] auto u8() -> std::uint8_t {
    std::array<std::byte, 1> bytes{};
    read_exact(bytes);
    return std::to_integer<std::uint8_t>(bytes.front());
  }

  [[nodiscard]] auto u32() -> std::uint32_t {
    std::array<std::byte, 4> bytes{};
    read_exact(bytes);
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      value |= static_cast<std::uint32_t>(
                   std::to_integer<std::uint8_t>(bytes[index]))
               << (index * 8U);
    }
    return value;
  }

  [[nodiscard]] auto u64() -> std::uint64_t {
    std::array<std::byte, 8> bytes{};
    read_exact(bytes);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      value |= static_cast<std::uint64_t>(
                   std::to_integer<std::uint8_t>(bytes[index]))
               << (index * 8U);
    }
    return value;
  }

  [[nodiscard]] auto i64() -> std::int64_t {
    return std::bit_cast<std::int64_t>(u64());
  }

  [[nodiscard]] auto boolean() -> bool {
    const auto value = u8();
    if (value > 1U) {
      fail(FactBatchArtifactErrorCode::corrupt,
           "FactBatch artifact contains a non-boolean value");
    }
    return value == 1U;
  }

  [[nodiscard]] auto string() -> std::string {
    const auto length = u64();
    if (length > limits_.max_string_bytes ||
        !std::in_range<std::size_t>(length)) {
      fail(FactBatchArtifactErrorCode::limit_exceeded,
           "FactBatch artifact string exceeds the configured limit");
    }
    if (length > payload_size_ - offset_) {
      fail(FactBatchArtifactErrorCode::truncated,
           "FactBatch artifact string exceeds its remaining payload");
    }
    std::string value(static_cast<std::size_t>(length), '\0');
    read_exact(std::as_writable_bytes(std::span(value.data(), value.size())));
    return value;
  }

  [[nodiscard]] auto count() -> std::uint64_t {
    const auto value = u64();
    if (value > limits_.max_collection_entries ||
        value > limits_.max_total_entries - total_entries_ ||
        !std::in_range<std::size_t>(value)) {
      fail(FactBatchArtifactErrorCode::limit_exceeded,
           "FactBatch artifact collection exceeds the configured limit");
    }
    if (value > payload_size_ - offset_) {
      fail(FactBatchArtifactErrorCode::truncated,
           "FactBatch artifact collection exceeds its remaining payload");
    }
    total_entries_ += value;
    return value;
  }

  template <typename T, typename Read>
  [[nodiscard]] auto optional(Read read) -> std::optional<T> {
    if (!boolean()) {
      return std::nullopt;
    }
    return read();
  }

  template <typename T, typename Read>
  [[nodiscard]] auto vector(Read read) -> std::vector<T> {
    const auto length = count();
    std::vector<T> values;
    std::generate_n(std::back_inserter(values), length, read);
    return values;
  }

  [[nodiscard]] auto offset() const -> std::uint64_t { return offset_; }

private:
  const util::ImmutableByteArtifact &storage_;
  std::uint64_t payload_size_;
  const FactBatchArtifactDecodeLimits &limits_;
  std::uint64_t offset_ = 0;
  std::uint64_t total_entries_ = 0;
};

namespace {

template <typename Enum> void write_enum(BinaryWriter &writer, Enum value) {
  writer.u8(static_cast<std::uint8_t>(std::to_underlying(value)));
}

template <typename Enum>
auto read_enum(FactBatchArtifactInput &reader, std::uint8_t minimum,
               std::uint8_t maximum, std::string_view name) -> Enum {
  const auto value = reader.u8();
  if (value < minimum || value > maximum) {
    fail(FactBatchArtifactErrorCode::corrupt,
         "FactBatch artifact contains an invalid " + std::string(name));
  }
  return static_cast<Enum>(value);
}

void write_file_identity(BinaryWriter &writer,
                         const PortableFileIdentity &value) {
  writer.string(value.component_path);
  writer.string(value.directory_path);
  writer.string(value.file_name);
}

auto read_file_identity(FactBatchArtifactInput &reader)
    -> PortableFileIdentity {
  return {.component_path = reader.string(),
          .directory_path = reader.string(),
          .file_name = reader.string()};
}

void write_configuration_content(BinaryWriter &writer,
                                 const PortableConfigurationContent &value) {
  writer.optional<std::string>(value.driver,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.working_dir,
                               [&](const auto &item) { writer.string(item); });
  writer.vector<std::string>(value.arguments,
                             [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.lang_mode,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.resource_dir,
                               [&](const auto &item) { writer.string(item); });
}

auto read_configuration_content(FactBatchArtifactInput &reader)
    -> PortableConfigurationContent {
  PortableConfigurationContent value;
  value.driver = reader.optional<std::string>([&] { return reader.string(); });
  value.working_dir =
      reader.optional<std::string>([&] { return reader.string(); });
  value.arguments = reader.vector<std::string>([&] { return reader.string(); });
  value.lang_mode =
      reader.optional<std::string>([&] { return reader.string(); });
  value.resource_dir =
      reader.optional<std::string>([&] { return reader.string(); });
  return value;
}

void write_configuration_identity(BinaryWriter &writer,
                                  const ConfigurationIdentity &value) {
  writer.string(value.semantic_universe);
  writer.string(value.translation_unit);
  writer.string(value.normalized_configuration);
  writer.string(value.identity_source);
  write_configuration_content(writer, value.content);
}

auto read_configuration_identity(FactBatchArtifactInput &reader)
    -> ConfigurationIdentity {
  ConfigurationIdentity value;
  value.semantic_universe = reader.string();
  value.translation_unit = reader.string();
  value.normalized_configuration = reader.string();
  value.identity_source = reader.string();
  value.content = read_configuration_content(reader);
  return value;
}

void write_partition_key(BinaryWriter &writer, const FactPartitionKey &value) {
  write_file_identity(writer, value.file);
  write_configuration_identity(writer, value.configuration);
}

auto read_partition_key(FactBatchArtifactInput &reader) -> FactPartitionKey {
  return {.file = read_file_identity(reader),
          .configuration = read_configuration_identity(reader)};
}

void write_symbol_key(BinaryWriter &writer, const SymbolNaturalKey &value) {
  write_partition_key(writer, value.partition);
  writer.string(value.usr);
  writer.optional<std::string>(value.local_anchor,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.linkage,
                               [&](const auto &item) { writer.string(item); });
}

auto read_symbol_key(FactBatchArtifactInput &reader) -> SymbolNaturalKey {
  SymbolNaturalKey value;
  value.partition = read_partition_key(reader);
  value.usr = reader.string();
  value.local_anchor =
      reader.optional<std::string>([&] { return reader.string(); });
  value.linkage = reader.optional<std::string>([&] { return reader.string(); });
  return value;
}

void write_apply_order(BinaryWriter &writer, const LegacyApplyOrderKey &value) {
  writer.string(value.component_path);
  writer.string(value.directory_path);
  writer.string(value.file_name);
  writer.u64(value.first_seen);
  writer.u64(value.conflict_ordinal);
}

auto read_apply_order(FactBatchArtifactInput &reader) -> LegacyApplyOrderKey {
  return {.component_path = reader.string(),
          .directory_path = reader.string(),
          .file_name = reader.string(),
          .first_seen = reader.u64(),
          .conflict_ordinal = reader.u64()};
}

void write_symbol_location(BinaryWriter &writer, const SymbolRecord &value) {
  writer.string(value.file);
  writer.string(value.usr);
  writer.string(value.spelling);
  writer.i64(value.kind);
  writer.optional<std::string>(value.qual_name,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.display_name,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.type_info,
                               [&](const auto &item) { writer.string(item); });
  writer.i64(value.line);
  writer.i64(value.col);
  writer.i64(value.end_line);
  writer.i64(value.end_col);
  writer.optional<std::int64_t>(value.decl_line,
                                [&](auto item) { writer.i64(item); });
  writer.optional<std::int64_t>(value.decl_col,
                                [&](auto item) { writer.i64(item); });
  writer.boolean(value.is_definition);
  writer.boolean(value.is_pure);
  writer.boolean(value.is_static);
  writer.boolean(value.is_instantiation);
}

void write_symbol_identity(BinaryWriter &writer, const SymbolRecord &value) {
  writer.optional<std::string>(value.callable_kind,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.template_origin,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.template_form,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.linkage,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.access,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.parent_usr,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.const_value,
                               [&](const auto &item) { writer.string(item); });
  writer.boolean(value.resolved);
  writer.string(value.semantic_universe);
  writer.string(value.normalized_configuration);
  writer.optional<std::string>(value.identity_source,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.identity_translation_unit,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.local_anchor,
                               [&](const auto &item) { writer.string(item); });
  writer.string(value.kind_name);
}

void write_symbol_record(BinaryWriter &writer, const SymbolRecord &value) {
  write_symbol_location(writer, value);
  write_symbol_identity(writer, value);
}

void read_symbol_location(FactBatchArtifactInput &reader, SymbolRecord &value) {
  value.file = reader.string();
  value.usr = reader.string();
  value.spelling = reader.string();
  const auto kind = reader.i64();
  if (kind < std::numeric_limits<int>::min() ||
      kind > std::numeric_limits<int>::max()) {
    fail(FactBatchArtifactErrorCode::corrupt,
         "FactBatch symbol kind is outside the supported integer range");
  }
  value.kind = static_cast<int>(kind);
  value.qual_name =
      reader.optional<std::string>([&] { return reader.string(); });
  value.display_name =
      reader.optional<std::string>([&] { return reader.string(); });
  value.type_info =
      reader.optional<std::string>([&] { return reader.string(); });
  value.line = reader.i64();
  value.col = reader.i64();
  value.end_line = reader.i64();
  value.end_col = reader.i64();
  value.decl_line = reader.optional<std::int64_t>([&] { return reader.i64(); });
  value.decl_col = reader.optional<std::int64_t>([&] { return reader.i64(); });
  value.is_definition = reader.boolean();
  value.is_pure = reader.boolean();
  value.is_static = reader.boolean();
  value.is_instantiation = reader.boolean();
}

void read_symbol_identity(FactBatchArtifactInput &reader, SymbolRecord &value) {
  value.callable_kind =
      reader.optional<std::string>([&] { return reader.string(); });
  value.template_origin =
      reader.optional<std::string>([&] { return reader.string(); });
  value.template_form =
      reader.optional<std::string>([&] { return reader.string(); });
  value.linkage = reader.optional<std::string>([&] { return reader.string(); });
  value.access = reader.optional<std::string>([&] { return reader.string(); });
  value.parent_usr =
      reader.optional<std::string>([&] { return reader.string(); });
  value.const_value =
      reader.optional<std::string>([&] { return reader.string(); });
  value.resolved = reader.boolean();
  value.semantic_universe = reader.string();
  value.normalized_configuration = reader.string();
  value.identity_source =
      reader.optional<std::string>([&] { return reader.string(); });
  value.identity_translation_unit =
      reader.optional<std::string>([&] { return reader.string(); });
  value.local_anchor =
      reader.optional<std::string>([&] { return reader.string(); });
  value.kind_name = reader.string();
}

auto read_symbol_record(FactBatchArtifactInput &reader) -> SymbolRecord {
  SymbolRecord value;
  read_symbol_location(reader, value);
  read_symbol_identity(reader, value);
  return value;
}

void write_edge(BinaryWriter &writer, const EdgeRecord &value) {
  writer.i64(value.src_id);
  writer.i64(value.dst_id);
  writer.i64(value.kind);
  writer.i64(value.count);
  writer.optional<std::int64_t>(value.base_access,
                                [&](auto item) { writer.i64(item); });
  writer.optional<std::int64_t>(value.is_virtual,
                                [&](auto item) { writer.i64(item); });
}

auto read_edge(FactBatchArtifactInput &reader) -> EdgeRecord {
  return {.src_id = reader.i64(),
          .dst_id = reader.i64(),
          .kind = reader.i64(),
          .count = reader.i64(),
          .base_access =
              reader.optional<std::int64_t>([&] { return reader.i64(); }),
          .is_virtual =
              reader.optional<std::int64_t>([&] { return reader.i64(); })};
}

void write_edge_site(BinaryWriter &writer, const EdgeSiteRecord &value) {
  writer.i64(value.edge_id);
  writer.i64(value.file_id);
  writer.i64(value.line);
  writer.i64(value.col);
  writer.i64(value.conditional);
  writer.optional<std::string>(value.recv_src_kind,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.recv_type_usr,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.recv_decl_usr,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::int64_t>(value.recv_param_pos,
                                [&](auto item) { writer.i64(item); });
  writer.optional<std::int64_t>(value.recv_type_is_value,
                                [&](auto item) { writer.i64(item); });
}

auto read_edge_site(FactBatchArtifactInput &reader) -> EdgeSiteRecord {
  EdgeSiteRecord value;
  value.edge_id = reader.i64();
  value.file_id = reader.i64();
  value.line = reader.i64();
  value.col = reader.i64();
  value.conditional = reader.i64();
  value.recv_src_kind =
      reader.optional<std::string>([&] { return reader.string(); });
  value.recv_type_usr =
      reader.optional<std::string>([&] { return reader.string(); });
  value.recv_decl_usr =
      reader.optional<std::string>([&] { return reader.string(); });
  value.recv_param_pos =
      reader.optional<std::int64_t>([&] { return reader.i64(); });
  value.recv_type_is_value =
      reader.optional<std::int64_t>([&] { return reader.i64(); });
  return value;
}

void write_call_arg(BinaryWriter &writer, const CallArgRecord &value) {
  writer.i64(value.edge_id);
  writer.i64(value.file_id);
  writer.i64(value.line);
  writer.i64(value.col);
  writer.i64(value.position);
  writer.string(value.src_kind);
  writer.optional<std::string>(value.type_usr,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.decl_usr,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.callee_usr,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::int64_t>(value.type_is_value,
                                [&](auto item) { writer.i64(item); });
}

auto read_call_arg(FactBatchArtifactInput &reader) -> CallArgRecord {
  CallArgRecord value;
  value.edge_id = reader.i64();
  value.file_id = reader.i64();
  value.line = reader.i64();
  value.col = reader.i64();
  value.position = reader.i64();
  value.src_kind = reader.string();
  value.type_usr =
      reader.optional<std::string>([&] { return reader.string(); });
  value.decl_usr =
      reader.optional<std::string>([&] { return reader.string(); });
  value.callee_usr =
      reader.optional<std::string>([&] { return reader.string(); });
  value.type_is_value =
      reader.optional<std::int64_t>([&] { return reader.i64(); });
  return value;
}

void write_template_param(BinaryWriter &writer,
                          const TemplateParamRecord &value) {
  writer.i64(value.owner_id);
  writer.i64(value.position);
  writer.i64(value.param_kind);
  writer.optional<std::string>(value.name,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.default_txt,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::int64_t>(value.type_id,
                                [&](auto item) { writer.i64(item); });
  writer.optional<std::int64_t>(value.default_type_id,
                                [&](auto item) { writer.i64(item); });
  writer.optional<std::int64_t>(value.default_ref_id,
                                [&](auto item) { writer.i64(item); });
}

auto read_template_param(FactBatchArtifactInput &reader)
    -> TemplateParamRecord {
  TemplateParamRecord value;
  value.owner_id = reader.i64();
  value.position = reader.i64();
  value.param_kind = reader.i64();
  value.name = reader.optional<std::string>([&] { return reader.string(); });
  value.default_txt =
      reader.optional<std::string>([&] { return reader.string(); });
  value.type_id = reader.optional<std::int64_t>([&] { return reader.i64(); });
  value.default_type_id =
      reader.optional<std::int64_t>([&] { return reader.i64(); });
  value.default_ref_id =
      reader.optional<std::int64_t>([&] { return reader.i64(); });
  return value;
}

void write_template_arg(BinaryWriter &writer, const TemplateArgRecord &value) {
  writer.i64(value.owner_id);
  writer.i64(value.position);
  writer.i64(value.pack_index);
  writer.i64(value.arg_kind);
  writer.optional<std::int64_t>(value.ref_id,
                                [&](auto item) { writer.i64(item); });
  writer.optional<std::string>(value.literal,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::int64_t>(value.type_id,
                                [&](auto item) { writer.i64(item); });
}

auto read_template_arg(FactBatchArtifactInput &reader) -> TemplateArgRecord {
  TemplateArgRecord value;
  value.owner_id = reader.i64();
  value.position = reader.i64();
  value.pack_index = reader.i64();
  value.arg_kind = reader.i64();
  value.ref_id = reader.optional<std::int64_t>([&] { return reader.i64(); });
  value.literal = reader.optional<std::string>([&] { return reader.string(); });
  value.type_id = reader.optional<std::int64_t>([&] { return reader.i64(); });
  return value;
}

void write_type_node(BinaryWriter &writer, const TypeNodeRecord &value) {
  writer.string(value.type_key);
  writer.string(value.spelling);
  writer.i64(value.kind);
  writer.boolean(value.is_const);
  writer.boolean(value.is_volatile);
  writer.boolean(value.is_restrict);
  writer.optional<std::string>(value.decl_usr,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::int64_t>(value.canonical_id,
                                [&](auto item) { writer.i64(item); });
  writer.optional<std::string>(value.extent,
                               [&](const auto &item) { writer.string(item); });
}

auto read_type_node(FactBatchArtifactInput &reader) -> TypeNodeRecord {
  TypeNodeRecord value;
  value.type_key = reader.string();
  value.spelling = reader.string();
  value.kind = reader.i64();
  value.is_const = reader.boolean();
  value.is_volatile = reader.boolean();
  value.is_restrict = reader.boolean();
  value.decl_usr =
      reader.optional<std::string>([&] { return reader.string(); });
  value.canonical_id =
      reader.optional<std::int64_t>([&] { return reader.i64(); });
  value.extent = reader.optional<std::string>([&] { return reader.string(); });
  return value;
}

void write_type_edge(BinaryWriter &writer, const TypeEdgeRecord &value) {
  writer.i64(value.src_id);
  writer.i64(value.kind);
  writer.i64(value.position);
  writer.i64(value.dst_id);
}

auto read_type_edge(FactBatchArtifactInput &reader) -> TypeEdgeRecord {
  return {.src_id = reader.i64(),
          .kind = reader.i64(),
          .position = reader.i64(),
          .dst_id = reader.i64()};
}

void write_parameter(BinaryWriter &writer, const ParameterRecord &value) {
  writer.i64(value.position);
  writer.i64(value.pack_index);
  writer.optional<std::string>(value.name,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::int64_t>(value.type_id,
                                [&](auto item) { writer.i64(item); });
  writer.optional<std::int64_t>(value.declared_type_id,
                                [&](auto item) { writer.i64(item); });
  writer.optional<std::int64_t>(value.adjusted_type_id,
                                [&](auto item) { writer.i64(item); });
  writer.optional<std::string>(value.default_text,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.default_origin,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::string>(value.reference_semantics,
                               [&](const auto &item) { writer.string(item); });
  writer.optional<std::int64_t>(value.file_id,
                                [&](auto item) { writer.i64(item); });
  writer.optional<std::int64_t>(value.line,
                                [&](auto item) { writer.i64(item); });
  writer.optional<std::int64_t>(value.col,
                                [&](auto item) { writer.i64(item); });
}

auto read_parameter(FactBatchArtifactInput &reader) -> ParameterRecord {
  ParameterRecord value;
  value.position = reader.i64();
  value.pack_index = reader.i64();
  value.name = reader.optional<std::string>([&] { return reader.string(); });
  value.type_id = reader.optional<std::int64_t>([&] { return reader.i64(); });
  value.declared_type_id =
      reader.optional<std::int64_t>([&] { return reader.i64(); });
  value.adjusted_type_id =
      reader.optional<std::int64_t>([&] { return reader.i64(); });
  value.default_text =
      reader.optional<std::string>([&] { return reader.string(); });
  value.default_origin =
      reader.optional<std::string>([&] { return reader.string(); });
  value.reference_semantics =
      reader.optional<std::string>([&] { return reader.string(); });
  value.file_id = reader.optional<std::int64_t>([&] { return reader.i64(); });
  value.line = reader.optional<std::int64_t>([&] { return reader.i64(); });
  value.col = reader.optional<std::int64_t>([&] { return reader.i64(); });
  return value;
}

void write_parameter_fact(BinaryWriter &writer,
                          const ParameterFactRecord &value) {
  writer.i64(value.owner_id);
  write_parameter(writer, value.parameter);
}

auto read_parameter_fact(FactBatchArtifactInput &reader)
    -> ParameterFactRecord {
  return {.owner_id = reader.i64(), .parameter = read_parameter(reader)};
}

void write_symbol_type(BinaryWriter &writer, const SymbolTypeRecord &value) {
  writer.i64(value.symbol_id);
  writer.i64(value.kind);
  writer.i64(value.type_id);
}

auto read_symbol_type(FactBatchArtifactInput &reader) -> SymbolTypeRecord {
  return {
      .symbol_id = reader.i64(), .kind = reader.i64(), .type_id = reader.i64()};
}

void write_definition(BinaryWriter &writer, const DefinitionFactRecord &value) {
  writer.i64(value.id);
  writer.i64(value.symbol_id);
  writer.i64(value.file_id);
  writer.i64(value.line);
  writer.i64(value.col);
  writer.i64(value.end_line);
  writer.i64(value.end_col);
  writer.optional<std::string>(value.init_text,
                               [&](const auto &item) { writer.string(item); });
}

auto read_definition(FactBatchArtifactInput &reader) -> DefinitionFactRecord {
  DefinitionFactRecord value;
  value.id = reader.i64();
  value.symbol_id = reader.i64();
  value.file_id = reader.i64();
  value.line = reader.i64();
  value.col = reader.i64();
  value.end_line = reader.i64();
  value.end_col = reader.i64();
  value.init_text =
      reader.optional<std::string>([&] { return reader.string(); });
  return value;
}

void write_definition_edge(BinaryWriter &writer,
                           const DefinitionEdgeRecord &value) {
  writer.i64(value.definition_id);
  writer.i64(value.destination_id);
  writer.i64(value.kind);
}

auto read_definition_edge(FactBatchArtifactInput &reader)
    -> DefinitionEdgeRecord {
  return {.definition_id = reader.i64(),
          .destination_id = reader.i64(),
          .kind = reader.i64()};
}

void write_declaration_site(BinaryWriter &writer,
                            const DeclarationSiteRecord &value) {
  write_symbol_key(writer, value.symbol);
  write_partition_key(writer, value.partition);
  writer.i64(value.line);
  writer.i64(value.col);
  writer.i64(value.end_line);
  writer.i64(value.end_col);
  writer.boolean(value.is_definition);
}

auto read_declaration_site(FactBatchArtifactInput &reader)
    -> DeclarationSiteRecord {
  return {.symbol = read_symbol_key(reader),
          .partition = read_partition_key(reader),
          .line = reader.i64(),
          .col = reader.i64(),
          .end_line = reader.i64(),
          .end_col = reader.i64(),
          .is_definition = reader.boolean()};
}

void write_include(BinaryWriter &writer, const IncludeDirectiveRecord &value) {
  write_partition_key(writer, value.partition);
  write_file_identity(writer, value.source);
  writer.optional<PortableFileIdentity>(
      value.destination,
      [&](const auto &item) { write_file_identity(writer, item); });
  writer.string(value.destination_path);
  writer.string(value.spelling);
  write_enum(writer, value.directive);
  writer.i64(value.line);
  writer.i64(value.col);
  writer.i64(value.begin_offset);
  writer.i64(value.end_offset);
  writer.string(value.conditional_fingerprint);
  writer.boolean(value.is_angled);
  writer.boolean(value.resolved);
  writer.boolean(value.is_system);
  writer.boolean(value.guarded);
}

auto read_include(FactBatchArtifactInput &reader) -> IncludeDirectiveRecord {
  IncludeDirectiveRecord value;
  value.partition = read_partition_key(reader);
  value.source = read_file_identity(reader);
  value.destination = reader.optional<PortableFileIdentity>(
      [&] { return read_file_identity(reader); });
  value.destination_path = reader.string();
  value.spelling = reader.string();
  value.directive =
      read_enum<IncludeDirectiveKind>(reader, 1, 5, "include directive kind");
  value.line = reader.i64();
  value.col = reader.i64();
  value.begin_offset = reader.i64();
  value.end_offset = reader.i64();
  value.conditional_fingerprint = reader.string();
  value.is_angled = reader.boolean();
  value.resolved = reader.boolean();
  value.is_system = reader.boolean();
  value.guarded = reader.boolean();
  return value;
}

void write_macro(BinaryWriter &writer, const MacroUseRecord &value) {
  write_partition_key(writer, value.partition);
  write_file_identity(writer, value.source);
  writer.optional<PortableFileIdentity>(
      value.definition,
      [&](const auto &item) { write_file_identity(writer, item); });
  writer.string(value.definition_path);
  writer.string(value.name);
  writer.i64(value.count);
}

auto read_macro(FactBatchArtifactInput &reader) -> MacroUseRecord {
  MacroUseRecord value;
  value.partition = read_partition_key(reader);
  value.source = read_file_identity(reader);
  value.definition = reader.optional<PortableFileIdentity>(
      [&] { return read_file_identity(reader); });
  value.definition_path = reader.string();
  value.name = reader.string();
  value.count = reader.i64();
  return value;
}

void write_diagnostic(BinaryWriter &writer, const DiagnosticFactRecord &value) {
  write_partition_key(writer, value.partition);
  write_enum(writer, value.severity);
  writer.string(value.spelling);
  writer.optional<PortableFileIdentity>(
      value.location_file,
      [&](const auto &item) { write_file_identity(writer, item); });
  writer.optional<std::int64_t>(value.line,
                                [&](auto item) { writer.i64(item); });
  writer.optional<std::int64_t>(value.col,
                                [&](auto item) { writer.i64(item); });
}

auto read_diagnostic(FactBatchArtifactInput &reader) -> DiagnosticFactRecord {
  DiagnosticFactRecord value;
  value.partition = read_partition_key(reader);
  value.severity =
      read_enum<DiagnosticSeverity>(reader, 0, 3, "diagnostic severity");
  value.spelling = reader.string();
  value.location_file = reader.optional<PortableFileIdentity>(
      [&] { return read_file_identity(reader); });
  value.line = reader.optional<std::int64_t>([&] { return reader.i64(); });
  value.col = reader.optional<std::int64_t>([&] { return reader.i64(); });
  return value;
}

void write_evidence(BinaryWriter &writer, const EvidenceRecord &value) {
  writer.string(value.producer);
  writer.string(value.construct);
  writer.string(value.file);
  writer.i64(value.line);
  writer.i64(value.col);
  write_enum(writer, value.completeness);
  write_enum(writer, value.trust);
  writer.string(value.detail);
}

auto read_evidence(FactBatchArtifactInput &reader) -> EvidenceRecord {
  EvidenceRecord value;
  value.producer = reader.string();
  value.construct = reader.string();
  value.file = reader.string();
  value.line = reader.i64();
  value.col = reader.i64();
  value.completeness =
      read_enum<FactCompleteness>(reader, 0, 3, "fact completeness");
  value.trust = read_enum<FactTrust>(reader, 0, 2, "fact trust");
  value.detail = reader.string();
  return value;
}

void write_presentation(BinaryWriter &writer, const PresentationIntent &value) {
  writer.i64(value.symbol_id);
  writer.vector<std::string>(value.display_args,
                             [&](const auto &item) { writer.string(item); });
}

auto read_presentation(FactBatchArtifactInput &reader) -> PresentationIntent {
  return {.symbol_id = reader.i64(),
          .display_args =
              reader.vector<std::string>([&] { return reader.string(); })};
}

void write_cleanup(BinaryWriter &writer, const LifecycleCleanupIntent &value) {
  write_partition_key(writer, value.partition);
  write_enum(writer, value.kind);
  write_file_identity(writer, value.target);
  writer.string(value.prior_generation.token);
}

auto read_cleanup(FactBatchArtifactInput &reader) -> LifecycleCleanupIntent {
  return {.partition = read_partition_key(reader),
          .kind = read_enum<LifecycleCleanupKind>(reader, 0, 4, "cleanup kind"),
          .target = read_file_identity(reader),
          .prior_generation = {.token = reader.string()}};
}

void write_applicability(BinaryWriter &writer,
                         const ApplicabilityOwnershipRecord &value) {
  write_partition_key(writer, value.partition);
  write_file_identity(writer, value.file);
  write_enum(writer, value.role);
  write_enum(writer, value.state);
  writer.optional<std::string>(value.reason,
                               [&](const auto &item) { writer.string(item); });
  writer.string(value.generation.token);
}

auto read_applicability(FactBatchArtifactInput &reader)
    -> ApplicabilityOwnershipRecord {
  ApplicabilityOwnershipRecord value;
  value.partition = read_partition_key(reader);
  value.file = read_file_identity(reader);
  value.role = read_enum<ApplicabilityRole>(reader, 0, 1, "applicability role");
  value.state =
      read_enum<ApplicabilityState>(reader, 0, 4, "applicability state");
  value.reason = reader.optional<std::string>([&] { return reader.string(); });
  value.generation.token = reader.string();
  return value;
}

void write_symbol_order(BinaryWriter &writer,
                        const SymbolEmissionMetadata &value) {
  write_symbol_key(writer, value.symbol);
  write_apply_order(writer, value.apply_order);
  writer.string(value.record_key);
  writer.u64(value.first_seen);
  writer.u64(value.last_seen);
}

auto read_symbol_order(FactBatchArtifactInput &reader)
    -> SymbolEmissionMetadata {
  return {.symbol = read_symbol_key(reader),
          .apply_order = read_apply_order(reader),
          .record_key = reader.string(),
          .first_seen = reader.u64(),
          .last_seen = reader.u64()};
}

template <typename T, typename Write>
void write_records(BinaryWriter &writer, const std::vector<T> &values,
                   Write write) {
  writer.vector<T>(values, [&](const T &value) { write(writer, value); });
}

template <typename T, typename Read>
auto read_records(FactBatchArtifactInput &reader, Read read) -> std::vector<T> {
  return reader.vector<T>([&] { return read(reader); });
}

void write_fact_records(BinaryWriter &writer, const FactRecords &records) {
  write_records(writer, records.symbols, write_symbol_record);
  write_records(writer, records.declaration_sites, write_declaration_site);
  write_records(writer, records.relations, write_edge);
  write_records(writer, records.edge_sites, write_edge_site);
  write_records(writer, records.call_args, write_call_arg);
  write_records(writer, records.template_params, write_template_param);
  write_records(writer, records.template_args, write_template_arg);
  write_records(writer, records.type_nodes, write_type_node);
  write_records(writer, records.type_edges, write_type_edge);
  write_records(writer, records.parameters, write_parameter_fact);
  write_records(writer, records.symbol_types, write_symbol_type);
  write_records(writer, records.definitions, write_definition);
  write_records(writer, records.definition_edges, write_definition_edge);
  write_records(writer, records.includes, write_include);
  write_records(writer, records.macros, write_macro);
  write_records(writer, records.diagnostics, write_diagnostic);
  write_records(writer, records.evidence, write_evidence);
  write_records(writer, records.presentation_intents, write_presentation);
  write_records(writer, records.lifecycle_cleanup, write_cleanup);
  write_records(writer, records.applicability, write_applicability);
  write_records(writer, records.symbol_order, write_symbol_order);
}

auto read_fact_records(FactBatchArtifactInput &reader) -> FactRecords {
  FactRecords records;
  records.symbols = read_records<SymbolRecord>(reader, read_symbol_record);
  records.declaration_sites =
      read_records<DeclarationSiteRecord>(reader, read_declaration_site);
  records.relations = read_records<EdgeRecord>(reader, read_edge);
  records.edge_sites = read_records<EdgeSiteRecord>(reader, read_edge_site);
  records.call_args = read_records<CallArgRecord>(reader, read_call_arg);
  records.template_params =
      read_records<TemplateParamRecord>(reader, read_template_param);
  records.template_args =
      read_records<TemplateArgRecord>(reader, read_template_arg);
  records.type_nodes = read_records<TypeNodeRecord>(reader, read_type_node);
  records.type_edges = read_records<TypeEdgeRecord>(reader, read_type_edge);
  records.parameters =
      read_records<ParameterFactRecord>(reader, read_parameter_fact);
  records.symbol_types =
      read_records<SymbolTypeRecord>(reader, read_symbol_type);
  records.definitions =
      read_records<DefinitionFactRecord>(reader, read_definition);
  records.definition_edges =
      read_records<DefinitionEdgeRecord>(reader, read_definition_edge);
  records.includes = read_records<IncludeDirectiveRecord>(reader, read_include);
  records.macros = read_records<MacroUseRecord>(reader, read_macro);
  records.diagnostics =
      read_records<DiagnosticFactRecord>(reader, read_diagnostic);
  records.evidence = read_records<EvidenceRecord>(reader, read_evidence);
  records.presentation_intents =
      read_records<PresentationIntent>(reader, read_presentation);
  records.lifecycle_cleanup =
      read_records<LifecycleCleanupIntent>(reader, read_cleanup);
  records.applicability =
      read_records<ApplicabilityOwnershipRecord>(reader, read_applicability);
  records.symbol_order =
      read_records<SymbolEmissionMetadata>(reader, read_symbol_order);
  return records;
}

void write_string_map(BinaryWriter &writer,
                      const std::map<std::int64_t, std::string> &values) {
  writer.u64(values.size());
  for (const auto &[key, value] : values) {
    writer.i64(key);
    writer.string(value);
  }
}

auto read_string_map(FactBatchArtifactInput &reader)
    -> std::map<std::int64_t, std::string> {
  const auto count = reader.count();
  std::map<std::int64_t, std::string> values;
  std::optional<std::int64_t> previous_key;
  for (std::uint64_t index = 0; index < count; ++index) {
    const auto key = reader.i64();
    if (previous_key && key <= *previous_key) {
      fail(FactBatchArtifactErrorCode::corrupt,
           "FactBatch artifact handle keys are not canonically ordered");
    }
    values.emplace_hint(values.end(), key, reader.string());
    previous_key = key;
  }
  return values;
}

void write_file_map(BinaryWriter &writer,
                    const std::map<std::int64_t, FactPartitionKey> &values) {
  writer.u64(values.size());
  for (const auto &[key, value] : values) {
    writer.i64(key);
    write_partition_key(writer, value);
  }
}

auto read_file_map(FactBatchArtifactInput &reader)
    -> std::map<std::int64_t, FactPartitionKey> {
  const auto count = reader.count();
  std::map<std::int64_t, FactPartitionKey> values;
  std::optional<std::int64_t> previous_key;
  for (std::uint64_t index = 0; index < count; ++index) {
    const auto key = reader.i64();
    if (previous_key && key <= *previous_key) {
      fail(FactBatchArtifactErrorCode::corrupt,
           "FactBatch file handle keys are not canonically ordered");
    }
    values.emplace_hint(values.end(), key, read_partition_key(reader));
    previous_key = key;
  }
  return values;
}

void write_partitions(BinaryWriter &writer,
                      const std::vector<FileFactPartition> &partitions) {
  writer.u64(partitions.size());
  for (const auto &partition : partitions) {
    write_partition_key(writer, partition.key);
    writer.u64(partition.members.size());
    for (const auto &[family, members] : partition.members) {
      write_enum(writer, family);
      writer.u64(members.size());
      for (const std::size_t member : members) {
        writer.u64(member);
      }
    }
  }
}

auto read_partition_members(FactBatchArtifactInput &reader)
    -> std::vector<std::size_t> {
  const auto member_count = reader.count();
  std::vector<std::size_t> members;
  for (std::uint64_t member_index = 0; member_index < member_count;
       ++member_index) {
    const auto member = reader.u64();
    if (!std::in_range<std::size_t>(member)) {
      fail(FactBatchArtifactErrorCode::limit_exceeded,
           "FactBatch partition member index is too large");
    }
    members.push_back(static_cast<std::size_t>(member));
  }
  return members;
}

auto read_partition(FactBatchArtifactInput &reader) -> FileFactPartition {
  FileFactPartition partition;
  partition.key = read_partition_key(reader);
  const auto family_count = reader.count();
  std::optional<FactFamily> previous_family;
  for (std::uint64_t family_index = 0; family_index < family_count;
       ++family_index) {
    const auto family = read_enum<FactFamily>(reader, 0, 19, "fact family");
    if (previous_family &&
        std::to_underlying(family) <= std::to_underlying(*previous_family)) {
      fail(FactBatchArtifactErrorCode::corrupt,
           "FactBatch partition families are not canonically ordered");
    }
    partition.members.emplace_hint(partition.members.end(), family,
                                   read_partition_members(reader));
    previous_family = family;
  }
  return partition;
}

auto read_partitions(FactBatchArtifactInput &reader)
    -> std::vector<FileFactPartition> {
  const auto partition_count = reader.count();
  std::vector<FileFactPartition> partitions;
  std::generate_n(std::back_inserter(partitions), partition_count,
                  [&] { return read_partition(reader); });
  return partitions;
}

void write_info(BinaryWriter &writer, const FactBatch &batch,
                const FactBatchArtifactMetadata &metadata) {
  writer.u32(kFactBatchArtifactWireVersion);
  writer.string(batch.producer());
  writer.u32(batch.producer_version());
  writer.u32(metadata.pass_version);
  writer.u32(metadata.extractor_version);
  writer.u32(metadata.storage_schema_version);
  writer.i64(metadata.catalog_version);
  writer.string(metadata.catalog_hash);
  write_enum(writer, batch.completeness());
  write_enum(writer, metadata.trust);
  writer.boolean(metadata.truncated);
  const auto dependencies =
      canonical_dependencies(metadata.dependency_identities);
  writer.vector<std::string>(dependencies,
                             [&](const auto &item) { writer.string(item); });
}

auto read_info(FactBatchArtifactInput &reader) -> FactBatchArtifactInfo {
  FactBatchArtifactInfo info;
  info.wire_version = reader.u32();
  info.producer = reader.string();
  info.producer_version = reader.u32();
  info.pass_version = reader.u32();
  info.extractor_version = reader.u32();
  info.storage_schema_version = reader.u32();
  info.catalog_version = reader.i64();
  info.catalog_hash = reader.string();
  info.completeness =
      read_enum<FactCompleteness>(reader, 0, 3, "artifact completeness");
  info.trust = read_enum<FactTrust>(reader, 0, 2, "artifact trust");
  info.truncated = reader.boolean();
  info.dependency_identities =
      reader.vector<std::string>([&] { return reader.string(); });
  if (!std::ranges::is_sorted(info.dependency_identities) ||
      std::ranges::adjacent_find(info.dependency_identities) !=
          info.dependency_identities.end()) {
    fail(FactBatchArtifactErrorCode::corrupt,
         "FactBatch dependency identities are not canonical");
  }
  return info;
}

auto read_tail(const util::ImmutableByteArtifact &storage, std::uint64_t offset,
               std::size_t size) -> std::vector<std::byte> {
  std::vector<std::byte> bytes(size);
  try {
    storage.read_exact(offset, bytes);
  } catch (const util::ArtifactIoError &error) {
    if (error.code() == util::ArtifactIoErrorCode::io_error) {
      fail(FactBatchArtifactErrorCode::io_error, error.what());
    }
    fail(FactBatchArtifactErrorCode::truncated, error.what());
  }
  return bytes;
}

auto digest_prefix(const util::ImmutableByteArtifact &storage,
                   std::uint64_t payload_size) -> std::string {
  try {
    return storage.digest_prefix(payload_size);
  } catch (const util::ArtifactIoError &) {
    fail(FactBatchArtifactErrorCode::io_error,
         "cannot hash the FactBatch artifact payload");
  }
}

auto verify_footer(const util::ImmutableByteArtifact &storage)
    -> std::pair<std::uint64_t, std::string> {
  if (storage.size() < kFooterSize) {
    fail(FactBatchArtifactErrorCode::truncated,
         "FactBatch artifact is shorter than its digest footer");
  }
  const auto payload_size = storage.size() - kFooterSize;
  const auto footer = read_tail(storage, payload_size, kFooterSize);
  if (!std::ranges::equal(kDigestMagic,
                          std::span(footer).first(kDigestMagic.size()))) {
    fail(FactBatchArtifactErrorCode::corrupt,
         "FactBatch artifact digest footer magic is invalid");
  }
  const auto digest_begin =
      footer.begin() + static_cast<std::ptrdiff_t>(kDigestMagic.size());
  const std::string expected(reinterpret_cast<const char *>(&*digest_begin),
                             kDigestTextSize);
  if (!expected.starts_with("sha256:") ||
      !std::ranges::all_of(expected.substr(7), [](unsigned char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
      })) {
    fail(FactBatchArtifactErrorCode::corrupt,
         "FactBatch artifact digest footer is malformed");
  }
  const auto actual = digest_prefix(storage, payload_size);
  if (actual != expected) {
    fail(FactBatchArtifactErrorCode::corrupt,
         "FactBatch artifact content digest does not match its payload");
  }
  return {payload_size, expected};
}

void validate_wire_preamble(const util::ImmutableByteArtifact &storage) {
  std::array<std::byte, kMagic.size() + sizeof(std::uint32_t)> preamble{};
  try {
    storage.read_exact(0, preamble);
  } catch (const util::ArtifactIoError &error) {
    if (error.code() == util::ArtifactIoErrorCode::io_error) {
      fail(FactBatchArtifactErrorCode::io_error, error.what());
    }
    fail(FactBatchArtifactErrorCode::truncated, error.what());
  }
  if (!std::ranges::equal(kMagic, std::span(preamble).first(kMagic.size()))) {
    fail(FactBatchArtifactErrorCode::incompatible,
         "FactBatch artifact wire magic is unsupported");
  }
  std::uint32_t wire_version = 0;
  for (std::size_t index = 0; index < sizeof(wire_version); ++index) {
    wire_version |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(
                        preamble[kMagic.size() + index]))
                    << (index * 8U);
  }
  if (wire_version != kFactBatchArtifactWireVersion) {
    fail(FactBatchArtifactErrorCode::incompatible,
         "FactBatch artifact wire version is unsupported");
  }
}

void validate_compatibility(
    const FactBatchArtifactInfo &info,
    const FactBatchArtifactCompatibility &compatibility) {
  auto incompatible = [&](std::string_view dimension) {
    fail(FactBatchArtifactErrorCode::incompatible,
         "FactBatch artifact has an incompatible " + std::string(dimension));
  };
  if (info.wire_version != compatibility.wire_version) {
    incompatible("wire version");
  }
  if (compatibility.producer && info.producer != *compatibility.producer) {
    incompatible("producer identity");
  }
  if (compatibility.producer_version &&
      info.producer_version != *compatibility.producer_version) {
    incompatible("producer version");
  }
  if (info.pass_version != compatibility.pass_version) {
    incompatible("pass version");
  }
  if (info.extractor_version != compatibility.extractor_version) {
    incompatible("extractor version");
  }
  if (info.storage_schema_version != compatibility.storage_schema_version) {
    incompatible("storage schema version");
  }
  if (info.catalog_version != compatibility.catalog_version ||
      info.catalog_hash != compatibility.catalog_hash) {
    incompatible("catalog identity");
  }
  if (compatibility.require_trusted && info.trust != FactTrust::trusted) {
    incompatible("trust level");
  }
  if (compatibility.dependency_identities &&
      info.dependency_identities !=
          canonical_dependencies(*compatibility.dependency_identities)) {
    fail(FactBatchArtifactErrorCode::stale,
         "FactBatch artifact dependency identities are stale");
  }
  if (info.truncated && !compatibility.allow_truncated) {
    fail(FactBatchArtifactErrorCode::truncated,
         "FactBatch artifact is marked truncated");
  }
  if (info.completeness != FactCompleteness::complete &&
      !compatibility.allow_partial) {
    fail(FactBatchArtifactErrorCode::partial,
         "FactBatch artifact is not complete");
  }
}

auto family_size(const FactRecords &records, FactFamily family) -> std::size_t {
  switch (family) {
  case FactFamily::symbols:
    return records.symbols.size();
  case FactFamily::declaration_sites:
    return records.declaration_sites.size();
  case FactFamily::relations:
    return records.relations.size();
  case FactFamily::edge_sites:
    return records.edge_sites.size();
  case FactFamily::call_arguments:
    return records.call_args.size();
  case FactFamily::template_parameters:
    return records.template_params.size();
  case FactFamily::template_arguments:
    return records.template_args.size();
  case FactFamily::types:
    return records.type_nodes.size();
  case FactFamily::type_edges:
    return records.type_edges.size();
  case FactFamily::parameters:
    return records.parameters.size();
  case FactFamily::symbol_types:
    return records.symbol_types.size();
  case FactFamily::definitions:
    return records.definitions.size();
  case FactFamily::definition_edges:
    return records.definition_edges.size();
  case FactFamily::includes:
    return records.includes.size();
  case FactFamily::macros:
    return records.macros.size();
  case FactFamily::diagnostics:
    return records.diagnostics.size();
  case FactFamily::evidence:
    return records.evidence.size();
  case FactFamily::presentation_intents:
    return records.presentation_intents.size();
  case FactFamily::lifecycle_cleanup:
    return records.lifecycle_cleanup.size();
  case FactFamily::applicability:
    return records.applicability.size();
  }
  return 0;
}

void validate_partitions(const FactRecords &records,
                         const std::vector<FileFactPartition> &partitions) {
  for (const auto &partition : partitions) {
    for (const auto &[family, members] : partition.members) {
      const auto size = family_size(records, family);
      if (std::ranges::any_of(
              members, [size](std::size_t index) { return index >= size; })) {
        fail(FactBatchArtifactErrorCode::corrupt,
             "FactBatch partition contains an out-of-range record index");
      }
    }
  }
}

} // namespace

class FactBatchArtifactCodec {
public:
  [[nodiscard]] static auto
  encode(const FactBatch &batch, const FactBatchArtifactEncodeOptions &options)
      -> FactBatchArtifact {
    if (!batch.has_canonical_layout()) {
      fail(FactBatchArtifactErrorCode::noncanonical,
           "FactBatch artifacts require canonical_batch() input");
    }
    if (batch.producer().empty() || batch.producer_version() == 0 ||
        options.metadata.pass_version == 0 ||
        options.metadata.extractor_version == 0 ||
        options.metadata.storage_schema_version == 0 ||
        options.metadata.catalog_version <= 0 ||
        options.metadata.catalog_hash.empty()) {
      fail(FactBatchArtifactErrorCode::incompatible,
           "FactBatch artifact version identities must be explicit");
    }
    if (std::ranges::any_of(options.metadata.dependency_identities,
                            &std::string::empty)) {
      fail(FactBatchArtifactErrorCode::incompatible,
           "FactBatch dependency identities must be non-empty");
    }

    util::SpoolingByteSink spool(
        {.spill_threshold_bytes = options.spill_threshold_bytes,
         .spill_directory = options.spill_directory,
         .max_bytes = options.max_artifact_bytes});
    BinaryWriter writer(spool);
    writer.bytes(kMagic);
    write_info(writer, batch, options.metadata);
    write_fact_records(writer, batch.records());
    write_partitions(writer, batch.partitions());
    write_string_map(writer, batch.symbol_keys());
    write_string_map(writer, batch.relation_keys());
    write_string_map(writer, batch.type_keys());
    write_string_map(writer, batch.definition_keys());
    write_file_map(writer, batch.file_keys());

    const auto digest = spool.digest_prefix();
    writer.bytes(kDigestMagic);
    writer.bytes(std::as_bytes(std::span(digest.data(), digest.size())));
    return FactBatchArtifact(spool.finish());
  }

  [[nodiscard]] static auto
  decode(const FactBatchArtifact &artifact,
         const FactBatchArtifactCompatibility &compatibility)
      -> std::pair<FactBatch, FactBatchArtifactInfo> {
    if (!artifact.storage_.valid()) {
      fail(FactBatchArtifactErrorCode::missing,
           "FactBatch artifact handle is empty");
    }
    if (artifact.storage_.size() > compatibility.limits.max_artifact_bytes) {
      fail(FactBatchArtifactErrorCode::limit_exceeded,
           "FactBatch artifact exceeds the configured byte limit");
    }
    validate_wire_preamble(artifact.storage_);
    const auto [payload_size, digest] = verify_footer(artifact.storage_);
    FactBatchArtifactInput reader(artifact, payload_size, compatibility.limits);
    std::array<std::byte, kMagic.size()> magic{};
    reader.read_exact(magic);
    if (magic != kMagic) {
      fail(FactBatchArtifactErrorCode::incompatible,
           "FactBatch artifact wire magic is unsupported");
    }
    auto info = read_info(reader);
    validate_compatibility(info, compatibility);

    auto data = std::make_shared<FactBatch::Data>();
    data->producer = info.producer;
    data->producer_version = info.producer_version;
    data->completeness = info.completeness;
    data->canonical = true;
    data->records = read_fact_records(reader);
    data->partitions = read_partitions(reader);
    data->symbol_keys = read_string_map(reader);
    data->relation_keys = read_string_map(reader);
    data->type_keys = read_string_map(reader);
    data->definition_keys = read_string_map(reader);
    data->file_keys = read_file_map(reader);
    if (reader.offset() != payload_size) {
      fail(FactBatchArtifactErrorCode::corrupt,
           "FactBatch artifact has unrecognized trailing payload bytes");
    }
    validate_partitions(data->records, data->partitions);
    FactBatch batch(std::move(data));
    if (!batch.has_canonical_layout()) {
      fail(FactBatchArtifactErrorCode::corrupt,
           "FactBatch artifact payload is not canonically ordered");
    }
    info.content_digest = digest;
    info.byte_size = artifact.storage_.size();
    return {std::move(batch), std::move(info)};
  }
};

FactBatchArtifact::FactBatchArtifact(util::ImmutableByteArtifact storage)
    : storage_(std::move(storage)) {}

auto FactBatchArtifact::from_bytes(std::vector<std::byte> bytes)
    -> FactBatchArtifact {
  return FactBatchArtifact(
      util::ImmutableByteArtifact::from_bytes(std::move(bytes)));
}

auto FactBatchArtifact::from_file(std::string_view path) -> FactBatchArtifact {
  try {
    return FactBatchArtifact(util::ImmutableByteArtifact::from_file(path));
  } catch (const util::ArtifactIoError &error) {
    const auto code = error.code() == util::ArtifactIoErrorCode::missing
                          ? FactBatchArtifactErrorCode::missing
                          : FactBatchArtifactErrorCode::io_error;
    fail(code, error.what());
  }
}

auto FactBatchArtifact::byte_size() const -> std::uint64_t {
  return storage_.size();
}

auto FactBatchArtifact::spilled() const -> bool { return storage_.spilled(); }

auto FactBatchArtifact::peak_memory_bytes() const -> std::size_t {
  return storage_.peak_memory_bytes();
}

void FactBatchArtifact::write_to(std::ostream &output) const {
  try {
    storage_.write_to(output);
  } catch (const util::ArtifactIoError &error) {
    const auto code = error.code() == util::ArtifactIoErrorCode::missing
                          ? FactBatchArtifactErrorCode::missing
                          : FactBatchArtifactErrorCode::io_error;
    fail(code, error.what());
  }
}

void FactBatchArtifact::materialize(std::string_view path) const {
  try {
    storage_.materialize(path);
  } catch (const util::ArtifactIoError &error) {
    const auto code = error.code() == util::ArtifactIoErrorCode::missing
                          ? FactBatchArtifactErrorCode::missing
                          : FactBatchArtifactErrorCode::io_error;
    fail(code, error.what());
  }
}

auto encode_fact_batch_artifact(const FactBatch &batch,
                                const FactBatchArtifactEncodeOptions &options)
    -> FactBatchArtifact {
  try {
    return FactBatchArtifactCodec::encode(batch, options);
  } catch (const util::ArtifactIoError &error) {
    switch (error.code()) {
    case util::ArtifactIoErrorCode::limit_exceeded:
      fail(FactBatchArtifactErrorCode::limit_exceeded, error.what());
    case util::ArtifactIoErrorCode::spill_failure:
      fail(FactBatchArtifactErrorCode::spill_failure, error.what());
    case util::ArtifactIoErrorCode::missing:
    case util::ArtifactIoErrorCode::short_read:
    case util::ArtifactIoErrorCode::io_error:
      fail(FactBatchArtifactErrorCode::io_error, error.what());
    }
  }
}

auto decode_fact_batch_artifact(
    const FactBatchArtifact &artifact,
    const FactBatchArtifactCompatibility &compatibility)
    -> FactBatchArtifactDecodeResult {
  try {
    auto [batch, info] =
        FactBatchArtifactCodec::decode(artifact, compatibility);
    return {.batch = std::move(batch), .info = std::move(info)};
  } catch (const FactBatchArtifactError &error) {
    return {.diagnostics = {{.code = error.code(), .message = error.what()}}};
  } catch (const std::bad_alloc &) {
    return {
        .diagnostics = {{.code = FactBatchArtifactErrorCode::limit_exceeded,
                         .message = "FactBatch artifact allocation failed"}}};
  } catch (const std::exception &error) {
    return {.diagnostics = {{.code = FactBatchArtifactErrorCode::corrupt,
                             .message = error.what()}}};
  }
}

} // namespace cidx::ast
