// Plain-data semantic model of cidx-diff (docs/diff.md "Semantic model"):
// the normalized textual Behaviour IR of a callable, the observable
// structural profile of a class, unsupported-construct markers, and the
// frozen verdict/evidence vocabularies. Clang-free by design.
#pragma once

#include <string>
#include <vector>

#include "diff/syntax_ir.hpp"

namespace cidx {
namespace diff {

// Tri-state semantic verdict values.
namespace verdict {
inline constexpr const char *kEquivalent = "equivalent";
inline constexpr const char *kDifferent = "different";
inline constexpr const char *kUnknown = "unknown";
} // namespace verdict

// Evidence levels — fixed vocabulary (the last three are reserved for M3+).
namespace evidence {
inline constexpr const char *kIdenticalSourceAndConfig =
    "identical-source-and-config";
inline constexpr const char *kNormalizedIr = "normalized-ir";
inline constexpr const char *kSummaryContradiction = "summary-contradiction";
inline constexpr const char *kBoundedCounterexample = "bounded-counterexample";
inline constexpr const char *kProvedUnderAssumptions =
    "proved-under-assumptions";
inline constexpr const char *kUnsupportedOrIncomplete =
    "unsupported-or-incomplete";
} // namespace evidence

// A construct outside the supported subset, recorded with its location
// instead of being approximated (inline asm, volatile/atomic, try/catch,
// coroutines, lambdas, ...). side is filled by compare ("left"/"right") when
// the marker is attached to an entity pair.
struct Unsupported {
  std::string what;
  SrcRange range;
  std::string side;
};

// Normalized behavior summary of one callable: canonical signature plus the
// CFG lowered to textual blocks in CFG order (alpha-renamed values, wrapper
// erasure only — see the contract for the exact normalization set). The
// signature components are kept split so a contradiction names its axis.
struct BehaviourIR {
  std::string signature; // return_type(params) quals noexcept virtual
  std::string return_type;
  std::vector<std::string> param_types; // canonical; "..." marks varargs
  std::vector<std::string> param_defaults; // "= <expr>" per parameter; "" when
                                           // the parameter has no default
  std::string quals;                    // "const", "const &", ... ; "" if none
  bool is_noexcept = false;
  bool is_virtual = false;
  std::string storage;        // "static"/"extern"; "" otherwise
  bool is_inline = false;     // inline written in the source
  std::string constexpr_kind; // "constexpr"/"consteval"/"constinit"; "" none
  bool is_noreturn = false;
  bool is_static_method = false;
  std::string access;         // "public"/"protected"/"private"; "" non-member
  bool is_explicit = false;   // explicit ctor/conversion
  bool is_deleted = false;    // = delete
  bool is_defaulted = false;  // = default
  bool is_final = false;      // final specifier on the method
  bool is_pure = false;       // pure virtual (= 0)
  bool is_override = false;   // overrides a base method
  bool has_body = false;
  std::vector<std::string> blocks; // one normalized text line per CFG element,
                                   // grouped by block in CFG order
  std::vector<Unsupported> unsupported;
};

// Observable structural contract of a class/struct/union. Every row is a
// canonical text line so profile comparison is plain sequence equality and
// contradictions can be named verbatim in reports.
struct ClassProfile {
  std::vector<std::string> bases;   // ordered: access, virtual, name
  std::vector<std::string> fields;  // ordered: name, canonical type, bitfield
                                    // width, mutable
  std::vector<std::string> methods; // sorted set: canonical signature, access,
                                    // virtual/override/final/pure, static,
                                    // deleted/defaulted
  bool polymorphic = false;
  bool abstract = false;
  std::vector<std::string> template_params;
  std::vector<std::string> nested; // nested record names
  std::vector<std::string> extras; // sorted set: "friend <sig|type>",
                                   // "using <qualified name>",
                                   // "alias <name> = <type>",
                                   // "static <name> : <type>",
                                   // "static_assert <fingerprint>",
                                   // "layout size:<N> align:<M>", "final"
  std::vector<Unsupported> unsupported; // class-scope member decl kinds the
                                        // profile cannot capture
  std::string fingerprint;         // sha1 over every row above
};

} // namespace diff
} // namespace cidx
