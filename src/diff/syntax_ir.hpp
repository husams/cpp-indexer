// Plain-data syntax model of cidx-diff (docs/diff.md "Syntax model"): each
// side's target subtree lowered to a typed node tree with deterministic
// structural fingerprints, plus the edit operations produced by comparing two
// such trees. Clang-free by design — built by analyze.cpp (cidx_diff_ast),
// consumed by compare/report.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cidx {
namespace diff {

// 1-based line/column extent, end exclusive of nothing (matches index.db
// extents: end is one past the last token).
struct SrcRange {
  int line = 0;
  int col = 0;
  int end_line = 0;
  int end_col = 0;
};

// One lowered AST node: Clang AST class / decl kind name, a salient label
// (name, operator spelling, literal value, resolved callee, cast kind, type
// spelling), a human-facing detail, and a SHA-1 structural fingerprint over
// kind/label/children. Whitespace and comments are invisible here.
struct SynNode {
  std::string kind;
  std::string label;
  std::string detail;
  SrcRange range;
  std::vector<SynNode> children;
  std::string fingerprint;
};

// A diffable declaration: top-level entity or member of a matched class.
// Semantic payloads (Behaviour IR, class profile) live in SideAnalysis
// (analyze.hpp) and are referenced by index; -1 = none.
struct Entity {
  std::string kind;      // function|method|class|struct|union|enum|variable|
                         // namespace|typedef|other
  std::string name;      // qualified name
  std::string signature; // qualified signature; "" when not a callable
  std::string usr;
  SrcRange range;
  SynNode syntax;
  std::string fingerprint; // structural: syntax.kind + children fingerprints,
                           // EXCLUDING the entity's own label so a pure rename
                           // still fingerprints identically (heuristic tier)
  bool is_definition = false;
  std::string slice; // raw source text of the extent (identical-source
                     // evidence)
  std::vector<Entity> members;
  int behaviour = -1; // index into SideAnalysis::behaviours
  int profile = -1;   // index into SideAnalysis::profiles
};

// One syntax edit operation. op: added|removed|replaced|changed|renamed.
struct EditOp {
  std::string op;
  std::string node; // node kind
  std::string detail;
  std::optional<SrcRange> left_range;
  std::optional<SrcRange> right_range;
};

// Alignment bounds (docs/diff.md): child sequences longer than kLcsLimit
// fall back to position-wise pairing; reports set truncated past kEditCap.
constexpr int kLcsLimit = 512;
constexpr int kEditCap = 1000;

} // namespace diff
} // namespace cidx
