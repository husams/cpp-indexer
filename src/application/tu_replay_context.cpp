#include "application/tu_replay_context.hpp"

#include <bit>
#include <utility>

namespace cidx::application {

namespace {

// Little-endian, length-prefixed, no padding. Every field is written in a
// fixed order so the same context always encodes to the same bytes and the
// sidecar's payload digest is meaningful.
class Writer {
public:
  void u8(std::uint8_t value) {
    bytes_.push_back(static_cast<std::byte>(value));
  }

  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
  }

  void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

  void text(std::string_view value) {
    u64(value.size());
    for (const char character : value) {
      u8(static_cast<std::uint8_t>(character));
    }
  }

  void optional_i64(const std::optional<std::int64_t> &value) {
    u8(value.has_value() ? 1U : 0U);
    if (value) {
      i64(*value);
    }
  }

  void optional_text(const std::optional<std::string> &value) {
    u8(value.has_value() ? 1U : 0U);
    if (value) {
      text(*value);
    }
  }

  void optional_double(const std::optional<double> &value) {
    u8(value.has_value() ? 1U : 0U);
    if (value) {
      u64(std::bit_cast<std::uint64_t>(*value));
    }
  }

  void texts(const std::vector<std::string> &values) {
    u64(values.size());
    for (const std::string &value : values) {
      text(value);
    }
  }

  void optional_texts(const std::optional<std::vector<std::string>> &values) {
    u8(values.has_value() ? 1U : 0U);
    if (values) {
      texts(*values);
    }
  }

  [[nodiscard]] auto take() && -> std::vector<std::byte> {
    return std::move(bytes_);
  }

private:
  std::vector<std::byte> bytes_;
};

class Reader {
public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  auto u8() -> std::uint8_t {
    if (offset_ >= bytes_.size()) {
      throw TuReplayContextError("replay context is truncated");
    }
    return std::to_integer<std::uint8_t>(bytes_[offset_++]);
  }

  auto u64() -> std::uint64_t {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
      value |= static_cast<std::uint64_t>(u8()) << shift;
    }
    return value;
  }

  auto i64() -> std::int64_t { return static_cast<std::int64_t>(u64()); }

  auto text() -> std::string {
    const std::uint64_t size = u64();
    if (size > bytes_.size() - offset_) {
      throw TuReplayContextError("replay context string exceeds the payload");
    }
    std::string value;
    value.reserve(static_cast<std::size_t>(size));
    for (std::uint64_t index = 0; index < size; ++index) {
      value.push_back(static_cast<char>(u8()));
    }
    return value;
  }

  auto flag() -> bool {
    const std::uint8_t value = u8();
    if (value > 1U) {
      throw TuReplayContextError("replay context flag is not boolean");
    }
    return value == 1U;
  }

  auto optional_text() -> std::optional<std::string> {
    if (!flag()) {
      return std::nullopt;
    }
    return text();
  }

  auto optional_i64() -> std::optional<std::int64_t> {
    if (!flag()) {
      return std::nullopt;
    }
    return i64();
  }

  auto optional_double() -> std::optional<double> {
    if (!flag()) {
      return std::nullopt;
    }
    return std::bit_cast<double>(u64());
  }

  auto texts() -> std::vector<std::string> {
    const std::uint64_t count = u64();
    // One entry costs at least a length prefix, so a count larger than the
    // remaining payload is corrupt rather than merely large.
    if (count > bytes_.size() - offset_) {
      throw TuReplayContextError("replay context collection exceeds the "
                                 "payload");
    }
    std::vector<std::string> values;
    values.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
      values.push_back(text());
    }
    return values;
  }

  auto optional_texts() -> std::optional<std::vector<std::string>> {
    if (!flag()) {
      return std::nullopt;
    }
    return texts();
  }

  void expect_end() const {
    if (offset_ != bytes_.size()) {
      throw TuReplayContextError("replay context has trailing bytes");
    }
  }

private:
  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0;
};

void write_partition(Writer &writer, const ast::FactPartitionKey &partition) {
  writer.text(partition.file.component_path);
  writer.text(partition.file.directory_path);
  writer.text(partition.file.file_name);
  writer.text(partition.configuration.semantic_universe);
  writer.text(partition.configuration.translation_unit);
  writer.text(partition.configuration.normalized_configuration);
  writer.text(partition.configuration.identity_source);
  writer.optional_text(partition.configuration.content.driver);
  writer.optional_text(partition.configuration.content.working_dir);
  writer.texts(partition.configuration.content.arguments);
  writer.optional_text(partition.configuration.content.lang_mode);
  writer.optional_text(partition.configuration.content.resource_dir);
}

auto read_partition(Reader &reader) -> ast::FactPartitionKey {
  ast::FactPartitionKey partition;
  partition.file.component_path = reader.text();
  partition.file.directory_path = reader.text();
  partition.file.file_name = reader.text();
  partition.configuration.semantic_universe = reader.text();
  partition.configuration.translation_unit = reader.text();
  partition.configuration.normalized_configuration = reader.text();
  partition.configuration.identity_source = reader.text();
  partition.configuration.content.driver = reader.optional_text();
  partition.configuration.content.working_dir = reader.optional_text();
  partition.configuration.content.arguments = reader.texts();
  partition.configuration.content.lang_mode = reader.optional_text();
  partition.configuration.content.resource_dir = reader.optional_text();
  return partition;
}

auto role_from_wire(std::uint8_t value) -> ast::PlannedFileRole {
  switch (value) {
  case 0U:
    return ast::PlannedFileRole::translation_unit;
  case 1U:
    return ast::PlannedFileRole::owned_header;
  default:
    throw TuReplayContextError("replay context route role is unknown");
  }
}

} // namespace

TuReplayContextError::TuReplayContextError(const std::string &message)
    : std::runtime_error(message) {}

auto build_tu_replay_context(const ast::IndexOneOutcome &outcome)
    -> TuReplayContext {
  if (!outcome.publication) {
    throw TuReplayContextError("outcome has no publication to replay");
  }
  const ast::ExtractedFactPublication &publication = *outcome.publication;
  TuReplayContext context{.translation_unit = publication.translation_unit,
                          .generation = publication.expected_generation,
                          .routes = {},
                          .stored = outcome.stored,
                          .headers = outcome.headers,
                          .diagnostics = outcome.diagnostics};
  for (const ast::PlannedFileRoute &route : publication.route_plan.routes()) {
    if (route.translation_unit != publication.translation_unit) {
      continue;
    }
    context.routes.push_back(
        {.role = route.role,
         .path = route.path,
         .discovery_ordinal = route.discovery_ordinal,
         .is_translation_unit_row =
             route.role == ast::PlannedFileRole::translation_unit,
         .mtime = route.snapshot.mtime,
         .md5 = route.snapshot.md5,
         .compile_options = route.compile_options,
         .driver = route.driver,
         .cleanup_symbols = route.cleanup_symbols,
         .partition = route.extraction.partition});
  }
  return context;
}

auto encode_tu_replay_context(const TuReplayContext &context)
    -> std::vector<std::byte> {
  Writer writer;
  writer.text(kTuReplayContextVersion);
  writer.text(context.translation_unit);
  writer.text(context.generation);
  writer.u64(context.routes.size());
  for (const TuReplayRoute &route : context.routes) {
    writer.u8(static_cast<std::uint8_t>(std::to_underlying(route.role)));
    writer.text(route.path);
    writer.u64(route.discovery_ordinal);
    writer.u8(route.is_translation_unit_row ? 1U : 0U);
    writer.optional_double(route.mtime);
    writer.optional_text(route.md5);
    writer.optional_texts(route.compile_options);
    writer.optional_text(route.driver);
    writer.u8(route.cleanup_symbols ? 1U : 0U);
    write_partition(writer, route.partition);
  }
  writer.i64(context.stored);
  writer.i64(context.headers.indexed);
  writer.i64(context.headers.symbols);
  writer.i64(context.headers.already);
  writer.i64(context.headers.system);
  writer.i64(context.headers.unowned);
  writer.u64(context.diagnostics.size());
  for (const cidx::Diagnostic &diagnostic : context.diagnostics) {
    writer.i64(diagnostic.severity);
    writer.text(diagnostic.spelling);
    writer.optional_text(diagnostic.file_path);
    writer.optional_i64(diagnostic.line);
    writer.optional_i64(diagnostic.col);
  }
  return std::move(writer).take();
}

auto decode_tu_replay_context(std::span<const std::byte> bytes)
    -> TuReplayContext {
  Reader reader(bytes);
  if (reader.text() != kTuReplayContextVersion) {
    throw TuReplayContextError("replay context contract is not supported");
  }
  TuReplayContext context;
  context.translation_unit = reader.text();
  context.generation = reader.text();
  const std::uint64_t routes = reader.u64();
  if (routes > bytes.size()) {
    throw TuReplayContextError("replay context route count exceeds the "
                               "payload");
  }
  context.routes.reserve(static_cast<std::size_t>(routes));
  for (std::uint64_t index = 0; index < routes; ++index) {
    TuReplayRoute route;
    route.role = role_from_wire(reader.u8());
    route.path = reader.text();
    route.discovery_ordinal = static_cast<std::size_t>(reader.u64());
    route.is_translation_unit_row = reader.flag();
    route.mtime = reader.optional_double();
    route.md5 = reader.optional_text();
    route.compile_options = reader.optional_texts();
    route.driver = reader.optional_text();
    route.cleanup_symbols = reader.flag();
    route.partition = read_partition(reader);
    context.routes.push_back(std::move(route));
  }
  context.stored = static_cast<int>(reader.i64());
  context.headers.indexed = static_cast<int>(reader.i64());
  context.headers.symbols = static_cast<int>(reader.i64());
  context.headers.already = static_cast<int>(reader.i64());
  context.headers.system = static_cast<int>(reader.i64());
  context.headers.unowned = static_cast<int>(reader.i64());
  const std::uint64_t diagnostics = reader.u64();
  if (diagnostics > bytes.size()) {
    throw TuReplayContextError("replay context diagnostic count exceeds the "
                               "payload");
  }
  context.diagnostics.reserve(static_cast<std::size_t>(diagnostics));
  for (std::uint64_t index = 0; index < diagnostics; ++index) {
    cidx::Diagnostic diagnostic;
    diagnostic.severity = static_cast<int>(reader.i64());
    diagnostic.spelling = reader.text();
    diagnostic.file_path = reader.optional_text();
    diagnostic.line = reader.optional_i64();
    diagnostic.col = reader.optional_i64();
    context.diagnostics.push_back(std::move(diagnostic));
  }
  reader.expect_end();
  if (context.translation_unit.empty() || context.routes.empty()) {
    throw TuReplayContextError("replay context has no publication route");
  }
  return context;
}

auto rebuild_route_plan(const TuReplayContext &context,
                        std::int64_t translation_unit_file_id)
    -> ast::OwnedHeaderRoutePlan {
  std::vector<ast::OwnedHeaderRouteCandidate> candidates;
  candidates.reserve(context.routes.size());
  for (const TuReplayRoute &route : context.routes) {
    candidates.push_back(
        {.role = route.role,
         .translation_unit = context.translation_unit,
         .translation_unit_file_id = translation_unit_file_id,
         .path = route.path,
         .discovery_ordinal = route.discovery_ordinal,
         .existing_file_id =
             route.is_translation_unit_row
                 ? std::optional<std::int64_t>(translation_unit_file_id)
                 : std::nullopt,
         .snapshot = {.mtime = route.mtime, .md5 = route.md5},
         .compile_options = route.compile_options,
         .driver = route.driver,
         .cleanup_symbols = route.cleanup_symbols,
         .partition = route.partition});
  }
  return ast::plan_owned_header_routes(context.generation,
                                       std::move(candidates));
}

} // namespace cidx::application
