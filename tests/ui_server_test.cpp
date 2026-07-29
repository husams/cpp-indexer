// HSE-92: integration tests for the loopback live explorer (`cidx ui open`).
// Drives the real serve_live() HTTP loop over a real (127.0.0.1) socket
// against an in-memory Storage fixture, exercising exactly the scenarios
// HSE-92's acceptance criteria call out: search, expand in/out, witness
// path, grouping/filters, cancellation, shutdown, invalid token/origin, and
// oversized responses.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <set>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "cli/commands.hpp"
#include "graph/query.hpp"
#include "storage/records.hpp"
#include "storage/storage.hpp"
#include "ui/assets.hpp"
#include "ui/graph_view.hpp"
#include "ui/server.hpp"

using cidx::Storage;
namespace ui = cidx::ui;
namespace json_out = cidx::json_out;

namespace {

cidx::Symbol symbol(const char *usr, const char *name, const char *kind) {
  cidx::Symbol result;
  result.usr = usr;
  result.spelling = name;
  result.qual_name = name;
  result.kind = kind;
  result.is_definition = true;
  result.resolved = true;
  return result;
}

// Populates a small a -> b -> c call chain plus an a -> v "uses" edge so
// tests can exercise expand in/out, a bounded witness path, and node-kind
// filtering (function vs variable) against one fixture.
struct Fixture {
  Storage db{":memory:"};

  Fixture() {
    const int64_t component =
        db.add_component("test", "/tmp/cidx-ui-server-test");
    const int64_t directory = db.add_directory(component, "");
    const int64_t file = db.add_file(directory, "main.cpp");
    auto sym_a = symbol("USR::a", "ns::a", "function");
    sym_a.file_id = file;
    sym_a.line = 1;
    auto sym_b = symbol("USR::b", "ns::b", "function");
    sym_b.file_id = file;
    sym_b.line = 2;
    auto sym_c = symbol("USR::c", "ns::c", "function");
    sym_c.file_id = file;
    sym_c.line = 3;
    auto sym_v = symbol("USR::v", "ns::v", "variable");
    sym_v.file_id = file;
    sym_v.line = 4;
    const int64_t a = db.add_symbol(sym_a);
    const int64_t b = db.add_symbol(sym_b);
    const int64_t c = db.add_symbol(sym_c);
    const int64_t v = db.add_symbol(sym_v);

    const auto add_site = [&](int64_t edge_id, int line, bool conditional) {
      cidx::EdgeSite site;
      site.edge_id = edge_id;
      site.file_id = file;
      site.line = line;
      site.col = 4;
      site.conditional = conditional ? 1 : 0;
      db.add_edge_site(site);
    };
    const auto add_edge = [&](int64_t src, int64_t dst, const char *kind,
                              int line) {
      cidx::Edge edge;
      edge.src_id = src;
      edge.dst_id = dst;
      edge.kind = cidx::graph::edge_kinds_map().at(kind);
      const int64_t edge_id = db.add_edge(edge);
      add_site(edge_id, line, /*conditional=*/false);
      return edge_id;
    };
    add_edge(a, b, "calls", 10);
    add_edge(b, c, "uses", 20);
    const int64_t a_uses_v = add_edge(a, v, "uses", 30);
    // A second site on the SAME edge, sorted after the first (line 31 >
    // line 30) and conditional: a tiny site_limit must not be allowed to
    // silently drop this from the applicability decision (HSE-92 review).
    add_site(a_uses_v, 31, /*conditional=*/true);
  }
};

// atoi() cannot report conversion errors; every numeric field this test
// parses comes from output the test itself controls, so a clean 0 default
// on malformed input is a safe, deliberate fallback rather than a silent
// error.
int parse_int(std::string_view text) {
  int value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} ? value : 0;
}

// ---- Minimal blocking HTTP/1.1 client over a real loopback socket ---------

struct HttpResponse {
  int status = 0;
  std::string headers;
  std::string body;
};

HttpResponse http_get(int port, const std::string &target,
                      const std::optional<std::string> &origin = std::nullopt) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&address),
                    sizeof(address)) == 0);
  std::ostringstream request;
  request << "GET " << target << " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
  if (origin) {
    request << "Origin: " << *origin << "\r\n";
  }
  request << "Connection: close\r\n\r\n";
  const std::string wire = request.str();
  std::size_t sent = 0;
  while (sent < wire.size()) {
    const ssize_t written =
        ::send(fd, wire.data() + sent, wire.size() - sent, 0);
    if (written <= 0) {
      break;
    }
    sent += static_cast<std::size_t>(written);
  }
  std::string response;
  std::array<char, 8192> buffer{};
  ssize_t read = 0;
  while ((read = ::recv(fd, buffer.data(), buffer.size(), 0)) > 0) {
    response.append(buffer.data(), static_cast<std::size_t>(read));
  }
  ::close(fd);
  HttpResponse result;
  const std::size_t status_space = response.find(' ');
  if (status_space != std::string::npos) {
    result.status = parse_int(response.c_str() + status_space + 1);
  }
  const std::size_t body_start = response.find("\r\n\r\n");
  if (body_start != std::string::npos) {
    result.headers = response.substr(0, body_start);
    result.body = response.substr(body_start + 4);
  }
  return result;
}

// Same wire behavior as http_get(), but reports failures via the return
// value (status -1) instead of doctest's REQUIRE/CHECK -- those macros are
// not documented as safe to call concurrently from multiple background
// threads within one TEST_CASE, which an admission-limit test (HSE-92 round
// 3) needs many of at once.
HttpResponse plain_http_get(int port, const std::string &target) {
  HttpResponse result;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    result.status = -1;
    return result;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    result.status = -1;
    return result;
  }
  const std::string wire = "GET " + target +
                           " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                           "Connection: close\r\n\r\n";
  std::size_t sent = 0;
  while (sent < wire.size()) {
    const ssize_t written =
        ::send(fd, wire.data() + sent, wire.size() - sent, 0);
    if (written <= 0) {
      break;
    }
    sent += static_cast<std::size_t>(written);
  }
  std::string response;
  std::array<char, 8192> buffer{};
  ssize_t read = 0;
  while ((read = ::recv(fd, buffer.data(), buffer.size(), 0)) > 0) {
    response.append(buffer.data(), static_cast<std::size_t>(read));
  }
  ::close(fd);
  const std::size_t status_space = response.find(' ');
  if (status_space != std::string::npos) {
    result.status = parse_int(response.c_str() + status_space + 1);
  }
  return result;
}

// Opens a raw loopback connection and closes it immediately without sending
// (or after sending only a partial request line), simulating a client
// aborting a request mid-flight.
void abrupt_disconnect(int port, bool send_partial) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&address),
                    sizeof(address)) == 0);
  if (send_partial) {
    const std::string partial = "GET /api/graph?token=";
    (void)::send(fd, partial.data(), partial.size(), 0);
  }
  ::close(fd);
}

// A std::streambuf that hands the first fully-written line to a promise.
// serve_live() writes exactly one line (the loopback URL) before entering
// its blocking accept loop, so this lets the test thread wait for that line
// without a racy poll/sleep over a shared std::ostringstream.
class FirstLineCapture : public std::streambuf {
public:
  explicit FirstLineCapture(std::promise<std::string> &target)
      : target_(target) {}

protected:
  int overflow(int ch) override {
    if (ch == std::char_traits<char>::eof()) {
      return ch;
    }
    if (ch == '\n') {
      if (!delivered_) {
        target_.set_value(line_);
        delivered_ = true;
      }
      line_.clear();
    } else {
      line_.push_back(static_cast<char>(ch));
    }
    return ch;
  }

private:
  std::promise<std::string> &target_;
  std::string line_;
  bool delivered_ = false;
};

struct RunningServer {
  std::promise<std::string> url_promise;
  FirstLineCapture capture_buf{url_promise};
  std::ostream out{&capture_buf};
  std::ostringstream err;
  // Owned copies, not references: the constructor's `graph`/`search`/
  // `evidence` parameters are frequently bound to a temporary GraphProvider
  // built at the call site (e.g. `RunningServer(graph_provider_for(db))`),
  // whose lifetime ends at the end of the constructor-call statement. The
  // server thread below runs asynchronously for the lifetime of this
  // object, so it must close over storage that lives exactly as long as
  // RunningServer does -- these members, not the constructor's parameters.
  ui::GraphProvider graph_provider;
  ui::GraphProvider search_provider;
  ui::GraphProvider evidence_provider;
  ui::ServerOptions options{.port = 0, .launch_browser = false};
  std::thread thread;
  int port = 0;
  std::string token;

  explicit RunningServer(ui::GraphProvider graph, ui::GraphProvider search = {},
                         ui::GraphProvider evidence = {},
                         ui::ServerOptions server_options =
                             ui::ServerOptions{.port = 0,
                                               .launch_browser = false})
      : graph_provider(std::move(graph)), search_provider(std::move(search)),
        evidence_provider(std::move(evidence)), options(server_options) {
    thread = std::thread([this] {
      ui::serve_live(ui::render_html(json_out::Value::obj({}),
                                     ui::RenderMode::LoopbackLive),
                     graph_provider, search_provider, evidence_provider,
                     options, out, err);
    });
    auto future = url_promise.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(5)) ==
            std::future_status::ready);
    const std::string url = future.get();
    const std::size_t port_start = url.find("127.0.0.1:") + 10;
    const std::size_t port_end = url.find('/', port_start);
    port = parse_int(url.substr(port_start, port_end - port_start));
    const std::size_t token_key = url.find("token=");
    REQUIRE(token_key != std::string::npos);
    token = url.substr(token_key + 6);
  }

  // Authenticated shutdown, then join with a bounded timeout so a hung
  // server loop fails the test instead of the test suite. A destructor must
  // never let an exception escape (http_get()/thread::join() can both
  // throw), so a failure here becomes a best-effort detach rather than a
  // terminate().
  ~RunningServer() {
    try {
      if (thread.joinable()) {
        (void)http_get(port, "/api/shutdown?token=" + token);
        thread.join();
      }
    } catch (...) {
      try {
        if (thread.joinable()) {
          thread.detach();
        }
      } catch (...) {
        // Best-effort cleanup only; a destructor must never propagate.
        (void)0;
      }
    }
  }

  RunningServer(const RunningServer &) = delete;
  RunningServer &operator=(const RunningServer &) = delete;
};

ui::GraphProvider graph_provider_for(Storage &db) {
  return [&db](std::string_view target, const ui::CancelToken &should_cancel)
             -> std::optional<std::string> {
    try {
      const ui::GraphViewRequest request =
          cidx::cli::parse_live_graph_request(ui::GraphViewRequest{}, target);
      return json_out::dumps_indent2(
          ui::build_graph_view(db, request, should_cancel));
    } catch (const std::exception &) {
      return std::nullopt;
    }
  };
}

ui::GraphProvider search_provider_for(Storage &db) {
  return [&db](std::string_view target, const ui::CancelToken &should_cancel)
             -> std::optional<std::string> {
    (void)should_cancel;
    const std::string needle = "q=";
    const std::size_t start = target.find(needle);
    if (start == std::string_view::npos) {
      return std::nullopt;
    }
    const std::size_t value_start = start + needle.size();
    std::size_t value_end = target.find('&', value_start);
    if (value_end == std::string_view::npos) {
      value_end = target.size();
    }
    const std::string text(target.substr(value_start, value_end - value_start));
    return json_out::dumps_indent2(
        ui::search_candidates(db, text, std::nullopt, std::nullopt, 25));
  };
}

ui::GraphProvider evidence_provider_for(Storage &db) {
  return [&db](std::string_view target, const ui::CancelToken &should_cancel)
             -> std::optional<std::string> {
    (void)should_cancel;
    const std::string needle = "edge=";
    const std::size_t start = target.find(needle);
    if (start == std::string_view::npos) {
      return std::nullopt;
    }
    const std::size_t value_start = start + needle.size();
    std::size_t value_end = target.find('&', value_start);
    if (value_end == std::string_view::npos) {
      value_end = target.size();
    }
    const std::string edge_id(
        target.substr(value_start, value_end - value_start));
    // Boundary-aware lookup, matching graph_provider_for's own convention:
    // a plain substring search for "site_offset=" would also match inside
    // an unrelated longer key.
    const auto param =
        [&](std::string_view name) -> std::optional<std::string> {
      const std::string param_needle = std::string(name) + "=";
      std::size_t pos = 0;
      while (true) {
        pos = target.find(param_needle, pos);
        if (pos == std::string_view::npos) {
          return std::nullopt;
        }
        if (pos > 0 && (target[pos - 1] == '?' || target[pos - 1] == '&')) {
          break;
        }
        pos += 1;
      }
      const std::size_t param_value_start = pos + param_needle.size();
      std::size_t param_value_end = target.find('&', param_value_start);
      if (param_value_end == std::string_view::npos) {
        param_value_end = target.size();
      }
      return std::string(target.substr(param_value_start,
                                       param_value_end - param_value_start));
    };
    int site_offset = 0;
    if (const auto value = param("site_offset")) {
      site_offset = parse_int(*value);
    }
    int site_limit = 200;
    if (const auto value = param("site_limit")) {
      site_limit = parse_int(*value);
    }
    try {
      return json_out::dumps_indent2(ui::load_edge_evidence(
          db, edge_id, std::nullopt, site_offset, site_limit));
    } catch (const std::exception &) {
      return std::nullopt;
    }
  };
}

// The portable edge id of the (fixture-unique) edge of the given relation
// kind. edge_value() emits "id" before "kind" within one JSON object, so the
// nearest preceding "id": "edge:v1:..." occurrence belongs to that edge.
std::string edge_portable_id_for_kind(const std::string &json,
                                      const std::string &kind) {
  const std::string kind_marker = R"("kind": ")" + kind + R"(")";
  const std::size_t kind_pos = json.find(kind_marker);
  if (kind_pos == std::string::npos) {
    return {};
  }
  const std::string id_marker = R"("id": "edge:v1:)";
  const std::size_t id_pos = json.rfind(id_marker, kind_pos);
  if (id_pos == std::string::npos) {
    return {};
  }
  const std::size_t value_start = id_pos + std::string(R"("id": ")").size();
  const std::size_t value_end = json.find('"', value_start);
  return value_end == std::string::npos
             ? std::string{}
             : json.substr(value_start, value_end - value_start);
}

// Every portable edge id of the given relation kind present in this page's
// JSON body (unlike edge_portable_id_for_kind(), which assumes exactly one
// and returns only the first). Used to union edge ids delivered across a
// multi-page continuation walk (HSE-92 round 3).
std::vector<std::string> edge_portable_ids_for_kind(const std::string &json,
                                                    const std::string &kind) {
  std::vector<std::string> ids;
  const std::string kind_marker = R"("kind": ")" + kind + R"(")";
  std::size_t search_from = 0;
  while (true) {
    const std::size_t kind_pos = json.find(kind_marker, search_from);
    if (kind_pos == std::string::npos) {
      break;
    }
    search_from = kind_pos + kind_marker.size();
    const std::string id_marker = R"("id": "edge:v1:)";
    const std::size_t id_pos = json.rfind(id_marker, kind_pos);
    if (id_pos == std::string::npos) {
      continue;
    }
    const std::size_t value_start = id_pos + std::string(R"("id": ")").size();
    const std::size_t value_end = json.find('"', value_start);
    if (value_end == std::string::npos) {
      continue;
    }
    ids.push_back(json.substr(value_start, value_end - value_start));
  }
  return ids;
}

} // namespace

TEST_CASE("Live explorer: search resolves a typed candidate list") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db),
                       search_provider_for(fixture.db));
  CHECK_FALSE(server.options.launch_browser);
  const auto response =
      http_get(server.port, "/api/search?token=" + server.token + "&q=ns::a");
  CHECK(response.status == 200);
  CHECK(response.body.find("cidx.graph-view.search.v1") != std::string::npos);
  CHECK(response.body.find("USR::a") != std::string::npos);
}

TEST_CASE("Live explorer: expand out and expand in traverse adjacent edges") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto out_response =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&root=ns::a&direction=out");
  CHECK(out_response.status == 200);
  CHECK(out_response.body.find("USR::a") != std::string::npos);
  CHECK(out_response.body.find("USR::b") != std::string::npos);

  const auto in_response =
      http_get(server.port,
               "/api/graph?token=" + server.token + "&root=ns::b&direction=in");
  CHECK(in_response.status == 200);
  CHECK(in_response.body.find("USR::b") != std::string::npos);
  CHECK(in_response.body.find("USR::a") != std::string::npos);
}

TEST_CASE("Live explorer: a bounded witness path resolves a -> b -> c") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto response =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&input_kind=path&input=ns::a->ns::c");
  CHECK(response.status == 200);
  CHECK(response.body.find("witness-path") != std::string::npos);
  CHECK(response.body.find("USR::a") != std::string::npos);
  CHECK(response.body.find("USR::b") != std::string::npos);
  CHECK(response.body.find("USR::c") != std::string::npos);
}

// Server-side filter, NOT client-side grouping (HSE-92 review: this test was
// previously misnamed "grouping", but it never exercises Cytoscape compound
// parent assignment -- see web/grouping_dom_test.js for the real grouping
// coverage, which loads web/app.js and calls applyGrouping() end-to-end).
TEST_CASE(
    "Live explorer: node_kind filter restricts the result by symbol kind") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto response =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&root=ns::a&direction=out&node_kind=function");
  CHECK(response.status == 200);
  CHECK(response.body.find("USR::b") != std::string::npos);
  CHECK(response.body.find("USR::v") == std::string::npos);
}

TEST_CASE("Live explorer: a continuation token pages a truncated result "
          "deterministically") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto first =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&root=ns::a&direction=out&limit=1");
  CHECK(first.status == 200);
  CHECK(first.body.find(R"("available": true)") != std::string::npos);
  const std::string token_marker = R"("token": ")";
  const std::size_t token_start = first.body.find(token_marker);
  REQUIRE(token_start != std::string::npos);
  const std::size_t value_start = token_start + token_marker.size();
  const std::size_t value_end = first.body.find('"', value_start);
  REQUIRE(value_end != std::string::npos);
  const std::string continuation =
      first.body.substr(value_start, value_end - value_start);
  CHECK_FALSE(continuation.empty());

  const auto second = http_get(
      server.port,
      "/api/graph?token=" + server.token +
          "&root=ns::a&direction=out&limit=1&continuation=" + continuation);
  CHECK(second.status == 200);
  // Repeating the exact same normalized query+continuation is deterministic.
  const auto repeat = http_get(
      server.port,
      "/api/graph?token=" + server.token +
          "&root=ns::a&direction=out&limit=1&continuation=" + continuation);
  CHECK(second.body == repeat.body);
}

TEST_CASE("Live explorer: a continuation token from a different query is "
          "rejected") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto first =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&root=ns::a&direction=out&limit=1");
  const std::string token_marker = R"("token": ")";
  const std::size_t token_start = first.body.find(token_marker);
  REQUIRE(token_start != std::string::npos);
  const std::size_t value_start = token_start + token_marker.size();
  const std::size_t value_end = first.body.find('"', value_start);
  const std::string continuation =
      first.body.substr(value_start, value_end - value_start);

  // Same token, different root: the provider throws GraphViewError, so
  // build_graph_view's graph_provider catch path answers 400.
  const auto mismatched = http_get(
      server.port,
      "/api/graph?token=" + server.token +
          "&root=ns::b&direction=out&limit=1&continuation=" + continuation);
  CHECK(mismatched.status == 400);
}

TEST_CASE("Live explorer: a continuation token is rejected when replayed "
          "under a different filter") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto first = http_get(
      server.port, "/api/graph?token=" + server.token +
                       "&root=ns::a&direction=out&limit=1&node_kind=function");
  CHECK(first.status == 200);
  const std::string token_marker = R"("token": ")";
  const std::size_t token_start = first.body.find(token_marker);
  REQUIRE(token_start != std::string::npos);
  const std::size_t value_start = token_start + token_marker.size();
  const std::size_t value_end = first.body.find('"', value_start);
  REQUIRE(value_end != std::string::npos);
  const std::string continuation =
      first.body.substr(value_start, value_end - value_start);

  // Identical root/direction/limit, but a DIFFERENT node_kind filter: the
  // token must not be accepted, since it was minted under a different
  // normalized query (HSE-92 review: filters were not bound into the
  // continuation query identity).
  const auto mismatched = http_get(
      server.port, "/api/graph?token=" + server.token +
                       "&root=ns::a&direction=out&limit=1&node_kind=variable"
                       "&continuation=" +
                       continuation);
  CHECK(mismatched.status == 400);
}

// Counts the node entries actually delivered in THIS page's own "nodes"
// array (isolated by bracket-depth matching, so the unrelated
// metadata.root_resolution.candidates[].usr field -- always present once
// per page for an unambiguous root, regardless of budget -- is never
// mistaken for a delivered node).
int count_delivered_nodes(const std::string &body) {
  const std::string marker = R"("nodes": [)";
  const std::size_t start = body.find(marker);
  if (start == std::string::npos) {
    return 0;
  }
  const std::size_t open = start + marker.size() - 1;
  int depth = 0;
  std::size_t end = std::string::npos;
  for (std::size_t i = open; i < body.size(); ++i) {
    if (body[i] == '[') {
      ++depth;
    } else if (body[i] == ']') {
      --depth;
      if (depth == 0) {
        end = i;
        break;
      }
    }
  }
  if (end == std::string::npos) {
    return 0;
  }
  const std::string section = body.substr(open, end - open + 1);
  const std::string needle = R"("usr": ")";
  int count = 0;
  std::size_t pos = 0;
  while ((pos = section.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

TEST_CASE("Live explorer: continuation reaches every candidate without any "
          "single page exceeding node_budget (star repro)") {
  // HSE-92 round 2, two blockers reproduced together with the reviewer's
  // own numbers ("a root with three call targets and limit=1"):
  //  - fix 1: make_query_plan() used to cap the underlying query itself at
  //    node_budget+1 regardless of node_offset, so continuation only ever
  //    re-paged that same capped prefix -- later candidates were never
  //    reachable no matter how many pages were requested.
  //  - fix 2: a bridging edge endpoint used to be pushed OUTSIDE
  //    delivered_node_count/node_budget entirely, so a high-degree root
  //    could return one primary node plus up to edge_budget bridge
  //    endpoints in a single page, blowing past the declared node_budget.
  Storage db(":memory:");
  const int64_t component = db.add_component("test", "/tmp/cidx-ui-star-test");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "star.cpp");
  const auto make_node = [&](const char *usr, const char *name, int line) {
    auto sym = symbol(usr, name, "function");
    sym.file_id = file;
    sym.line = line;
    return db.add_symbol(sym);
  };
  const int64_t root = make_node("USR::star_root", "ns::star_root", 1);
  const int64_t target0 = make_node("USR::star_t0", "ns::star_t0", 2);
  const int64_t target1 = make_node("USR::star_t1", "ns::star_t1", 3);
  const int64_t target2 = make_node("USR::star_t2", "ns::star_t2", 4);
  const int64_t target3 = make_node("USR::star_t3", "ns::star_t3", 5);
  const int64_t target4 = make_node("USR::star_t4", "ns::star_t4", 6);
  const auto add_edge = [&](int64_t src, int64_t dst, int line) {
    cidx::Edge edge;
    edge.src_id = src;
    edge.dst_id = dst;
    edge.kind = cidx::graph::edge_kinds_map().at("calls");
    const int64_t edge_id = db.add_edge(edge);
    cidx::EdgeSite site;
    site.edge_id = edge_id;
    site.file_id = file;
    site.line = line;
    site.col = 4;
    site.conditional = 0;
    db.add_edge_site(site);
  };
  add_edge(root, target0, 10);
  add_edge(root, target1, 20);
  add_edge(root, target2, 30);
  add_edge(root, target3, 40);
  add_edge(root, target4, 50);

  RunningServer server(graph_provider_for(db));
  const std::string base = "/api/graph?token=" + server.token +
                           "&root=ns::star_root&direction=out&depth=1&limit=1"
                           "&edge_limit=1";

  std::set<std::string> seen;
  std::set<std::string> seen_edges;
  std::string continuation;
  for (int page = 0; page < 24; ++page) {
    std::string url = base;
    if (!continuation.empty()) {
      url += "&continuation=";
      url += continuation;
    }
    const auto response = http_get(server.port, url);
    CHECK(response.status == 200);
    // Fix 2: node_budget is 1 for this whole walk -- no page, including one
    // that bridges a cross-page edge endpoint in, may ever deliver more.
    CHECK(count_delivered_nodes(response.body) <= 1);
    for (const char *usr : {"USR::star_root", "USR::star_t0", "USR::star_t1",
                            "USR::star_t2", "USR::star_t3", "USR::star_t4"}) {
      if (response.body.contains(usr)) {
        seen.insert(usr);
      }
    }
    for (const auto &edge_id :
         edge_portable_ids_for_kind(response.body, "calls")) {
      seen_edges.insert(edge_id);
    }
    const std::string token_marker = R"("token": ")";
    const std::size_t token_start = response.body.find(token_marker);
    if (token_start == std::string::npos) {
      break;
    }
    const std::size_t value_start = token_start + token_marker.size();
    const std::size_t value_end = response.body.find('"', value_start);
    if (value_end == std::string::npos) {
      break;
    }
    continuation = response.body.substr(value_start, value_end - value_start);
    if (continuation.empty()) {
      break;
    }
  }
  // Fix 1: every candidate is eventually reachable across the full
  // continuation walk -- not just the root and one target, repeated
  // forever.
  CHECK(seen.contains("USR::star_root"));
  CHECK(seen.contains("USR::star_t0"));
  CHECK(seen.contains("USR::star_t1"));
  CHECK(seen.contains("USR::star_t2"));
  CHECK(seen.contains("USR::star_t3"));
  CHECK(seen.contains("USR::star_t4"));
  // HSE-92 round 3: cross-page edges are eventually delivered too, not
  // permanently dropped once their source node's own page has gone by --
  // reviewer's exact repro (root -> t0,t1,t2, depth=1&limit=1, union edge
  // ids across the full continuation walk) used to see 0/3 `calls` edges.
  CHECK(seen_edges.size() == 5);
}

TEST_CASE("Live explorer: continuation reaches every candidate when USRs "
          "sort in the reverse of rowid order (HSE-92 review P1-1/P2-2)") {
  // The star-repro test above passes even with the P1-1/P2-2 bug because
  // its fixture's USRs (star_root, star_t0..t4) happen to sort in the same
  // order they were inserted, so the candidate universe's re-sorted-by-
  // portable-id prefix never shifts between pages. Real USRs have no such
  // property. This fixture inserts the root LAST alphabetically and the
  // targets in the EXACT REVERSE of their alphabetical order (t9 first,
  // t5 last) -- the reviewer's own repro -- so a continuation
  // implementation that pages by POSITION over a re-derived, growing
  // candidate list drops the alphabetically-earliest targets forever: only
  // 4 of 6 nodes were ever delivered before the fix.
  Storage db(":memory:");
  const int64_t component =
      db.add_component("test", "/tmp/cidx-ui-reverse-test");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "reverse.cpp");
  const auto make_node = [&](const char *usr, const char *name, int line) {
    auto sym = symbol(usr, name, "function");
    sym.file_id = file;
    sym.line = line;
    return db.add_symbol(sym);
  };
  // Insertion (and rowid) order: root, t9, t8, t7, t6, t5.
  // Alphabetical (portable-id) order: t5, t6, t7, t8, t9, z_root.
  const int64_t root = make_node("USR::z_root", "ns::z_root", 1);
  const int64_t t9 = make_node("USR::t9", "ns::t9", 2);
  const int64_t t8 = make_node("USR::t8", "ns::t8", 3);
  const int64_t t7 = make_node("USR::t7", "ns::t7", 4);
  const int64_t t6 = make_node("USR::t6", "ns::t6", 5);
  const int64_t t5 = make_node("USR::t5", "ns::t5", 6);
  const auto add_edge = [&](int64_t src, int64_t dst, int line) {
    cidx::Edge edge;
    edge.src_id = src;
    edge.dst_id = dst;
    edge.kind = cidx::graph::edge_kinds_map().at("calls");
    const int64_t edge_id = db.add_edge(edge);
    cidx::EdgeSite site;
    site.edge_id = edge_id;
    site.file_id = file;
    site.line = line;
    site.col = 4;
    site.conditional = 0;
    db.add_edge_site(site);
  };
  add_edge(root, t9, 10);
  add_edge(root, t8, 20);
  add_edge(root, t7, 30);
  add_edge(root, t6, 40);
  add_edge(root, t5, 50);

  RunningServer server(graph_provider_for(db));
  const std::string base = "/api/graph?token=" + server.token +
                           "&root=ns::z_root&direction=out&depth=1&limit=1"
                           "&edge_limit=1";

  std::set<std::string> seen;
  std::string continuation;
  for (int page = 0; page < 40; ++page) {
    std::string url = base;
    if (!continuation.empty()) {
      url += "&continuation=";
      url += continuation;
    }
    const auto response = http_get(server.port, url);
    CHECK(response.status == 200);
    for (const char *usr : {"USR::z_root", "USR::t9", "USR::t8", "USR::t7",
                            "USR::t6", "USR::t5"}) {
      if (response.body.contains(usr)) {
        seen.insert(usr);
      }
    }
    const std::string token_marker = R"("token": ")";
    const std::size_t token_start = response.body.find(token_marker);
    if (token_start == std::string::npos) {
      break;
    }
    const std::size_t value_start = token_start + token_marker.size();
    const std::size_t value_end = response.body.find('"', value_start);
    if (value_end == std::string::npos) {
      break;
    }
    continuation = response.body.substr(value_start, value_end - value_start);
    if (continuation.empty()) {
      break;
    }
  }
  CHECK(seen.contains("USR::z_root"));
  CHECK(seen.contains("USR::t9"));
  CHECK(seen.contains("USR::t8"));
  CHECK(seen.contains("USR::t7"));
  // Before the fix, these two -- the alphabetically-earliest targets, which
  // only enter the growing candidate universe on a LATER page -- were
  // permanently skipped.
  CHECK(seen.contains("USR::t6"));
  CHECK(seen.contains("USR::t5"));
}

TEST_CASE("Live explorer: namespace filter matches a symbol whose "
          "qualified name carries a parameter-list signature (HSE-92 "
          "review P1-2)") {
  // `graph::Sym::name` is COALESCE(qual_name, spelling), and qual_name
  // carries the FULL signature for functions/methods (parameter types,
  // and for methods, trailing cv-qualifiers) -- e.g.
  // "ns::Class::method(const std::string &) const". A plain
  // rfind("::") lands inside the last parameter type instead of the real
  // namespace boundary, so `namespace=ns` never matched a real function.
  Storage db(":memory:");
  const int64_t component =
      db.add_component("test", "/tmp/cidx-ui-ns-sig-test");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "ns_sig.cpp");
  const auto make_node = [&](const char *usr, const char *name, int line) {
    auto sym = symbol(usr, name, "function");
    sym.file_id = file;
    sym.line = line;
    return db.add_symbol(sym);
  };
  const int64_t caller = make_node("USR::caller", "ns::caller()", 1);
  const int64_t callee =
      make_node("USR::callee", "ns::callee(const std::string &)", 2);
  cidx::Edge edge;
  edge.src_id = caller;
  edge.dst_id = callee;
  edge.kind = cidx::graph::edge_kinds_map().at("calls");
  const int64_t edge_id = db.add_edge(edge);
  cidx::EdgeSite site;
  site.edge_id = edge_id;
  site.file_id = file;
  site.line = 5;
  site.col = 4;
  site.conditional = 0;
  db.add_edge_site(site);

  RunningServer server(graph_provider_for(db));
  const auto matching = http_get(
      server.port, "/api/graph?token=" + server.token +
                       "&root=ns::caller()&direction=out&depth=1&namespace=ns");
  CHECK(matching.status == 200);
  CHECK(matching.body.contains("USR::callee"));

  const auto non_matching = http_get(
      server.port,
      "/api/graph?token=" + server.token +
          "&root=ns::caller()&direction=out&depth=1&namespace=other_ns");
  CHECK(non_matching.status == 200);
  CHECK_FALSE(non_matching.body.contains("USR::callee"));
}

TEST_CASE("Live explorer: filters progress beyond the first raw candidate "
          "chunk and namespace is bound to continuation") {
  Storage db(":memory:");
  const int64_t component = db.add_component("test", "/tmp/cidx-ui-filter");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file_a = db.add_file(directory, "a.cpp");
  const int64_t file_b = db.add_file(directory, "b.cpp");
  const auto make_node = [&](const char *usr, const char *name, int64_t file) {
    auto sym = symbol(usr, name, "function");
    sym.file_id = file;
    return db.add_symbol(sym);
  };
  const int64_t root = make_node("USR::root", "first::root", file_a);
  const int64_t early = make_node("USR::early", "first::early", file_a);
  const int64_t early2 = make_node("USR::early2", "first::early2", file_a);
  const int64_t early3 = make_node("USR::early3", "first::early3", file_a);
  const int64_t wanted = make_node("USR::wanted", "wanted::target", file_b);
  const int64_t wrong_namespace =
      make_node("USR::wrong_namespace", "other::target", file_b);
  const auto add_edge = [&](int64_t dst) {
    cidx::Edge edge;
    edge.src_id = root;
    edge.dst_id = dst;
    edge.kind = cidx::graph::edge_kinds_map().at("calls");
    db.add_edge(edge);
  };
  add_edge(early);
  add_edge(early2);
  add_edge(early3);
  add_edge(wanted);
  add_edge(wrong_namespace);

  RunningServer server(graph_provider_for(db));
  const std::string base =
      "/api/graph?token=" + server.token +
      "&root=first::root&direction=out&depth=1&limit=1&file=b.cpp"
      "&namespace=wanted&edge=calls";
  auto response = http_get(server.port, base);
  std::string combined = response.body;
  for (int page = 0; page < 8; ++page) {
    const std::string marker = R"("token": ")";
    const std::size_t start = response.body.find(marker);
    if (start == std::string::npos) {
      break;
    }
    const std::size_t value_start = start + marker.size();
    const std::size_t value_end = response.body.find('"', value_start);
    if (value_end == std::string::npos) {
      break;
    }
    const std::string token =
        response.body.substr(value_start, value_end - value_start);
    std::string continuation_url = base;
    continuation_url += "&continuation=";
    continuation_url += token;
    response = http_get(server.port, continuation_url);
    CHECK(response.status == 200);
    combined += response.body;
  }
  CHECK(combined.contains("USR::wanted"));
  CHECK_FALSE(combined.contains("USR::early"));
  CHECK_FALSE(combined.contains("USR::wrong_namespace"));
}

TEST_CASE("Live explorer: an applicability filter that empties every edge "
          "window still lets the continuation chain terminate (HSE-92 "
          "senior-dev round-1)") {
  // Reviewer's exact repro: root + 5 targets, 5 UNCONDITIONAL `calls` edges,
  // applicability=conditional (so passes_applicability_filter() excludes
  // every single one of them), edge_limit=1 so each page's edge-offset
  // window only ever contains one candidate. With node limit=50 all 6 nodes
  // land on page 1, so every subsequent page is a pure edge-continuation
  // page. The bug: delivered_edge_count used to be computed from how many
  // edges were actually EMITTED after the applicability filter (always 0
  // here), so next_edge_offset never advanced past the same value and the
  // server handed back the byte-identical token forever -- "Load more"
  // became a permanent no-op. The fix advances the offset by how many
  // candidates this page CONSUMED from the window (1, every page,
  // regardless of the filter outcome), so the chain must reach "no more
  // candidates" and emit `"token": null` within a small, bounded number of
  // pages.
  Storage db(":memory:");
  const int64_t component =
      db.add_component("test", "/tmp/cidx-ui-applicability-terminate");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "applic.cpp");
  const auto make_node = [&](const char *usr, const char *name, int line) {
    auto sym = symbol(usr, name, "function");
    sym.file_id = file;
    sym.line = line;
    return db.add_symbol(sym);
  };
  const int64_t root = make_node("USR::applic_root", "ns::applic_root", 1);
  const int64_t t0 = make_node("USR::applic_t0", "ns::applic_t0", 2);
  const int64_t t1 = make_node("USR::applic_t1", "ns::applic_t1", 3);
  const int64_t t2 = make_node("USR::applic_t2", "ns::applic_t2", 4);
  const int64_t t3 = make_node("USR::applic_t3", "ns::applic_t3", 5);
  const int64_t t4 = make_node("USR::applic_t4", "ns::applic_t4", 6);
  const auto add_edge = [&](int64_t dst, int line) {
    cidx::Edge edge;
    edge.src_id = root;
    edge.dst_id = dst;
    edge.kind = cidx::graph::edge_kinds_map().at("calls");
    const int64_t edge_id = db.add_edge(edge);
    cidx::EdgeSite site;
    site.edge_id = edge_id;
    site.file_id = file;
    site.line = line;
    site.col = 4;
    site.conditional =
        0; // unconditional -- excluded by applicability=conditional
    db.add_edge_site(site);
  };
  add_edge(t0, 10);
  add_edge(t1, 20);
  add_edge(t2, 30);
  add_edge(t3, 40);
  add_edge(t4, 50);

  RunningServer server(graph_provider_for(db));
  const std::string base =
      "/api/graph?token=" + server.token +
      "&root=ns::applic_root&direction=out&depth=1&limit=50&edge_limit=1"
      "&applicability=conditional";

  std::string continuation;
  std::set<std::string> tokens_seen;
  bool terminated = false;
  for (int page = 0; page < 10; ++page) {
    std::string url = base;
    if (!continuation.empty()) {
      url += "&continuation=";
      url += continuation;
    }
    const auto response = http_get(server.port, url);
    CHECK(response.status == 200);
    // Every candidate `calls` edge is unconditional, so applicability=
    // conditional must exclude every one of them, on every page.
    CHECK(edge_portable_ids_for_kind(response.body, "calls").empty());
    const std::string token_marker = R"("token": ")";
    const std::size_t token_start = response.body.find(token_marker);
    if (token_start == std::string::npos) {
      // "token": null -- the chain has genuinely run out of candidates.
      terminated = true;
      break;
    }
    const std::size_t value_start = token_start + token_marker.size();
    const std::size_t value_end = response.body.find('"', value_start);
    REQUIRE(value_end != std::string::npos);
    continuation = response.body.substr(value_start, value_end - value_start);
    REQUIRE_FALSE(continuation.empty());
    // A non-terminating chain re-mints the exact same token forever; a
    // repeated token proves the offset never advanced.
    CHECK(tokens_seen.insert(continuation).second);
  }
  CHECK(terminated);
}

TEST_CASE("Live explorer: a byte budget that trims every edge window still "
          "lets the continuation chain terminate (HSE-92 senior-dev "
          "round-2)") {
  // Reviewer's exact repro shape: a root with 9 unconditional `calls`
  // targets, node/edge budgets wide enough that every node and edge would
  // normally fit on page one (limit=10, edge_limit=9), but a byte_limit
  // small enough that byte-trim pops every edge (and, once those run out,
  // primary nodes too) back out of the rendered response on every page.
  // The bug: byte-trim decremented `delivered_edge_count`/
  // `delivered_node_count` for every popped item, and apply_continuation_
  // token() used those SAME post-trim counters to advance the next
  // continuation offset -- so a page that legitimately consumed every
  // remaining candidate, but couldn't fit any of them under the byte
  // budget, re-minted the exact same node_offset/edge_offset forever:
  // "edges": [] and the byte-identical token on every page, "Load more"
  // a permanent no-op. The fix freezes the offset-advancing counters
  // BEFORE byte-trim runs and stops minting a token at all once neither
  // candidate window has more to offer AND this page's own consumption
  // didn't move either offset -- so the chain must reach `"token": null`
  // within a small, bounded number of pages regardless of how aggressively
  // byte-trim empties the rendered page.
  Storage db(":memory:");
  const int64_t component =
      db.add_component("test", "/tmp/cidx-ui-byte-trim-terminate");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "bytetrim.cpp");
  const auto make_node = [&](const char *usr, const char *name, int line) {
    auto sym = symbol(usr, name, "function");
    sym.file_id = file;
    sym.line = line;
    return db.add_symbol(sym);
  };
  const int64_t root = make_node("USR::bytetrim_root", "ns::bytetrim_root", 1);
  std::vector<int64_t> targets;
  for (int index = 0; index < 9; ++index) {
    const std::string usr = "USR::bytetrim_t" + std::to_string(index);
    const std::string name = "ns::bytetrim_t" + std::to_string(index);
    targets.push_back(make_node(usr.c_str(), name.c_str(), index + 2));
  }
  const auto add_edge = [&](int64_t dst, int line) {
    cidx::Edge edge;
    edge.src_id = root;
    edge.dst_id = dst;
    edge.kind = cidx::graph::edge_kinds_map().at("calls");
    const int64_t edge_id = db.add_edge(edge);
    cidx::EdgeSite site;
    site.edge_id = edge_id;
    site.file_id = file;
    site.line = line;
    site.col = 4;
    site.conditional = 0;
    db.add_edge_site(site);
  };
  for (int index = 0; index < 9; ++index) {
    add_edge(targets[static_cast<std::size_t>(index)], 100 + index * 10);
  }

  RunningServer server(graph_provider_for(db));
  const std::string base =
      "/api/graph?token=" + server.token +
      "&root=ns::bytetrim_root&direction=out&depth=1&limit=10&edge_limit=9"
      "&byte_limit=24000";

  std::string continuation;
  std::set<std::string> tokens_seen;
  bool terminated = false;
  for (int page = 0; page < 20; ++page) {
    std::string url = base;
    if (!continuation.empty()) {
      url += "&continuation=";
      url += continuation;
    }
    const auto response = http_get(server.port, url);
    CHECK(response.status == 200);
    const std::string token_marker = R"("token": ")";
    const std::size_t token_start = response.body.find(token_marker);
    if (token_start == std::string::npos) {
      // "token": null -- the chain has genuinely run out of candidates
      // (rather than looping on byte-trimmed, byte-identical pages).
      terminated = true;
      break;
    }
    const std::size_t value_start = token_start + token_marker.size();
    const std::size_t value_end = response.body.find('"', value_start);
    REQUIRE(value_end != std::string::npos);
    continuation = response.body.substr(value_start, value_end - value_start);
    REQUIRE_FALSE(continuation.empty());
    // A non-terminating chain re-mints the exact same token forever (the
    // reported defect: byte-identical bodies with the same token on every
    // page); a repeated token proves the offset never advanced.
    CHECK(tokens_seen.insert(continuation).second);
  }
  CHECK(terminated);
}

TEST_CASE("Live explorer: the edge-continuation phase reuses its frozen full "
          "scan instead of re-reading storage on every page (HSE-92 "
          "senior-dev round-2 cost finding)") {
  // Deterministic (non-flaky) proof that the full adjacency scan performed
  // once node delivery is exhausted is memoized rather than re-executed
  // against storage on every edge-continuation page: a root with 5 targets
  // but only 2 wired-up `calls` edges, node budget wide enough that all 6
  // nodes land on page one (so the edge-continuation phase, and therefore
  // the scan this test cares about, starts immediately), edge_limit=1 so
  // paging needs (at least) two edge pages to exhaust 2 edges. A THIRD edge
  // (root -> target2, an existing, already-delivered candidate node) is
  // inserted directly into storage between the first and second page
  // requests -- late enough that a FRESH scan on page two would see it, but
  // a cached one (frozen from page one) would not. If the full scan were
  // still being re-read from storage every page (the pre-fix behavior), the
  // newly-inserted edge would surface by the following page and the chain
  // would run at least one page longer to deliver it; with the scan
  // memoized, it can never appear and the chain converges after exactly the
  // original 2 edges.
  Storage db(":memory:");
  const int64_t component =
      db.add_component("test", "/tmp/cidx-ui-edge-scan-cache");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "cache.cpp");
  const auto make_node = [&](const char *usr, const char *name, int line) {
    auto sym = symbol(usr, name, "function");
    sym.file_id = file;
    sym.line = line;
    return db.add_symbol(sym);
  };
  const int64_t root = make_node("USR::cache_root", "ns::cache_root", 1);
  std::vector<int64_t> targets;
  for (int index = 0; index < 5; ++index) {
    const std::string usr = "USR::cache_t" + std::to_string(index);
    const std::string name = "ns::cache_t" + std::to_string(index);
    targets.push_back(make_node(usr.c_str(), name.c_str(), index + 2));
  }
  const auto add_edge = [&](int64_t dst, int line) {
    cidx::Edge edge;
    edge.src_id = root;
    edge.dst_id = dst;
    edge.kind = cidx::graph::edge_kinds_map().at("calls");
    const int64_t edge_id = db.add_edge(edge);
    cidx::EdgeSite site;
    site.edge_id = edge_id;
    site.file_id = file;
    site.line = line;
    site.col = 4;
    site.conditional = 0;
    db.add_edge_site(site);
  };
  // Only the first two targets start out connected.
  add_edge(targets[0], 10);
  add_edge(targets[1], 20);

  RunningServer server(graph_provider_for(db));
  const std::string base =
      "/api/graph?token=" + server.token +
      "&root=ns::cache_root&direction=out&depth=1&limit=10&edge_limit=1";

  std::set<std::string> seen_edges;
  std::string continuation;
  bool inserted_late_edge = false;
  bool terminated = false;
  for (int page = 0; page < 10; ++page) {
    std::string url = base;
    if (!continuation.empty()) {
      url += "&continuation=";
      url += continuation;
    }
    const auto response = http_get(server.port, url);
    CHECK(response.status == 200);
    for (const auto &edge_id :
         edge_portable_ids_for_kind(response.body, "calls")) {
      seen_edges.insert(edge_id);
    }
    if (!inserted_late_edge) {
      // Inserted after the first page's response has already been read --
      // late enough that only a scan re-executed against storage on a LATER
      // page could ever observe it.
      add_edge(targets[2], 30);
      inserted_late_edge = true;
    }
    const std::string token_marker = R"("token": ")";
    const std::size_t token_start = response.body.find(token_marker);
    if (token_start == std::string::npos) {
      terminated = true;
      break;
    }
    const std::size_t value_start = token_start + token_marker.size();
    const std::size_t value_end = response.body.find('"', value_start);
    REQUIRE(value_end != std::string::npos);
    continuation = response.body.substr(value_start, value_end - value_start);
    REQUIRE_FALSE(continuation.empty());
  }
  CHECK(inserted_late_edge);
  CHECK(terminated);
  // The chain converges having delivered exactly the 2 edges that existed
  // when the frozen scan was first computed -- the late-inserted third edge
  // never surfaces, proving the scan was served from cache rather than
  // re-read from storage on the pages that followed the insert.
  CHECK(seen_edges.size() == 2);
}

// Rebuilds a continuation token with a FORGED node_offset/edge_offset pair
// but the SAME query-identity prefix as a real, server-issued token (HSE-92
// review P2-1 / senior-dev round-1 P3 finding: this rejection path had no
// regression test at all). Continuation tokens are
// "cont:v3:<len>:<identity><node_offset>,<edge_offset>" -- everything up to
// and including the length-prefixed identity block is kept byte-for-byte;
// only the trailing offset pair is replaced.
std::string forge_continuation(const std::string &real_token,
                               const std::string &node_offset,
                               const std::string &edge_offset) {
  const std::string prefix = "cont:v3:";
  REQUIRE(real_token.starts_with(prefix));
  const std::size_t length_start = prefix.size();
  const std::size_t colon = real_token.find(':', length_start);
  REQUIRE(colon != std::string::npos);
  const std::size_t identity_len = static_cast<std::size_t>(parse_int(
      std::string_view(real_token).substr(length_start, colon - length_start)));
  const std::size_t identity_end = colon + 1 + identity_len;
  REQUIRE(identity_end <= real_token.size());
  return real_token.substr(0, identity_end) + node_offset + "," + edge_offset;
}

TEST_CASE("Live explorer rejects a continuation token with a forged offset "
          "(HSE-92 review P2-1)") {
  Storage db(":memory:");
  const int64_t component = db.add_component("test", "/tmp/cidx-ui-p2-1-test");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "p2_1.cpp");
  const auto make_node = [&](const char *usr, const char *name, int line) {
    auto sym = symbol(usr, name, "function");
    sym.file_id = file;
    sym.line = line;
    return db.add_symbol(sym);
  };
  const int64_t root = make_node("USR::p2_1_root", "ns::p2_1_root", 1);
  const int64_t target = make_node("USR::p2_1_target", "ns::p2_1_target", 2);
  cidx::Edge edge;
  edge.src_id = root;
  edge.dst_id = target;
  edge.kind = cidx::graph::edge_kinds_map().at("calls");
  db.add_edge(edge);

  RunningServer server(graph_provider_for(db));
  const std::string base = "/api/graph?token=" + server.token +
                           "&root=ns::p2_1_root&direction=out&depth=1&limit=1";
  const auto seed = http_get(server.port, base);
  REQUIRE(seed.status == 200);
  const std::string token_marker = R"("token": ")";
  const std::size_t token_start = seed.body.find(token_marker);
  REQUIRE(token_start != std::string::npos);
  const std::size_t value_start = token_start + token_marker.size();
  const std::size_t value_end = seed.body.find('"', value_start);
  REQUIRE(value_end != std::string::npos);
  const std::string real_token =
      seed.body.substr(value_start, value_end - value_start);

  const auto get_with_token = [&](const std::string &token) {
    return http_get(server.port, base + "&continuation=" + token);
  };

  // One past the fixed candidate-universe cap (kCandidateUniverseCap =
  // 200000): must be rejected, not silently accepted as an enormous skip.
  CHECK(get_with_token(forge_continuation(real_token, "200001", "0")).status ==
        400);
  // The original review's exact repro value.
  CHECK(
      get_with_token(forge_continuation(real_token, "2147483647", "2147483647"))
          .status == 400);
  // A negative offset must never bind into SQL LIMIT arithmetic.
  CHECK(get_with_token(forge_continuation(real_token, "-1", "0")).status ==
        400);
  // The cap boundary itself is a legitimate value a real token could carry
  // and must not be spuriously rejected.
  CHECK(get_with_token(forge_continuation(real_token, "200000", "0")).status ==
        200);
}

TEST_CASE("Live explorer: applicability is decided from the complete edge "
          "fact, not the bounded evidence prefix") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  // ns::a -uses-> ns::v has two sites: an unconditional one (sorted first)
  // and a conditional one (sorted second). site_limit=0 would, under the
  // old bounded-prefix bug, only ever see the first (unconditional) site
  // and misreport the edge as "universal".
  const auto response =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&root=ns::a&direction=out&depth=1&site_limit=0"
                                "&applicability=universal");
  CHECK(response.status == 200);
  CHECK(response.body.find("USR::v") != std::string::npos);
  // The uses edge to v is actually conditional (mixed sites), so filtering
  // to applicability=universal must exclude it.
  CHECK(response.body.find(R"("kind": "uses")") == std::string::npos);

  const auto conditional_only =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&root=ns::a&direction=out&depth=1&site_limit=0"
                                "&applicability=conditional");
  CHECK(conditional_only.status == 200);
  CHECK(conditional_only.body.find(R"("kind": "uses")") != std::string::npos);
}

TEST_CASE("Live explorer: evidence endpoint loads bounded sites for an edge") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db), {},
                       evidence_provider_for(fixture.db));
  const auto graph = http_get(server.port, "/api/graph?token=" + server.token +
                                               "&root=ns::a&direction=out");
  const std::string edge_id = edge_portable_id_for_kind(graph.body, "calls");
  REQUIRE_FALSE(edge_id.empty());
  const auto evidence = http_get(
      server.port, "/api/evidence?token=" + server.token + "&edge=" + edge_id);
  CHECK(evidence.status == 200);
  CHECK(evidence.body.find("cidx.graph-view.evidence.v1") != std::string::npos);
  CHECK(evidence.body.find(R"("line": 10)") != std::string::npos);
}

TEST_CASE("Live explorer: evidence pagination reaches every site exactly "
          "once, even when raw storage order differs from delivery order") {
  // Critic finding: load_edge_evidence() used to fetch a raw-order PREFIX
  // (bounded_offset + bounded_limit + 1 rows, ordered by file_id, line,
  // col in SQL) and only THEN sort that prefix by the redacted-path-based
  // site_sort_key() before slicing by offset -- since file_id (insertion)
  // order does not match path order, a raw-order prefix is not a
  // delivery-order prefix, so successive pages could duplicate some sites
  // and permanently skip others. These 3 files are inserted in the OPPOSITE
  // of their path order (c.cpp, b.cpp, a.cpp) to force exactly that
  // mismatch.
  Storage db(":memory:");
  const int64_t component =
      db.add_component("test", "/tmp/cidx-ui-evidence-order-test");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file_c = db.add_file(directory, "c.cpp");
  const int64_t file_b = db.add_file(directory, "b.cpp");
  const int64_t file_a = db.add_file(directory, "a.cpp");
  auto sym_src = symbol("USR::evidence_src", "ns::evidence_src", "function");
  sym_src.file_id = file_c;
  sym_src.line = 1;
  auto sym_dst = symbol("USR::evidence_dst", "ns::evidence_dst", "function");
  sym_dst.file_id = file_c;
  sym_dst.line = 2;
  const int64_t src = db.add_symbol(sym_src);
  const int64_t dst = db.add_symbol(sym_dst);
  cidx::Edge edge;
  edge.src_id = src;
  edge.dst_id = dst;
  edge.kind = cidx::graph::edge_kinds_map().at("calls");
  const int64_t edge_row_id = db.add_edge(edge);
  const auto add_site = [&](int64_t file_id, int line) {
    cidx::EdgeSite site;
    site.edge_id = edge_row_id;
    site.file_id = file_id;
    site.line = line;
    site.col = 1;
    site.conditional = 0;
    db.add_edge_site(site);
  };
  // Inserted in file_id (raw storage) order c, b, a -- the OPPOSITE of
  // their path order.
  add_site(file_c, 10);
  add_site(file_b, 10);
  add_site(file_a, 10);

  RunningServer server(graph_provider_for(db), {}, evidence_provider_for(db));
  const auto graph =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&root=ns::evidence_src&direction=out");
  const std::string edge_id = edge_portable_id_for_kind(graph.body, "calls");
  REQUIRE_FALSE(edge_id.empty());

  int matches_a = 0;
  int matches_b = 0;
  int matches_c = 0;
  for (int offset = 0; offset < 3; ++offset) {
    const auto evidence =
        http_get(server.port, "/api/evidence?token=" + server.token +
                                  "&edge=" + edge_id + "&site_offset=" +
                                  std::to_string(offset) + "&site_limit=1");
    CHECK(evidence.status == 200);
    if (evidence.body.contains("a.cpp")) {
      ++matches_a;
    }
    if (evidence.body.contains("b.cpp")) {
      ++matches_b;
    }
    if (evidence.body.contains("c.cpp")) {
      ++matches_c;
    }
  }
  // Each of the 3 sites must appear on EXACTLY ONE of the 3 pages -- no
  // duplicates, no permanent gaps across the union of all pages.
  CHECK(matches_a == 1);
  CHECK(matches_b == 1);
  CHECK(matches_c == 1);
}

TEST_CASE("Live explorer: evidence is retrievable for an edge past the old "
          "bounded adjacency-scan cap") {
  // Critic finding: load_edge_evidence() used to resolve the edge by
  // scanning up to 2000 adjacent edges of the source and linear-searching
  // for the target -- any source with more than 2000 out-edges of that
  // kind could never have evidence loaded for an edge past that cap, even
  // though the edge is real. This fixture gives one source 2001 outgoing
  // "calls" edges and asks for evidence on the LAST one.
  Storage db(":memory:");
  const int64_t component =
      db.add_component("test", "/tmp/cidx-ui-evidence-hub-test");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "hub.cpp");
  auto sym_hub = symbol("USR::evidence_hub", "ns::evidence_hub", "function");
  sym_hub.file_id = file;
  sym_hub.line = 1;
  const int64_t hub = db.add_symbol(sym_hub);
  constexpr int kOutDegree = 2001; // one past the old 2000-edge scan cap
  for (int i = 0; i < kOutDegree; ++i) {
    const std::string usr = "USR::evidence_target_" + std::to_string(i);
    const std::string name = "ns::evidence_target_" + std::to_string(i);
    auto sym_target = symbol(usr.c_str(), name.c_str(), "function");
    sym_target.file_id = file;
    sym_target.line = i + 2;
    const int64_t target = db.add_symbol(sym_target);
    cidx::Edge edge;
    edge.src_id = hub;
    edge.dst_id = target;
    edge.kind = cidx::graph::edge_kinds_map().at("calls");
    const int64_t edge_row_id = db.add_edge(edge);
    cidx::EdgeSite site;
    site.edge_id = edge_row_id;
    site.file_id = file;
    site.line = i + 2;
    site.col = 1;
    site.conditional = 0;
    db.add_edge_site(site);
  }

  RunningServer server(graph_provider_for(db), {}, evidence_provider_for(db));
  // Query FROM the last target's own side (direction=in, depth=1): its
  // in-degree is 1 (only hub calls it), so this response is tiny and
  // unambiguous regardless of hub's 2001-edge out-degree -- the graph
  // traversal that PRODUCES this edge id is unrelated to
  // load_edge_evidence()'s own (separately capped) re-lookup, which is
  // exactly what is under test below.
  const std::string last_target_name =
      "ns::evidence_target_" + std::to_string(kOutDegree - 1);
  const auto graph = http_get(server.port, "/api/graph?token=" + server.token +
                                               "&root=" + last_target_name +
                                               "&direction=in&depth=1");
  const std::string edge_id = edge_portable_id_for_kind(graph.body, "calls");
  REQUIRE_FALSE(edge_id.empty());

  const auto evidence = http_get(
      server.port, "/api/evidence?token=" + server.token + "&edge=" + edge_id);
  CHECK(evidence.status == 200);
  CHECK(evidence.body.find("cidx.graph-view.evidence.v1") != std::string::npos);
  CHECK_FALSE(evidence.body.contains("edge no longer exists"));
}

TEST_CASE(
    "Live explorer: a byte budget truncates the response deterministically") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  // The unrestricted response for this fixture is ~24 KiB; 20500 bytes
  // exercises the soft node/edge/site trimming path (a valid, deterministic
  // truncated response) rather than the hard GraphViewFailureKind::Oversized
  // error, which only fires when even the minimal finalized skeleton cannot
  // fit the budget -- exercised below via the smallest allowed byte_limit.
  const auto truncated =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&root=ns::a&direction=out&byte_limit=20500");
  CHECK(truncated.status == 200);
  CHECK(truncated.body.find(R"("truncated": true)") != std::string::npos);
  CHECK(truncated.body.find("byte_budget") != std::string::npos);
  // Repeating the identical bounded request is deterministic.
  const auto repeat =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&root=ns::a&direction=out&byte_limit=20500");
  CHECK(truncated.body == repeat.body);

  // The smallest allowed byte_limit (1024) cannot fit even this fixture's
  // minimal metadata skeleton: the request must fail cleanly (400) rather
  // than emit a truncated-but-misleading artifact.
  const auto oversized =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&root=ns::a&direction=out&byte_limit=1024");
  CHECK(oversized.status == 400);
}

TEST_CASE("Live explorer rejects a request with an invalid token") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto response = http_get(server.port, "/?token=not-the-real-token");
  CHECK(response.status == 404);
}

TEST_CASE("Live explorer rejects a request from an unapproved Origin") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto same_origin =
      http_get(server.port, "/?token=" + server.token,
               "http://127.0.0.1:" + std::to_string(server.port));
  CHECK(same_origin.status == 200);
  const auto cross_origin = http_get(server.port, "/?token=" + server.token,
                                     std::string("http://evil.example"));
  CHECK(cross_origin.status == 403);
}

TEST_CASE("Live explorer emits restrictive security headers") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto response = http_get(server.port, "/?token=" + server.token);
  CHECK(response.status == 200);
  CHECK(response.headers.find("X-Content-Type-Options: nosniff") !=
        std::string::npos);
  CHECK(response.headers.find("Referrer-Policy: no-referrer") !=
        std::string::npos);
  CHECK(response.headers.find("frame-ancestors 'none'") != std::string::npos);
  CHECK(response.headers.find("style-src 'nonce-cidx-static'") !=
        std::string::npos);
}

TEST_CASE("Live explorer bounds transport requests and provider responses") {
  const ui::GraphProvider oversized_provider = [](std::string_view,
                                                  const ui::CancelToken &) {
    return std::optional<std::string>(std::string(1024, 'x'));
  };
  ui::ServerOptions options{.port = 0,
                            .launch_browser = false,
                            .max_request_bytes = 256,
                            .max_response_bytes = 128};
  RunningServer server(oversized_provider, {}, {}, options);
  const auto oversized_response =
      http_get(server.port, "/api/graph?token=" + server.token);
  CHECK(oversized_response.status == 413);
  const auto oversized_request =
      http_get(server.port,
               "/?token=" + server.token + "&padding=" + std::string(300, 'x'));
  CHECK(oversized_request.status == 400);
}

TEST_CASE(
    "Live explorer tolerates a client aborting mid-request (cancellation)") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  abrupt_disconnect(server.port, /*send_partial=*/false);
  abrupt_disconnect(server.port, /*send_partial=*/true);
  // The server must still be alive and answering ordinary requests after
  // both aborted connections.
  const auto response = http_get(server.port, "/?token=" + server.token);
  CHECK(response.status == 200);
}

TEST_CASE("Live explorer: a client disconnect during a slow query does not "
          "block the accept loop, and the server never crashes") {
  // A deliberately slow/blocking provider, standing in for a long-running
  // query. `completed` proves it actually ran to completion in the
  // background even though the requesting client abandons the connection
  // before that happens.
  std::atomic<bool> completed{false};
  const ui::GraphProvider slow_provider =
      [&completed](
          std::string_view target,
          const ui::CancelToken &should_cancel) -> std::optional<std::string> {
    (void)target;
    (void)should_cancel; // deliberately non-cooperative: proves the server
                         // itself bounds an uncooperative provider's impact.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    completed = true;
    return std::string(
        R"({"schema": "cidx.graph-view.v1", "nodes": [], "edges": [], "metadata": {}})");
  };
  RunningServer server(slow_provider);

  // Connect, send a well-formed request for the slow route, then close the
  // socket immediately WITHOUT reading any response -- exactly the
  // "cancel an in-flight query" scenario the previous abrupt_disconnect()
  // test could not exercise (that one disconnects before a valid request
  // even starts).
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(server.port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&address),
                    sizeof(address)) == 0);
  const std::string request = "GET /api/graph?token=" + server.token +
                              "&root=ns::a HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                              "Connection: close\r\n\r\n";
  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t written =
        ::send(fd, request.data() + sent, request.size() - sent, 0);
    if (written <= 0) {
      break;
    }
    sent += static_cast<std::size_t>(written);
  }
  ::close(fd); // Abandon the connection before the slow query even finishes.

  const auto probe_start = std::chrono::steady_clock::now();
  // While the slow query is still (deliberately) sleeping, an ordinary
  // second request must still be accepted and answered promptly -- if the
  // accept loop were blocked synchronously on the first (abandoned)
  // connection's query, as it was before this fix, this would take at
  // least the full 300ms sleep instead of a few milliseconds.
  const auto second = http_get(server.port, "/?token=" + server.token);
  const auto elapsed = std::chrono::steady_clock::now() - probe_start;
  CHECK(second.status == 200);
  CHECK(elapsed < std::chrono::milliseconds(250));

  // The abandoned query keeps running (providers have no cooperative
  // cancellation point) and must complete without crashing the server,
  // even though nothing will ever read its result.
  for (int attempt = 0; attempt < 50 && !completed.load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(completed.load());
  // The server must still be answering requests afterward.
  const auto third = http_get(server.port, "/?token=" + server.token);
  CHECK(third.status == 200);
}

TEST_CASE("Live explorer shuts down cleanly on an authenticated request") {
  Fixture fixture;
  ui::GraphProvider graph = graph_provider_for(fixture.db);
  std::promise<std::string> url_promise;
  FirstLineCapture capture_buf(url_promise);
  std::ostream out(&capture_buf);
  std::ostringstream err;
  std::thread thread([&] {
    ui::serve_live(
        ui::render_html(json_out::Value::obj({}), ui::RenderMode::LoopbackLive),
        graph, ui::ServerOptions{.port = 0, .launch_browser = false}, out, err);
  });
  auto future = url_promise.get_future();
  REQUIRE(future.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
  const std::string url = future.get();
  const std::size_t port_start = url.find("127.0.0.1:") + 10;
  const std::size_t port_end = url.find('/', port_start);
  const int port = parse_int(url.substr(port_start, port_end - port_start));
  const std::size_t token_key = url.find("token=");
  const std::string token = url.substr(token_key + 6);

  const auto response = http_get(port, "/api/shutdown?token=" + token);
  CHECK(response.status == 200);
  CHECK(response.body.find(R"("stopped": true)") != std::string::npos);
  REQUIRE(thread.joinable());
  thread.join(); // must return promptly; a hang fails the test via ctest's
                 // own timeout rather than hanging forever.
}

TEST_CASE("Live explorer: shutdown returns promptly even while an abandoned "
          "provider ignores cancellation entirely (HSE-92 round 2)") {
  // Reviewer's exact repro: "use a provider blocked forever on a latch,
  // send a valid graph request, disconnect, then request authenticated
  // shutdown -- the shutdown response is 200 but serve_live() never
  // returns." This provider deliberately ignores `should_cancel` entirely
  // (ANY provider, cooperative or not, must never be able to hang
  // serve_live()'s own shutdown).
  std::atomic<bool> release_latch{false};
  std::atomic<bool> completed{false};
  const ui::GraphProvider stuck_provider =
      [&release_latch, &completed](
          std::string_view target,
          const ui::CancelToken &should_cancel) -> std::optional<std::string> {
    (void)target;
    (void)should_cancel;
    while (!release_latch.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    completed.store(true, std::memory_order_release);
    return std::string(
        R"({"schema": "cidx.graph-view.v1", "nodes": [], "edges": [], "metadata": {}})");
  };

  std::promise<std::string> url_promise;
  FirstLineCapture capture_buf(url_promise);
  std::ostream out(&capture_buf);
  std::ostringstream err;
  std::thread thread([&] {
    ui::serve_live(
        ui::render_html(json_out::Value::obj({}), ui::RenderMode::LoopbackLive),
        stuck_provider, ui::ServerOptions{.port = 0, .launch_browser = false},
        out, err);
  });
  auto future = url_promise.get_future();
  REQUIRE(future.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
  const std::string url = future.get();
  const std::size_t port_start = url.find("127.0.0.1:") + 10;
  const std::size_t port_end = url.find('/', port_start);
  const int port = parse_int(url.substr(port_start, port_end - port_start));
  const std::size_t token_key = url.find("token=");
  const std::string token = url.substr(token_key + 6);

  // Send a valid graph request, then abandon the connection before the
  // (permanently, until released below) stuck provider ever returns.
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&address),
                    sizeof(address)) == 0);
  const std::string request = "GET /api/graph?token=" + token +
                              "&root=ns::a HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                              "Connection: close\r\n\r\n";
  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t written =
        ::send(fd, request.data() + sent, request.size() - sent, 0);
    if (written <= 0) {
      break;
    }
    sent += static_cast<std::size_t>(written);
  }
  ::close(fd); // Abandon before the provider's (indefinite) sleep finishes.

  // Give the connection thread's 20ms disconnect-poll a moment to notice
  // and detach the stuck worker before shutdown is requested.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const auto response = http_get(port, "/api/shutdown?token=" + token);
  CHECK(response.status == 200);

  // The whole point of this test: serve_live() must return promptly even
  // though the abandoned provider is still running (deliberately, until
  // released below) -- a hang here fails via ctest's own timeout rather
  // than blocking forever.
  REQUIRE(thread.joinable());
  thread.join();

  // Release the deliberately-stuck provider so it does not leak a
  // background thread for the rest of this test binary's lifetime, and
  // confirm it eventually finishes safely (no crash) before this test's
  // captured locals go out of scope.
  release_latch.store(true, std::memory_order_release);
  for (int attempt = 0; attempt < 100 && !completed.load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(completed.load());
}

TEST_CASE("Live explorer: an abandoned worker never outlives the caller's "
          "own copy of the GraphProvider/Storage it was invoked with "
          "(HSE-92 round 3)") {
  // Reviewer's exact repro: block a provider on a latch, send a valid graph
  // request, disconnect, request shutdown, let serve_live() return so the
  // caller destroys its local GraphProvider/Storage, THEN release the
  // worker. The previous test in this file kept its provider/latch alive
  // for its own entire duration, which never actually exercised that
  // sequence -- this one deliberately drops every reference *this test*
  // holds (simulating the caller doing so) before ever releasing the latch,
  // and asserts via a `canary`'s own destruction that the abandoned
  // worker's OWN independently-owned copy (not a dangling reference) is
  // what keeps it alive in the meantime.
  struct Canary {
    std::atomic<bool> *destroyed = nullptr;
    ~Canary() {
      if (destroyed != nullptr) {
        destroyed->store(true, std::memory_order_release);
      }
    }
  };
  std::atomic<bool> canary_destroyed{false};
  // Constructed in place (not `make_shared<Canary>(Canary{...})`): the
  // latter would build a temporary `Canary` sharing the SAME `destroyed`
  // pointer, move/copy it into the shared allocation, and then destroy that
  // now-redundant temporary -- whose own destructor would ALSO (harmlessly
  // but misleadingly) fire and set `destroyed`, making this test falsely
  // report destruction immediately regardless of the real object's actual
  // lifetime.
  auto canary = std::make_shared<Canary>();
  canary->destroyed = &canary_destroyed;
  std::atomic<bool> release_latch{false};
  std::atomic<bool> completed{false};
  ui::GraphProvider stuck_provider =
      [canary, &release_latch, &completed](
          std::string_view target,
          const ui::CancelToken &should_cancel) -> std::optional<std::string> {
    (void)target;
    (void)should_cancel;
    while (!release_latch.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    completed.store(true, std::memory_order_release);
    return std::string(
        R"({"schema": "cidx.graph-view.v1", "nodes": [], "edges": [], "metadata": {}})");
  };

  std::promise<std::string> url_promise;
  FirstLineCapture capture_buf(url_promise);
  std::ostream out(&capture_buf);
  std::ostringstream err;
  std::thread thread([&] {
    ui::serve_live(
        ui::render_html(json_out::Value::obj({}), ui::RenderMode::LoopbackLive),
        stuck_provider, ui::ServerOptions{.port = 0, .launch_browser = false},
        out, err);
  });
  auto future = url_promise.get_future();
  REQUIRE(future.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
  const std::string url = future.get();
  const std::size_t port_start = url.find("127.0.0.1:") + 10;
  const std::size_t port_end = url.find('/', port_start);
  const int port = parse_int(url.substr(port_start, port_end - port_start));
  const std::size_t token_key = url.find("token=");
  const std::string token = url.substr(token_key + 6);

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&address),
                    sizeof(address)) == 0);
  const std::string request = "GET /api/graph?token=" + token +
                              "&root=ns::a HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                              "Connection: close\r\n\r\n";
  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t written =
        ::send(fd, request.data() + sent, request.size() - sent, 0);
    if (written <= 0) {
      break;
    }
    sent += static_cast<std::size_t>(written);
  }
  ::close(fd);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const auto response = http_get(port, "/api/shutdown?token=" + token);
  CHECK(response.status == 200);

  // serve_live() must still return promptly (the round-2 requirement is
  // unchanged) even though the abandoned worker is still blocked.
  REQUIRE(thread.joinable());
  thread.join();

  // Simulate the caller destroying its own local GraphProvider/Storage the
  // moment serve_live() returns: drop every reference *this test* holds.
  stuck_provider = {};
  canary.reset();
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  // If the abandoned worker's own copy were not independently keeping the
  // canary alive (e.g. if it only held a dangling reference back to this
  // test's now-cleared locals), it would already be destroyed here.
  CHECK_FALSE(canary_destroyed.load());

  // Only now release the still-running worker, and confirm it both
  // completes safely and, once it does, the canary it was the last owner of
  // is finally freed -- no permanent leak either.
  release_latch.store(true, std::memory_order_release);
  for (int attempt = 0; attempt < 100 && !completed.load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(completed.load());
  for (int attempt = 0; attempt < 100 && !canary_destroyed.load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(canary_destroyed.load());
}

TEST_CASE("Live explorer: concurrent provider work is bounded by an "
          "admission limit (HSE-92 round 3)") {
  // Reviewer's exact repro, scaled down for a fast/deterministic test: a
  // provider blocked on a latch, and MANY concurrent authenticated
  // /api/graph requests. Every one of them must not be allowed to enter the
  // provider concurrently -- only up to `kCap` may, the rest must be
  // rejected (503) immediately, without ever spawning a provider worker.
  constexpr int kCap = 3;
  constexpr int kRequests = 9;
  std::atomic<int> in_flight{0};
  std::atomic<int> max_seen{0};
  std::atomic<bool> release_latch{false};
  const ui::GraphProvider capped_provider =
      [&](std::string_view target,
          const ui::CancelToken &should_cancel) -> std::optional<std::string> {
    (void)target;
    (void)should_cancel;
    const int now = in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
    int seen = max_seen.load(std::memory_order_acquire);
    while (now > seen && !max_seen.compare_exchange_weak(
                             seen, now, std::memory_order_acq_rel)) {
    }
    while (!release_latch.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    in_flight.fetch_sub(1, std::memory_order_acq_rel);
    return std::string(
        R"({"schema": "cidx.graph-view.v1", "nodes": [], "edges": [], "metadata": {}})");
  };

  std::promise<std::string> url_promise;
  FirstLineCapture capture_buf(url_promise);
  std::ostream out(&capture_buf);
  std::ostringstream err;
  std::thread server_thread([&] {
    ui::serve_live(
        ui::render_html(json_out::Value::obj({}), ui::RenderMode::LoopbackLive),
        capped_provider, ui::GraphProvider{}, ui::GraphProvider{},
        ui::ServerOptions{.port = 0,
                          .launch_browser = false,
                          .max_concurrent_requests = kCap},
        out, err);
  });
  auto future = url_promise.get_future();
  REQUIRE(future.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
  const std::string url = future.get();
  const std::size_t port_start = url.find("127.0.0.1:") + 10;
  const std::size_t port_end = url.find('/', port_start);
  const int port = parse_int(url.substr(port_start, port_end - port_start));
  const std::size_t token_key = url.find("token=");
  const std::string token = url.substr(token_key + 6);

  std::vector<int> statuses(kRequests, -1);
  std::vector<std::thread> clients;
  clients.reserve(kRequests);
  for (int i = 0; i < kRequests; ++i) {
    clients.emplace_back([&, i] {
      statuses[i] =
          plain_http_get(port, "/api/graph?token=" + token + "&root=ns::a")
              .status;
    });
  }
  // Give the (single-threaded) accept loop time to admit-or-reject every one
  // of these `kRequests` connections before releasing the latch.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(max_seen.load() <= kCap);
  CHECK(in_flight.load() > 0);
  CHECK(in_flight.load() <= kCap);

  release_latch.store(true, std::memory_order_release);
  for (auto &client : clients) {
    client.join();
  }

  int ok_count = 0;
  int busy_count = 0;
  for (const int status : statuses) {
    if (status == 200) {
      ++ok_count;
    } else if (status == 503) {
      ++busy_count;
    }
  }
  CHECK(ok_count <= kCap);
  CHECK(busy_count > 0);
  CHECK(ok_count + busy_count == kRequests);

  (void)plain_http_get(port, "/api/shutdown?token=" + token);
  REQUIRE(server_thread.joinable());
  server_thread.join();
}
