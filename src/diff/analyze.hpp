// Clang-free interface of the diff analyzer. analyze.cpp is the ONLY
// clang-including diff TU (OBJECT library cidx_diff_ast, same boundary as
// cidx_astgraph_ast): it parses one side under its ParseConfig and lowers the
// target scope into the plain-data IRs below.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "diff/behaviour_ir.hpp"
#include "diff/syntax_ir.hpp"
#include "diff/target.hpp"

namespace cidx {
namespace diff {

// Symbol selector (docs/diff.md "Selectors"): exact USR, exact qualified
// signature, exact qualified name (+ optional kind filter), or line:N.
struct Selector {
  std::string raw;
  std::optional<std::string> kind; // --kind filter for the name tier
};

// Everything one parsed side contributes to matching/verdicts/reporting.
// Entities reference behaviours/profiles by index (Entity::behaviour /
// Entity::profile).
struct SideAnalysis {
  ParseConfig config;
  std::vector<Entity> entities; // top-level, source order
  std::vector<BehaviourIR> behaviours;
  std::vector<ClassProfile> profiles;
};

// Parse the side and build its IRs. File scope when selector is nullopt;
// symbol scope resolves the selector in the configured parse and keeps the
// single selected entity. Throws CidxError on parse failure or on an
// ambiguous/unmatched selector.
SideAnalysis analyze_side(const ParseConfig &config,
                          const std::optional<Selector> &selector);

} // namespace diff
} // namespace cidx
