#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ast/front_end_reuse.hpp"
#include "ast/index_engine.hpp"
#include "cli/application_adapter.hpp"
#include "cli/args.hpp"
#include "profile/index_profile.hpp"
#include "storage/storage.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

TEST_CASE("do-not-ship plan selects none without an artifact path") {
  cidx::TranslationUnitConfig configuration;
  configuration.driver = "clang++";
  configuration.arguments = {"-std=c++23"};
  const auto enabled = cidx::ast::plan_front_end_reuse(configuration);
  const auto disabled = cidx::ast::plan_front_end_reuse(configuration, true);
  CHECK(enabled.mechanism == cidx::ast::FrontEndReuseMechanism::none);
  CHECK_FALSE(enabled.inject);
  CHECK(enabled.identity.mechanism == "none");
  CHECK(enabled.reason.find("no reusable artifact") != std::string::npos);
  CHECK(disabled.identity.sha256 == enabled.identity.sha256);
  CHECK(disabled.reason.find("explicitly disabled") != std::string::npos);
}

TEST_CASE("both CLI paths accept explicit front-end reuse disable") {
  const cidx::cli::ParsedArgs compatibility =
      cidx::cli::parse_args({"index", "--no-front-end-reuse"});
  CHECK(compatibility.no_front_end_reuse);

  const auto parsed =
      cidx::cli::parse_application_request({"index", "--no-front-end-reuse"});
  const auto *command =
      std::get_if<cidx::application::CommandRequest>(&parsed.value);
  REQUIRE(command != nullptr);
  const auto *request = std::get_if<cidx::application::IndexRequest>(command);
  REQUIRE(request != nullptr);
  CHECK(request->no_front_end_reuse);
}

TEST_CASE("production indexing profile distinguishes explicit disable") {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("cidx_front_end_reuse_profile_" + std::to_string(::getpid()));
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
  REQUIRE(std::filesystem::create_directories(root));
  const std::filesystem::path source = root / "fixture.cpp";
  std::ofstream(source) << "int profile_fixture() { return 42; }\n";

  const auto run_profile = [&](bool disabled,
                               const std::filesystem::path &profile_path) {
    cidx::Storage db(":memory:");
    db.add_component("app", root.string());
    const auto file_id = db.add_file_path(
        source.string(), std::nullopt, std::nullopt,
        std::vector<std::string>{"-std=c++23"}, std::string("c++"));
    const auto file = db.get_file_by_id(file_id);
    REQUIRE(file.has_value());
    {
      cidx::profile::Session profile(profile_path.string(), std::nullopt);
      const auto outcome = cidx::ast::run_index_one(
          db, *file, source.string(), false, cidx::ast::IndexFailurePoint::none,
          disabled);
      CHECK_FALSE(outcome.parse_failed);
      profile.finish();
    }
    std::ifstream input(profile_path);
    REQUIRE(input.good());
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
  };

  const std::string disabled_profile =
      run_profile(true, root / "disabled.json");
  const std::string default_profile = run_profile(false, root / "default.json");
  CHECK(disabled_profile.find("\"front_end_reuse.explicitly_disabled\": 1") !=
        std::string::npos);
  CHECK(default_profile.find("\"front_end_reuse.explicitly_disabled\": 0") !=
        std::string::npos);
  CHECK(disabled_profile.find("\"front_end_reuse.mechanism.none\": 1") !=
        std::string::npos);
  CHECK(default_profile.find("\"front_end_reuse.mechanism.none\": 1") !=
        std::string::npos);
  std::filesystem::remove_all(root, cleanup_error);
}
