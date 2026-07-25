#include "util/subprocess.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace cidx {

namespace {

struct Sink {
  int fd;
  std::string *buf;
  bool open;
};

void close_fd(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

RunResult spawn_failure(const std::string &what, int err) {
  RunResult res;
  res.exit_code = 127;
  res.err = what + ": " + std::strerror(err);
  return res;
}

} // namespace

RunResult run(const std::vector<std::string> &argv, double timeout_sec,
              const std::size_t output_limit) {
  RunResult res;
  if (argv.empty()) {
    res.exit_code = 127;
    res.err = "empty argv";
    return res;
  }

  std::array<int, 2> out_pipe = {-1, -1};
  std::array<int, 2> err_pipe = {-1, -1};
  if (::pipe(out_pipe.data()) != 0) {
    return spawn_failure("pipe", errno);
  }
  if (::pipe(err_pipe.data()) != 0) {
    const int e = errno;
    close_fd(out_pipe[0]);
    close_fd(out_pipe[1]);
    return spawn_failure("pipe", e);
  }
  const int devnull = ::open("/dev/null", O_RDONLY);
  if (devnull < 0) {
    const int e = errno;
    close_fd(out_pipe[0]);
    close_fd(out_pipe[1]);
    close_fd(err_pipe[0]);
    close_fd(err_pipe[1]);
    return spawn_failure("open /dev/null", e);
  }

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, devnull, 0);
  posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
  posix_spawn_file_actions_adddup2(&fa, err_pipe[1], 2);
  posix_spawn_file_actions_addclose(&fa, devnull);
  posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
  posix_spawn_file_actions_addclose(&fa, out_pipe[1]);
  posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
  posix_spawn_file_actions_addclose(&fa, err_pipe[1]);

  std::vector<char *> cargv;
  cargv.reserve(argv.size() + 1);
  for (const auto &a : argv) {
    cargv.push_back(const_cast<char *>(a.c_str()));
  }
  cargv.push_back(nullptr);

  pid_t pid = -1;
  const int rc =
      ::posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close_fd(devnull);
  close_fd(out_pipe[1]);
  close_fd(err_pipe[1]);
  if (rc != 0) {
    close_fd(out_pipe[0]);
    close_fd(err_pipe[0]);
    return spawn_failure("spawn " + argv[0], rc);
  }

  using clock = std::chrono::steady_clock;
  const auto deadline =
      clock::now() + std::chrono::duration_cast<clock::duration>(
                         std::chrono::duration<double>(timeout_sec));

  std::array<Sink, 2> sinks = {
      {{.fd = out_pipe[0], .buf = &res.out, .open = true},
       {.fd = err_pipe[0], .buf = &res.err, .open = true}}};
  std::array<char, 4096> buf{};
  while (sinks[0].open || sinks[1].open) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              clock::now())
            .count();
    if (remaining <= 0) {
      ::kill(pid, SIGKILL);
      res.timed_out = true;
      break;
    }
    std::array<struct pollfd, 2> pfds{};
    int nfds = 0;
    for (const auto &s : sinks) {
      if (s.open) {
        pfds[nfds].fd = s.fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        ++nfds;
      }
    }
    const int pr = ::poll(pfds.data(), static_cast<nfds_t>(nfds),
                          static_cast<int>(remaining));
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (pr == 0) {
      continue; // poll timed out; the deadline check above fires next loop
    }
    int idx = 0;
    for (auto &s : sinks) {
      if (!s.open) {
        continue;
      }
      const struct pollfd &p = pfds[idx++];
      if ((p.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        continue;
      }
      const auto got = ::read(s.fd, buf.data(), buf.size());
      if (got > 0) {
        const auto bytes = static_cast<std::size_t>(got);
        const std::size_t remaining =
            output_limit == 0 || res.captured_bytes >= output_limit
                ? bytes
                : std::min(bytes, output_limit - res.captured_bytes);
        s.buf->append(buf.data(), remaining);
        res.captured_bytes += remaining;
        if (output_limit != 0 && remaining < bytes) {
          res.output_limited = true;
          ::kill(pid, SIGKILL);
          break;
        }
      } else if (got == 0 || (errno != EINTR && errno != EAGAIN)) {
        close_fd(s.fd);
        s.open = false;
      }
    }
  }
  for (auto &s : sinks) {
    if (s.open) {
      close_fd(s.fd);
      s.open = false;
    }
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  struct rusage usage{};
  if (::getrusage(RUSAGE_CHILDREN, &usage) == 0) {
#ifdef __APPLE__
    res.peak_bytes = static_cast<std::size_t>(usage.ru_maxrss);
#else
    res.peak_bytes = static_cast<std::size_t>(usage.ru_maxrss) * 1024U;
#endif
  }
  if (WIFEXITED(status)) {
    res.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    res.exit_code = -WTERMSIG(status); // Python returncode parity
  }
  return res;
}

} // namespace cidx
