// Display-name rewrite for callable template specializations
// (update_callable_template_display_name).
//
// Pure string surgery, no Clang dependency: given a callable's stored display
// name and the rendered template arguments, splice a `<...>` clause into the
// name. Isolated here so the argument-emission logic (call_template_args) reads
// as data flow rather than nested string manipulation.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cidx::lt {

// Splice the template-argument clause built from `display_args` into `display`,
// replacing an existing top-level `<...>` that precedes the parameter list, or
// inserting/appending it otherwise. Returns the rewritten name, or nullopt when
// no rewrite applies: empty args, any `?` placeholder arg, an empty display
// name, or an unchanged result.
std::optional<std::string>
rewrite_template_display_name(const std::string &display,
                              const std::vector<std::string> &display_args);

} // namespace cidx::lt
