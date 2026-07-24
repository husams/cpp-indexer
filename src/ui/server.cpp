#include "ui/server.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace cidx::ui {
namespace {

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
  const std::string needle = "token=" + std::string(expected);
  return target.contains(needle);
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

} // namespace

int serve_live(const std::string &html, const std::string &graph_json,
               const ServerOptions &options, std::ostream &out,
               std::ostream &err) {
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
  const std::string url =
      "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) +
      "/?token=" + access_token;
  out << url << "\n" << std::flush;
  if (options.launch_browser) {
    launch_browser(url);
  }

  std::array<char, 8192> buffer{};
  while (true) {
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
    bool sent = false;
    if (target_end == std::string::npos || !has_token(target, access_token)) {
      (void)send_response(client, 404, "text/plain", "not found\n");
      ::close(client);
      continue;
    }
    if (target.starts_with("/?") || target.starts_with("/index.html?")) {
      sent = send_response(client, 200, "text/html; charset=utf-8", html);
    } else if (target.starts_with("/api/graph?")) {
      sent = send_response(client, 200, "application/json", graph_json);
    } else {
      sent = send_response(client, 404, "text/plain", "not found\n");
    }
    ::close(client);
    if (!sent) {
      continue;
    }
  }
  ::close(server);
  return 0;
}

} // namespace cidx::ui
