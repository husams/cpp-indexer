#include "clangx_lt/display_name_rewrite.hpp"

#include <algorithm>

namespace cidx::lt {

std::optional<std::string>
rewrite_template_display_name(const std::string &display,
                              const std::vector<std::string> &display_args) {
  // update_callable_template_display_name: skip on empty or any '?'.
  if (display_args.empty() ||
      std::find(display_args.begin(), display_args.end(), "?") !=
          display_args.end())
    return std::nullopt;
  if (display.empty())
    return std::nullopt;

  std::string rendered = "<";
  for (size_t i = 0; i < display_args.size(); ++i) {
    if (i != 0)
      rendered += ", ";
    rendered += display_args[i];
  }
  rendered += ">";

  std::string out = display;
  const size_t start = out.find('<');
  const size_t params = out.find('(');
  bool replaced = false;
  if (start != std::string::npos &&
      (params == std::string::npos || start < params)) {
    int depth = 0;
    for (size_t i = start; i < out.size(); ++i) {
      if (out[i] == '<')
        ++depth;
      else if (out[i] == '>' && --depth == 0) {
        out = out.substr(0, start) + rendered + out.substr(i + 1);
        replaced = true;
        break;
      }
    }
  }
  if (!replaced) {
    if (params != std::string::npos)
      out = out.substr(0, params) + rendered + out.substr(params);
    else
      out += rendered;
  }
  if (out == display)
    return std::nullopt;
  return out;
}

} // namespace cidx::lt
