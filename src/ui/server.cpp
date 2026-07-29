#include "ui/server.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <sstream>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace cidx::ui {
namespace {

// POSIX default disposition for SIGPIPE is process termination. A client
// that disconnects (or is killed) between our accept() and a subsequent
// send() -- exactly the "abrupt cancellation" scenario this explorer must
// tolerate -- makes that send() raise SIGPIPE, which would otherwise take
// down the entire server process rather than just failing that one
// response. Ignoring it for the scope of serve_live() turns that send()
// into an ordinary EPIPE return (already handled by send_all()'s retry/
// failure path) without changing SIGPIPE disposition for the rest of the
// `cidx` process.
class IgnoreSigpipe {
public:
  IgnoreSigpipe() : previous_(std::signal(SIGPIPE, SIG_IGN)) {}
  ~IgnoreSigpipe() {
    if (previous_ != SIG_ERR) {
      std::signal(SIGPIPE, previous_);
    }
  }
  IgnoreSigpipe(const IgnoreSigpipe &) = delete;
  IgnoreSigpipe &operator=(const IgnoreSigpipe &) = delete;

private:
  using Handler = void (*)(int);
  Handler previous_;
};

std::string token() {
  std::random_device random;
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (int i = 0; i < 16; ++i) {
    out << std::setw(2) << (random() & 0xffU);
  }
  return out.str();
}

bool constant_time_equal(std::string_view left, std::string_view right) {
  const std::size_t length = std::max(left.size(), right.size());
  auto difference = static_cast<std::uint8_t>(left.size() ^ right.size());
  for (std::size_t index = 0; index < length; ++index) {
    const auto left_byte =
        index < left.size() ? static_cast<unsigned char>(left[index]) : 0U;
    const auto right_byte =
        index < right.size() ? static_cast<unsigned char>(right[index]) : 0U;
    difference =
        static_cast<std::uint8_t>(difference | (left_byte ^ right_byte));
  }
  return difference == 0;
}

bool has_token(std::string_view target, std::string_view expected) {
  const std::size_t question = target.find('?');
  if (question == std::string_view::npos) {
    return false;
  }
  std::size_t start = question + 1;
  while (start <= target.size()) {
    const std::size_t end = target.find('&', start);
    const std::string_view field = target.substr(
        start,
        end == std::string_view::npos ? target.size() - start : end - start);
    if (field.starts_with("token=") &&
        constant_time_equal(field.substr(6), expected)) {
      return true;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

// Case-insensitive HTTP header lookup over the raw request text (request
// line plus CRLF-terminated header lines). Returns the trimmed value, or
// nullopt when the header is absent.
std::optional<std::string> find_header(std::string_view request,
                                       std::string_view name) {
  std::size_t line_start = request.find("\r\n");
  if (line_start == std::string_view::npos) {
    return std::nullopt;
  }
  line_start += 2;
  const auto ieq = [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  };
  while (line_start < request.size()) {
    const std::size_t line_end = request.find("\r\n", line_start);
    const std::string_view line =
        request.substr(line_start, line_end == std::string_view::npos
                                       ? request.size() - line_start
                                       : line_end - line_start);
    if (line.empty()) {
      break; // blank line: end of headers
    }
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos && colon == name.size() &&
        std::equal(line.begin(),
                   line.begin() + static_cast<std::ptrdiff_t>(colon),
                   name.begin(), ieq)) {
      std::string_view value = line.substr(colon + 1);
      while (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
      }
      while (!value.empty() && value.back() == '\r') {
        value.remove_suffix(1);
      }
      return std::string(value);
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    line_start = line_end + 2;
  }
  return std::nullopt;
}

void launch_browser(const std::string &url) {
  const pid_t child = fork();
  if (child != 0) {
    return;
  }
#ifdef __APPLE__
  execlp("open", "open", url.c_str(), static_cast<char *>(nullptr));
#else
  execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char *>(nullptr));
#endif
  _exit(127);
}

bool send_all(int fd, std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t sent =
        ::send(fd, bytes.data() + offset, bytes.size() - offset, 0);
    if (sent > 0) {
      offset += static_cast<std::size_t>(sent);
      continue;
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

bool send_response(int fd, int code, std::string_view type,
                   std::string_view body) {
  const char *reason = "Bad Request";
  if (code == 200) {
    reason = "OK";
  } else if (code == 403) {
    reason = "Forbidden";
  } else if (code == 404) {
    reason = "Not Found";
  } else if (code == 503) {
    reason = "Service Unavailable";
  } else if (code == 413) {
    reason = "Payload Too Large";
  }
  std::ostringstream head;
  head << "HTTP/1.1 " << code << " " << reason << "\r\n"
       << "Content-Type: " << type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Cache-Control: no-store\r\n"
       << "X-Content-Type-Options: nosniff\r\n"
       << "Referrer-Policy: no-referrer\r\n"
       << "Permissions-Policy: camera=(), geolocation=(), microphone=()\r\n"
       << "Cross-Origin-Resource-Policy: same-origin\r\n"
       << "Content-Security-Policy: default-src 'none'; "
          "script-src 'nonce-cidx-static'; style-src 'nonce-cidx-static'; "
          "connect-src 'self'; img-src data:; base-uri 'none'; "
          "form-action 'none'; frame-ancestors 'none'\r\n"
       << "Connection: close\r\n\r\n";
  const std::string header = head.str();
  return send_all(fd, header) && send_all(fd, body);
}

bool send_bounded_response(int fd, int code, std::string_view type,
                           std::string_view body, std::size_t max_body_bytes) {
  if (body.size() > max_body_bytes) {
    return send_response(fd, 413, "text/plain; charset=utf-8",
                         "response exceeds the local explorer limit\n");
  }
  return send_response(fd, code, type, body);
}

bool read_request(int client, std::string &request, std::size_t max_bytes,
                  int timeout_ms) {
  if (max_bytes == 0) {
    return false;
  }
  request.clear();
  request.reserve(std::min<std::size_t>(max_bytes, 4096));
  const int wait_ms = std::max(0, timeout_ms);
  std::array<char, 4096> buffer{};
  while (!request.contains("\r\n\r\n")) {
    if (request.size() == max_bytes) {
      return false;
    }
    pollfd ready{};
    ready.fd = client;
    ready.events = POLLIN;
    const int polled = ::poll(&ready, 1, wait_ms);
    if (polled <= 0 || (ready.revents & (POLLERR | POLLNVAL)) != 0) {
      return false;
    }
    const std::size_t capacity =
        std::min(buffer.size(), max_bytes - request.size());
    const ssize_t received = ::recv(client, buffer.data(), capacity, 0);
    if (received <= 0) {
      return false;
    }
    request.append(buffer.data(), static_cast<std::size_t>(received));
  }
  return true;
}

struct StaticGraphProvider {
  std::string graph_json;

  std::optional<std::string>
  operator()(std::string_view target, const CancelToken &should_cancel) const {
    (void)target;
    (void)should_cancel;
    return graph_json;
  }
};

// Whether the peer has closed (or reset) the connection, checked without
// blocking. A readable socket that yields 0 bytes on a MSG_PEEK is the
// standard way to detect an orderly close; POLLHUP/POLLERR cover the
// abrupt-reset case.
bool peer_disconnected(int fd) {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  const int ready = ::poll(&pfd, 1, 0);
  if (ready <= 0) {
    return false;
  }
  if ((pfd.revents & (POLLHUP | POLLERR)) != 0) {
    return true;
  }
  if ((pfd.revents & POLLIN) != 0) {
    char probe = 0;
    const ssize_t peeked = ::recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    if (peeked == 0) {
      return true;
    }
  }
  return false;
}

// Runs `provider(target, should_cancel)` on its own thread while this
// (caller's) thread concurrently polls the client socket for disconnection
// AND the server-wide `shutting_down` flag, so a client that goes away -- or
// a server shutdown -- is noticed WHILE the query is still running, not only
// at the final send() (HSE-92 review: "cancellation" must mean more than
// closing a connection before a valid request even starts).
//
// `should_cancel` is a REAL cooperative cancellation channel threaded into
// the provider: build_graph_view()'s node/edge assembly loops poll it and
// unwind early once it reports true. A provider that ignores it entirely is
// still bounded from THIS function's perspective (round 2 fix): once
// disconnection or shutdown is noticed, this function DETACHES the worker
// and returns immediately instead of unconditionally joining it -- so this
// connection (and, at shutdown, the accept loop's own drain of every
// connection) is never blocked by one slow or uncooperative provider.
//
// The worker's shared state (`value`, `done`, `cancel_requested`) is
// heap-allocated (shared_ptr) and captured BY VALUE, specifically so it
// stays valid for the worker's entire lifetime even when this function
// returns while the worker is still running (the detach path). `provider`
// is captured BY VALUE too (an owned std::function copy), not by reference
// to this function's own parameter: a detached worker can keep running
// after serve_live() itself has returned, at which point any REFERENCE
// chasing back to the caller's own GraphProvider variable (or, transitively,
// a raw pointer/reference the provider's own closure holds to a Storage the
// caller destroys once serve_live() returns) would be a genuine
// use-after-free -- this is exactly why production graph/search/evidence
// providers (see cli/commands_ui.cpp) hold a `std::shared_ptr<Storage>`
// rather than a raw pointer: as long as this worker's own captured copy of
// `provider` is alive, that shared_ptr keeps the Storage alive too, with no
// need for serve_live() to block its own return on an uncooperative
// provider ever finishing.
// Admission control (HSE-92 round 3): atomically claims one of
// `cap` concurrent provider-work slots, returning false (claiming nothing)
// once `cap` is already in use. Paired with `active_work`'s own decrement
// once a claimed unit of work actually finishes (see run_provider_route's
// worker below) -- not with connection/thread teardown, so an abandoned but
// still-running provider still correctly counts as active work.
bool try_admit(std::atomic<int> &active_work, int cap) {
  int current = active_work.load(std::memory_order_acquire);
  while (current < cap) {
    if (active_work.compare_exchange_weak(current, current + 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

void run_provider_route(
    int client, GraphProvider provider, const std::string &target,
    std::string_view empty_message, std::atomic<bool> &shutting_down,
    const std::shared_ptr<std::atomic<bool>> &route_finished,
    const std::shared_ptr<std::atomic<int>> &active_work,
    std::size_t max_response_bytes) {
  // This whole body runs as a std::thread entry function (see serve_live()'s
  // connection_threads): an uncaught exception anywhere in it -- even one as
  // unlikely as `target`'s copy-construction into the worker lambda's
  // closure, or std::thread/std::make_shared under extreme resource
  // pressure -- would otherwise call std::terminate() and take down the
  // whole server over a single request. Best-effort clean up the socket and
  // still mark the route finished (so it is reaped) rather than let
  // anything escape.
  try {
    auto value = std::make_shared<std::optional<std::string>>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto cancel_requested = std::make_shared<std::atomic<bool>>(false);
    // `target` is copied into its own heap allocation HERE (an ordinary
    // statement this function's own try/catch already covers) rather than
    // directly into the worker lambda's capture list: capturing a
    // shared_ptr by value is a noexcept refcount bump, not a
    // std::string copy-construction -- clang-tidy's bugprone-exception-escape
    // (rightly) treats ANY exception reachable from constructing the
    // callable passed to std::thread as unable to safely propagate, since
    // the actual invocation runs on a separate call stack no caller-side
    // try/catch can reach.
    auto target_copy = std::make_shared<std::string>(target);
    // `provider` is MOVED into the worker's own closure -- its own owned
    // std::function state, not a reference to this function's `provider`
    // parameter: that parameter's own storage (this stack frame) can be
    // gone long before an abandoned worker's detached thread finishes, so
    // the worker must carry an independent copy it fully owns for its own
    // lifetime. `provider` is a by-value parameter used nowhere else in
    // this function once the worker is constructed, so moving (rather than
    // copying) it into the closure avoids an unnecessary extra copy.
    std::thread worker([provider = std::move(provider), target_copy, value,
                        done, cancel_requested, active_work] {
      const CancelToken should_cancel = [cancel_requested] {
        return cancel_requested->load(std::memory_order_acquire);
      };
      try {
        *value =
            provider ? provider(*target_copy, should_cancel) : std::nullopt;
      } catch (const std::exception &) {
        *value = std::nullopt;
      }
      done->store(true, std::memory_order_release);
      // Release this admitted unit of work only once the provider
      // invocation itself has actually finished -- an abandoned/detached
      // worker that ignores cancellation and keeps running still correctly
      // counts against the concurrency cap for as long as it runs.
      if (active_work) {
        active_work->fetch_sub(1, std::memory_order_acq_rel);
      }
    });
    bool abandoned = false;
    while (!done->load(std::memory_order_acquire)) {
      if (peer_disconnected(client) ||
          shutting_down.load(std::memory_order_acquire)) {
        abandoned = true;
        cancel_requested->store(true, std::memory_order_release);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (abandoned) {
      // Do not wait for a provider that may never honor cancellation:
      // detach rather than join, so this connection thread (and any
      // shutdown drain waiting on it) is never blocked.
      worker.detach();
      ::close(client);
      if (route_finished) {
        route_finished->store(true, std::memory_order_release);
      }
      return;
    }
    worker.join();
    if (*value) {
      (void)send_bounded_response(client, 200, "application/json", **value,
                                  max_response_bytes);
    } else {
      (void)send_response(client, 400, "text/plain", empty_message);
    }
    ::close(client);
    if (route_finished) {
      route_finished->store(true, std::memory_order_release);
    }
  } catch (...) {
    ::close(client);
    if (route_finished) {
      route_finished->store(true, std::memory_order_release);
    }
  }
}

} // namespace

int serve_live(const std::string &html, const GraphProvider &graph_provider,
               const GraphProvider &search_provider,
               const GraphProvider &evidence_provider,
               const ServerOptions &options, std::ostream &out,
               std::ostream &err) {
  const IgnoreSigpipe ignore_sigpipe;
  const int server = ::socket(AF_INET, SOCK_STREAM, 0);
  if (server < 0) {
    err << "error: cidx ui: cannot create loopback socket: "
        << std::strerror(errno) << "\n";
    return 1;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<uint16_t>(options.port));
  if (::bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
          0 ||
      ::listen(server, 8) != 0) {
    err << "error: cidx ui: cannot bind loopback socket: "
        << std::strerror(errno) << "\n";
    ::close(server);
    return 1;
  }
  socklen_t length = sizeof(address);
  if (::getsockname(server, reinterpret_cast<sockaddr *>(&address), &length) !=
      0) {
    err << "error: cidx ui: cannot inspect loopback socket\n";
    ::close(server);
    return 1;
  }
  const std::string access_token = token();
  const std::string bound_port = std::to_string(ntohs(address.sin_port));
  const std::string url =
      "http://127.0.0.1:" + bound_port + "/?token=" + access_token;
  // Same-origin defense in depth beyond loopback-only binding: a same-host
  // page (e.g. served from a different loopback port, or a DNS-rebound
  // hostname pointed at 127.0.0.1) could still guess/observe the ephemeral
  // port. Any browser-issued cross-origin fetch carries an Origin header
  // naming the requesting page's own origin; a request from this explorer's
  // own page carries none (top-level navigation) or exactly this origin
  // (same-page fetch), so any OTHER declared Origin is rejected regardless
  // of token validity.
  const std::string expected_origin = "http://127.0.0.1:" + bound_port;
  out << url << "\n" << std::flush;
  if (options.launch_browser) {
    launch_browser(url);
  }

  // Connection threads for provider-dispatching routes (see
  // run_provider_route). Each carries its own `finished` flag so
  // `reap_finished_connections()` can join and drop it as soon as its
  // request completes, DURING the session -- not only at shutdown (HSE-92
  // round 2: a long session must not accumulate one joinable pthread per
  // request, and thread construction can eventually fail if it does).
  // `shutting_down` is threaded into every in-flight request's cancellation
  // token in addition to per-connection client disconnect, so shutdown
  // itself never unconditionally blocks on a still-running provider either.
  // Static/shutdown/error routes stay on the accept-loop thread; they are
  // fast and never block on a provider.
  struct ConnectionThread {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
  };
  std::vector<ConnectionThread> connection_threads;
  std::atomic<bool> shutting_down{false};
  // HSE-92 round 3: bounds how many graph/search/evidence provider
  // invocations may be genuinely in flight at once (see try_admit() /
  // run_provider_route() above) -- resource use must be a measured bound,
  // not scale with however many concurrent requests a client sends.
  const auto active_work = std::make_shared<std::atomic<int>>(0);
  const int max_concurrent = std::max(1, options.max_concurrent_requests);
  const auto reap_finished_connections = [&] {
    std::erase_if(connection_threads, [](ConnectionThread &entry) {
      if (!entry.finished->load(std::memory_order_acquire)) {
        return false;
      }
      if (entry.thread.joinable()) {
        entry.thread.join();
      }
      return true;
    });
  };
  bool shutdown_requested = false;
  while (!shutdown_requested) {
    reap_finished_connections();
    const int client = ::accept(server, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    std::string request;
    if (!read_request(client, request, options.max_request_bytes,
                      options.request_timeout_ms)) {
      (void)send_bounded_response(client, 400, "text/plain; charset=utf-8",
                                  "bad request\n", options.max_response_bytes);
      ::close(client);
      continue;
    }
    const std::size_t line_end = request.find("\r\n");
    if (line_end == std::string::npos || line_end > 4096 ||
        !request.starts_with("GET ")) {
      (void)send_bounded_response(client, 400, "text/plain; charset=utf-8",
                                  "bad request\n", options.max_response_bytes);
      ::close(client);
      continue;
    }
    const std::size_t target_end = request.find(' ', 4);
    const std::string_view target(
        request.data() + 4,
        target_end == std::string::npos ? 0 : target_end - 4);
    if (target_end == std::string::npos || !target.starts_with("/") ||
        !has_token(target, access_token)) {
      (void)send_bounded_response(client, 404, "text/plain; charset=utf-8",
                                  "not found\n", options.max_response_bytes);
      ::close(client);
      continue;
    }
    if (const auto origin = find_header(request, "Origin");
        origin && *origin != expected_origin) {
      (void)send_bounded_response(client, 403, "text/plain; charset=utf-8",
                                  "forbidden: origin mismatch\n",
                                  options.max_response_bytes);
      ::close(client);
      continue;
    }
    if (target.starts_with("/?") || target.starts_with("/index.html?")) {
      (void)send_bounded_response(client, 200, "text/html; charset=utf-8", html,
                                  options.max_response_bytes);
      ::close(client);
    } else if (target.starts_with("/api/graph?")) {
      // `target` is a view into `request`, which this accept-loop iteration
      // owns -- copy it before handing the route off to a thread that will
      // outlive this iteration.
      // `graph_provider` (this function's own reference parameter) is passed
      // BY VALUE here -- std::thread copies it into its own storage right
      // now, on THIS (accept-loop) thread, while it is still definitely
      // valid -- rather than std::cref(): run_provider_route's own
      // `provider` parameter is likewise by-value now, and its worker
      // captures its OWN further copy, so an abandoned/detached worker never
      // chases a reference back to state this function -- or its caller --
      // may have already destroyed.
      if (!try_admit(*active_work, max_concurrent)) {
        (void)send_bounded_response(
            client, 503, "text/plain; charset=utf-8",
            "server busy: too many concurrent requests\n",
            options.max_response_bytes);
        ::close(client);
      } else {
        auto finished = std::make_shared<std::atomic<bool>>(false);
        connection_threads.push_back(ConnectionThread{
            .thread = std::thread(run_provider_route, client, graph_provider,
                                  std::string(target), "bad graph request\n",
                                  std::ref(shutting_down), finished,
                                  active_work, options.max_response_bytes),
            .finished = finished});
      }
    } else if (target.starts_with("/api/search?")) {
      if (!try_admit(*active_work, max_concurrent)) {
        (void)send_bounded_response(
            client, 503, "text/plain; charset=utf-8",
            "server busy: too many concurrent requests\n",
            options.max_response_bytes);
        ::close(client);
      } else {
        auto finished = std::make_shared<std::atomic<bool>>(false);
        connection_threads.push_back(ConnectionThread{
            .thread = std::thread(run_provider_route, client, search_provider,
                                  std::string(target), "bad search request\n",
                                  std::ref(shutting_down), finished,
                                  active_work, options.max_response_bytes),
            .finished = finished});
      }
    } else if (target.starts_with("/api/evidence?")) {
      if (!try_admit(*active_work, max_concurrent)) {
        (void)send_bounded_response(
            client, 503, "text/plain; charset=utf-8",
            "server busy: too many concurrent requests\n",
            options.max_response_bytes);
        ::close(client);
      } else {
        auto finished = std::make_shared<std::atomic<bool>>(false);
        connection_threads.push_back(ConnectionThread{
            .thread = std::thread(run_provider_route, client, evidence_provider,
                                  std::string(target), "bad evidence request\n",
                                  std::ref(shutting_down), finished,
                                  active_work, options.max_response_bytes),
            .finished = finished});
      }
    } else if (target.starts_with("/api/shutdown?")) {
      (void)send_bounded_response(client, 200, "application/json",
                                  "{\"stopped\": true}\n",
                                  options.max_response_bytes);
      ::close(client);
      shutting_down.store(true, std::memory_order_release);
      shutdown_requested = true;
    } else {
      (void)send_bounded_response(client, 404, "text/plain; charset=utf-8",
                                  "not found\n", options.max_response_bytes);
      ::close(client);
    }
  }
  for (auto &entry : connection_threads) {
    if (entry.thread.joinable()) {
      entry.thread.join();
    }
  }
  ::close(server);
  return 0;
}

int serve_live(const std::string &html, const GraphProvider &graph_provider,
               const ServerOptions &options, std::ostream &out,
               std::ostream &err) {
  return serve_live(html, graph_provider, GraphProvider{}, GraphProvider{},
                    options, out, err);
}

int serve_live(const std::string &html, const std::string &graph_json,
               const ServerOptions &options, std::ostream &out,
               std::ostream &err) {
  return serve_live(html, StaticGraphProvider{graph_json}, GraphProvider{},
                    GraphProvider{}, options, out, err);
}

} // namespace cidx::ui
