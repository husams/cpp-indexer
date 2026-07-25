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
    const std::string_view line = request.substr(
        line_start, line_end == std::string_view::npos
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

  std::optional<std::string> operator()(std::string_view target) const {
    (void)target;
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

// Runs `provider(target)` on its own thread while this (caller's) thread
// concurrently polls the client socket for disconnection, so a client that
// goes away mid-query is noticed WHILE the query is still running rather
// than only at the final send() (HSE-92 review: "cancellation" must mean
// more than closing a connection before a valid request even starts).
// Providers are budget-bounded and have no cooperative cancellation point of
// their own, so this cannot abort mid-computation -- what it DOES guarantee
// is that a disconnected client is never waited on or written to once
// noticed, and that the accept loop calling this (via its own connection
// thread, see serve_live()) is never blocked by one slow/abandoned
// connection.
//
// The inner `worker` is always joined (never detached): a detached worker
// could still be running `provider(target)` -- referencing `provider`,
// which is only valid for serve_live()'s lifetime -- after serve_live()
// itself returns, a dangling reference. Joining bounds this function's own
// runtime to the provider's (budget-bounded) completion time in the worst
// case, but never blocks the ACCEPT loop, since it always runs on its own
// connection thread.
// Deliberately built on a plain atomic completion flag rather than
// std::promise/std::future: worker.join() below always runs before this
// function returns, so the shared `value`/`done` locals stay valid for the
// worker's entire lifetime without needing a heap-allocated shared state.
// This also sidesteps a clang-analyzer false positive (proven via its own
// emitted path notes -- it reports "Returning from 'future::wait_for'"
// immediately before flagging the very next statement as still "inside" that
// call's internal mutex) that misattributes peer_disconnected()'s recv() as
// running inside libc++ future's internal critical section when
// future::wait_for is polled in a loop.
void run_provider_route(int client, const GraphProvider &provider,
                        const std::string &target,
                        std::string_view empty_message) {
  std::optional<std::string> value;
  std::atomic<bool> done{false};
  // `target` is captured by reference, not by value: it is run_provider_route's
  // own parameter, guaranteed valid for this whole call (worker.join() below
  // always runs before this function returns), and capturing by value would
  // insert a std::string copy-construction into this closure -- itself a
  // (however unlikely) throwing operation running as part of a std::thread
  // entry function, where an uncaught exception calls std::terminate().
  std::thread worker([&provider, &target, &value, &done] {
    try {
      value = provider ? provider(target) : std::nullopt;
    } catch (const std::exception &) {
      value = std::nullopt;
    }
    done.store(true, std::memory_order_release);
  });
  bool disconnected = false;
  while (!done.load(std::memory_order_acquire)) {
    if (peer_disconnected(client)) {
      disconnected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  worker.join();
  if (!disconnected) {
    if (value) {
      (void)send_response(client, 200, "application/json", *value);
    } else {
      (void)send_response(client, 400, "text/plain", empty_message);
    }
  }
  ::close(client);
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
  // run_provider_route): kept here, not detached, so serve_live() can join
  // every one of them before it returns -- the alternative (a detached
  // thread outliving serve_live()) would leave it holding a dangling
  // reference to the provider parameters. Static/shutdown/error routes stay
  // on the accept-loop thread; they are fast and never block on a provider.
  std::vector<std::thread> connection_threads;
  std::array<char, 8192> buffer{};
  bool shutdown_requested = false;
  while (!shutdown_requested) {
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
      connection_threads.emplace_back(run_provider_route, client,
                                      std::cref(graph_provider),
                                      std::string(target),
                                      "bad graph request\n");
    } else if (target.starts_with("/api/search?")) {
      connection_threads.emplace_back(run_provider_route, client,
                                      std::cref(search_provider),
                                      std::string(target),
                                      "bad search request\n");
    } else if (target.starts_with("/api/evidence?")) {
      connection_threads.emplace_back(run_provider_route, client,
                                      std::cref(evidence_provider),
                                      std::string(target),
                                      "bad evidence request\n");
    } else if (target.starts_with("/api/shutdown?")) {
      (void)send_response(client, 200, "application/json",
                          "{\"stopped\": true}\n");
      ::close(client);
      shutdown_requested = true;
    } else {
      (void)send_response(client, 404, "text/plain", "not found\n");
      ::close(client);
    }
  }
  for (auto &connection_thread : connection_threads) {
    if (connection_thread.joinable()) {
      connection_thread.join();
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
