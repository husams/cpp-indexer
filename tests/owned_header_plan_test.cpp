#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "application/controlled_header_writer.hpp"
#include "ast/fact_extraction.hpp"
#include "ast/owned_header_plan.hpp"
#include "ast/storage_symbol_sink.hpp"
#include "storage/sqlite_adapters.hpp"
#include "storage/storage.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
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

class TemporaryDatabase {
public:
  TemporaryDatabase()
      : path_(
            std::filesystem::temp_directory_path() /
            ("cidx-s099-plan-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             ".db")),
        storage_(path_.string()) {
    static_cast<void>(storage_.add_component("s099", "/tmp/cidx-s099", "repo"));
  }

  ~TemporaryDatabase() {
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(path_, ignored));
  }

  TemporaryDatabase(const TemporaryDatabase &) = delete;
  TemporaryDatabase &operator=(const TemporaryDatabase &) = delete;

  [[nodiscard]] auto storage() -> Storage & { return storage_; }

private:
  std::filesystem::path path_;
  Storage storage_;
};

void seed_symbol(Storage &storage, std::int64_t file_id) {
  storage::SqliteStoragePorts sqlite(storage);
  storage::AstStoragePorts ports{.workspace = sqlite.workspace_catalog_read(),
                                 .source = sqlite.source_read(),
                                 .source_write = sqlite.source_write(),
                                 .symbols_read = sqlite.symbol_read(),
                                 .symbols_write = sqlite.symbol_write(),
                                 .types_write = sqlite.type_write(),
                                 .facts_write = sqlite.fact_write(),
                                 .definitions_write = sqlite.definition_write(),
                                 .unit_of_work = sqlite.unit_of_work()};
  ast::StorageSymbolSink sink(ports);
  auto transaction = ports.unit_of_work.begin();
  sink.set_current_file_id(file_id);
  ast::SymbolRecord record;
  record.file = "/tmp/cidx-s099/main.cpp";
  record.usr = "s099-main";
  record.spelling = "main";
  record.kind = 8;
  record.kind_name = "function";
  sink.emit(record);
  transaction->commit();
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

TEST_CASE("controlled lifecycle mutation rolls back with the complete TU") {
  TemporaryDatabase database;
  Storage &storage = database.storage();
  const std::string main = "/tmp/cidx-s099/main.cpp";
  const std::string header = "/tmp/cidx-s099/header.hpp";
  const std::int64_t main_id = storage.add_file_path(main);
  seed_symbol(storage, main_id);
  REQUIRE(storage.symbols_in_file(main_id).size() == 1);
  REQUIRE(!storage.get_file(header).has_value());

  const ast::OwnedHeaderRoutePlan plan = ast::plan_owned_header_routes(
      "generation-9",
      {candidate(ast::PlannedFileRole::translation_unit, main, main, 0,
                 main_id),
       candidate(ast::PlannedFileRole::owned_header, main, header, 1)});

  storage::SqliteStoragePorts sqlite(storage);
  auto transaction = sqlite.unit_of_work().begin();
  application::ControlledHeaderWriter writer(sqlite.source_write(),
                                             sqlite.symbol_write());
  const auto applied =
      writer.apply(plan, main, "generation-9",
                   [](const std::string &, const ast::PlannedSourceSnapshot &) {
                     return true;
                   });
  REQUIRE(applied.ok());
  CHECK(storage.get_file(header).has_value());
  CHECK(storage.symbols_in_file(main_id).empty());
  transaction->rollback();

  CHECK(!storage.get_file(header).has_value());
  CHECK(storage.symbols_in_file(main_id).size() == 1);
}

TEST_CASE("stale plans are rejected before lifecycle mutation") {
  TemporaryDatabase database;
  Storage &storage = database.storage();
  const std::string main = "/tmp/cidx-s099/main.cpp";
  const std::string header = "/tmp/cidx-s099/header.hpp";
  const std::int64_t main_id = storage.add_file_path(main);
  seed_symbol(storage, main_id);
  const ast::OwnedHeaderRoutePlan plan = ast::plan_owned_header_routes(
      "generation-11",
      {candidate(ast::PlannedFileRole::translation_unit, main, main, 0,
                 main_id),
       candidate(ast::PlannedFileRole::owned_header, main, header, 1)});
  storage::SqliteStoragePorts sqlite(storage);
  application::ControlledHeaderWriter writer(sqlite.source_write(),
                                             sqlite.symbol_write());

  const auto generation_rejected =
      writer.apply(plan, main, "generation-12",
                   [](const std::string &, const ast::PlannedSourceSnapshot &) {
                     return true;
                   });
  REQUIRE(!generation_rejected.ok());
  CHECK(generation_rejected.rejection->kind ==
        application::HeaderPlanRejectionKind::generation_mismatch);
  CHECK(!storage.get_file(header).has_value());
  CHECK(storage.symbols_in_file(main_id).size() == 1);

  const auto source_rejected = writer.apply(
      plan, main, "generation-11",
      [&header](const std::string &path, const ast::PlannedSourceSnapshot &) {
        return path != header;
      });
  REQUIRE(!source_rejected.ok());
  CHECK(source_rejected.rejection->kind ==
        application::HeaderPlanRejectionKind::stale_source);
  CHECK(source_rejected.rejection->path == header);
  CHECK(!storage.get_file(header).has_value());
  CHECK(storage.symbols_in_file(main_id).size() == 1);
}
