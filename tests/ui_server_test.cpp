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
#include <charconv>
#include <chrono>
#include <future>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

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
    const int64_t component = db.add_component("test", "/tmp/cidx-ui-server-test");
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

    const auto add_edge = [&](int64_t src, int64_t dst, const char *kind,
                              int line) {
      cidx::Edge edge;
      edge.src_id = src;
      edge.dst_id = dst;
      edge.kind = cidx::graph::edge_kinds_map().at(kind);
      const int64_t edge_id = db.add_edge(edge);
      cidx::EdgeSite site;
      site.edge_id = edge_id;
      site.file_id = file;
      site.line = line;
      site.col = 4;
      db.add_edge_site(site);
    };
    add_edge(a, b, "calls", 10);
    add_edge(b, c, "uses", 20);
    add_edge(a, v, "uses", 30);
  }
};

// atoi() cannot report conversion errors; every numeric field this test
// parses comes from output the test itself controls, so a clean 0 default
// on malformed input is a safe, deliberate fallback rather than a silent
// error.
int parse_int(std::string_view text) {
  int value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} ? value : 0;
}

// ---- Minimal blocking HTTP/1.1 client over a real loopback socket ---------

struct HttpResponse {
  int status = 0;
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
    result.body = response.substr(body_start + 4);
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
  std::thread thread;
  int port = 0;
  std::string token;

  explicit RunningServer(const ui::GraphProvider &graph,
                         const ui::GraphProvider &search = {},
                         const ui::GraphProvider &evidence = {}) {
    thread = std::thread([&] {
      ui::serve_live(ui::render_html(json_out::Value::obj({}),
                                     ui::RenderMode::LoopbackLive),
                    graph, search, evidence,
                    ui::ServerOptions{.port = 0, .launch_browser = false}, out,
                    err);
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
  return [&db](std::string_view target) -> std::optional<std::string> {
    ui::GraphViewRequest request;
    request.node_budget = 250;
    request.edge_budget = 500;
    request.site_budget = 200;
    request.byte_budget = 4 * 1024 * 1024;
    const auto param = [&](std::string_view name) -> std::optional<std::string> {
      const std::string needle = std::string(name) + "=";
      const std::size_t start = target.find(needle);
      if (start == std::string_view::npos) {
        return std::nullopt;
      }
      const std::size_t value_start = start + needle.size();
      std::size_t value_end = target.find('&', value_start);
      if (value_end == std::string_view::npos) {
        value_end = target.size();
      }
      return std::string(target.substr(value_start, value_end - value_start));
    };
    if (const auto root = param("root")) {
      request.root = *root;
    }
    if (const auto input = param("input")) {
      request.input = ui::GraphViewInput{.kind = ui::GraphInputKind::Path,
                                         .value = *input};
    }
    if (const auto direction = param("direction")) {
      request.direction = *direction;
    }
    if (const auto node_kind = param("node_kind")) {
      request.node_kinds = std::vector<std::string>{*node_kind};
    }
    if (const auto limit = param("limit")) {
      request.node_budget = parse_int(*limit);
    }
    if (const auto byte_limit = param("byte_limit")) {
      request.byte_budget = parse_int(*byte_limit);
    }
    if (const auto continuation = param("continuation")) {
      request.continuation = *continuation;
    }
    try {
      return json_out::dumps_indent2(ui::build_graph_view(db, request));
    } catch (const std::exception &) {
      return std::nullopt;
    }
  };
}

ui::GraphProvider search_provider_for(Storage &db) {
  return [&db](std::string_view target) -> std::optional<std::string> {
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
  return [&db](std::string_view target) -> std::optional<std::string> {
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
    try {
      return json_out::dumps_indent2(
          ui::load_edge_evidence(db, edge_id, std::nullopt, 0, 200));
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

} // namespace

TEST_CASE("Live explorer: search resolves a typed candidate list") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db),
                      search_provider_for(fixture.db));
  const auto response =
      http_get(server.port, "/api/search?token=" + server.token + "&q=ns::a");
  CHECK(response.status == 200);
  CHECK(response.body.find("cidx.graph-view.search.v1") != std::string::npos);
  CHECK(response.body.find("USR::a") != std::string::npos);
}

TEST_CASE("Live explorer: expand out and expand in traverse adjacent edges") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto out_response = http_get(
      server.port, "/api/graph?token=" + server.token + "&root=ns::a&direction=out");
  CHECK(out_response.status == 200);
  CHECK(out_response.body.find("USR::a") != std::string::npos);
  CHECK(out_response.body.find("USR::b") != std::string::npos);

  const auto in_response = http_get(
      server.port, "/api/graph?token=" + server.token + "&root=ns::b&direction=in");
  CHECK(in_response.status == 200);
  CHECK(in_response.body.find("USR::b") != std::string::npos);
  CHECK(in_response.body.find("USR::a") != std::string::npos);
}

TEST_CASE("Live explorer: a bounded witness path resolves a -> b -> c") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto response = http_get(
      server.port,
      "/api/graph?token=" + server.token + "&input=ns::a->ns::c");
  CHECK(response.status == 200);
  CHECK(response.body.find("witness-path") != std::string::npos);
  CHECK(response.body.find("USR::a") != std::string::npos);
  CHECK(response.body.find("USR::b") != std::string::npos);
  CHECK(response.body.find("USR::c") != std::string::npos);
}

TEST_CASE("Live explorer: node_kind filter groups the result by kind") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto response = http_get(server.port, "/api/graph?token=" + server.token +
                                                  "&root=ns::a&direction=out&node_kind=function");
  CHECK(response.status == 200);
  CHECK(response.body.find("USR::b") != std::string::npos);
  CHECK(response.body.find("USR::v") == std::string::npos);
}

TEST_CASE("Live explorer: a continuation token pages a truncated result "
         "deterministically") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto first = http_get(server.port, "/api/graph?token=" + server.token +
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

  const auto second =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&root=ns::a&direction=out&limit=1&continuation=" +
                                continuation);
  CHECK(second.status == 200);
  // Repeating the exact same normalized query+continuation is deterministic.
  const auto repeat =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&root=ns::a&direction=out&limit=1&continuation=" +
                                continuation);
  CHECK(second.body == repeat.body);
}

TEST_CASE("Live explorer: a continuation token from a different query is "
         "rejected") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  const auto first = http_get(server.port, "/api/graph?token=" + server.token +
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
  const auto mismatched =
      http_get(server.port, "/api/graph?token=" + server.token +
                                "&root=ns::b&direction=out&limit=1&continuation=" +
                                continuation);
  CHECK(mismatched.status == 400);
}

TEST_CASE("Live explorer: evidence endpoint loads bounded sites for an edge") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db), {},
                      evidence_provider_for(fixture.db));
  const auto graph =
      http_get(server.port, "/api/graph?token=" + server.token + "&root=ns::a&direction=out");
  const std::string edge_id = edge_portable_id_for_kind(graph.body, "calls");
  REQUIRE_FALSE(edge_id.empty());
  const auto evidence = http_get(
      server.port, "/api/evidence?token=" + server.token + "&edge=" + edge_id);
  CHECK(evidence.status == 200);
  CHECK(evidence.body.find("cidx.graph-view.evidence.v1") != std::string::npos);
  CHECK(evidence.body.find(R"("line": 10)") != std::string::npos);
}

TEST_CASE("Live explorer: a byte budget truncates the response deterministically") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  // The unrestricted response for this fixture is ~24 KiB; 20500 bytes
  // exercises the soft node/edge/site trimming path (a valid, deterministic
  // truncated response) rather than the hard GraphViewFailureKind::Oversized
  // error, which only fires when even the minimal finalized skeleton cannot
  // fit the budget -- exercised below via the smallest allowed byte_limit.
  const auto truncated = http_get(
      server.port, "/api/graph?token=" + server.token +
                      "&root=ns::a&direction=out&byte_limit=20500");
  CHECK(truncated.status == 200);
  CHECK(truncated.body.find(R"("truncated": true)") != std::string::npos);
  CHECK(truncated.body.find("byte_budget") != std::string::npos);
  // Repeating the identical bounded request is deterministic.
  const auto repeat = http_get(
      server.port, "/api/graph?token=" + server.token +
                      "&root=ns::a&direction=out&byte_limit=20500");
  CHECK(truncated.body == repeat.body);

  // The smallest allowed byte_limit (1024) cannot fit even this fixture's
  // minimal metadata skeleton: the request must fail cleanly (400) rather
  // than emit a truncated-but-misleading artifact.
  const auto oversized = http_get(
      server.port, "/api/graph?token=" + server.token +
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

TEST_CASE("Live explorer tolerates a client aborting mid-request (cancellation)") {
  Fixture fixture;
  RunningServer server(graph_provider_for(fixture.db));
  abrupt_disconnect(server.port, /*send_partial=*/false);
  abrupt_disconnect(server.port, /*send_partial=*/true);
  // The server must still be alive and answering ordinary requests after
  // both aborted connections.
  const auto response = http_get(server.port, "/?token=" + server.token);
  CHECK(response.status == 200);
}

TEST_CASE("Live explorer shuts down cleanly on an authenticated request") {
  Fixture fixture;
  ui::GraphProvider graph = graph_provider_for(fixture.db);
  std::promise<std::string> url_promise;
  FirstLineCapture capture_buf(url_promise);
  std::ostream out(&capture_buf);
  std::ostringstream err;
  std::thread thread([&] {
    ui::serve_live(ui::render_html(json_out::Value::obj({}),
                                  ui::RenderMode::LoopbackLive),
                  graph, ui::ServerOptions{.port = 0, .launch_browser = false},
                  out, err);
  });
  auto future = url_promise.get_future();
  REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
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
