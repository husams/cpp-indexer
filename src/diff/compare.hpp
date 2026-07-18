// Clang-free comparison layer of cidx-diff: entity matching (docs/diff.md
// "Matching"), the AST edit script, and the tri-state semantic verdicts.
#pragma once

#include <string>
#include <vector>

#include "diff/analyze.hpp"
#include "diff/syntax_ir.hpp"
#include "diff/target.hpp"

namespace cidx {
namespace diff {

// One matched (or one-sided) entity pair with its syntax edits and semantic
// verdict. left/right point into the respective SideAnalysis (top-level
// entities or, for member rows, Entity::members of a record pair); nullptr
// marks the missing side (status added/removed). member rows exist only for
// a symbol-scope record target and are excluded from the aggregate syntax
// and unsupported counts (the record pair already carries them).
struct EntityPair {
  const Entity *left = nullptr;
  const Entity *right = nullptr;
  bool member = false;
  std::string status; // matched|added|removed|renamed
  std::string match;  // usr|signature|name|fingerprint; "" when one-sided
  int confidence = 0; // 100|95|85|70; 0 when one-sided
  std::vector<EditOp> edits;
  bool subtree_differs = false; // structural fingerprints differ (or one
                                // side is missing); computed before the
                                // capped edit script, so the shared op cap
                                // can never mask a changed entity
  bool truncated = false;
  std::string verdict; // verdict::k*
  std::string evidence;
  std::string detail;
  std::vector<std::string> changes; // profile/signature contradictions
  std::vector<Unsupported> unsupported;
};

// Whole-comparison result: per-entity rows (sorted for reporting) plus the
// aggregate syntax and semantic summaries.
struct Comparison {
  std::vector<EntityPair> pairs;
  bool syntax_changed = false;
  int edit_count = 0;
  bool truncated = false;
  std::string verdict;
  std::string evidence;
  std::string detail;
  std::vector<std::string> assumptions;
  int unsupported_count = 0;
};

// match_mode: "strict" (usr/signature tiers only) or "heuristic". Symbol
// scope force-pairs the two explicitly selected entities even when no
// matching tier holds (no tier, no confidence) and appends member rows for a
// record target. delta gates identical-source-and-config evidence and the
// whole-file / class equivalent downgrade.
Comparison compare_sides(const SideAnalysis &left, const SideAnalysis &right,
                         const std::string &match_mode,
                         const ConfigDelta &delta, const std::string &scope);

} // namespace diff
} // namespace cidx
