#pragma once

#include <string>

namespace cidx {

int parse_libclang_major(const std::string &version_string);
int linked_libclang_major();
std::string configured_libclang_library_path() noexcept;
void warn_if_runtime_libclang_ignored();

} // namespace cidx
