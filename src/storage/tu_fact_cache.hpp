// Manifest-governed optional TU FactBatch cache sidecar.
#pragma once

#include "storage/artifacts.hpp"
#include "storage/tu_dependency_planner.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cidx {
class Storage;
}

namespace cidx::storage {

inline constexpr std::string_view kTuFactCacheArtifactSchema =
    "cidx-tu-fact-cache/v1";

enum class TuFactCacheStatus : std::uint8_t {
  hit,
  missing,
  stale,
  corrupt,
  incompatible,
  partial,
  truncated,
  untrusted,
  unavailable,
};

struct TuFactCachePublication {
  std::string workspace_identity;
  std::string source_identity;
  std::string configuration_identity;
  std::string cache_identity;
  std::vector<std::byte> fact_batch_artifact;
  std::vector<std::byte> replay_context;
  TranslationUnitDependencyEvidence dependency_evidence;
};

struct TuFactCacheLookup {
  TuFactCacheStatus status = TuFactCacheStatus::missing;
  std::vector<std::byte> fact_batch_artifact;
  std::vector<std::byte> replay_context;
  TranslationUnitDependencyEvidence dependency_evidence;
  std::vector<ArtifactDiagnostic> diagnostics;

  [[nodiscard]] auto usable() const -> bool {
    return status == TuFactCacheStatus::hit;
  }
};

[[nodiscard]] auto to_string(TuFactCacheStatus status) -> std::string_view;

class TuFactCache final {
public:
  explicit TuFactCache(Storage &storage, std::string root = {});

  [[nodiscard]] auto publish(const TuFactCachePublication &publication)
      -> ArtifactRecord;
  [[nodiscard]] auto lookup(std::string_view workspace_identity,
                            std::string_view source_identity,
                            std::string_view configuration_identity,
                            std::string_view expected_cache_identity)
      -> TuFactCacheLookup;
  [[nodiscard]] auto dependency_evidence()
      -> std::vector<TranslationUnitDependencyEvidence>;

  void lease(std::string_view workspace_identity,
             std::string_view source_identity,
             std::string_view configuration_identity, std::string_view lease_id,
             std::string_view purpose);
  void unlease(std::string_view workspace_identity,
               std::string_view source_identity,
               std::string_view configuration_identity,
               std::string_view lease_id);
  void pin(std::string_view workspace_identity,
           std::string_view source_identity,
           std::string_view configuration_identity, std::string_view pin_id,
           std::string_view reason);
  void unpin(std::string_view workspace_identity,
             std::string_view source_identity,
             std::string_view configuration_identity, std::string_view pin_id);
  [[nodiscard]] auto recover() -> std::size_t;

  [[nodiscard]] static auto logical_id(std::string_view workspace_identity,
                                       std::string_view source_identity,
                                       std::string_view configuration_identity)
      -> std::string;

private:
  [[nodiscard]] auto read_current(const ArtifactRecord &record,
                                  TuFactCacheStatus status)
      -> TuFactCacheLookup;

  ArtifactStore artifacts_;
};

} // namespace cidx::storage
