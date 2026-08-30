#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "ast/spillable_fact_buffer.hpp"
#include "ast/spillable_identity_index.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace {

auto encode(const std::string &value) -> std::vector<std::byte> {
  const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
  return {bytes.begin(), bytes.end()};
}

auto decode(std::span<const std::byte> bytes) -> std::string {
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

} // namespace

TEST_CASE("fact payload segments retain order and clean up") {
  const auto root =
      std::filesystem::temp_directory_path() / "cidx-spillable-extraction-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  std::filesystem::path segment;
  {
    cidx::ast::SpillableFactBuffer<std::string> buffer(
        {.family = cidx::ast::FactFamily::symbols,
         .spill_threshold_bytes = 1,
         .max_total_bytes = 4096,
         .spill_directory = root},
        encode, decode);
    CHECK(buffer.append("first") == 0);
    CHECK(buffer.append("second") == 1);
    REQUIRE(buffer.spilled());
    REQUIRE(buffer.segments().size() == 2);
    segment = buffer.segments().front().path;
    std::vector<std::string> values;
    buffer.for_each_in_order([&](std::uint64_t, const std::string &value) {
      values.push_back(value);
    });
    CHECK(values == std::vector<std::string>{"first", "second"});
    CHECK(std::filesystem::exists(segment));
  }
  CHECK(!std::filesystem::exists(segment));
  std::filesystem::remove_all(root, ignored);
}

TEST_CASE("identity runs preserve exact lookup and stable handles") {
  const auto root =
      std::filesystem::temp_directory_path() / "cidx-spillable-identity-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  std::filesystem::path run;
  {
    cidx::ast::SpillableIdentityIndex index({.max_resident_identity_bytes = 128,
                                             .max_identity_entries = 1,
                                             .max_total_bytes = 4096,
                                             .spill_directory = root});
    CHECK(index.insert(cidx::ast::FactIdentityKind::symbol, "alpha", 17) ==
          cidx::ast::IdentityInsertResult::inserted);
    CHECK(index.insert(cidx::ast::FactIdentityKind::symbol, "beta", 23) ==
          cidx::ast::IdentityInsertResult::inserted);
    REQUIRE(index.spilled());
    REQUIRE(index.runs().size() == 1);
    run = index.runs().front().path;
    CHECK(index.lookup(cidx::ast::FactIdentityKind::symbol, "alpha") == 17);
    CHECK(index.lookup(cidx::ast::FactIdentityKind::symbol, "beta") == 23);
    CHECK(index.insert(cidx::ast::FactIdentityKind::symbol, "alpha", 17) ==
          cidx::ast::IdentityInsertResult::existing);
    CHECK(index.insert(cidx::ast::FactIdentityKind::symbol, "alpha", 99) ==
          cidx::ast::IdentityInsertResult::conflict);
    CHECK(std::filesystem::file_size(run) > 0);
  }
  CHECK(!std::filesystem::exists(run));
  std::filesystem::remove_all(root, ignored);
}
