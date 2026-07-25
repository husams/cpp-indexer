// Subprocess runner (design D9) — posix_spawnp-based equivalent of the
// Python driver probe:
//   subprocess.run(argv, input="", capture_output=True, text=True, timeout=30)
// Contract:
//   * stdin = /dev/null (empty-stdin parity)
//   * stdout and stderr captured separately via pipes
//   * timeout via poll + waitpid loop; the child is SIGKILLed on expiry and
//     timed_out is set
//   * exit_code: WEXITSTATUS, or -SIGNUM when killed (Python returncode parity)
//   * spawn failure (e.g. missing binary) -> exit_code 127, message in err;
//     never throws (the only consumer falls back to host defaults, G8)
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace cidx {

struct RunResult {
  int exit_code = -1;
  std::string out;
  std::string err;
  bool timed_out = false;
  bool output_limited = false;
  std::size_t captured_bytes = 0;
  std::size_t peak_bytes = 0;
};

RunResult run(const std::vector<std::string> &argv, double timeout_sec = 30.0,
              std::size_t output_limit = 0);

} // namespace cidx
