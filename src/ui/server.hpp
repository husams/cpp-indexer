#pragma once

#include <ostream>
#include <string>

namespace cidx::ui {

struct ServerOptions {
  int port = 0;
  bool launch_browser = true;
};

// Serve one immutable GraphView snapshot over loopback until interrupted.
int serve_live(const std::string &html, const std::string &graph_json,
               const ServerOptions &options, std::ostream &out,
               std::ostream &err);

} // namespace cidx::ui
