#pragma once

#include <cstdint>
#include <string>

#include "cli/json_out.hpp"

namespace cidx::ui {

enum class RenderMode : std::uint8_t { OfflineExport, LoopbackLive };

// Render a complete browser document with all assets inlined. Offline exports
// deliberately disable the loopback expansion transport at render time.
std::string render_html(const json_out::Value &view, RenderMode mode);

// Compatibility wrapper for callers that only need a static snapshot.
std::string render_html(const json_out::Value &view);

} // namespace cidx::ui
