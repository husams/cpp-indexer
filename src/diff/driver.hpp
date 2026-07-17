// Clang-free CLI + orchestration of cidx-diff. run() is the whole tool:
// parse argv (CLI11, subcommands `file` and `symbol`), resolve each side's
// configuration from its index, analyze, compare, report. Returns the
// process exit code (0 success, 2 usage, 1 operational) and writes only to
// the given streams, so tests drive it in-process.
#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace cidx {
namespace diff {

int run(const std::vector<std::string> &argv, std::ostream &out,
        std::ostream &err);

} // namespace diff
} // namespace cidx
