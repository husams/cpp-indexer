#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "util/front_end_reuse.hpp"

#include <fstream>
#include <string>
#include <sys/stat.h>

#include <unistd.h>

namespace {

std::string make_temp_dir() {
  std::string pattern = "/tmp/cidx_front_end_reuse_XXXXXX";
  char *path = ::mkdtemp(pattern.data());
  REQUIRE(path != nullptr);
  return path;
}

} // namespace

TEST_CASE("missing build-declared PCH is diagnosed before Clang") {
  cidx::TranslationUnitConfig configuration;
  configuration.working_dir = make_temp_dir();
  configuration.arguments = {"-std=c++23", "-include-pch", "missing.pch"};
  const auto diagnostic = cidx::preflight_build_declared_pch(configuration);
  REQUIRE(diagnostic.has_value());
  const auto message = diagnostic.value_or("");
  CHECK(message.find("build-declared PCH") != std::string::npos);
  CHECK(message.find("missing or unreadable") != std::string::npos);
  CHECK(configuration.arguments ==
        std::vector<std::string>{"-std=c++23", "-include-pch", "missing.pch"});
}

TEST_CASE("existing build-declared PCH is preserved for Clang diagnostics") {
  const std::string directory = make_temp_dir();
  const std::string path = directory + "/declared.pch";
  std::ofstream output(path);
  REQUIRE(output.good());
  output << "not a precompiled header";
  output.close();

  cidx::TranslationUnitConfig configuration;
  configuration.working_dir = directory;
  configuration.arguments = {"-include-pch=" + path};
  CHECK_FALSE(cidx::preflight_build_declared_pch(configuration).has_value());
  CHECK(configuration.arguments ==
        std::vector<std::string>{"-include-pch=" + path});
}

TEST_CASE("unreadable build-declared PCH is diagnosed before Clang") {
  const std::string directory = make_temp_dir();
  const std::string path = directory + "/unreadable.pch";
  std::ofstream output(path);
  REQUIRE(output.good());
  output << "not readable by the indexing process";
  output.close();
  REQUIRE(::chmod(path.c_str(), 0) == 0);

  cidx::TranslationUnitConfig configuration;
  configuration.working_dir = directory;
  configuration.arguments = {"-include-pch", "unreadable.pch"};
  const auto diagnostic = cidx::preflight_build_declared_pch(configuration);
  CHECK(::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0);
  REQUIRE(diagnostic.has_value());
  CHECK(diagnostic.value_or("").find("missing or unreadable") !=
        std::string::npos);
}
