#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ast/fact_batch_artifact.hpp"
#include "util/hashing.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

using namespace cidx;

namespace {

class TempDirectory {
public:
  TempDirectory() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("cidx-s100-artifact-" + std::to_string(nonce));
    std::filesystem::create_directory(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TempDirectory(const TempDirectory &) = delete;
  auto operator=(const TempDirectory &) -> TempDirectory & = delete;

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }

private:
  std::filesystem::path path_;
};

auto partition() -> ast::FactPartitionKey {
  return {
      .file = {.component_path = "/repo",
               .directory_path = "src",
               .file_name = "main.cpp"},
      .configuration = {.semantic_universe = "workspace:v1",
                        .translation_unit = "src/main.cpp",
                        .normalized_configuration = "sha256:config",
                        .identity_source = "compile-command",
                        .content = {.driver = "clang++",
                                    .working_dir = "/repo",
                                    .arguments = {"-std=c++23", "-DVALUE=1"},
                                    .lang_mode = "c++",
                                    .resource_dir = "/llvm/resource"}}};
}

auto fixture_batch(ast::FactCompleteness completeness =
                       ast::FactCompleteness::complete) -> ast::FactBatch {
  ast::FactBatchRecorder recorder("fact-batch-artifact-test");
  const auto route = partition();
  recorder.set_partition(route, 77);
  recorder.set_completeness(completeness);

  const auto first = recorder.mint_symbol({.usr = "c:@F@first#",
                                           .spelling = "first",
                                           .qual_name = "first()",
                                           .display_name = "first()",
                                           .type_info = "void ()",
                                           .kind_name = "function",
                                           .decl_file_id = 77,
                                           .decl_line = 4,
                                           .decl_col = 2,
                                           .decl_path = "src/main.cpp",
                                           .identity_source = "src/main.cpp",
                                           .linkage = "external",
                                           .callable_kind = "free",
                                           .template_origin = std::nullopt,
                                           .template_form = std::nullopt,
                                           .parent_usr = std::nullopt});
  const auto second = recorder.mint_symbol({.usr = "c:@F@second#",
                                            .spelling = "second",
                                            .qual_name = "second()",
                                            .display_name = "second()",
                                            .type_info = "int ()",
                                            .kind_name = "function",
                                            .decl_file_id = 77,
                                            .decl_line = 8,
                                            .decl_col = 2,
                                            .decl_path = "src/main.cpp",
                                            .identity_source = "src/main.cpp",
                                            .linkage = "internal",
                                            .callable_kind = "free",
                                            .template_origin = std::nullopt,
                                            .template_form = std::nullopt,
                                            .parent_usr = std::nullopt});

  const ast::SymbolNaturalKey first_key = {.partition = route,
                                           .usr = "c:@F@first#",
                                           .local_anchor = std::nullopt,
                                           .linkage = "external"};
  recorder.emit(ast::DeclarationSiteRecord{.symbol = first_key,
                                           .partition = route,
                                           .line = 4,
                                           .col = 2,
                                           .end_line = 6,
                                           .end_col = 1,
                                           .is_definition = true});
  const auto edge = recorder.add_edge({.src_id = first,
                                       .dst_id = second,
                                       .kind = 1,
                                       .count = 2,
                                       .base_access = 1,
                                       .is_virtual = 0});
  recorder.add_edge_site({.edge_id = edge,
                          .file_id = 77,
                          .line = 5,
                          .col = 3,
                          .conditional = 1,
                          .recv_src_kind = "local",
                          .recv_type_usr = "c:@S@Receiver",
                          .recv_decl_usr = "c:main.cpp@receiver",
                          .recv_param_pos = 0,
                          .recv_type_is_value = 1});
  recorder.add_call_arg({.edge_id = edge,
                         .file_id = 77,
                         .line = 5,
                         .col = 10,
                         .position = 0,
                         .src_kind = "literal",
                         .type_usr = "c:i",
                         .decl_usr = "c:arg",
                         .callee_usr = "c:@F@second#",
                         .type_is_value = 1});
  recorder.add_template_param({.owner_id = first,
                               .position = 0,
                               .param_kind = 1,
                               .name = "T",
                               .default_txt = "int",
                               .type_id = std::nullopt,
                               .default_type_id = std::nullopt,
                               .default_ref_id = std::nullopt});
  recorder.add_template_arg({.owner_id = second,
                             .position = 0,
                             .pack_index = -1,
                             .arg_kind = 2,
                             .ref_id = first,
                             .literal = "42",
                             .type_id = std::nullopt});
  const auto int_type = recorder.intern_type_node({.type_key = "builtin:int",
                                                   .spelling = "int",
                                                   .kind = 1,
                                                   .is_const = true,
                                                   .is_volatile = false,
                                                   .is_restrict = false,
                                                   .decl_usr = std::nullopt,
                                                   .canonical_id = std::nullopt,
                                                   .extent = "int"});
  const auto pointer_type =
      recorder.intern_type_node({.type_key = "pointer:builtin:int",
                                 .spelling = "const int *",
                                 .kind = 2,
                                 .is_const = false,
                                 .is_volatile = false,
                                 .is_restrict = false,
                                 .decl_usr = std::nullopt,
                                 .canonical_id = int_type,
                                 .extent = "const int *"});
  recorder.add_type_edge(pointer_type, 1, 0, int_type);
  recorder.replace_parameters(first, {{.position = 0,
                                       .pack_index = -1,
                                       .name = "value",
                                       .type_id = int_type,
                                       .declared_type_id = int_type,
                                       .adjusted_type_id = pointer_type,
                                       .default_text = "0",
                                       .default_origin = "source",
                                       .reference_semantics = "value",
                                       .file_id = 77,
                                       .line = 4,
                                       .col = 12}});
  recorder.add_symbol_type(first, 1, pointer_type);
  const auto definition = recorder.get_or_create_definition(
      first, 77, 4, 2, 6, 1, std::optional<std::string>("return;"));
  recorder.add_def_edge(definition, second, 1);

  const ast::PortableFileIdentity file = {.component_path = "/repo",
                                          .directory_path = "src",
                                          .file_name = "main.cpp"};
  const ast::PortableFileIdentity header = {.component_path = "/repo",
                                            .directory_path = "include",
                                            .file_name = "api.hpp"};
  recorder.emit(ast::IncludeDirectiveRecord{
      .partition = route,
      .source = file,
      .destination = header,
      .destination_path = "include/api.hpp",
      .spelling = "api.hpp",
      .directive = ast::IncludeDirectiveKind::include,
      .line = 1,
      .col = 1,
      .begin_offset = 0,
      .end_offset = 18,
      .conditional_fingerprint = "sha256:condition",
      .is_angled = false,
      .resolved = true,
      .is_system = false,
      .guarded = true});
  recorder.emit(ast::MacroUseRecord{.partition = route,
                                    .source = file,
                                    .definition = header,
                                    .definition_path = "include/api.hpp",
                                    .name = "API",
                                    .count = 3});
  recorder.emit(
      ast::DiagnosticFactRecord{.partition = route,
                                .severity = ast::DiagnosticSeverity::warning,
                                .spelling = "fixture warning",
                                .location_file = file,
                                .line = 5,
                                .col = 3});
  recorder.emit(ast::EvidenceRecord{.producer = "fixture-pass",
                                    .construct = "CallExpr",
                                    .file = "src/main.cpp",
                                    .line = 5,
                                    .col = 3,
                                    .completeness = completeness,
                                    .trust = ast::FactTrust::trusted,
                                    .detail = "direct AST evidence"});
  recorder.emit(ast::PresentationIntent{.symbol_id = first,
                                        .display_args = {"int", "value"}});
  recorder.emit(ast::LifecycleCleanupIntent{
      .partition = route,
      .kind = ast::LifecycleCleanupKind::relations,
      .target = file,
      .prior_generation = {.token = "sha256:prior-generation"}});
  recorder.emit(ast::ApplicabilityOwnershipRecord{
      .partition = route,
      .file = header,
      .role = ast::ApplicabilityRole::header,
      .state = ast::ApplicabilityState::registered,
      .reason = "owned header",
      .generation = {.token = "sha256:generation"}});
  return recorder.canonical_batch();
}

auto encode_options(const std::filesystem::path &spill = {})
    -> ast::FactBatchArtifactEncodeOptions {
  return {.metadata = {.pass_version = 3,
                       .extractor_version = 4,
                       .storage_schema_version = 40,
                       .catalog_version = 7,
                       .catalog_hash = "sha256:golden-catalog",
                       .trust = ast::FactTrust::trusted,
                       .truncated = false,
                       .dependency_identities = {"sha256:dep-b", "sha256:dep-a",
                                                 "sha256:dep-a"}},
          .spill_threshold_bytes = std::size_t{8} * 1024U * 1024U,
          .spill_directory = spill.string()};
}

auto compatibility(const ast::FactBatchArtifactEncodeOptions &options)
    -> ast::FactBatchArtifactCompatibility {
  return {.wire_version = ast::kFactBatchArtifactWireVersion,
          .producer = "fact-batch-artifact-test",
          .producer_version = 1,
          .pass_version = options.metadata.pass_version,
          .extractor_version = options.metadata.extractor_version,
          .storage_schema_version = options.metadata.storage_schema_version,
          .catalog_version = options.metadata.catalog_version,
          .catalog_hash = options.metadata.catalog_hash,
          .dependency_identities = options.metadata.dependency_identities,
          .allow_partial = false,
          .allow_truncated = false,
          .require_trusted = true,
          .limits = {}};
}

auto wire_compatibility(const ast::FactBatchArtifactEncodeOptions &options)
    -> ast::FactBatchArtifactCompatibility {
  auto result = compatibility(options);
  result.dependency_identities.reset();
  return result;
}

auto bytes(const ast::FactBatchArtifact &artifact) -> std::vector<std::byte> {
  std::ostringstream output(std::ios::binary);
  artifact.write_to(output);
  const auto text = output.str();
  const auto raw = std::as_bytes(std::span(text.data(), text.size()));
  return {raw.begin(), raw.end()};
}

void refresh_test_digest(std::vector<std::byte> &artifact_bytes) {
  constexpr std::size_t kFooterMagicSize = 8;
  constexpr std::size_t kDigestTextSize = 71;
  constexpr std::size_t kFooterSize = kFooterMagicSize + kDigestTextSize;
  const auto payload = std::span<const std::byte>(artifact_bytes)
                           .first(artifact_bytes.size() - kFooterSize);
  const auto digest = cidx::sha256_hex(payload);
  const auto digest_bytes =
      std::as_bytes(std::span(digest.data(), digest.size()));
  std::ranges::copy(digest_bytes,
                    artifact_bytes.end() -
                        static_cast<std::ptrdiff_t>(kDigestTextSize));
}

class TestWireWriter {
public:
  void bytes(std::span<const std::byte> value) {
    payload_.insert(payload_.end(), value.begin(), value.end());
  }

  void u8(std::uint8_t value) {
    payload_.push_back(static_cast<std::byte>(value));
  }

  void u32(std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
      u8(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
    }
  }

  void u64(std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
      u8(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
    }
  }

  void i64(std::int64_t value) { u64(std::bit_cast<std::uint64_t>(value)); }

  void string(std::string_view value) {
    u64(value.size());
    bytes(std::as_bytes(std::span(value.data(), value.size())));
  }

  void partition_key(const ast::FactPartitionKey &value) {
    string(value.file.component_path);
    string(value.file.directory_path);
    string(value.file.file_name);
    string(value.configuration.semantic_universe);
    string(value.configuration.translation_unit);
    string(value.configuration.normalized_configuration);
    string(value.configuration.identity_source);
    const auto &content = value.configuration.content;
    optional_string(content.driver);
    optional_string(content.working_dir);
    u64(content.arguments.size());
    for (const auto &argument : content.arguments) {
      string(argument);
    }
    optional_string(content.lang_mode);
    optional_string(content.resource_dir);
  }

  [[nodiscard]] auto finish() const -> std::vector<std::byte> {
    auto result = payload_;
    constexpr std::array<std::byte, 8> footer_magic = {
        std::byte{'F'}, std::byte{'B'}, std::byte{'D'}, std::byte{'I'},
        std::byte{'G'}, std::byte{'0'}, std::byte{'0'}, std::byte{'1'}};
    const auto digest = cidx::sha256_hex(std::span<const std::byte>(payload_));
    result.insert(result.end(), footer_magic.begin(), footer_magic.end());
    const auto digest_bytes =
        std::as_bytes(std::span(digest.data(), digest.size()));
    result.insert(result.end(), digest_bytes.begin(), digest_bytes.end());
    return result;
  }

private:
  void optional_string(const std::optional<std::string> &value) {
    u8(value.has_value() ? 1U : 0U);
    if (value) {
      string(*value);
    }
  }

  std::vector<std::byte> payload_;
};

constexpr std::array<std::byte, 8> kTestWireMagic = {
    std::byte{'C'}, std::byte{'I'}, std::byte{'D'}, std::byte{'X'},
    std::byte{'F'}, std::byte{'B'}, std::byte{'0'}, std::byte{'1'}};

void write_test_info(TestWireWriter &writer,
                     const std::vector<std::string> &dependencies = {},
                     std::uint8_t completeness = 0, std::uint8_t trust = 0,
                     std::uint8_t truncated = 0) {
  writer.bytes(kTestWireMagic);
  writer.u32(ast::kFactBatchArtifactWireVersion);
  writer.string("fact-batch-artifact-test");
  writer.u32(1);
  writer.u32(3);
  writer.u32(4);
  writer.u32(40);
  writer.i64(7);
  writer.string("sha256:golden-catalog");
  writer.u8(completeness);
  writer.u8(trust);
  writer.u8(truncated);
  writer.u64(dependencies.size());
  for (const auto &dependency : dependencies) {
    writer.string(dependency);
  }
}

void write_empty_record_vectors(TestWireWriter &writer) {
  for (std::size_t family = 0; family < 21; ++family) {
    writer.u64(0);
  }
}

void write_empty_maps(TestWireWriter &writer) {
  for (std::size_t map = 0; map < 5; ++map) {
    writer.u64(0);
  }
}

auto empty_wire_bytes() -> std::vector<std::byte> {
  TestWireWriter writer;
  write_test_info(writer);
  write_empty_record_vectors(writer);
  writer.u64(0);
  write_empty_maps(writer);
  return writer.finish();
}

auto resident_set_bytes() -> std::uint64_t {
#ifdef __APPLE__
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
    throw std::runtime_error("task_info failed");
  }
  return info.resident_size;
#else
  std::ifstream status("/proc/self/statm");
  std::uint64_t total_pages = 0;
  std::uint64_t resident_pages = 0;
  if (!(status >> total_pages >> resident_pages)) {
    throw std::runtime_error("cannot read process resident memory");
  }
  return resident_pages * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
}

auto first_diagnostic(const ast::FactBatchArtifactDecodeResult &result)
    -> ast::FactBatchArtifactErrorCode {
  if (result.diagnostics.empty()) {
    throw std::logic_error("expected a FactBatch artifact diagnostic");
  }
  return result.diagnostics.front().code;
}

auto require_info(const ast::FactBatchArtifactDecodeResult &result)
    -> const ast::FactBatchArtifactInfo & {
  if (!result.info.has_value()) {
    throw std::logic_error("expected FactBatch artifact metadata");
  }
  return result.info.value();
}

auto require_batch(const ast::FactBatchArtifactDecodeResult &result)
    -> const ast::FactBatch & {
  if (!result.batch.has_value()) {
    throw std::logic_error("expected a decoded FactBatch");
  }
  return result.batch.value();
}

void require_diagnostic(const ast::FactBatchArtifactDecodeResult &result,
                        ast::FactBatchArtifactErrorCode expected) {
  const auto actual = first_diagnostic(result);
  if (actual != expected) {
    throw std::logic_error("unexpected FactBatch artifact diagnostic: " +
                           std::string(ast::to_string(actual)));
  }
}

} // namespace

TEST_CASE("canonical FactBatch artifact round trips every fact family") {
  const auto batch = fixture_batch();
  const auto options = encode_options();
  const auto artifact = ast::encode_fact_batch_artifact(batch, options);
  const auto result =
      ast::decode_fact_batch_artifact(artifact, compatibility(options));

  REQUIRE(result.usable());
  const auto &info = require_info(result);
  CHECK(info.producer == "fact-batch-artifact-test");
  CHECK(info.producer_version == 1);
  CHECK(info.pass_version == 3);
  CHECK(info.extractor_version == 4);
  CHECK(info.storage_schema_version == 40);
  CHECK(info.catalog_version == 7);
  CHECK(info.catalog_hash == "sha256:golden-catalog");
  CHECK(info.completeness == ast::FactCompleteness::complete);
  CHECK(info.trust == ast::FactTrust::trusted);
  CHECK_FALSE(info.truncated);
  CHECK(info.dependency_identities ==
        std::vector<std::string>{"sha256:dep-a", "sha256:dep-b"});
  CHECK(info.content_digest ==
        "sha256:"
        "09beca8c16f8d49d60fe5eebbc151f46a9937fdd1d267e8d7b14fb2febcfa2e7");

  const auto replay =
      ast::encode_fact_batch_artifact(require_batch(result), options);
  CHECK(bytes(replay) == bytes(artifact));
}

TEST_CASE("FactBatch artifact rejects noncanonical input") {
  ast::FactBatchRecorder recorder("snapshot");
  const auto snapshot = recorder.snapshot();
  try {
    static_cast<void>(ast::encode_fact_batch_artifact(snapshot));
    FAIL("expected a noncanonical artifact rejection");
  } catch (const ast::FactBatchArtifactError &error) {
    CHECK(error.code() == ast::FactBatchArtifactErrorCode::noncanonical);
  }
}

TEST_CASE("FactBatch artifact rejects corrupt truncated and oversized bytes") {
  const auto options = encode_options();
  const auto artifact =
      ast::encode_fact_batch_artifact(fixture_batch(), options);
  auto corrupted = bytes(artifact);
  corrupted.at(corrupted.size() / 2U) ^= std::byte{0x01};
  auto result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(std::move(corrupted)),
      compatibility(options));
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  auto shortened = bytes(artifact);
  shortened.resize(shortened.size() - 10U);
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(std::move(shortened)),
      compatibility(options));
  const auto shortened_code = first_diagnostic(result);
  if (shortened_code != ast::FactBatchArtifactErrorCode::corrupt &&
      shortened_code != ast::FactBatchArtifactErrorCode::truncated) {
    throw std::logic_error("unexpected shortened artifact diagnostic");
  }

  auto limited = compatibility(options);
  limited.limits.max_artifact_bytes = artifact.byte_size() - 1U;
  result = ast::decode_fact_batch_artifact(artifact, limited);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::limit_exceeded);

  auto string_limited = compatibility(options);
  string_limited.limits.max_string_bytes = 2;
  result = ast::decode_fact_batch_artifact(artifact, string_limited);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::limit_exceeded);
}

TEST_CASE("FactBatch decoder rejects hostile declarations before allocation") {
  const auto options = encode_options();
  const auto expected = wire_compatibility(options);

  TestWireWriter huge_string;
  huge_string.bytes(kTestWireMagic);
  huge_string.u32(ast::kFactBatchArtifactWireVersion);
  huge_string.u64(std::uint64_t{16} * 1024U * 1024U);
  auto result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(huge_string.finish()), expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::truncated);

  TestWireWriter huge_collection;
  write_test_info(huge_collection);
  huge_collection.u64(10'000'000);
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(huge_collection.finish()), expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::truncated);

  TestWireWriter one_dependency;
  write_test_info(one_dependency, {"sha256:dependency"});
  write_empty_record_vectors(one_dependency);
  one_dependency.u64(0);
  write_empty_maps(one_dependency);
  const auto dependency_artifact =
      ast::FactBatchArtifact::from_bytes(one_dependency.finish());
  auto collection_limited = expected;
  collection_limited.limits.max_collection_entries = 0;
  result =
      ast::decode_fact_batch_artifact(dependency_artifact, collection_limited);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::limit_exceeded);
  auto total_limited = expected;
  total_limited.limits.max_total_entries = 0;
  result = ast::decode_fact_batch_artifact(dependency_artifact, total_limited);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::limit_exceeded);
}

TEST_CASE("FactBatch decoder exercises authenticated corruption branches") {
  const auto options = encode_options();
  const auto expected = wire_compatibility(options);

  TestWireWriter invalid_boolean;
  write_test_info(invalid_boolean, {}, 0, 0, 2);
  auto result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(invalid_boolean.finish()), expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  TestWireWriter invalid_enum;
  write_test_info(invalid_enum, {}, 4);
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(invalid_enum.finish()), expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  TestWireWriter unsorted_dependencies;
  write_test_info(unsorted_dependencies, {"sha256:z", "sha256:a"});
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(unsorted_dependencies.finish()),
      expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  TestWireWriter duplicate_handle;
  write_test_info(duplicate_handle);
  write_empty_record_vectors(duplicate_handle);
  duplicate_handle.u64(0);
  duplicate_handle.u64(2);
  duplicate_handle.i64(7);
  duplicate_handle.string("first");
  duplicate_handle.i64(7);
  duplicate_handle.string("second");
  for (std::size_t map = 0; map < 4; ++map) {
    duplicate_handle.u64(0);
  }
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(duplicate_handle.finish()), expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  TestWireWriter unsorted_handles;
  write_test_info(unsorted_handles);
  write_empty_record_vectors(unsorted_handles);
  unsorted_handles.u64(0);
  unsorted_handles.u64(2);
  unsorted_handles.i64(8);
  unsorted_handles.string("later");
  unsorted_handles.i64(7);
  unsorted_handles.string("earlier");
  for (std::size_t map = 0; map < 4; ++map) {
    unsorted_handles.u64(0);
  }
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(unsorted_handles.finish()), expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  TestWireWriter duplicate_family;
  write_test_info(duplicate_family);
  write_empty_record_vectors(duplicate_family);
  duplicate_family.u64(1);
  duplicate_family.partition_key(partition());
  duplicate_family.u64(2);
  duplicate_family.u8(std::to_underlying(ast::FactFamily::symbols));
  duplicate_family.u64(0);
  duplicate_family.u8(std::to_underlying(ast::FactFamily::symbols));
  duplicate_family.u64(0);
  write_empty_maps(duplicate_family);
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(duplicate_family.finish()), expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  TestWireWriter unsorted_families;
  write_test_info(unsorted_families);
  write_empty_record_vectors(unsorted_families);
  unsorted_families.u64(1);
  unsorted_families.partition_key(partition());
  unsorted_families.u64(2);
  unsorted_families.u8(std::to_underlying(ast::FactFamily::evidence));
  unsorted_families.u64(0);
  unsorted_families.u8(std::to_underlying(ast::FactFamily::symbols));
  unsorted_families.u64(0);
  write_empty_maps(unsorted_families);
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(unsorted_families.finish()), expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  TestWireWriter invalid_family;
  write_test_info(invalid_family);
  write_empty_record_vectors(invalid_family);
  invalid_family.u64(1);
  invalid_family.partition_key(partition());
  invalid_family.u64(1);
  invalid_family.u8(20);
  invalid_family.u64(0);
  write_empty_maps(invalid_family);
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(invalid_family.finish()), expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  TestWireWriter out_of_range_member;
  write_test_info(out_of_range_member);
  write_empty_record_vectors(out_of_range_member);
  out_of_range_member.u64(1);
  out_of_range_member.partition_key(partition());
  out_of_range_member.u64(1);
  out_of_range_member.u8(std::to_underlying(ast::FactFamily::symbols));
  out_of_range_member.u64(1);
  out_of_range_member.u64(0);
  write_empty_maps(out_of_range_member);
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(out_of_range_member.finish()),
      expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  TestWireWriter trailing_payload;
  write_test_info(trailing_payload);
  write_empty_record_vectors(trailing_payload);
  trailing_payload.u64(0);
  write_empty_maps(trailing_payload);
  trailing_payload.u8(0xff);
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(trailing_payload.finish()), expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  auto unsupported_magic = empty_wire_bytes();
  unsupported_magic.front() ^= std::byte{0x01};
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(std::move(unsupported_magic)),
      expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::incompatible);

  auto unsupported_version = empty_wire_bytes();
  unsupported_version.at(kTestWireMagic.size()) = std::byte{0x02};
  refresh_test_digest(unsupported_version);
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(std::move(unsupported_version)),
      expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::incompatible);

  auto malformed_footer_magic = empty_wire_bytes();
  malformed_footer_magic.at(malformed_footer_magic.size() - 79U) ^=
      std::byte{0x01};
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(std::move(malformed_footer_magic)),
      expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);
  auto malformed_digest = empty_wire_bytes();
  malformed_digest.at(malformed_digest.size() - 71U) = std::byte{'x'};
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(std::move(malformed_digest)),
      expected);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);
}

TEST_CASE("FactBatch decoder rejects authenticated noncanonical ordering") {
  const auto options = encode_options();

  TestWireWriter evidence_order;
  write_test_info(evidence_order);
  for (std::size_t family = 0; family < 16; ++family) {
    evidence_order.u64(0);
  }
  evidence_order.u64(2);
  for (const std::string_view producer : {"z-producer", "a-producer"}) {
    evidence_order.string(producer);
    evidence_order.string("construct");
    evidence_order.string("src/main.cpp");
    evidence_order.i64(1);
    evidence_order.i64(1);
    evidence_order.u8(std::to_underlying(ast::FactCompleteness::complete));
    evidence_order.u8(std::to_underlying(ast::FactTrust::trusted));
    evidence_order.string("detail");
  }
  for (std::size_t family = 17; family < 21; ++family) {
    evidence_order.u64(0);
  }
  evidence_order.u64(1);
  evidence_order.partition_key(partition());
  evidence_order.u64(1);
  evidence_order.u8(std::to_underlying(ast::FactFamily::evidence));
  evidence_order.u64(2);
  evidence_order.u64(0);
  evidence_order.u64(1);
  write_empty_maps(evidence_order);
  auto result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(evidence_order.finish()),
      wire_compatibility(options));
  INFO("evidence record order");
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  TestWireWriter partition_order;
  write_test_info(partition_order);
  write_empty_record_vectors(partition_order);
  partition_order.u64(2);
  auto later = partition();
  later.file.file_name = "z.cpp";
  partition_order.partition_key(later);
  partition_order.u64(0);
  auto earlier = partition();
  earlier.file.file_name = "a.cpp";
  partition_order.partition_key(earlier);
  partition_order.u64(0);
  write_empty_maps(partition_order);
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(partition_order.finish()),
      wire_compatibility(options));
  INFO("partition order");
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  TestWireWriter symbol_order;
  write_test_info(symbol_order);
  for (std::size_t family = 0; family < 20; ++family) {
    symbol_order.u64(0);
  }
  symbol_order.u64(1);
  symbol_order.partition_key(partition());
  symbol_order.string("c:@F@symbol#");
  symbol_order.u8(0);
  symbol_order.u8(0);
  symbol_order.string("/repo");
  symbol_order.string("src");
  symbol_order.string("main.cpp");
  symbol_order.u64(0);
  symbol_order.u64(1);
  symbol_order.string("record-key");
  symbol_order.u64(0);
  symbol_order.u64(0);
  symbol_order.u64(0);
  write_empty_maps(symbol_order);
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(symbol_order.finish()),
      wire_compatibility(options));
  INFO("symbol emission order");
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);

  TestWireWriter partition_ownership;
  write_test_info(partition_ownership);
  for (std::size_t family = 0; family < 15; ++family) {
    partition_ownership.u64(0);
  }
  partition_ownership.u64(1);
  partition_ownership.partition_key(partition());
  partition_ownership.u8(std::to_underlying(ast::DiagnosticSeverity::warning));
  partition_ownership.string("mismatched ownership");
  partition_ownership.u8(0);
  partition_ownership.u8(0);
  partition_ownership.u8(0);
  for (std::size_t family = 16; family < 21; ++family) {
    partition_ownership.u64(0);
  }
  partition_ownership.u64(1);
  auto mismatched_partition = partition();
  mismatched_partition.file.file_name = "other.cpp";
  partition_ownership.partition_key(mismatched_partition);
  partition_ownership.u64(1);
  partition_ownership.u8(std::to_underlying(ast::FactFamily::diagnostics));
  partition_ownership.u64(1);
  partition_ownership.u64(0);
  write_empty_maps(partition_ownership);
  result = ast::decode_fact_batch_artifact(
      ast::FactBatchArtifact::from_bytes(partition_ownership.finish()),
      wire_compatibility(options));
  INFO("record partition ownership");
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::corrupt);
}

TEST_CASE("FactBatch compatibility rejects stale incompatible and partial "
          "artifacts") {
  const auto options = encode_options();
  const auto artifact =
      ast::encode_fact_batch_artifact(fixture_batch(), options);

  auto incompatible = compatibility(options);
  ++incompatible.pass_version;
  auto result = ast::decode_fact_batch_artifact(artifact, incompatible);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::incompatible);

  auto stale = compatibility(options);
  stale.dependency_identities = std::vector<std::string>{"sha256:new-dep"};
  result = ast::decode_fact_batch_artifact(artifact, stale);
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::stale);

  const auto partial_artifact = ast::encode_fact_batch_artifact(
      fixture_batch(ast::FactCompleteness::partial), options);
  result =
      ast::decode_fact_batch_artifact(partial_artifact, compatibility(options));
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::partial);

  auto truncated_options = options;
  truncated_options.metadata.truncated = true;
  const auto truncated_artifact =
      ast::encode_fact_batch_artifact(fixture_batch(), truncated_options);
  result = ast::decode_fact_batch_artifact(truncated_artifact,
                                           compatibility(truncated_options));
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::truncated);

  auto untrusted_options = options;
  untrusted_options.metadata.trust = ast::FactTrust::provisional;
  const auto untrusted_artifact =
      ast::encode_fact_batch_artifact(fixture_batch(), untrusted_options);
  result = ast::decode_fact_batch_artifact(untrusted_artifact,
                                           compatibility(untrusted_options));
  require_diagnostic(result, ast::FactBatchArtifactErrorCode::incompatible);
}

TEST_CASE("FactBatch artifact spill is bounded and transfer neutral") {
  TempDirectory temporary;
  auto options = encode_options(temporary.path());
  options.spill_threshold_bytes = 64;
  const auto artifact =
      ast::encode_fact_batch_artifact(fixture_batch(), options);
  CHECK(artifact.spilled());
  CHECK(artifact.peak_memory_bytes() <= options.spill_threshold_bytes);

  const auto materialized = temporary.path() / "transfer.fb";
  artifact.materialize(materialized.string());
  const auto loaded = ast::FactBatchArtifact::from_file(materialized.string());
  const auto result =
      ast::decode_fact_batch_artifact(loaded, compatibility(options));
  REQUIRE(result.usable());
  CHECK(require_info(result).byte_size == artifact.byte_size());
  CHECK(require_info(result).content_digest ==
        "sha256:"
        "09beca8c16f8d49d60fe5eebbc151f46a9937fdd1d267e8d7b14fb2febcfa2e7");
  CHECK(bytes(loaded) == bytes(artifact));

  REQUIRE(std::filesystem::remove(materialized));
  std::ofstream(materialized, std::ios::binary) << "replacement generation";
  const auto stable_result =
      ast::decode_fact_batch_artifact(loaded, compatibility(options));
  REQUIRE(stable_result.usable());
  CHECK(bytes(loaded) == bytes(artifact));

  const auto grown_path = temporary.path() / "grown.fb";
  artifact.materialize(grown_path.string());
  const auto grown = ast::FactBatchArtifact::from_file(grown_path.string());
  std::ofstream(grown_path, std::ios::binary | std::ios::app) << "extra bytes";
  CHECK(bytes(grown).size() == grown.byte_size());
  REQUIRE(
      ast::decode_fact_batch_artifact(grown, compatibility(options)).usable());

  const auto shortened_path = temporary.path() / "shortened.fb";
  artifact.materialize(shortened_path.string());
  const auto shortened =
      ast::FactBatchArtifact::from_file(shortened_path.string());
  std::filesystem::resize_file(shortened_path, artifact.byte_size() - 1U);
  try {
    std::ostringstream output(std::ios::binary);
    shortened.write_to(output);
    FAIL("expected a short backing file transfer failure");
  } catch (const ast::FactBatchArtifactError &error) {
    CHECK(error.code() == ast::FactBatchArtifactErrorCode::io_error);
  }
}

TEST_CASE("FactBatch artifact reports missing and spill failures") {
  TempDirectory temporary;
  try {
    static_cast<void>(ast::FactBatchArtifact::from_file(
        (temporary.path() / "missing.fb").string()));
    FAIL("expected a missing artifact rejection");
  } catch (const ast::FactBatchArtifactError &error) {
    CHECK(error.code() == ast::FactBatchArtifactErrorCode::missing);
  }

  const auto not_a_directory = temporary.path() / "not-a-directory";
  std::ofstream(not_a_directory) << "fixture";
  auto options = encode_options(not_a_directory);
  options.spill_threshold_bytes = 0;
  try {
    static_cast<void>(
        ast::encode_fact_batch_artifact(fixture_batch(), options));
    FAIL("expected a spill failure");
  } catch (const ast::FactBatchArtifactError &error) {
    CHECK(error.code() == ast::FactBatchArtifactErrorCode::spill_failure);
  }

  auto size_limited = encode_options(temporary.path());
  size_limited.max_artifact_bytes = 64;
  try {
    static_cast<void>(
        ast::encode_fact_batch_artifact(fixture_batch(), size_limited));
    FAIL("expected an encode byte limit failure");
  } catch (const ast::FactBatchArtifactError &error) {
    CHECK(error.code() == ast::FactBatchArtifactErrorCode::limit_exceeded);
  }
}

TEST_CASE("FactBatch artifact supports an unbuffered zero-memory spill") {
  TempDirectory temporary;
  auto options = encode_options(temporary.path());
  options.spill_threshold_bytes = 0;
  const auto artifact =
      ast::encode_fact_batch_artifact(fixture_batch(), options);
  CHECK(artifact.spilled());
  CHECK(artifact.peak_memory_bytes() == 0);
  CHECK(ast::decode_fact_batch_artifact(artifact, compatibility(options))
            .usable());
}

TEST_CASE(
    "FactBatch artifact benchmark records size throughput and peak memory") {
  TempDirectory temporary;
  ast::FactBatchRecorder recorder("fact-batch-artifact-benchmark");
  recorder.set_partition(partition());
  for (int index = 0; index < 20'000; ++index) {
    static_cast<void>(recorder.mint_symbol(
        {.usr = "c:@F@bench" + std::to_string(index) + "#",
         .spelling = "bench" + std::to_string(index),
         .qual_name = "bench" + std::to_string(index) + "()",
         .display_name = "bench()",
         .type_info = std::nullopt,
         .kind_name = "function",
         .decl_file_id = std::nullopt,
         .decl_line = std::nullopt,
         .decl_col = std::nullopt,
         .decl_path = "src/main.cpp",
         .is_instantiation = false,
         .is_named_instance = false,
         .identity_source = "src/main.cpp",
         .linkage = std::nullopt,
         .callable_kind = std::nullopt,
         .template_origin = std::nullopt,
         .template_form = std::nullopt,
         .parent_usr = std::nullopt}));
  }
  auto options = encode_options(temporary.path());
  options.spill_threshold_bytes = 200'000;
  const auto batch = recorder.canonical_batch();
  auto memory_options = options;
  memory_options.spill_threshold_bytes = std::size_t{64} * 1024U * 1024U;
  const auto memory_start = std::chrono::steady_clock::now();
  const auto memory_artifact =
      ast::encode_fact_batch_artifact(batch, memory_options);
  const auto memory_elapsed = std::chrono::steady_clock::now() - memory_start;
  const auto memory_milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(memory_elapsed)
          .count();

  const auto rss_before = resident_set_bytes();
  const auto start = std::chrono::steady_clock::now();
  const auto artifact = ast::encode_fact_batch_artifact(batch, options);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  const auto rss_after = resident_set_bytes();
  const auto rss_growth = rss_after > rss_before ? rss_after - rss_before : 0;
  CAPTURE(artifact.byte_size());
  CAPTURE(artifact.peak_memory_bytes());
  CAPTURE(memory_milliseconds);
  CAPTURE(milliseconds);
  CAPTURE(rss_growth);
  CHECK(artifact.byte_size() > 1U * 1024U * 1024U);
  CHECK_FALSE(memory_artifact.spilled());
  CHECK(artifact.spilled());
  CHECK(artifact.peak_memory_bytes() <= options.spill_threshold_bytes);
  constexpr std::size_t kRssSlack = std::size_t{8} * 1024U * 1024U;
  CHECK(rss_growth <= options.spill_threshold_bytes + kRssSlack);
  const auto spill_time_ceiling = (memory_milliseconds * 5) + 2'000;
  CHECK(milliseconds <= spill_time_ceiling);
  CHECK(bytes(memory_artifact) == bytes(artifact));
}
