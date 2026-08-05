// Deterministic reverse-dependency planning for TU/configuration cache entries.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cidx::storage {

enum class DependencyEvidenceState : std::uint8_t {
  complete,
  incomplete,
  stale,
  corrupt,
  unavailable,
};

enum class DependencyFallbackReason : std::uint8_t {
  none,
  incomplete_evidence,
  stale_evidence,
  corrupt_evidence,
  unavailable_evidence,
  missing_generation,
  unresolved_dependency,
};

enum class TuDependencyKind : std::uint8_t {
  include,
  macro,
  generated,
  toolchain,
};

struct TuDependencyEdge {
  std::string source;
  std::string destination;
  // Content identity captured when the edge was observed. An empty value is
  // incomplete evidence and must never authorize a cache hit.
  std::string destination_content_sha256;
  std::string conditional_context;
  std::string provenance;
  TuDependencyKind kind = TuDependencyKind::include;
  bool resolved = true;
  bool system = false;

  auto operator<=>(const TuDependencyEdge &) const = default;
};

struct TranslationUnitDependencyEvidence {
  std::string source;
  std::string configuration;
  std::string generation;
  DependencyEvidenceState state = DependencyEvidenceState::complete;
  std::vector<TuDependencyEdge> edges;
};

struct AffectedTranslationUnit {
  std::string source;
  std::string configuration;

  auto operator<=>(const AffectedTranslationUnit &) const = default;
};

struct TuDependencyPlan {
  std::vector<AffectedTranslationUnit> affected;
  std::size_t affected_count = 0;
  std::size_t proven_unaffected_count = 0;
  std::size_t visited_dependency_nodes = 0;
  std::size_t visited_dependency_edges = 0;
  bool complete = true;
  DependencyFallbackReason fallback_reason = DependencyFallbackReason::none;
};

[[nodiscard]] auto to_string(DependencyFallbackReason reason)
    -> std::string_view;
[[nodiscard]] auto to_string(TuDependencyKind kind) -> std::string_view;

[[nodiscard]] auto capture_translation_unit_dependency_evidence(
    std::string source, std::string configuration, std::string generation,
    std::vector<TuDependencyEdge> edges, bool complete)
    -> TranslationUnitDependencyEvidence;

[[nodiscard]] auto plan_affected_translation_units(
    const std::vector<std::string> &changed_dependencies,
    const std::vector<TranslationUnitDependencyEvidence> &evidence)
    -> TuDependencyPlan;

} // namespace cidx::storage
