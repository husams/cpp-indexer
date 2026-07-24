#pragma once

#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace cidx::ui {

struct ServerOptions {
  int port = 0;
  bool launch_browser = true;
};

using GraphProvider =
    std::function<std::optional<std::string>(std::string_view target)>;

// Serve a GraphView snapshot and bounded live slices over loopback until
// interrupted. The provider receives the authenticated request target.
int serve_live(const std::string &html, const GraphProvider &graph_provider,
               const ServerOptions &options, std::ostream &out,
               std::ostream &err);

// Compatibility overload for callers that only have one immutable snapshot.
int serve_live(const std::string &html, const std::string &graph_json,
               const ServerOptions &options, std::ostream &out,
               std::ostream &err);

} // namespace cidx::ui
