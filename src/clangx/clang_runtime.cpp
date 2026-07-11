#include "clangx/clang_runtime.hpp"

#include "clangx/clang_raii.hpp"
#include "util/env.hpp"
#include "util/logger.hpp"

#include <clang-c/Index.h>

#include <cstdlib>
#include <optional>
#include <regex>
#include <string>

namespace cidx {

int parse_libclang_major(const std::string &version_string) {
  static const std::regex kVersionRe("version (\\d+)");
  std::smatch match;
  if (!std::regex_search(version_string, match, kVersionRe)) {
    return 0;
  }
  return std::atoi(match[1].str().c_str());
}

int linked_libclang_major() {
  static const int major = [] {
    const CxString version(::clang_getClangVersion());
    return parse_libclang_major(version.str());
  }();
  return major;
}

std::string configured_libclang_library_path() noexcept {
  return std::string(CIDX_LIBCLANG_PATH);
}

void warn_if_runtime_libclang_ignored() {
  static bool warned = false;
  if (warned) {
    return;
  }
  const std::optional<std::string> env = get_env("CIDX_LIBCLANG");
  if (env && !env->empty()) {
    warned = true;
    Logger::root().warning(
        "cidx",
        "CIDX_LIBCLANG is set but ignored: this build links libclang at " +
            configured_libclang_library_path() + " (set at build time)");
  }
}

} // namespace cidx
