#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ast/front_end_reuse.hpp"
#include "cli/application_adapter.hpp"
#include "cli/args.hpp"

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
