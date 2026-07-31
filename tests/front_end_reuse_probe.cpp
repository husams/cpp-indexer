#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Program.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

struct Trial {
  bool successful = false;
  long long wall_microseconds = 0;
};

Trial run_syntax_trial(const std::string &source,
                       const std::vector<std::string> &args) {
  const auto started = std::chrono::steady_clock::now();
  const bool successful = clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<clang::SyntaxOnlyAction>(), source, args,
      "front_end_reuse_probe.cc");
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started);
  return {.successful = successful, .wall_microseconds = elapsed.count()};
}

Trial run_astunit_trial(const std::string &source,
                        const std::vector<std::string> &args) {
  const auto started = std::chrono::steady_clock::now();
  const bool successful =
      clang::tooling::buildASTFromCodeWithArgs(
          source, args, "front_end_reuse_probe.cc") != nullptr;
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started);
  return {.successful = successful, .wall_microseconds = elapsed.count()};
}

bool generate_pch(const std::string &prefix,
                  const std::filesystem::path &pch_path) {
  std::filesystem::path prefix_path = pch_path;
  prefix_path.replace_extension(".h");
  std::ofstream output(prefix_path);
  if (!output) {
    return false;
  }
  output << prefix;
  output.close();
  const std::string compiler =
      std::filesystem::is_regular_file("/opt/homebrew/opt/llvm/bin/clang++")
          ? "/opt/homebrew/opt/llvm/bin/clang++"
          : "clang++";
  const std::vector<std::string> args = {
      compiler,     "-std=c++23",      "-x",
      "c++-header", "-Xclang",         "-emit-pch",
      "-o",         pch_path.string(), prefix_path.string()};
  std::vector<llvm::StringRef> arg_refs;
  arg_refs.reserve(args.size());
  for (const std::string &arg : args) {
    arg_refs.emplace_back(arg);
  }
  const std::array<std::optional<llvm::StringRef>, 3> redirects = {
      std::nullopt, llvm::StringRef("/dev/null"), llvm::StringRef("/dev/null")};
  const bool successful = llvm::sys::ExecuteAndWait(
                              compiler, arg_refs, std::nullopt, redirects) == 0;
  std::error_code cleanup_error;
  std::filesystem::remove(prefix_path, cleanup_error);
  return successful && std::filesystem::is_regular_file(pch_path);
}

} // namespace

int run_probe() {
  constexpr int trials = 3;
  const std::string prefix = R"cpp(
namespace probe {
template <typename T> struct Value { T value; };
}
)cpp";
  const std::string source = R"cpp(
namespace probe {
Value<int> make_value(int value) { return {value}; }
}
)cpp";
  const std::string control_source = prefix + source;
  const std::vector<std::string> control_args = {"-std=c++23"};
  const std::filesystem::path pch_path =
      std::filesystem::temp_directory_path() /
      ("cidx_front_end_reuse_" + std::to_string(::getpid()) + ".pch");
  std::error_code cleanup_error;
  std::filesystem::remove(pch_path, cleanup_error);
  const auto pch_setup_started = std::chrono::steady_clock::now();
  const bool pch_ready = generate_pch(prefix, pch_path);
  const auto pch_setup_elapsed =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - pch_setup_started);
  const std::vector<std::string> pch_args = {"-std=c++23", "-include-pch",
                                             pch_path.string()};
  const std::vector<std::string> astunit_args = {"-std=c++23"};
  constexpr std::array<std::string_view, 3> candidates = {
      "no-reuse", "generated-umbrella-pch", "precompiled-preamble-astunit"};

  std::cout << R"({"contract":"front_end_reuse_probe/v1","trials":)" << trials
            << R"(,"candidates":[)";
  bool first_candidate = true;
  for (const std::string_view candidate : candidates) {
    if (!first_candidate) {
      std::cout << ',';
    }
    first_candidate = false;
    const bool is_control = candidate == "no-reuse";
    const bool is_pch = candidate == "generated-umbrella-pch";
    const std::string &candidate_source = is_control ? control_source : source;
    const std::vector<std::string> *candidate_args = &astunit_args;
    if (is_control) {
      candidate_args = &control_args;
    } else if (is_pch) {
      candidate_args = &pch_args;
    }
    if (is_pch && !pch_ready) {
      return 1;
    }
    std::string setup =
        "ASTUnit construction with preamble reuse disabled by isolated "
        "ownership";
    if (is_control) {
      setup = "SyntaxOnlyAction";
    } else if (is_pch) {
      setup = "GeneratePCHAction then SyntaxOnlyAction with -include-pch";
    }
    std::cout << R"({"name":")" << candidate << R"(","decision":")"
              << (is_control ? "control" : "reject") << R"(","setup":")"
              << setup << R"(","setup_wall_microseconds":)"
              << (is_pch ? pch_setup_elapsed.count() : 0)
              << R"(,"trial_wall_microseconds":[)";
    std::vector<Trial> results;
    results.reserve(trials);
    for (int trial = 0; trial < trials; ++trial) {
      const Trial result =
          is_control || is_pch
              ? run_syntax_trial(candidate_source, *candidate_args)
              : run_astunit_trial(candidate_source, *candidate_args);
      if (!result.successful && !is_pch) {
        return 1;
      }
      results.push_back(result);
    }
    bool first_trial = true;
    for (const Trial &result : results) {
      if (!first_trial) {
        std::cout << ',';
      }
      first_trial = false;
      std::cout << result.wall_microseconds;
    }
    std::cout << R"(],"trial_successful":[)";
    bool first_success = true;
    for (const Trial &result : results) {
      if (!first_success) {
        std::cout << ',';
      }
      first_success = false;
      std::cout << (result.successful ? "true" : "false");
    }
    std::cout << "]}";
  }
  std::cout << R"(],"rejected_candidates_require_adr":true})" << '\n';
  std::filesystem::remove(pch_path, cleanup_error);
  return 0;
}

int main() {
  try {
    return run_probe();
  } catch (...) {
    return 1;
  }
}
