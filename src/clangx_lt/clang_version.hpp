// Compile-time version of the Clang C++ API cidx links against (clang-cpp +
// libLLVM). Replaces the retired runtime libclang version query
// (linked_libclang_major). Clang-free header so cidx_core TUs can include it.
#pragma once

namespace cidx {

// Major version of the Clang the LibTooling engine is built against
// (CLANG_VERSION_MAJOR). Used by Toolchain for the gnuc-masquerade version cap.
int clang_version_major();

} // namespace cidx
