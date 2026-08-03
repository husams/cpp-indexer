#include "storage/tu_fact_cache.hpp"

#include "storage/storage.hpp"
#include "util/hashing.hpp"
#include "util/version.hpp"
#include "workspace/tu_fact_cache_identity.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace cidx::storage {

namespace {

constexpr std::string_view kAttachmentName = "tu_fact_cache";

auto hex_encode(std::span<const std::byte> bytes) -> std::string {
  constexpr std::array<char, 16> digits = {'0', '1', '2', '3', '4', '5',
                                           '6', '7', '8', '9', 'a', 'b',
                                           'c', 'd', 'e', 'f'};
  std::string output;
  output.reserve(bytes.size() * 2U);
  for (const std::byte byte : bytes) {
    const auto value = std::to_integer<unsigned int>(byte);
    output.push_back(digits[(value >> 4U) & 0x0fU]);
    output.push_back(digits[value & 0x0fU]);
  }
  return output;
}

auto hex_value(char value) -> unsigned int {
  if (value >= '0' && value <= '9') {
    return static_cast<unsigned int>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<unsigned int>(value - 'a') + 10U;
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<unsigned int>(value - 'A') + 10U;
  }
  throw std::invalid_argument("invalid hexadecimal cache payload");
}

auto hex_decode(std::string_view encoded) -> std::vector<std::byte> {
  if ((encoded.size() % 2U) != 0U) {
    throw std::invalid_argument("odd-length hexadecimal cache payload");
  }
  std::vector<std::byte> output;
  output.reserve(encoded.size() / 2U);
  for (std::size_t index = 0; index < encoded.size(); index += 2U) {
    const unsigned int value =
        (hex_value(encoded[index]) << 4U) | hex_value(encoded[index + 1U]);
    output.push_back(static_cast<std::byte>(value));
  }
  return output;
}

auto status_from_diagnostics(const std::vector<ArtifactDiagnostic> &diagnostics)
    -> TuFactCacheStatus {
  TuFactCacheStatus result = TuFactCacheStatus::hit;
  for (const ArtifactDiagnostic &diagnostic : diagnostics) {
    const std::string &code = diagnostic.code;
    if (code == "corrupt" || code == "invalid_location") {
      return TuFactCacheStatus::corrupt;
    }
    if (code == "incompatible" || code == "unsupported_contract" ||
        code == "missing_required_relation" || code == "missing_relation" ||
        code == "invalid_relation") {
      result = TuFactCacheStatus::incompatible;
    } else if (code == "truncated" &&
               result != TuFactCacheStatus::incompatible) {
      result = TuFactCacheStatus::truncated;
    } else if (code == "partial" && result == TuFactCacheStatus::hit) {
      result = TuFactCacheStatus::partial;
    } else if (code == "untrusted" && result == TuFactCacheStatus::hit) {
      result = TuFactCacheStatus::untrusted;
    } else if (code == "missing" && result == TuFactCacheStatus::hit) {
      result = TuFactCacheStatus::missing;
    } else if (result == TuFactCacheStatus::hit) {
      result = TuFactCacheStatus::unavailable;
    }
  }
  return result;
}

auto evidence_state(TuFactCacheStatus status) -> DependencyEvidenceState {
  switch (status) {
  case TuFactCacheStatus::hit:
    return DependencyEvidenceState::complete;
  case TuFactCacheStatus::stale:
    return DependencyEvidenceState::stale;
  case TuFactCacheStatus::corrupt:
    return DependencyEvidenceState::corrupt;
  case TuFactCacheStatus::missing:
  case TuFactCacheStatus::unavailable:
    return DependencyEvidenceState::unavailable;
  case TuFactCacheStatus::incompatible:
  case TuFactCacheStatus::partial:
  case TuFactCacheStatus::truncated:
  case TuFactCacheStatus::untrusted:
    return DependencyEvidenceState::incomplete;
  }
  return DependencyEvidenceState::unavailable;
}

auto dependency_kind(std::string_view value) -> TuDependencyKind {
  if (value == "macro") {
    return TuDependencyKind::macro;
  }
  if (value == "generated") {
    return TuDependencyKind::generated;
  }
  if (value == "toolchain") {
    return TuDependencyKind::toolchain;
  }
  return TuDependencyKind::include;
}

auto make_spec(const TuFactCachePublication &publication) -> ArtifactSpec {
  return {
      .logical_id = TuFactCache::logical_id(publication.workspace_identity,
                                            publication.source_identity,
                                            publication.configuration_identity),
      .kind = "tu-fact-cache",
      .artifact_schema = std::string(kTuFactCacheArtifactSchema),
      .catalog_version = catalog::kCatalogVersion,
      .catalog_hash = std::string(catalog::kCatalogHash),
      .producer_version =
          "cidx-tu-fact-cache " + std::string(version::kFullProductVersion),
      .engine_version = "cidx " + std::string(version::kFullProductVersion),
      .workspace_identity = publication.workspace_identity,
      .tu_identity = canonical_tu_cache_path(publication.source_identity),
      .configuration_identity = publication.configuration_identity,
      .input_fact_set_identity = publication.cache_identity,
      .completeness = publication.dependency_evidence.state ==
                              DependencyEvidenceState::complete
                          ? ArtifactCompleteness::complete
                          : ArtifactCompleteness::partial,
      .truncation = ArtifactTruncation::none,
      .trust = ArtifactTrust::reader_verified,
      .evidence = "source",
      .attachment_name = std::string(kAttachmentName),
      .exposed_relations = {"tu_dependency", "tu_fact_cache",
                            "tu_replay_context"},
      .retention_policy = "optional",
  };
}

} // namespace

auto to_string(TuFactCacheStatus status) -> std::string_view {
  switch (status) {
  case TuFactCacheStatus::hit:
    return "hit";
  case TuFactCacheStatus::missing:
    return "missing";
  case TuFactCacheStatus::stale:
    return "stale";
  case TuFactCacheStatus::corrupt:
    return "corrupt";
  case TuFactCacheStatus::incompatible:
    return "incompatible";
  case TuFactCacheStatus::partial:
    return "partial";
  case TuFactCacheStatus::truncated:
    return "truncated";
  case TuFactCacheStatus::untrusted:
    return "untrusted";
  case TuFactCacheStatus::unavailable:
    return "unavailable";
  }
  return "unavailable";
}

TuFactCache::TuFactCache(Storage &storage, std::string root)
    : artifacts_(storage, std::move(root)) {}

auto TuFactCache::logical_id(std::string_view workspace_identity,
                             std::string_view source_identity,
                             std::string_view configuration_identity)
    -> std::string {
  const std::string slot = std::string(workspace_identity) + '\0' +
                           canonical_tu_cache_path(source_identity) + '\0' +
                           std::string(configuration_identity);
  return "tu-fact-cache:" + sha256_hex(slot);
}

auto TuFactCache::publish(const TuFactCachePublication &publication)
    -> ArtifactRecord {
  if (publication.fact_batch_artifact.empty() ||
      publication.replay_context.empty() ||
      publication.cache_identity.empty() ||
      publication.dependency_evidence.generation.empty() ||
      publication.cache_identity !=
          publication.dependency_evidence.generation ||
      std::ranges::any_of(publication.dependency_evidence.edges,
                          [](const TuDependencyEdge &edge) {
                            return edge.resolved &&
                                   edge.destination_content_sha256.empty();
                          })) {
    throw StorageError("TU FactBatch cache publication is incomplete");
  }
  const ArtifactSpec spec = make_spec(publication);
  const std::string payload_hex = hex_encode(publication.fact_batch_artifact);
  const std::string payload_hash =
      sha256_hex(std::span<const std::byte>(publication.fact_batch_artifact));
  const std::string context_hex = hex_encode(publication.replay_context);
  const std::string context_hash =
      sha256_hex(std::span<const std::byte>(publication.replay_context));
  const ArtifactRecord record =
      artifacts_.publish(spec, [&](SqliteDb &sidecar) {
        sidecar.exec(
            "CREATE TABLE tu_fact_cache (singleton INTEGER PRIMARY KEY "
            "CHECK(singleton = 1), cache_identity TEXT NOT NULL, "
            "payload_hex TEXT NOT NULL, payload_sha256 TEXT NOT NULL);"
            "CREATE TABLE tu_replay_context (singleton INTEGER PRIMARY KEY "
            "CHECK(singleton = 1), context_hex TEXT NOT NULL, "
            "context_sha256 TEXT NOT NULL);"
            "CREATE TABLE tu_dependency (source TEXT NOT NULL, destination "
            "TEXT NOT NULL, destination_content_sha256 TEXT NOT NULL, "
            "conditional_context TEXT NOT NULL, resolved "
            "INTEGER NOT NULL CHECK(resolved IN (0,1)), provenance TEXT NOT "
            "NULL, kind TEXT NOT NULL, system INTEGER NOT NULL CHECK(system "
            "IN (0,1)), PRIMARY KEY(source, destination, "
            "destination_content_sha256, conditional_context, provenance, "
            "kind, resolved, system));");
        auto cache = sidecar.prepare(
            "INSERT INTO tu_fact_cache(singleton, cache_identity, payload_hex, "
            "payload_sha256) VALUES (1, ?, ?, ?)");
        cache.bind(1, std::string_view(publication.cache_identity));
        cache.bind(2, std::string_view(payload_hex));
        cache.bind(3, std::string_view(payload_hash));
        cache.step_done();
        auto context = sidecar.prepare(
            "INSERT INTO tu_replay_context(singleton, context_hex, "
            "context_sha256) VALUES (1, ?, ?)");
        context.bind(1, std::string_view(context_hex));
        context.bind(2, std::string_view(context_hash));
        context.step_done();
        auto edge = sidecar.prepare(
            "INSERT OR IGNORE INTO tu_dependency(source, destination, "
            "destination_content_sha256, conditional_context, resolved, "
            "provenance, kind, system) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        for (const TuDependencyEdge &dependency :
             publication.dependency_evidence.edges) {
          const std::string source = canonical_tu_cache_path(dependency.source);
          const std::string destination =
              canonical_tu_cache_path(dependency.destination);
          edge.bind(1, std::string_view(source));
          edge.bind(2, std::string_view(destination));
          edge.bind(3, std::string_view(dependency.destination_content_sha256));
          edge.bind(4, std::string_view(dependency.conditional_context));
          edge.bind(5, static_cast<std::int64_t>(dependency.resolved));
          edge.bind(6, std::string_view(dependency.provenance));
          edge.bind(7, to_string(dependency.kind));
          edge.bind(8, static_cast<std::int64_t>(dependency.system));
          edge.step_done();
          edge.reset();
        }
      });
  static_cast<void>(artifacts_.recover());
  return record;
}

auto TuFactCache::read_current(const ArtifactRecord &record,
                               TuFactCacheStatus status) -> TuFactCacheLookup {
  TuFactCacheLookup result{
      .status = status,
      .fact_batch_artifact = {},
      .replay_context = {},
      .dependency_evidence = {.source = record.spec.tu_identity,
                              .configuration =
                                  record.spec.configuration_identity,
                              .generation = record.spec.input_fact_set_identity,
                              .state = evidence_state(status),
                              .edges = {}},
      .diagnostics = {},
  };
  if (status != TuFactCacheStatus::hit) {
    return result;
  }
  try {
    artifacts_.read_current(
        record.spec.logical_id,
        [&](SqliteDb &database, std::string_view attachment_name) {
          const std::string prefix = '"' + std::string(attachment_name) + "\".";
          auto cache = database.prepare(
              "SELECT cache_identity, payload_hex, payload_sha256 FROM " +
              prefix + "tu_fact_cache WHERE singleton = 1");
          if (!cache.step() ||
              cache.col_text(0) != record.spec.input_fact_set_identity) {
            result.status = TuFactCacheStatus::incompatible;
            result.dependency_evidence.state =
                DependencyEvidenceState::incomplete;
            result.diagnostics.push_back(
                {.code = "incompatible",
                 .message =
                     "TU cache payload identity does not match the manifest"});
            return;
          }
          result.fact_batch_artifact = hex_decode(cache.col_text(1));
          if (sha256_hex(std::span<const std::byte>(
                  result.fact_batch_artifact)) != cache.col_text(2)) {
            result.status = TuFactCacheStatus::corrupt;
            result.fact_batch_artifact.clear();
            result.dependency_evidence.state = DependencyEvidenceState::corrupt;
            result.diagnostics.push_back(
                {.code = "corrupt",
                 .message = "TU cache payload hash validation failed"});
            return;
          }
          auto context = database.prepare(
              "SELECT context_hex, context_sha256 FROM " + prefix +
              "tu_replay_context WHERE singleton = 1");
          if (!context.step()) {
            result.status = TuFactCacheStatus::incompatible;
            result.fact_batch_artifact.clear();
            result.dependency_evidence.state =
                DependencyEvidenceState::incomplete;
            result.diagnostics.push_back(
                {.code = "incompatible",
                 .message = "TU cache replay context is missing"});
            return;
          }
          result.replay_context = hex_decode(context.col_text(0));
          if (sha256_hex(std::span<const std::byte>(result.replay_context)) !=
              context.col_text(1)) {
            result.status = TuFactCacheStatus::corrupt;
            result.fact_batch_artifact.clear();
            result.replay_context.clear();
            result.dependency_evidence.state = DependencyEvidenceState::corrupt;
            result.diagnostics.push_back(
                {.code = "corrupt",
                 .message = "TU cache replay-context hash validation failed"});
            return;
          }
          auto edges = database.prepare(
              "SELECT source, destination, destination_content_sha256, "
              "conditional_context, resolved, provenance, kind, system FROM " +
              prefix +
              "tu_dependency ORDER BY source, destination, "
              "conditional_context");
          while (edges.step()) {
            result.dependency_evidence.edges.push_back(
                {.source = edges.col_text(0),
                 .destination = edges.col_text(1),
                 .destination_content_sha256 = edges.col_text(2),
                 .conditional_context = edges.col_text(3),
                 .provenance = edges.col_text(5),
                 .kind = dependency_kind(edges.col_text(6)),
                 .resolved = edges.col_int64(4) != 0,
                 .system = edges.col_int64(7) != 0});
          }
        });
  } catch (const std::exception &error) {
    result.status = TuFactCacheStatus::corrupt;
    result.fact_batch_artifact.clear();
    result.replay_context.clear();
    result.dependency_evidence.state = DependencyEvidenceState::corrupt;
    result.diagnostics.push_back({.code = "corrupt", .message = error.what()});
  }
  return result;
}

auto TuFactCache::lookup(std::string_view workspace_identity,
                         std::string_view source_identity,
                         std::string_view configuration_identity,
                         std::string_view expected_cache_identity)
    -> TuFactCacheLookup {
  const std::string id =
      logical_id(workspace_identity, source_identity, configuration_identity);
  ArtifactValidation validation = artifacts_.validate(id);
  if (!validation.manifest) {
    return {.status = TuFactCacheStatus::missing,
            .fact_batch_artifact = {},
            .replay_context = {},
            .dependency_evidence =
                {.source = canonical_tu_cache_path(source_identity),
                 .configuration = std::string(configuration_identity),
                 .generation = {},
                 .state = DependencyEvidenceState::unavailable,
                 .edges = {}},
            .diagnostics = std::move(validation.diagnostics)};
  }
  if (validation.manifest->spec.workspace_identity != workspace_identity ||
      validation.manifest->spec.input_fact_set_identity !=
          expected_cache_identity) {
    TuFactCacheLookup stale =
        read_current(*validation.manifest, TuFactCacheStatus::stale);
    stale.diagnostics.push_back(
        {.code = "stale", .message = "TU cache identity changed"});
    return stale;
  }
  const TuFactCacheStatus status =
      status_from_diagnostics(validation.diagnostics);
  TuFactCacheLookup result = read_current(*validation.manifest, status);
  result.diagnostics = std::move(validation.diagnostics);
  return result;
}

auto TuFactCache::dependency_evidence()
    -> std::vector<TranslationUnitDependencyEvidence> {
  const auto [records, diagnostics] = artifacts_.export_plan(true);
  static_cast<void>(diagnostics);
  std::vector<TranslationUnitDependencyEvidence> result;
  for (const ArtifactRecord &record : records) {
    if (record.spec.kind != "tu-fact-cache") {
      continue;
    }
    const ArtifactValidation validation =
        artifacts_.validate(record.spec.logical_id);
    const TuFactCacheStatus status =
        status_from_diagnostics(validation.diagnostics);
    result.push_back(read_current(record, status).dependency_evidence);
  }
  std::ranges::sort(result, {}, [](const auto &entry) {
    return std::pair(entry.source, entry.configuration);
  });
  return result;
}

void TuFactCache::lease(std::string_view workspace_identity,
                        std::string_view source_identity,
                        std::string_view configuration_identity,
                        std::string_view lease_id, std::string_view purpose) {
  artifacts_.lease(
      logical_id(workspace_identity, source_identity, configuration_identity),
      lease_id, purpose);
}

void TuFactCache::unlease(std::string_view workspace_identity,
                          std::string_view source_identity,
                          std::string_view configuration_identity,
                          std::string_view lease_id) {
  artifacts_.unlease(
      logical_id(workspace_identity, source_identity, configuration_identity),
      lease_id);
}

void TuFactCache::pin(std::string_view workspace_identity,
                      std::string_view source_identity,
                      std::string_view configuration_identity,
                      std::string_view pin_id, std::string_view reason) {
  artifacts_.pin(
      logical_id(workspace_identity, source_identity, configuration_identity),
      pin_id, reason);
}

void TuFactCache::unpin(std::string_view workspace_identity,
                        std::string_view source_identity,
                        std::string_view configuration_identity,
                        std::string_view pin_id) {
  artifacts_.unpin(
      logical_id(workspace_identity, source_identity, configuration_identity),
      pin_id);
}

auto TuFactCache::recover() -> std::size_t { return artifacts_.recover(); }

} // namespace cidx::storage
