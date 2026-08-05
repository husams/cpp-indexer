#include "storage/tu_dependency_planner.hpp"

#include "workspace/tu_fact_cache_identity.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace cidx::storage {

namespace {

auto fallback_for(DependencyEvidenceState state) -> DependencyFallbackReason {
  switch (state) {
  case DependencyEvidenceState::complete:
    return DependencyFallbackReason::none;
  case DependencyEvidenceState::incomplete:
    return DependencyFallbackReason::incomplete_evidence;
  case DependencyEvidenceState::stale:
    return DependencyFallbackReason::stale_evidence;
  case DependencyEvidenceState::corrupt:
    return DependencyFallbackReason::corrupt_evidence;
  case DependencyEvidenceState::unavailable:
    return DependencyFallbackReason::unavailable_evidence;
  }
  return DependencyFallbackReason::unavailable_evidence;
}

auto all_targets(const std::vector<TranslationUnitDependencyEvidence> &evidence)
    -> std::vector<AffectedTranslationUnit> {
  std::vector<AffectedTranslationUnit> targets;
  targets.reserve(evidence.size());
  for (const auto &unit : evidence) {
    targets.push_back({.source = canonical_tu_cache_path(unit.source),
                       .configuration = unit.configuration});
  }
  std::ranges::sort(targets);
  targets.erase(std::ranges::unique(targets).begin(), targets.end());
  return targets;
}

auto fallback_plan(
    const std::vector<TranslationUnitDependencyEvidence> &evidence,
    DependencyFallbackReason reason) -> TuDependencyPlan {
  auto affected = all_targets(evidence);
  const std::size_t affected_count = affected.size();
  return {.affected = std::move(affected),
          .affected_count = affected_count,
          .proven_unaffected_count = 0,
          .visited_dependency_nodes = 0,
          .visited_dependency_edges = 0,
          .complete = false,
          .fallback_reason = reason};
}

} // namespace

auto to_string(DependencyFallbackReason reason) -> std::string_view {
  switch (reason) {
  case DependencyFallbackReason::none:
    return "none";
  case DependencyFallbackReason::incomplete_evidence:
    return "incomplete_evidence";
  case DependencyFallbackReason::stale_evidence:
    return "stale_evidence";
  case DependencyFallbackReason::corrupt_evidence:
    return "corrupt_evidence";
  case DependencyFallbackReason::unavailable_evidence:
    return "unavailable_evidence";
  case DependencyFallbackReason::missing_generation:
    return "missing_generation";
  case DependencyFallbackReason::unresolved_dependency:
    return "unresolved_dependency";
  }
  return "unavailable_evidence";
}

auto to_string(TuDependencyKind kind) -> std::string_view {
  switch (kind) {
  case TuDependencyKind::include:
    return "include";
  case TuDependencyKind::macro:
    return "macro";
  case TuDependencyKind::generated:
    return "generated";
  case TuDependencyKind::toolchain:
    return "toolchain";
  }
  return "include";
}

auto capture_translation_unit_dependency_evidence(
    std::string source, std::string configuration, std::string generation,
    std::vector<TuDependencyEdge> edges, bool complete)
    -> TranslationUnitDependencyEvidence {
  source = canonical_tu_cache_path(source);
  for (TuDependencyEdge &edge : edges) {
    edge.source = canonical_tu_cache_path(edge.source);
    edge.destination = canonical_tu_cache_path(edge.destination);
  }
  std::ranges::sort(edges, {}, [](const TuDependencyEdge &edge) {
    return std::tie(edge.source, edge.destination,
                    edge.destination_content_sha256, edge.conditional_context,
                    edge.provenance, edge.kind, edge.resolved, edge.system);
  });
  edges.erase(std::ranges::unique(edges).begin(), edges.end());
  const bool all_resolved = std::ranges::all_of(
      edges, [](const auto &edge) { return edge.resolved; });
  return {.source = std::move(source),
          .configuration = std::move(configuration),
          .generation = std::move(generation),
          .state = complete && all_resolved
                       ? DependencyEvidenceState::complete
                       : DependencyEvidenceState::incomplete,
          .edges = std::move(edges)};
}

auto plan_affected_translation_units(
    const std::vector<std::string> &changed_dependencies,
    const std::vector<TranslationUnitDependencyEvidence> &evidence)
    -> TuDependencyPlan {
  if (evidence.empty()) {
    return fallback_plan(evidence,
                         DependencyFallbackReason::unavailable_evidence);
  }
  for (const auto &unit : evidence) {
    if (unit.state != DependencyEvidenceState::complete) {
      return fallback_plan(evidence, fallback_for(unit.state));
    }
    if (unit.generation.empty()) {
      return fallback_plan(evidence,
                           DependencyFallbackReason::missing_generation);
    }
    if (std::ranges::any_of(unit.edges,
                            [](const auto &edge) { return !edge.resolved; })) {
      return fallback_plan(evidence,
                           DependencyFallbackReason::unresolved_dependency);
    }
  }

  std::set<std::string> changed;
  for (const std::string &dependency : changed_dependencies) {
    changed.insert(canonical_tu_cache_path(dependency));
  }

  TuDependencyPlan result;
  std::set<AffectedTranslationUnit> affected;
  for (const auto &unit : evidence) {
    std::map<std::string, std::vector<std::string>> reverse;
    for (const TuDependencyEdge &edge : unit.edges) {
      reverse[canonical_tu_cache_path(edge.destination)].push_back(
          canonical_tu_cache_path(edge.source));
    }
    for (auto &[_, sources] : reverse) {
      std::ranges::sort(sources);
      sources.erase(std::ranges::unique(sources).begin(), sources.end());
    }

    std::set<std::string> visited = changed;
    std::vector<std::string> pending(changed.begin(), changed.end());
    for (std::size_t index = 0; index < pending.size(); ++index) {
      const std::string &node = pending[index];
      ++result.visited_dependency_nodes;
      const auto incoming = reverse.find(node);
      if (incoming == reverse.end()) {
        continue;
      }
      result.visited_dependency_edges += incoming->second.size();
      for (const std::string &source : incoming->second) {
        if (visited.insert(source).second) {
          pending.push_back(source);
        }
      }
    }
    const std::string source = canonical_tu_cache_path(unit.source);
    if (visited.contains(source)) {
      affected.insert({.source = source, .configuration = unit.configuration});
    }
  }

  result.affected.assign(affected.begin(), affected.end());
  result.affected_count = result.affected.size();
  result.proven_unaffected_count =
      all_targets(evidence).size() - result.affected_count;
  return result;
}

} // namespace cidx::storage
