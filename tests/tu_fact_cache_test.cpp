#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ast/fact_batch_artifact.hpp"
#include "catalogs/generated_catalog.hpp"
#include "profile/index_profile.hpp"
#include "storage/artifacts.hpp"
#include "storage/storage.hpp"
#include "storage/tu_dependency_planner.hpp"
#include "storage/tu_fact_cache.hpp"
#include "workspace/tu_fact_cache_identity.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

class TempDirectory {
public:
  explicit TempDirectory(std::string_view suffix) {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("cidx-s076-" + std::string(suffix) + '-' + std::to_string(nonce));
    std::filesystem::create_directories(path_);
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

auto identity_input(const std::filesystem::path &root)
    -> cidx::TuFactCacheIdentityInput {
  return {
      .main_source_path = (root / "src/main.cpp").string(),
      .main_source_sha256 = "sha256:main",
      .translation_unit_descriptor =
          R"({"configuration":"debug","diagnostics":"all"})",
      .clang_identity = "clang-22|resource:/llvm|target:arm64|abi:default",
      .front_end_reuse_identity = "front-end-reuse/v1:none",
      .versions = {},
      .environment = {{.name = "SDKROOT",
                       .value_sha256 = "sha256:sdk-environment"}},
      .dependencies = {{.path = (root / "include/../include/api.hpp").string(),
                        .content_sha256 = "sha256:api",
                        .kind = "include",
                        .conditional_context = "ifdef:FEATURE"},
                       {.path = (root / "generated/config.inc").string(),
                        .content_sha256 = "sha256:generated",
                        .kind = "generated",
                        .conditional_context = ""}},
  };
}

auto partition() -> cidx::ast::FactPartitionKey {
  return {.file = {.component_path = "/repo",
                   .directory_path = "src",
                   .file_name = "main.cpp"},
          .configuration = {.semantic_universe = "workspace:v1",
                            .translation_unit = "src/main.cpp",
                            .normalized_configuration = "sha256:config",
                            .identity_source = "compile-command",
                            .content = {.driver = "clang++",
                                        .working_dir = "/repo",
                                        .arguments = {"-std=c++23"},
                                        .lang_mode = "c++",
                                        .resource_dir = "/llvm/resource"}}};
}

auto fixture_batch() -> cidx::ast::FactBatch {
  cidx::ast::FactBatchRecorder recorder("tu-fact-cache-test");
  recorder.set_partition(partition(), 7);
  recorder.set_completeness(cidx::ast::FactCompleteness::complete);
  static_cast<void>(recorder.mint_symbol({.usr = "c:@F@cached#",
                                          .spelling = "cached",
                                          .qual_name = "cached()",
                                          .display_name = "cached()",
                                          .type_info = "void ()",
                                          .kind_name = "function",
                                          .decl_file_id = 7,
                                          .decl_line = 3,
                                          .decl_col = 1,
                                          .decl_path = "src/main.cpp",
                                          .identity_source = "src/main.cpp",
                                          .linkage = "external",
                                          .callable_kind = "free",
                                          .template_origin = std::nullopt,
                                          .template_form = std::nullopt,
                                          .parent_usr = std::nullopt}));
  recorder.emit(cidx::ast::DiagnosticFactRecord{
      .partition = partition(),
      .severity = cidx::ast::DiagnosticSeverity::warning,
      .spelling = "cached warning",
      .location_file =
          cidx::ast::PortableFileIdentity{.component_path = "/repo",
                                          .directory_path = "src",
                                          .file_name = "main.cpp"},
      .line = 3,
      .col = 1});
  return recorder.canonical_batch();
}

auto artifact_bytes(const cidx::ast::FactBatch &batch)
    -> std::vector<std::byte> {
  const auto artifact = cidx::ast::encode_fact_batch_artifact(batch);
  std::ostringstream output(std::ios::binary);
  artifact.write_to(output);
  const std::string encoded = output.str();
  std::vector<std::byte> bytes;
  bytes.reserve(encoded.size());
  for (const unsigned char value : encoded) {
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

auto replay_context_bytes() -> std::vector<std::byte> {
  constexpr std::string_view encoded =
      R"({"version":"tu-replay-context/v1","generation":"7"})";
  std::vector<std::byte> bytes;
  bytes.reserve(encoded.size());
  for (const unsigned char value : encoded) {
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

auto evidence(const std::filesystem::path &root)
    -> cidx::storage::TranslationUnitDependencyEvidence {
  return {
      .source = (root / "src/main.cpp").string(),
      .configuration = "debug",
      .generation = "sha256:cache-v1",
      .state = cidx::storage::DependencyEvidenceState::complete,
      .edges = {{.source = (root / "src/main.cpp").string(),
                 .destination = (root / "generated/middle.inc").string(),
                 .destination_content_sha256 = "sha256:middle",
                 .conditional_context = "",
                 .provenance = "generated",
                 .kind = cidx::storage::TuDependencyKind::generated,
                 .resolved = true,
                 .system = false},
                {.source = (root / "generated/middle.inc").string(),
                 .destination = (root / "generated/leaf.def").string(),
                 .destination_content_sha256 = "sha256:leaf",
                 .conditional_context = "ifdef:FEATURE",
                 .provenance = "generated",
                 .kind = cidx::storage::TuDependencyKind::generated,
                 .resolved = true,
                 .system = false}},
  };
}

} // namespace

TEST_CASE("TU cache identity is canonical and content addressed") {
  TempDirectory root("identity");
  auto first_input = identity_input(root.path());
  auto reversed = first_input;
  std::ranges::reverse(reversed.dependencies);
  reversed.dependencies.back().path =
      (root.path() / "include/api.hpp").string();
  const auto first = cidx::make_tu_fact_cache_identity(first_input);
  const auto second = cidx::make_tu_fact_cache_identity(reversed);
  CHECK(first.version == "tu-fact-cache/v1");
  CHECK(first.canonical_bytes == second.canonical_bytes);
  CHECK(first.sha256 == second.sha256);
  CHECK(first.canonical_bytes.find("mtime") == std::string::npos);

  const auto changes = [&first](auto input, auto mutate) {
    mutate(input);
    CHECK(cidx::make_tu_fact_cache_identity(std::move(input)).sha256 !=
          first.sha256);
  };
  changes(identity_input(root.path()),
          [](auto &input) { input.main_source_sha256 = "sha256:changed"; });
  changes(identity_input(root.path()),
          [](auto &input) { input.translation_unit_descriptor += "changed"; });
  changes(identity_input(root.path()),
          [](auto &input) { input.clang_identity += "changed"; });
  changes(identity_input(root.path()), [](auto &input) {
    input.front_end_reuse_identity = "front-end-reuse/v1:disabled";
  });
  changes(identity_input(root.path()), [](auto &input) {
    input.front_end_reuse_identity = "front-end-reuse/v1:pch-enabled";
  });
  changes(identity_input(root.path()), [](auto &input) {
    input.environment.front().value_sha256 = "sha256:changed";
  });
  changes(identity_input(root.path()), [](auto &input) {
    input.dependencies.front().content_sha256 = "sha256:changed";
  });
  changes(identity_input(root.path()), [](auto &input) {
    input.dependencies.front().conditional_context = "ifdef:OTHER";
  });
  changes(identity_input(root.path()),
          [](auto &input) { input.environment.front().name = "CPATH"; });
  changes(identity_input(root.path()),
          [](auto &input) { input.versions.product_version = "0.0.0-test"; });
  changes(identity_input(root.path()),
          [](auto &input) { ++input.versions.artifact_version; });
  changes(identity_input(root.path()),
          [](auto &input) { ++input.versions.extractor_version; });
  changes(identity_input(root.path()),
          [](auto &input) { ++input.versions.pass_version; });
  changes(identity_input(root.path()),
          [](auto &input) { ++input.versions.catalog_version; });
  changes(identity_input(root.path()),
          [](auto &input) { input.versions.catalog_hash = "sha256:changed"; });
  changes(identity_input(root.path()),
          [](auto &input) { ++input.versions.storage_schema_version; });
}

TEST_CASE("symlink spelling is conservatively identity-distinct") {
  TempDirectory root("symlink");
  std::filesystem::create_directories(root.path() / "real");
  {
    std::ofstream target(root.path() / "real/header.hpp");
    target << "#pragma once\n";
  }
  std::error_code error;
  std::filesystem::create_directory_symlink(root.path() / "real",
                                            root.path() / "alias", error);
  if (error) {
    return;
  }
  auto direct = identity_input(root.path());
  direct.dependencies.front().path = (root.path() / "real/header.hpp").string();
  auto alias = direct;
  alias.dependencies.front().path = (root.path() / "alias/header.hpp").string();
  CHECK(cidx::make_tu_fact_cache_identity(direct).sha256 !=
        cidx::make_tu_fact_cache_identity(alias).sha256);
}

TEST_CASE("reverse planner crosses generated intermediates exactly once") {
  TempDirectory root("planner");
  auto first = evidence(root.path());
  auto second = first;
  second.source = (root.path() / "src/other.cpp").string();
  second.configuration = "release";
  second.edges.front().source = second.source;
  auto unaffected = first;
  unaffected.source = (root.path() / "src/unaffected.cpp").string();
  unaffected.configuration = "debug";
  unaffected.edges = {
      {.source = unaffected.source,
       .destination = (root.path() / "include/other.hpp").string(),
       .conditional_context = "",
       .provenance = "include",
       .kind = cidx::storage::TuDependencyKind::include,
       .resolved = true,
       .system = false}};

  const auto plan = cidx::storage::plan_affected_translation_units(
      {(root.path() / "generated/leaf.def").string()},
      {unaffected, second, first});
  REQUIRE(plan.complete);
  CHECK(plan.affected_count == 2);
  CHECK(plan.proven_unaffected_count == 1);
  CHECK(plan.visited_dependency_edges == 4);
  CHECK(plan.affected.front().source.ends_with("src/main.cpp"));
  CHECK(plan.affected.back().source.ends_with("src/other.cpp"));

  first.edges.push_back(
      {.source = (root.path() / "generated/middle.inc").string(),
       .destination = first.source,
       .conditional_context = "cycle",
       .provenance = "include",
       .kind = cidx::storage::TuDependencyKind::include,
       .resolved = true,
       .system = false});
  const auto cycle = cidx::storage::plan_affected_translation_units(
      {(root.path() / "generated/leaf.def").string()}, {first});
  CHECK(cycle.complete);
  CHECK(cycle.affected_count == 1);
}

TEST_CASE(
    "dependency capture preserves external macro and generated evidence") {
  TempDirectory root("capture");
  const std::string source = (root.path() / "src/main.cpp").string();
  const std::string generated = (root.path() / "generated/config.inc").string();
  const std::string definition = (root.path() / "generated/flags.def").string();
  const auto captured =
      cidx::storage::capture_translation_unit_dependency_evidence(
          source, "debug", "sha256:generation",
          {{.source = source,
            .destination = generated,
            .conditional_context = "ifdef:FEATURE",
            .provenance = "generator:configure",
            .kind = cidx::storage::TuDependencyKind::generated,
            .resolved = true,
            .system = false},
           {.source = generated,
            .destination = definition,
            .conditional_context = "macro:FEATURE",
            .provenance = "macro-expansion",
            .kind = cidx::storage::TuDependencyKind::macro,
            .resolved = true,
            .system = false}},
          true);
  CHECK(captured.state == cidx::storage::DependencyEvidenceState::complete);
  REQUIRE(captured.edges.size() == 2);
  CHECK(captured.edges.front().provenance == "macro-expansion");
  CHECK(captured.edges.back().kind ==
        cidx::storage::TuDependencyKind::generated);

  auto unresolved = captured.edges;
  unresolved.front().resolved = false;
  CHECK(cidx::storage::capture_translation_unit_dependency_evidence(
            source, "debug", "sha256:generation", std::move(unresolved), true)
            .state == cidx::storage::DependencyEvidenceState::incomplete);
}

TEST_CASE("incomplete dependency evidence falls back conservatively") {
  TempDirectory root("fallback");
  auto first = evidence(root.path());
  auto second = first;
  second.source = (root.path() / "src/other.cpp").string();
  second.configuration = "release";
  first.edges.back().resolved = false;
  const auto plan = cidx::storage::plan_affected_translation_units(
      {(root.path() / "generated/leaf.def").string()}, {second, first});
  CHECK_FALSE(plan.complete);
  CHECK(plan.affected_count == 2);
  CHECK(plan.proven_unaffected_count == 0);
  CHECK(plan.fallback_reason ==
        cidx::storage::DependencyFallbackReason::unresolved_dependency);
}

TEST_CASE("missing dependency evidence cannot prove a cache hit") {
  const auto plan = cidx::storage::plan_affected_translation_units(
      {"/generated/missing.inc"}, {});
  CHECK_FALSE(plan.complete);
  CHECK(plan.affected_count == 0);
  CHECK(plan.proven_unaffected_count == 0);
  CHECK(plan.fallback_reason ==
        cidx::storage::DependencyFallbackReason::unavailable_evidence);
}

TEST_CASE("all invalid evidence states re-extract every known target") {
  TempDirectory root("evidence-states");
  auto first = evidence(root.path());
  auto second = first;
  second.source = (root.path() / "src/other.cpp").string();
  const std::vector states{
      cidx::storage::DependencyEvidenceState::incomplete,
      cidx::storage::DependencyEvidenceState::stale,
      cidx::storage::DependencyEvidenceState::corrupt,
      cidx::storage::DependencyEvidenceState::unavailable,
  };
  for (const auto state : states) {
    first.state = state;
    const auto plan = cidx::storage::plan_affected_translation_units(
        {(root.path() / "unrelated.hpp").string()}, {second, first});
    CHECK_FALSE(plan.complete);
    CHECK(plan.affected_count == 2);
    CHECK(plan.proven_unaffected_count == 0);
  }
}

TEST_CASE("high fan-in and configuration variants remain exact") {
  TempDirectory root("fan-in");
  const std::string shared = (root.path() / "include/shared.hpp").string();
  std::vector<cidx::storage::TranslationUnitDependencyEvidence> units;
  for (int index = 0; index < 128; ++index) {
    const std::string source =
        (root.path() / ("src/tu_" + std::to_string(index) + ".cpp")).string();
    units.push_back(
        {.source = source,
         .configuration = index == 0 ? "debug" : "release",
         .generation = "generation:" + std::to_string(index),
         .state = cidx::storage::DependencyEvidenceState::complete,
         .edges = {{.source = source,
                    .destination = shared,
                    .conditional_context = index == 0 ? "ifdef:FEATURE" : "",
                    .provenance = "include",
                    .kind = cidx::storage::TuDependencyKind::include,
                    .resolved = true,
                    .system = false}}});
  }
  auto debug_variant = units.front();
  debug_variant.configuration = "asan";
  debug_variant.generation = "generation:asan";
  units.push_back(debug_variant);

  const auto plan =
      cidx::storage::plan_affected_translation_units({shared}, units);
  CHECK(plan.complete);
  CHECK(plan.affected_count == 129);
  CHECK(plan.proven_unaffected_count == 0);
  CHECK(plan.visited_dependency_edges == 129);
}

TEST_CASE("FactBatch cache round trips through the SQLite sidecar") {
  TempDirectory root("sidecar");
  cidx::Storage storage(":memory:");
  cidx::storage::TuFactCache cache(storage, root.path().string());
  const auto batch = fixture_batch();
  const auto bytes = artifact_bytes(batch);
  auto dependencies = evidence(root.path());
  const cidx::storage::TuFactCachePublication publication{
      .workspace_identity = "workspace:test",
      .source_identity = dependencies.source,
      .configuration_identity = dependencies.configuration,
      .cache_identity = dependencies.generation,
      .fact_batch_artifact = bytes,
      .replay_context = replay_context_bytes(),
      .dependency_evidence = dependencies,
  };
  const auto record = cache.publish(publication);
  CHECK(record.content_hash.starts_with("sha256:"));
  CHECK(record.byte_size > 0);

  const auto hit = cache.lookup(
      publication.workspace_identity, publication.source_identity,
      publication.configuration_identity, publication.cache_identity);
  INFO("status=" << cidx::storage::to_string(hit.status));
  for (const auto &diagnostic : hit.diagnostics) {
    INFO(diagnostic.code << ": " << diagnostic.message);
  }
  REQUIRE(hit.usable());
  CHECK(hit.fact_batch_artifact == bytes);
  CHECK(hit.replay_context == replay_context_bytes());
  const auto decoded = cidx::ast::decode_fact_batch_artifact(
      cidx::ast::FactBatchArtifact::from_bytes(hit.fact_batch_artifact));
  REQUIRE(decoded.usable());
  REQUIRE(decoded.batch.has_value());
  const auto &decoded_batch = *decoded.batch;
  CHECK(decoded_batch.records().symbols.size() == 1);
  CHECK(decoded_batch.records().diagnostics.size() == 1);
  CHECK(hit.dependency_evidence.edges.size() == 2);

  const auto stale =
      cache.lookup(publication.workspace_identity, publication.source_identity,
                   publication.configuration_identity, "sha256:changed");
  CHECK(stale.status == cidx::storage::TuFactCacheStatus::stale);
  CHECK(stale.fact_batch_artifact.empty());

  const auto all_evidence = cache.dependency_evidence();
  REQUIRE(all_evidence.size() == 1);
  const auto plan = cidx::storage::plan_affected_translation_units(
      {(root.path() / "generated/leaf.def").string()}, all_evidence);
  CHECK(plan.affected_count == 1);
}

TEST_CASE("deleted or corrupt cache state never harms the core database") {
  TempDirectory root("failure");
  cidx::Storage storage(":memory:");
  cidx::storage::TuFactCache cache(storage, root.path().string());
  auto dependencies = evidence(root.path());
  const cidx::storage::TuFactCachePublication publication{
      .workspace_identity = "workspace:test",
      .source_identity = dependencies.source,
      .configuration_identity = dependencies.configuration,
      .cache_identity = dependencies.generation,
      .fact_batch_artifact = artifact_bytes(fixture_batch()),
      .replay_context = replay_context_bytes(),
      .dependency_evidence = dependencies,
  };
  const auto record = cache.publish(publication);
  {
    std::ofstream corrupt(root.path() / record.relative_path,
                          std::ios::binary | std::ios::trunc);
    corrupt << "not sqlite";
  }
  CHECK(cache
            .lookup(publication.workspace_identity, publication.source_identity,
                    publication.configuration_identity,
                    publication.cache_identity)
            .status == cidx::storage::TuFactCacheStatus::corrupt);
  CHECK_NOTHROW(static_cast<void>(storage.stats()));

  std::filesystem::remove(root.path() / record.relative_path);
  CHECK(cache
            .lookup(publication.workspace_identity, publication.source_identity,
                    publication.configuration_identity,
                    publication.cache_identity)
            .status == cidx::storage::TuFactCacheStatus::missing);
  CHECK_NOTHROW(static_cast<void>(storage.stats()));
}

TEST_CASE("retention preserves current leased and pinned generations") {
  TempDirectory root("retention");
  cidx::Storage storage(":memory:");
  cidx::storage::TuFactCache cache(storage, root.path().string());
  auto dependencies = evidence(root.path());
  cidx::storage::TuFactCachePublication publication{
      .workspace_identity = "workspace:test",
      .source_identity = dependencies.source,
      .configuration_identity = dependencies.configuration,
      .cache_identity = dependencies.generation,
      .fact_batch_artifact = artifact_bytes(fixture_batch()),
      .replay_context = replay_context_bytes(),
      .dependency_evidence = dependencies,
  };
  const auto first = cache.publish(publication);
  cache.lease(publication.workspace_identity, publication.source_identity,
              publication.configuration_identity, "reader-1", "replay");
  publication.cache_identity = "sha256:cache-v2";
  publication.dependency_evidence.generation = publication.cache_identity;
  const auto second = cache.publish(publication);
  CHECK(std::filesystem::exists(root.path() / first.relative_path));
  CHECK(std::filesystem::exists(root.path() / second.relative_path));
  CHECK(cache.recover() == 0);

  cache.unlease(publication.workspace_identity, publication.source_identity,
                publication.configuration_identity, "reader-1");
  CHECK(cache.recover() == 1);
  CHECK_FALSE(std::filesystem::exists(root.path() / first.relative_path));
  CHECK(std::filesystem::exists(root.path() / second.relative_path));

  cache.pin(publication.workspace_identity, publication.source_identity,
            publication.configuration_identity, "qualification", "evidence");
  publication.cache_identity = "sha256:cache-v3";
  publication.dependency_evidence.generation = publication.cache_identity;
  const auto third = cache.publish(publication);
  CHECK(cache.recover() == 0);
  CHECK(std::filesystem::exists(root.path() / second.relative_path));
  cache.unpin(publication.workspace_identity, publication.source_identity,
              publication.configuration_identity, "qualification");
  CHECK(cache.recover() == 1);
  CHECK_FALSE(std::filesystem::exists(root.path() / second.relative_path));
  CHECK(std::filesystem::exists(root.path() / third.relative_path));
}

TEST_CASE("partial truncated untrusted and incompatible artifacts are misses") {
  TempDirectory root("classifications");
  cidx::Storage storage(":memory:");
  cidx::storage::TuFactCache cache(storage, root.path().string());
  const std::string workspace = "workspace:test";
  const std::string source = (root.path() / "src/main.cpp").string();
  const std::string configuration = "debug";
  const std::string cache_identity = "sha256:cache-v1";

  const auto base_spec = [&] {
    cidx::ArtifactSpec spec;
    spec.logical_id = cidx::storage::TuFactCache::logical_id(workspace, source,
                                                             configuration);
    spec.kind = "tu-fact-cache";
    spec.artifact_schema = "cidx-tu-fact-cache/v1";
    spec.catalog_version = cidx::catalog::kCatalogVersion;
    spec.catalog_hash = std::string(cidx::catalog::kCatalogHash);
    spec.producer_version = "cidx-tu-fact-cache test";
    spec.engine_version = "cidx test";
    spec.workspace_identity = workspace;
    spec.tu_identity = source;
    spec.configuration_identity = configuration;
    spec.input_fact_set_identity = cache_identity;
    spec.completeness = cidx::ArtifactCompleteness::complete;
    spec.truncation = cidx::ArtifactTruncation::none;
    spec.trust = cidx::ArtifactTrust::reader_verified;
    spec.attachment_name = "tu_fact_cache";
    spec.exposed_relations = {"tu_dependency", "tu_fact_cache",
                              "tu_replay_context"};
    spec.retention_policy = "optional";
    return spec;
  };
  const auto publish = [&](const cidx::ArtifactSpec &spec) {
    cidx::ArtifactStore artifacts(storage, root.path());
    static_cast<void>(artifacts.publish(spec, [](cidx::SqliteDb &sidecar) {
      sidecar.exec(
          "CREATE TABLE tu_fact_cache(singleton INTEGER PRIMARY KEY, "
          "cache_identity TEXT, payload_hex TEXT, payload_sha256 TEXT);"
          "CREATE TABLE tu_replay_context(singleton INTEGER PRIMARY KEY, "
          "context_hex TEXT, context_sha256 TEXT);"
          "CREATE TABLE tu_dependency(source TEXT, destination TEXT, "
          "destination_content_sha256 TEXT, conditional_context TEXT, "
          "resolved INTEGER);");
    }));
  };
  const auto classified_as = [&](const cidx::ArtifactSpec &spec,
                                 cidx::storage::TuFactCacheStatus status) {
    publish(spec);
    CHECK(
        cache.lookup(workspace, source, configuration, cache_identity).status ==
        status);
  };

  auto partial = base_spec();
  partial.completeness = cidx::ArtifactCompleteness::partial;
  classified_as(partial, cidx::storage::TuFactCacheStatus::partial);
  auto truncated = base_spec();
  truncated.truncation = cidx::ArtifactTruncation::truncated;
  classified_as(truncated, cidx::storage::TuFactCacheStatus::truncated);
  auto untrusted = base_spec();
  untrusted.trust = cidx::ArtifactTrust::unverified;
  classified_as(untrusted, cidx::storage::TuFactCacheStatus::untrusted);
  auto incompatible = base_spec();
  incompatible.catalog_hash = "sha256:incompatible";
  classified_as(incompatible, cidx::storage::TuFactCacheStatus::incompatible);
}

TEST_CASE("cache and dependency decisions are published in profile JSON") {
  TempDirectory root("profile");
  const auto output_path = root.path() / "profile.json";
  {
    cidx::profile::Session session(output_path.string(), std::nullopt);
    cidx::profile::add_counter("tu_fact_cache.hit");
    cidx::profile::add_counter("tu_fact_cache.parser_calls_avoided", 2);
    cidx::profile::add_counter("tu_dependency.affected_configurations", 3);
    cidx::profile::add_counter("tu_dependency.visited_edges", 5);
    session.finish();
  }
  std::ifstream input(output_path);
  const std::string profile{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
  CHECK(profile.find("\"tu_fact_cache.hit\": 1") != std::string::npos);
  CHECK(profile.find("\"tu_fact_cache.parser_calls_avoided\": 2") !=
        std::string::npos);
  CHECK(profile.find("\"tu_dependency.affected_configurations\": 3") !=
        std::string::npos);
  CHECK(profile.find("\"tu_dependency.visited_edges\": 5") !=
        std::string::npos);
}
