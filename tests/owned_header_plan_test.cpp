#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ast/fact_extraction.hpp"
#include "ast/owned_header_plan.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cidx;

namespace {

auto partition(std::string path, std::string translation_unit)
    -> ast::FactPartitionKey {
  const std::filesystem::path file(path);
  return {.file = {.component_path = "/tmp/cidx-s099",
                   .directory_path = file.parent_path().filename().string(),
                   .file_name = file.filename().string()},
          .configuration = {.semantic_universe = "workspace",
                            .translation_unit = std::move(translation_unit),
                            .normalized_configuration = "debug",
                            .identity_source = std::move(path)}};
}

auto candidate(ast::PlannedFileRole role, std::string translation_unit,
               std::string path, std::size_t ordinal,
               std::optional<std::int64_t> existing = std::nullopt)
    -> ast::OwnedHeaderRouteCandidate {
  const std::string identity = translation_unit;
  return {.role = role,
          .translation_unit = std::move(translation_unit),
          .translation_unit_file_id = existing.value_or(-1),
          .path = path,
          .discovery_ordinal = ordinal,
          .existing_file_id = existing,
          .snapshot = {.mtime = 10.0, .md5 = "snapshot"},
          .compile_options = std::vector<std::string>{"-std=c++23"},
          .driver = "clang++",
          .cleanup_symbols = true,
          .partition = partition(std::move(path), identity)};
}

auto descriptor() -> ast::ExtractionPassDescriptor {
  return {.id = "noop",
          .version = 1,
          .required_capabilities = {},
          .consumed_fact_families = {"route"},
          .produced_fact_families = {"noop"},
          .catalog_versions = {1},
          .dependencies = {},
          .scope = ast::PassScope::translation_unit,
          .traversal = ast::TraversalMode::lifecycle,
          .completeness = ast::FactCompleteness::complete,
          .trust = ast::FactTrust::trusted,
          .budget = {.declared = true}};
}

} // namespace

TEST_CASE("shared headers receive one deterministic pre-dispatch owner") {
  const std::string first = "/tmp/cidx-s099/a.cpp";
  const std::string second = "/tmp/cidx-s099/b.cpp";
  const std::string shared = "/tmp/cidx-s099/shared.hpp";
  std::vector<ast::OwnedHeaderRouteCandidate> reverse{
      candidate(ast::PlannedFileRole::translation_unit, second, second, 0, 2),
      candidate(ast::PlannedFileRole::owned_header, second, shared, 1),
      candidate(ast::PlannedFileRole::translation_unit, first, first, 0, 1),
      candidate(ast::PlannedFileRole::owned_header, first, shared, 4)};
  auto forward = reverse;
  std::ranges::reverse(forward);

  const ast::OwnedHeaderRoutePlan reverse_plan =
      ast::plan_owned_header_routes("generation-7", std::move(reverse));
  const ast::OwnedHeaderRoutePlan forward_plan =
      ast::plan_owned_header_routes("generation-7", std::move(forward));
  CHECK(reverse_plan.token() == forward_plan.token());
  CHECK(reverse_plan.routes().size() == 3);

  std::size_t assignments = 0;
  for (const ast::PlannedFileRoute &route : reverse_plan.routes()) {
    if (route.role == ast::PlannedFileRole::owned_header &&
        route.path == shared) {
      ++assignments;
      CHECK(route.translation_unit == first);
    }
  }
  CHECK(assignments == 1);

  ast::ExtractionPassRegistry registry;
  registry.register_pass(descriptor(),
                         [](ast::PassExecutionContext & /*execution*/) {});
  ast::IndexingPlan extraction;
  extraction.add("noop");
  const ast::SerialFactRoute route = reverse_plan.serial_route(first);
  CHECK(route.partitions.size() == 2);
  const auto result =
      ast::extract_serial_fact_batch({}, registry, extraction, route);
  REQUIRE(result.ok());
  REQUIRE(result.batch.has_value());
}
