#include "ui/server.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
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

bool has_token(std::string_view target, std::string_view expected) {
  const std::size_t question = target.find('?');
  if (question == std::string_view::npos) {
    return false;
  }
  const std::string expected_field = "token=" + std::string(expected);
  std::size_t start = question + 1;
  while (start <= target.size()) {
    const std::size_t end = target.find('&', start);
    const std::string_view field = target.substr(
        start,
        end == std::string_view::npos ? target.size() - start : end - start);
    if (field == expected_field) {
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
  }
  std::ostringstream head;
  head << "HTTP/1.1 " << code << " " << reason << "\r\n"
       << "Content-Type: " << type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
  const std::string header = head.str();
  return send_all(fd, header) && send_all(fd, body);
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
// is still captured by reference: it is only valid for as long as
// serve_live()'s caller keeps the underlying GraphProvider alive, which is
// the same lifetime contract detaching already relies on in production (the
// process exits shortly after serve_live() returns) -- a test harness that
// exercises this path must keep its own provider/storage alive until any
// abandoned worker it caused has itself signalled completion.
void run_provider_route(
    int client, const GraphProvider &provider, const std::string &target,
    std::string_view empty_message, std::atomic<bool> &shutting_down,
    const std::shared_ptr<std::atomic<bool>> &route_finished) {
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
    std::thread worker([&provider, target_copy, value, done, cancel_requested] {
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
      (void)send_response(client, 200, "application/json", **value);
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
  std::array<char, 8192> buffer{};
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
    const ssize_t read = ::recv(client, buffer.data(), buffer.size() - 1, 0);
    const std::string request =
        read > 0 ? std::string(buffer.data(), static_cast<std::size_t>(read))
                 : std::string();
    const std::size_t line_end = request.find("\r\n");
    if (line_end == std::string::npos || line_end > 4096 ||
        !request.starts_with("GET ")) {
      (void)send_response(client, 400, "text/plain", "bad request\n");
      ::close(client);
      continue;
    }
    const std::size_t target_end = request.find(' ', 4);
    const std::string_view target(
        request.data() + 4,
        target_end == std::string::npos ? 0 : target_end - 4);
    if (target_end == std::string::npos || !has_token(target, access_token)) {
      (void)send_response(client, 404, "text/plain", "not found\n");
      ::close(client);
      continue;
    }
    if (const auto origin = find_header(request, "Origin");
        origin && *origin != expected_origin) {
      (void)send_response(client, 403, "text/plain",
                          "forbidden: origin mismatch\n");
      ::close(client);
      continue;
    }
    if (target.starts_with("/?") || target.starts_with("/index.html?")) {
      (void)send_response(client, 200, "text/html; charset=utf-8", html);
      ::close(client);
    } else if (target.starts_with("/api/graph?")) {
      // `target` is a view into `buffer`, which the NEXT loop iteration
      // reuses for a different connection's bytes -- copy it before handing
      // off to a thread that will outlive this iteration.
      auto finished = std::make_shared<std::atomic<bool>>(false);
      connection_threads.push_back(ConnectionThread{
          .thread =
              std::thread(run_provider_route, client, std::cref(graph_provider),
                          std::string(target), "bad graph request\n",
                          std::ref(shutting_down), finished),
          .finished = finished});
    } else if (target.starts_with("/api/search?")) {
      auto finished = std::make_shared<std::atomic<bool>>(false);
      connection_threads.push_back(ConnectionThread{
          .thread = std::thread(run_provider_route, client,
                                std::cref(search_provider), std::string(target),
                                "bad search request\n", std::ref(shutting_down),
                                finished),
          .finished = finished});
    } else if (target.starts_with("/api/evidence?")) {
      auto finished = std::make_shared<std::atomic<bool>>(false);
      connection_threads.push_back(ConnectionThread{
          .thread = std::thread(run_provider_route, client,
                                std::cref(evidence_provider),
                                std::string(target), "bad evidence request\n",
                                std::ref(shutting_down), finished),
          .finished = finished});
    } else if (target.starts_with("/api/shutdown?")) {
      (void)send_response(client, 200, "application/json",
                          "{\"stopped\": true}\n");
      ::close(client);
      shutting_down.store(true, std::memory_order_release);
      shutdown_requested = true;
    } else {
      (void)send_response(client, 404, "text/plain", "not found\n");
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
