#pragma once

#include <string>

#include "cli/json_out.hpp"

namespace cidx::ui {

std::string render_html(const json_out::Value &view);

} // namespace cidx::ui
