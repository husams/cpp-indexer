#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ast/fact_batch.hpp"
#include "ast/owned_header_plan.hpp"
#include "storage/fact_batch_writer.hpp"
#include "storage/storage.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace cidx;

auto partition(std::string file, std::string source) -> ast::FactPartitionKey {
  return {
      .file = {.component_path = "/tmp/cidx-s073-writer",
               .directory_path = {},
               .file_name = std::move(file)},
      .configuration = {.semantic_universe = "workspace",
                        .translation_unit = "/tmp/cidx-s073-writer/main.cpp",
                        .normalized_configuration = "debug",
                        .identity_source = std::move(source)}};
}

auto symbol(const ast::FactPartitionKey &owner, std::string usr,
            std::string spelling) -> ast::SymbolRecord {
  return {.file = owner.file.portable_path(),
          .usr = std::move(usr),
          .spelling = std::move(spelling),
          .kind = 8,
          .line = 1,
          .col = 1,
          .end_line = 1,
          .end_col = 8,
          .is_definition = true,
          .linkage = "external",
          .resolved = true,
          .identity_source = owner.configuration.identity_source,
          .identity_translation_unit = owner.configuration.translation_unit,
          .kind_name = "function"};
}

struct Fixture {
  Fixture() {
    const std::int64_t universe =
        storage.add_semantic_universe("workspace", "workspace");
    const std::int64_t component =
        storage.add_component("writer", "/tmp/cidx-s073-writer", "repo");
    storage.set_component_semantic_universe(component, universe);
    main_file = storage.add_file_path("/tmp/cidx-s073-writer/main.cpp");
    const auto main = partition("main.cpp", "/tmp/cidx-s073-writer/main.cpp");
    const auto header =
        partition("header.hpp", "/tmp/cidx-s073-writer/header.hpp");
    plan = ast::plan_owned_header_routes(
        "generation-1",
        {{.role = ast::PlannedFileRole::translation_unit,
          .translation_unit = main.configuration.translation_unit,
          .translation_unit_file_id = main_file,
          .path = main.file.portable_path(),
          .existing_file_id = main_file,
          .snapshot = {.md5 = "main-md5"},
          .cleanup_symbols = true,
          .partition = main},
         {.role = ast::PlannedFileRole::owned_header,
          .translation_unit = main.configuration.translation_unit,
          .translation_unit_file_id = main_file,
          .path = header.file.portable_path(),
          .discovery_ordinal = 1,
          .snapshot = {.md5 = "header-md5"},
          .cleanup_symbols = true,
          .partition = header}});
  }

  auto batch() const -> ast::FactBatch {
    ast::FactBatchRecorder recorder("fact-batch-writer-test");
    const auto route = plan.serial_route("/tmp/cidx-s073-writer/main.cpp");
    REQUIRE(route.partitions.size() == 2);
    const auto &main = route.partitions[route.main_partition];
    const auto &header = route.partitions[1];
    recorder.set_partition(main.partition, main.transient_file_handle);
    recorder.emit(symbol(main.partition, "usr-main", "main"));
    const std::int64_t main_symbol =
        recorder.lookup_symbol_id("usr-main").value();
    recorder.set_partition(header.partition, header.transient_file_handle);
    recorder.emit(symbol(header.partition, "usr-header", "header"));
    const std::int64_t header_symbol =
        recorder.lookup_symbol_id("usr-header").value();
    static_cast<void>(
        recorder.mint_symbol({.usr = "usr-external",
                              .spelling = "External",
                              .qual_name = "External",
                              .display_name = "External",
                              .kind_name = "struct",
                              .decl_line = 7,
                              .decl_col = 3,
                              .decl_path = "/usr/include/external.hpp",
                              .is_named_instance = true,
                              .identity_source = "/usr/include/external.hpp"}));
    const std::int64_t relation = recorder.add_edge(
        {.src_id = main_symbol, .dst_id = header_symbol, .kind = 1});
    recorder.add_edge_site({.edge_id = relation,
                            .file_id = *main.transient_file_handle,
                            .line = 4,
                            .col = 3,
                            .recv_src_kind = "local",
                            .recv_decl_usr = "external-local"});
    const std::int64_t type = recorder.intern_type_node(
        {.type_key = "type:int", .spelling = "int", .kind = 1});
    recorder.add_symbol_type(main_symbol, 1, type);
    static_cast<void>(recorder.get_or_create_definition(
        main_symbol, *main.transient_file_handle, 1, 1, 1, 8, std::nullopt));
    recorder.emit(
        ast::DiagnosticFactRecord{.partition = main.partition,
                                  .severity = ast::DiagnosticSeverity::warning,
                                  .spelling = "writer warning",
                                  .location_file = main.partition.file,
                                  .line = 5,
                                  .col = 2});
    recorder.emit(ast::IncludeDirectiveRecord{
        .partition = main.partition,
        .source = main.partition.file,
        .destination = header.partition.file,
        .destination_path = header.partition.file.portable_path(),
        .spelling = "header.hpp",
        .line = 1,
        .col = 1});
    return recorder.canonical_batch();
  }

  auto context(storage::FactBatchWriterFailurePoint failure =
                   storage::FactBatchWriterFailurePoint::none) const
      -> storage::FactBatchPublicationContext {
    return {.route_plan = plan,
            .translation_unit = "/tmp/cidx-s073-writer/main.cpp",
            .expected_generation = "generation-1",
            .source_is_current =
                [](const std::string &, const ast::PlannedSourceSnapshot &) {
                  return true;
                },
            .failure = failure};
  }

  Storage storage;
  std::int64_t main_file = -1;
  ast::OwnedHeaderRoutePlan plan;
};

TEST_CASE("FactBatchWriter publishes the golden phase contract set-wise") {
  constexpr std::array expected{
      storage::FactBatchWriterPhase::validate_plan,
      storage::FactBatchWriterPhase::resolve_file_rows,
      storage::FactBatchWriterPhase::load_temporary_staging,
      storage::FactBatchWriterPhase::resolve_natural_keys,
      storage::FactBatchWriterPhase::apply_entities,
      storage::FactBatchWriterPhase::apply_annotations,
      storage::FactBatchWriterPhase::apply_relations,
      storage::FactBatchWriterPhase::apply_sites_and_external_identities,
      storage::FactBatchWriterPhase::publish_includes_and_applicability,
      storage::FactBatchWriterPhase::cleanup_stale_facts,
      storage::FactBatchWriterPhase::revalidate_sources,
      storage::FactBatchWriterPhase::mark_current,
      storage::FactBatchWriterPhase::commit};
  CHECK(storage::kFactBatchWriterPhaseOrder == expected);

  Fixture fixture;
  storage::FactBatchWriter writer(fixture.storage);
  const auto result = writer.apply(fixture.batch(), fixture.context());
  INFO(result.error.value_or(""));
  REQUIRE(result.ok());
  CHECK(result.report.commit_attempted);
  CHECK(result.report.statements_reused > 0);
  CHECK(result.report.statements_eliminated > 0);
  CHECK(result.report.families.at(ast::FactFamily::symbols).staged == 3);
  CHECK(result.symbol_ids.size() == 3);
  CHECK(result.relation_ids.size() == 1);
  CHECK(result.type_ids.size() == 1);
  CHECK(result.definition_ids.size() == 1);
  CHECK(fixture.storage.lookup_symbols_by_usr("usr-main").size() == 1);
  const auto external = fixture.storage.lookup_symbols_by_usr("usr-external");
  REQUIRE(external.size() == 1);
  CHECK(external.front().decl_path == "/usr/include/external.hpp");
  CHECK_FALSE(external.front().decl_file_id.has_value());
  auto named = fixture.storage.raw_db().prepare(
      "SELECT is_named_instance FROM symbol WHERE usr='usr-external'");
  REQUIRE(named.step());
  CHECK(named.col_int64(0) == 1);
  CHECK(fixture.storage.edge_count() == 1);
  auto include = fixture.storage.raw_db().prepare(
      "SELECT target.name FROM include_edge edge JOIN file target ON "
      "target.id=edge.dst_file_id WHERE edge.dst_path LIKE '%/header.hpp'");
  REQUIRE(include.step());
  CHECK(include.col_text(0) == "header.hpp");

  auto temporary = fixture.storage.raw_db().prepare(
      "SELECT COUNT(*) FROM sqlite_temp_schema WHERE name LIKE 'cidx_batch_%'");
  REQUIRE(temporary.step());
  CHECK(temporary.col_int64(0) == 13);
  auto persisted = fixture.storage.raw_db().prepare(
      "SELECT COUNT(*) FROM sqlite_schema WHERE name LIKE 'cidx_batch_%'");
  REQUIRE(persisted.step());
  CHECK(persisted.col_int64(0) == 0);
}

TEST_CASE("FactBatchWriter rolls back every main-table effect and retries") {
  Fixture fixture;
  storage::FactBatchWriter writer(fixture.storage);
  const ast::FactBatch batch = fixture.batch();
  const auto failed = writer.apply(
      batch,
      fixture.context(storage::FactBatchWriterFailurePoint::before_commit));
  CHECK_FALSE(failed.ok());
  CHECK(fixture.storage.lookup_symbols_by_usr("usr-main").empty());
  CHECK(fixture.storage.edge_count() == 0);

  const auto retried = writer.apply(batch, fixture.context());
  INFO(retried.error.value_or(""));
  CHECK(retried.ok());
  CHECK(fixture.storage.lookup_symbols_by_usr("usr-main").size() == 1);
  CHECK(fixture.storage.edge_count() == 1);
}

} // namespace
