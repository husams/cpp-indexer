#pragma once

#include "storage/records.hpp"

#include <optional>
#include <string>

namespace cidx {

// Build-declared -include-pch arguments are compilation semantics. This
// preflight only rejects missing/unreadable inputs; existing inputs remain in
// the original invocation so Clang owns incompatibility diagnostics.
[[nodiscard]] std::optional<std::string>
preflight_build_declared_pch(const TranslationUnitConfig &configuration);

} // namespace cidx
