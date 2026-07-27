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
  // HSE-92 round 3: caps how many graph/search/evidence provider
  // invocations may be genuinely in flight at once. Each such request
  // spawns both a connection thread and a provider worker thread, so
  // without a bound resource use scales with request/attacker count rather
  // than a measured limit. Requests beyond the cap are rejected immediately
  // (503) without ever spawning either thread.
  int max_concurrent_requests = 8;
};

// Cooperative cancellation: `true` once the request that owns this token
// should stop -- because its client disconnected, or because the server is
// shutting down. A provider with a long-running loop should poll it at
// reasonable checkpoints and bail out early; a provider that never checks it
// simply keeps running to completion (or until the process itself exits),
// exactly as before this existed.
using CancelToken = std::function<bool()>;

using GraphProvider = std::function<std::optional<std::string>(
    std::string_view target, const CancelToken &should_cancel)>;

// Serve a GraphView snapshot and bounded live slices over loopback until
// interrupted (Ctrl+C) or authenticated-shutdown (`GET /api/shutdown`). Every
// provider receives the full authenticated request target (path + query
// string); a default-constructed (empty) provider makes its endpoint answer
// 404. `search_provider` backs `/api/search`, `evidence_provider` backs
// `/api/evidence`; both are optional so existing single-graph callers are
// unaffected.
int serve_live(const std::string &html, const GraphProvider &graph_provider,
               const GraphProvider &search_provider,
               const GraphProvider &evidence_provider,
               const ServerOptions &options, std::ostream &out,
               std::ostream &err);

// Compatibility overload: graph-only live server (no search/evidence ops).
int serve_live(const std::string &html, const GraphProvider &graph_provider,
               const ServerOptions &options, std::ostream &out,
               std::ostream &err);

// Compatibility overload for callers that only have one immutable snapshot.
int serve_live(const std::string &html, const std::string &graph_json,
               const ServerOptions &options, std::ostream &out,
               std::ostream &err);

} // namespace cidx::ui
