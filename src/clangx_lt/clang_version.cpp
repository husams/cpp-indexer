#include "clangx_lt/clang_version.hpp"

#include "clang/Basic/Version.h"

namespace cidx {

int clang_version_major() { return CLANG_VERSION_MAJOR; }

} // namespace cidx
