#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Driver/CreateASTUnitFromArgs.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Program.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

namespace {

struct Trial {
  bool successful = false;
  long long wall_microseconds = 0;
  std::uint64_t peak_rss_bytes = 0;
  unsigned diagnostic_errors = 0;
  bool semantic_parity = false;
};

struct SemanticSummary {
  bool value_template = false;
  bool make_value = false;

  bool complete() const { return value_template && make_value; }
};

class SummaryVisitor final : public clang::RecursiveASTVisitor<SummaryVisitor> {
public:
  explicit SummaryVisitor(SemanticSummary &summary) : summary_(summary) {}

  bool VisitNamedDecl(const clang::NamedDecl *declaration) {
    const std::string qualified = declaration->getQualifiedNameAsString();
    summary_.value_template |= qualified == "probe::Value";
    summary_.make_value |= qualified == "probe::make_value";
    return true;
  }

private:
  SemanticSummary &summary_;
};

class SummaryConsumer final : public clang::ASTConsumer {
public:
  explicit SummaryConsumer(SemanticSummary &summary) : summary_(summary) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    SummaryVisitor visitor(summary_);
    visitor.TraverseDecl(context.getTranslationUnitDecl());
  }

private:
  SemanticSummary &summary_;
};

class SummaryAction final : public clang::ASTFrontendAction {
public:
  SummaryAction(SemanticSummary &summary, unsigned &diagnostic_errors)
      : summary_(summary), diagnostic_errors_(diagnostic_errors) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &, llvm::StringRef) override {
    return std::make_unique<SummaryConsumer>(summary_);
  }

  void EndSourceFileAction() override {
    diagnostic_errors_ = getCompilerInstance().getDiagnostics().getNumErrors();
    clang::ASTFrontendAction::EndSourceFileAction();
  }

private:
  SemanticSummary &summary_;
  unsigned &diagnostic_errors_;
};

std::uint64_t peak_rss_bytes() {
  struct rusage usage{};
  return ::getrusage(RUSAGE_SELF, &usage) == 0
             ? static_cast<std::uint64_t>(usage.ru_maxrss)
             : 0;
}

Trial run_syntax_trial(const std::string &source,
                       const std::vector<std::string> &args) {
  SemanticSummary summary;
  unsigned diagnostic_errors = 0;
  auto action = std::make_unique<SummaryAction>(summary, diagnostic_errors);
  const auto started = std::chrono::steady_clock::now();
  const bool successful = clang::tooling::runToolOnCodeWithArgs(
      std::move(action), source, args, "front_end_reuse_probe.cc");
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started);
  const unsigned observed_errors =
      diagnostic_errors == 0 && !successful ? 1 : diagnostic_errors;
  return {.successful = successful && observed_errors == 0,
          .wall_microseconds = elapsed.count(),
          .peak_rss_bytes = peak_rss_bytes(),
          .diagnostic_errors = observed_errors,
          .semantic_parity = summary.complete()};
}

Trial run_astunit_trial(const std::string &source) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("cidx_front_end_reuse_astunit_" + std::to_string(::getpid()));
  std::error_code cleanup_error;
  std::filesystem::remove_all(directory, cleanup_error);
  std::filesystem::create_directories(directory);
  const std::filesystem::path prefix_path = directory / "prefix.h";
  const std::filesystem::path source_path = directory / "probe.cc";
  std::ofstream(prefix_path) << R"cpp(
namespace probe {
template <typename T> struct Value { T value; };
}
)cpp";
  std::ofstream(source_path) << "#include \"prefix.h\"\n" << source;
  const std::string compiler =
      std::filesystem::is_regular_file("/opt/homebrew/opt/llvm/bin/clang++")
          ? "/opt/homebrew/opt/llvm/bin/clang++"
          : "clang++";
  const std::vector<std::string> command = {
      compiler, "-std=c++23", "-I", directory.string(), source_path.string()};
  std::vector<const char *> command_args;
  command_args.reserve(command.size());
  for (const std::string &argument : command) {
    command_args.push_back(argument.c_str());
  }
  auto diagnostic_options = std::make_shared<clang::DiagnosticOptions>();
  auto diagnostics = llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine>(
      new clang::DiagnosticsEngine(
          llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs>(
              new clang::DiagnosticIDs()),
          *diagnostic_options));
  SemanticSummary summary;
  const auto started = std::chrono::steady_clock::now();
  const auto ast = clang::CreateASTUnitFromCommandLine(
      command_args.data(), command_args.data() + command_args.size(),
      std::make_shared<clang::PCHContainerOperations>(), diagnostic_options,
      diagnostics, "", true, directory.string(), false,
      clang::CaptureDiagsKind::All, {}, true, 1);
  bool reparse_failed = true;
  bool preamble_built = false;
  if (ast != nullptr) {
    preamble_built = ast->getPreambleCounterForTests() > 0;
    reparse_failed =
        ast->Reparse(std::make_shared<clang::PCHContainerOperations>());
    preamble_built = preamble_built || ast->getPreambleCounterForTests() > 0;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started);
  if (ast != nullptr) {
    SummaryVisitor visitor(summary);
    visitor.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
  }
  const unsigned diagnostic_errors =
      ast == nullptr || diagnostics->hasErrorOccurred() ? 1 : 0;
  std::filesystem::remove_all(directory, cleanup_error);
  return {.successful = ast != nullptr && !reparse_failed && preamble_built &&
                        diagnostic_errors == 0 && summary.complete(),
          .wall_microseconds = elapsed.count(),
          .peak_rss_bytes = peak_rss_bytes(),
          .diagnostic_errors = diagnostic_errors,
          .semantic_parity = summary.complete()};
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
    } else {
      setup =
          "ASTUnit precompiled preamble build then owner-preserving reparse";
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
              : run_astunit_trial(candidate_source);
      if (is_control && !result.successful) {
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
    std::cout << R"(],"trial_diagnostic_errors":[)";
    bool first_diagnostic_count = true;
    for (const Trial &result : results) {
      if (!first_diagnostic_count) {
        std::cout << ',';
      }
      first_diagnostic_count = false;
      std::cout << result.diagnostic_errors;
    }
    std::cout << R"(],"trial_semantic_parity":[)";
    bool first_semantic_parity = true;
    for (const Trial &result : results) {
      if (!first_semantic_parity) {
        std::cout << ',';
      }
      first_semantic_parity = false;
      std::cout << (result.semantic_parity ? "true" : "false");
    }
    std::cout << R"(],"trial_peak_rss_bytes":[)";
    bool first_rss = true;
    for (const Trial &result : results) {
      if (!first_rss) {
        std::cout << ',';
      }
      first_rss = false;
      std::cout << result.peak_rss_bytes;
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
