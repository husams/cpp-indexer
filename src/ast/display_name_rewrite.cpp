#include "ast/display_name_rewrite.hpp"

#include <algorithm>

namespace cidx::ast {

namespace {

// Replace the display name's existing balanced <...> block (before the
// parameter list) with `rendered`, or insert it before the parameter list /
// append when none exists.
std::string splice_template_args(const std::string &display,
                                 const std::string &rendered) {
  const size_t start = display.find('<');
  const size_t params = display.find('(');
  if (start != std::string::npos &&
      (params == std::string::npos || start < params)) {
    int depth = 0;
    for (size_t i = start; i < display.size(); ++i) {
      if (display[i] == '<') {
        ++depth;
      } else if (display[i] == '>' && --depth == 0) {
        return display.substr(0, start) + rendered + display.substr(i + 1);
      }
    }
  }
  if (params != std::string::npos) {
    return display.substr(0, params) + rendered + display.substr(params);
  }
  return display + rendered;
}

} // namespace

std::optional<std::string>
rewrite_template_display_name(const std::string &display,
                              const std::vector<std::string> &display_args) {
  // update_callable_template_display_name: skip on empty or any '?'.
  if (display_args.empty() ||
      std::ranges::find(display_args, "?") != display_args.end()) {
    return std::nullopt;
  }
  if (display.empty()) {
    return std::nullopt;
  }

  std::string rendered = "<";
  for (size_t i = 0; i < display_args.size(); ++i) {
    if (i != 0) {
      rendered += ", ";
    }
    rendered += display_args[i];
  }
  rendered += ">";

  const std::string out = splice_template_args(display, rendered);
  if (out == display) {
    return std::nullopt;
  }
  return out;
}

} // namespace cidx::ast
