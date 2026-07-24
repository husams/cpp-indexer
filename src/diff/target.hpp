// Per-side configuration resolution of cidx-diff (docs/diff.md
// "Configuration resolution"): read-only index open, file-row lookup, the
// exact `cidx index` options pipeline, and the semantic-affecting option
// classification feeding the report's config_delta block. Clang-free.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cidx::diff {

// One side of the comparison as requested on the CLI (paths not yet
// resolved). side is "left" or "right" — used verbatim in error messages.
struct SideSpec {
  std::string side;
  std::string file;
  std::string db;
  std::optional<std::string> tu; // --left-tu / --right-tu
};

// Semantic-affecting classification of one side's resolved options
// (config_delta vocabulary). Spaced and glued spellings normalize to one
// canonical token so the two sides compare textually.
struct OptionClasses {
  std::optional<std::string> standard;  // -std= value, last wins
  std::optional<std::string> target;    // -target/--target value, last wins
  std::vector<std::string> definitions; // -D/-U, glued ("-DX=1", "-UX")
  std::vector<std::string> includes;    // -I/-isystem/-iquote/-F, in option
                                        // order ("<flag> <dir>")
  std::vector<std::string> other;       // everything else, in option order
};

// A side ready to parse: absolute paths, resolved options, stored driver.
// Toolchain flags + -ferror-limit=0 + -resource-dir are applied at parse
// time by analyze_side (same recipe as cidx-astgraph).
struct ParseConfig {
  std::string file;              // abs path of the diff target
  std::string db;                // abs index path
  std::optional<std::string> tu; // abs TU path when supplied
  std::string parse_file;        // file actually parsed (tu when set)
  bool restrict_to_file = false; // header via TU: only decls spelled in file
  std::optional<std::string> driver;
  std::optional<std::string> config_hash;
  std::vector<std::string> args; // sanitize + alias-resolved stored options
  OptionClasses classes;         // classify_options(args)
};

// Compact delta of semantic-affecting flags between the two sides
// (docs/diff.md "Configuration delta"). Pair fields are set only when the
// sides differ (left value, right value; "" = absent on that side).
struct ConfigDelta {
  bool identical = true;
  std::optional<std::pair<std::string, std::string>> standard; // JSON "std"
  std::optional<std::pair<std::string, std::string>> target;
  std::optional<std::pair<std::string, std::string>> driver;
  std::vector<std::string> definitions_added;   // right-only, sorted
  std::vector<std::string> definitions_removed; // left-only, sorted
  bool definitions_reordered = false; // same -D/-U multiset, different order
                                      // (e.g. "-DX=1 -UX" vs "-UX -DX=1")
  bool includes_changed = false;
  std::vector<std::string> options_left_only;  // sorted
  std::vector<std::string> options_right_only; // sorted
  bool options_reordered = false; // same `other` multiset, different order
                                  // (e.g. reversed "-include a.h -include b.h")
};

// Contract steps 1-5: read-only open (schema_version gate), registered file
// row, options pipeline, --left-tu/--right-tu header handling. Throws
// CidxError with the side name on every failure.
ParseConfig resolve_parse_config(const SideSpec &spec);

OptionClasses classify_options(const std::vector<std::string> &options);

ConfigDelta config_delta(const ParseConfig &left, const ParseConfig &right);

} // namespace cidx::diff
