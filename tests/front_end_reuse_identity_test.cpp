#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ast/front_end_reuse.hpp"
#include "util/json_read.hpp"

#include <fstream>
#include <iterator>
#include <string>

#ifndef CIDX_FRONT_END_REUSE_GOLDEN
#error "CIDX_FRONT_END_REUSE_GOLDEN must name the checked-in identity vector"
#endif
#ifndef CIDX_FRONT_END_REUSE_SCHEMA
#error "CIDX_FRONT_END_REUSE_SCHEMA must name the checked-in identity schema"
#endif

namespace {

cidx::TranslationUnitConfig fixture() {
  return cidx::TranslationUnitConfig{
      .id = -1,
      .descriptor_hash = "",
      .descriptor_json = "",
      .driver = "/opt/llvm/bin/clang++",
      .working_dir = "/tmp/front-end-reuse",
      .language = "c++",
      .standard = "c++23",
      .target = "arm64-apple-darwin",
      .abi_options = {"-fno-rtti", "-mavx2"},
      .sysroot = "/sdk",
      .resource_dir = "/opt/llvm/lib/clang/22",
      .include_paths = {"/inc/a", "/inc/b"},
      .macro_state = {"-DFOO=1", "-UBAR"},
      .relevant_environment = {"CPATH=/inc", "SDKROOT=/sdk"},
      .generated_inputs = {"/tmp/generated.hpp", "/tmp/macros.h"},
      .diagnostics_policy = "error-limit=0",
      .arguments = {"-std=c++23", "-target", "arm64-apple-darwin", "-fno-rtti",
                    "-I", "/inc/a"},
  };
}

std::string read_file(const char *path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

const cidx::json_out::Value *field(const cidx::json_out::Value &object,
                                   const std::string &name) {
  for (const auto &[key, value] : object.o) {
    if (key == name) {
      return &value;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("front-end reuse identity is versioned and golden") {
  const auto identity = cidx::ast::make_front_end_reuse_identity(fixture());
  const auto golden =
      cidx::json_read::parse(read_file(CIDX_FRONT_END_REUSE_GOLDEN));
  const auto schema = read_file(CIDX_FRONT_END_REUSE_SCHEMA);
  CHECK(schema.find("front-end-reuse/v1") != std::string::npos);
  CHECK(schema.find("canonical_bytes") != std::string::npos);
  CHECK(schema.find("sha256") != std::string::npos);
  REQUIRE(golden.t == cidx::json_out::Value::T::Obj);
  const auto *version = field(golden, "version");
  const auto *mechanism = field(golden, "mechanism");
  const auto *canonical = field(golden, "canonical_bytes");
  const auto *sha256 = field(golden, "sha256");
  REQUIRE(version != nullptr);
  REQUIRE(mechanism != nullptr);
  REQUIRE(canonical != nullptr);
  REQUIRE(sha256 != nullptr);
  CHECK(version->s == identity.version);
  CHECK(mechanism->s == "none");
  CHECK(canonical->s == identity.canonical_bytes);
  CHECK(sha256->s == identity.sha256);
}

TEST_CASE("equivalent prefix ordering has one identity") {
  const auto config = fixture();
  const std::vector<cidx::ast::FrontEndReusePrefixIdentity> first = {
      {.path = "/headers/b",
       .content_sha256 = "b",
       .dependency_sha256 = {"2", "1"}},
      {.path = "/headers/a", .content_sha256 = "a", .dependency_sha256 = {"3"}},
  };
  const std::vector<cidx::ast::FrontEndReusePrefixIdentity> second = {
      {.path = "/headers/a", .content_sha256 = "a", .dependency_sha256 = {"3"}},
      {.path = "/headers/b",
       .content_sha256 = "b",
       .dependency_sha256 = {"1", "2"}},
  };
  CHECK(cidx::ast::make_front_end_reuse_identity(config, first).sha256 ==
        cidx::ast::make_front_end_reuse_identity(config, second).sha256);
}

TEST_CASE("every compatibility input mutates the published identity") {
  const auto base = cidx::ast::make_front_end_reuse_identity(fixture());
  const auto differs = [&base](auto mutate) {
    auto changed = fixture();
    mutate(changed);
    CHECK(cidx::ast::make_front_end_reuse_identity(changed).sha256 !=
          base.sha256);
  };
  differs([](auto &config) { config.driver = "/usr/bin/clang++"; });
  differs([](auto &config) { config.working_dir = "/tmp/other"; });
  differs([](auto &config) { config.language = "c"; });
  differs([](auto &config) { config.standard = "c++20"; });
  differs([](auto &config) { config.target = "x86_64-linux-gnu"; });
  differs(
      [](auto &config) { config.abi_options.emplace_back("-fshort-wchar"); });
  differs([](auto &config) { config.sysroot = "/other-sdk"; });
  differs([](auto &config) { config.resource_dir = "/other-resource"; });
  differs([](auto &config) { config.include_paths.emplace_back("/inc/c"); });
  differs([](auto &config) { config.macro_state.emplace_back("-DNEW=1"); });
  differs(
      [](auto &config) { config.relevant_environment.emplace_back("NEW=1"); });
  differs(
      [](auto &config) { config.generated_inputs.emplace_back("/tmp/new.h"); });
  differs([](auto &config) { config.diagnostics_policy = "fatal-errors"; });
  differs(
      [](auto &config) { config.arguments.emplace_back("-fno-exceptions"); });
}

TEST_CASE("prefix content and dependency identity are compatibility inputs") {
  const auto config = fixture();
  const std::vector<cidx::ast::FrontEndReusePrefixIdentity> base = {
      {.path = "/headers/a", .content_sha256 = "a", .dependency_sha256 = {"1"}},
  };
  const auto identity = cidx::ast::make_front_end_reuse_identity(config, base);
  auto content_changed = base;
  content_changed.front().content_sha256 = "b";
  auto dependency_changed = base;
  dependency_changed.front().dependency_sha256.emplace_back("2");
  CHECK(cidx::ast::make_front_end_reuse_identity(config, content_changed)
            .sha256 != identity.sha256);
  CHECK(cidx::ast::make_front_end_reuse_identity(config, dependency_changed)
            .sha256 != identity.sha256);
}
