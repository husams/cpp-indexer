// Clang-free report rendering of cidx-diff (docs/diff.md "Report"):
// deterministic text and JSON, fixed key order, LF endings.
#pragma once

#include <ostream>
#include <string>

#include "diff/analyze.hpp"
#include "diff/compare.hpp"
#include "diff/target.hpp"

namespace cidx {
namespace diff {

// Report-level inputs that are not part of either side's analysis.
struct ReportSpec {
  std::string scope; // file|symbol
  std::string mode;  // syntax|semantic|both
  std::string match; // strict|heuristic
  int context = 0;   // text output only
  bool json = false;
};

void render_report(const ReportSpec &spec, const SideAnalysis &left,
                   const SideAnalysis &right, const ConfigDelta &delta,
                   const Comparison &cmp, std::ostream &out);

} // namespace diff
} // namespace cidx
