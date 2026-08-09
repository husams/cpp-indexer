#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ast/fact_batch.hpp"
#include "ast/owned_header_plan.hpp"
#include "storage/fact_batch_writer.hpp"
#include "storage/storage.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
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

  // additions=true appends one symbol, one edge and one definition to the
  // owned header so a republication genuinely creates new persistent ids.
  auto batch(bool additions = false) const -> ast::FactBatch {
    ast::FactBatchRecorder recorder("fact-batch-writer-test");
    const auto route = plan.serial_route("/tmp/cidx-s073-writer/main.cpp");
    REQUIRE(route.partitions.size() == 2);
    const auto &main = route.partitions[route.main_partition];
    const auto &header = route.partitions[1];
    const std::int64_t main_handle = main.transient_file_handle.value_or(-1);
    const std::int64_t header_handle =
        header.transient_file_handle.value_or(-1);
    REQUIRE(main_handle >= 0);
    REQUIRE(header_handle >= 0);
    recorder.set_partition(main.partition, main.transient_file_handle);
    recorder.emit(symbol(main.partition, "usr-main", "main"));
    const std::int64_t main_symbol =
        recorder.lookup_symbol_id("usr-main").value();
    recorder.set_partition(header.partition, header.transient_file_handle);
    recorder.emit(symbol(header.partition, "usr-header", "header"));
    const std::int64_t header_symbol =
        recorder.lookup_symbol_id("usr-header").value();
    if (additions) {
      ast::SymbolRecord added =
          symbol(header.partition, "usr-header-added", "headerAdded");
      added.line = 9;
      added.col = 1;
      added.end_line = 9;
      added.end_col = 12;
      recorder.emit(added);
      const std::int64_t added_symbol =
          recorder.lookup_symbol_id("usr-header-added").value();
      const std::int64_t added_relation = recorder.add_edge(
          {.src_id = header_symbol, .dst_id = added_symbol, .kind = 1});
      recorder.add_edge_site({.edge_id = added_relation,
                              .file_id = header_handle,
                              .line = 9,
                              .col = 5});
      static_cast<void>(recorder.get_or_create_definition(
          added_symbol, header_handle, 9, 1, 9, 12, std::nullopt));
    }
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
                            .file_id = main_handle,
                            .line = 4,
                            .col = 3,
                            .recv_src_kind = "local",
                            .recv_decl_usr = "external-local"});
    const std::int64_t type = recorder.intern_type_node(
        {.type_key = "type:int", .spelling = "int", .kind = 1});
    recorder.add_symbol_type(main_symbol, 1, type);
    static_cast<void>(recorder.get_or_create_definition(
        main_symbol, main_handle, 1, 1, 1, 8, std::nullopt));
    static_cast<void>(recorder.get_or_create_definition(
        header_symbol, header_handle, 1, 1, 1, 8, std::nullopt));
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

auto measured_context(const Fixture &fixture)
    -> storage::FactBatchPublicationContext {
  auto context = fixture.context();
  TranslationUnitConfig configuration;
  configuration.descriptor_hash = "writer-config";
  configuration.descriptor_json = "{}";
  context.configuration = std::move(configuration);
  context.measure_statements = true;
  return context;
}

auto queryable_fact_projection(Fixture &fixture) -> std::vector<std::string> {
  auto query = fixture.storage.raw_db().prepare(
      "SELECT value FROM ("
      "SELECT 'symbol|'||usr||'|'||spelling||'|'||kind||'|'||"
      "is_definition AS value FROM symbol UNION ALL "
      "SELECT 'edge|'||src.usr||'|'||dst.usr||'|'||edge.kind||'|'||"
      "edge.count FROM edge JOIN symbol src ON src.id=edge.src_id JOIN symbol "
      "dst ON dst.id=edge.dst_id UNION ALL "
      "SELECT 'definition|'||symbol.usr||'|'||file.name||'|'||definition.line||"
      "'|'||definition.col FROM definition JOIN symbol ON "
      "symbol.id=definition.symbol_id JOIN file ON file.id=definition.file_id "
      "UNION ALL SELECT 'symbol_type|'||symbol.usr||'|'||type_node.type_key||"
      "'|'||symbol_type.kind FROM symbol_type JOIN symbol ON "
      "symbol.id=symbol_type.symbol_id JOIN type_node ON "
      "type_node.id=symbol_type.type_id UNION ALL "
      "SELECT 'include|'||source.name||'|'||include_edge.dst_path||'|'||"
      "include_edge.count FROM include_edge JOIN file source ON "
      "source.id=include_edge.src_file_id) ORDER BY value");
  std::vector<std::string> rows;
  while (query.step()) {
    rows.push_back(query.col_text(0));
  }
  return rows;
}

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
  CHECK(result.report.families.at(ast::FactFamily::symbols).staged == 3);
  CHECK(result.symbol_ids.size() == 3);
  CHECK(result.relation_ids.size() == 1);
  CHECK(result.type_ids.size() == 1);
  CHECK(result.definition_ids.size() == 2);
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
  CHECK(temporary.col_int64(0) == 15);
  auto family_index = fixture.storage.raw_db().prepare(
      "SELECT COUNT(*) FROM sqlite_temp_schema WHERE "
      "name='cidx_batch_aux_family_batch' AND type='index'");
  REQUIRE(family_index.step());
  CHECK(family_index.col_int64(0) == 1);
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

TEST_CASE("FactBatchWriter preserves monotonic instantiation identity facts") {
  Fixture fixture;
  const auto route =
      fixture.plan.serial_route("/tmp/cidx-s073-writer/main.cpp");
  REQUIRE(route.partitions.size() == 2);
  const auto &main = route.partitions[route.main_partition];
  ast::FactBatchRecorder recorder("fact-batch-instantiation-test");
  recorder.set_partition(main.partition, main.transient_file_handle);
  ast::SymbolRecord instantiated =
      symbol(main.partition, "usr-instantiation", "instantiation");
  instantiated.is_instantiation = true;
  recorder.emit(instantiated);
  instantiated.is_instantiation = false;
  instantiated.line = 0;
  instantiated.col = 0;
  instantiated.end_line = 0;
  instantiated.end_col = 0;
  instantiated.is_definition = false;
  instantiated.resolved = false;
  recorder.emit(instantiated);

  storage::FactBatchWriter writer(fixture.storage);
  const auto result =
      writer.apply(recorder.canonical_batch(), fixture.context());
  INFO(result.error.value_or(""));
  REQUIRE(result.ok());
  auto stored = fixture.storage.raw_db().prepare(
      "SELECT is_instantiation FROM symbol WHERE usr='usr-instantiation'");
  REQUIRE(stored.step());
  CHECK(stored.col_int64(0) == 1);
}

TEST_CASE("FactBatchWriter lets authored definitions classify specialization") {
  Fixture fixture;
  const auto route =
      fixture.plan.serial_route("/tmp/cidx-s073-writer/main.cpp");
  REQUIRE(route.partitions.size() == 2);
  const auto &main = route.partitions[route.main_partition];
  ast::FactBatchRecorder recorder("fact-batch-specialization-test");
  recorder.set_partition(main.partition, main.transient_file_handle);
  ast::SymbolRecord stub =
      symbol(main.partition, "usr-specialization", "specialization");
  stub.line = 0;
  stub.col = 0;
  stub.end_line = 0;
  stub.end_col = 0;
  stub.is_definition = false;
  stub.resolved = false;
  stub.is_instantiation = true;
  stub.template_form = "explicit-instantiation";
  recorder.emit(stub);
  ast::SymbolRecord authored = stub;
  authored.line = 1;
  authored.col = 1;
  authored.end_line = 1;
  authored.end_col = 8;
  authored.is_definition = true;
  authored.resolved = true;
  authored.is_instantiation = false;
  authored.template_form = "explicit-specialization";
  recorder.emit(authored);
  recorder.emit(stub);

  storage::FactBatchWriter writer(fixture.storage);
  const auto result =
      writer.apply(recorder.canonical_batch(), fixture.context());
  INFO(result.error.value_or(""));
  REQUIRE(result.ok());
  auto stored = fixture.storage.raw_db().prepare(
      "SELECT is_instantiation FROM symbol WHERE usr='usr-specialization'");
  REQUIRE(stored.step());
  CHECK(stored.col_int64(0) == 0);
}

TEST_CASE(
    "FactBatchWriter reports observed insert update and ignore outcomes") {
  Fixture fixture;
  storage::FactBatchWriter writer(fixture.storage);
  const ast::FactBatch batch = fixture.batch();
  const auto first = writer.apply(batch, fixture.context());
  INFO(first.error.value_or(""));
  REQUIRE(first.ok());
  const auto second = writer.apply(batch, fixture.context());
  INFO(second.error.value_or(""));
  REQUIRE(second.ok());

  for (const auto &[family, rows] : second.report.families) {
    CAPTURE(static_cast<int>(family));
    CHECK(rows.inserted + rows.updated + rows.ignored == rows.staged);
  }
  CHECK(second.report.families.at(ast::FactFamily::symbols).updated > 0);
  CHECK(second.report.families.at(ast::FactFamily::types).updated > 0);
  CHECK(second.report.families.at(ast::FactFamily::definitions).updated > 0);
  CHECK(second.report.families.at(ast::FactFamily::relations).updated > 0);
  CHECK(second.report.families.at(ast::FactFamily::symbol_types).updated > 0);
  CHECK(second.report.families.at(ast::FactFamily::edge_sites).ignored > 0);
  CHECK(second.report.families.at(ast::FactFamily::diagnostics).ignored > 0);
}

TEST_CASE(
    "FactBatchWriter classifies fresh duplicate existing and mixed rows") {
  const auto totals = [](const storage::FactBatchWriterResult &result,
                         auto member) {
    std::uint64_t total = 0;
    for (const auto &[family, rows] : result.report.families) {
      static_cast<void>(family);
      total += rows.*member;
    }
    return total;
  };

  Fixture fixture;
  storage::FactBatchWriter writer(fixture.storage);
  const auto context = measured_context(fixture);
  const auto fresh = writer.apply(fixture.batch(), context);
  INFO(fresh.error.value_or(""));
  REQUIRE(fresh.ok());
  CHECK(totals(fresh, &storage::FactBatchWriterRows::inserted) > 0);
  CHECK(totals(fresh, &storage::FactBatchWriterRows::updated) == 0);
  CHECK(totals(fresh, &storage::FactBatchWriterRows::coalesced) > 0);

  const auto existing = writer.apply(fixture.batch(), context);
  INFO(existing.error.value_or(""));
  REQUIRE(existing.ok());
  CHECK(totals(existing, &storage::FactBatchWriterRows::updated) > 0);

  const auto mixed = writer.apply(fixture.batch(true), context);
  INFO(mixed.error.value_or(""));
  REQUIRE(mixed.ok());
  CHECK(totals(mixed, &storage::FactBatchWriterRows::inserted) > 0);
  CHECK(totals(mixed, &storage::FactBatchWriterRows::updated) > 0);
  CHECK(mixed.report.transactions_started == 1);
  CHECK(mixed.report.temporary_tables_checked == 15);
  CHECK_FALSE(mixed.report.phase_seconds.empty());
}

TEST_CASE(
    "FactBatchWriter fresh and incremental publications query identically") {
  Fixture fresh_fixture;
  storage::FactBatchWriter fresh_writer(fresh_fixture.storage);
  const auto fresh = fresh_writer.apply(fresh_fixture.batch(true),
                                        measured_context(fresh_fixture));
  INFO(fresh.error.value_or(""));
  REQUIRE(fresh.ok());

  Fixture incremental_fixture;
  storage::FactBatchWriter incremental_writer(incremental_fixture.storage);
  const auto initial = incremental_writer.apply(
      incremental_fixture.batch(), measured_context(incremental_fixture));
  INFO(initial.error.value_or(""));
  REQUIRE(initial.ok());
  const auto incremental = incremental_writer.apply(
      incremental_fixture.batch(true), measured_context(incremental_fixture));
  INFO(incremental.error.value_or(""));
  REQUIRE(incremental.ok());

  CHECK(queryable_fact_projection(fresh_fixture) ==
        queryable_fact_projection(incremental_fixture));
}

TEST_CASE("FactBatchWriter captures incremental transform invalidation") {
  Fixture fixture;
  storage::FactBatchWriter writer(fixture.storage);
  const ast::FactBatch batch = fixture.batch();
  const auto initial = writer.apply(batch, fixture.context());
  INFO(initial.error.value_or(""));
  REQUIRE(initial.ok());
  REQUIRE(fixture.storage.run_transform_pipeline().complete);
  const auto clean_changes = fixture.storage.pending_transform_changes();
  CHECK(clean_changes.empty());

  const auto header_file =
      fixture.storage.get_file("/tmp/cidx-s073-writer/header.hpp");
  REQUIRE(header_file.has_value());
  std::int64_t header_file_id = -1;
  if (header_file) {
    header_file_id = header_file->id;
  }
  REQUIRE(header_file_id >= 0);
  auto header_symbol = fixture.storage.raw_db().prepare(
      "SELECT id FROM symbol WHERE usr='usr-header'");
  REQUIRE(header_symbol.step());
  const std::int64_t header_symbol_id = header_symbol.col_int64(0);
  auto header_edge = fixture.storage.raw_db().prepare(
      "SELECT id FROM edge WHERE src_id=? OR dst_id=? ORDER BY id LIMIT 1");
  header_edge.bind(1, header_symbol_id);
  header_edge.bind(2, header_symbol_id);
  REQUIRE(header_edge.step());
  const std::int64_t header_edge_id = header_edge.col_int64(0);
  auto header_definition = fixture.storage.raw_db().prepare(
      "SELECT id FROM definition WHERE file_id=? ORDER BY id LIMIT 1");
  header_definition.bind(1, header_file_id);
  REQUIRE(header_definition.step());
  const std::int64_t header_definition_id = header_definition.col_int64(0);

  const auto replacement = writer.apply(batch, fixture.context());
  INFO(replacement.error.value_or(""));
  REQUIRE(replacement.ok());
  const auto changes = fixture.storage.pending_transform_changes();
  CHECK(changes.generation > clean_changes.generation);
  CHECK(std::ranges::find(changes.file_ids, fixture.main_file) !=
        changes.file_ids.end());
  CHECK(std::ranges::find(changes.file_ids, header_file_id) !=
        changes.file_ids.end());
  CHECK(std::ranges::find(changes.symbol_ids, header_symbol_id) !=
        changes.symbol_ids.end());
  CHECK(std::ranges::find(changes.edge_ids, header_edge_id) !=
        changes.edge_ids.end());
  CHECK(std::ranges::find(changes.definition_ids, header_definition_id) !=
        changes.definition_ids.end());
}

TEST_CASE(
    "FactBatchWriter records newly published facts as transform changes") {
  Fixture fixture;
  storage::FactBatchWriter writer(fixture.storage);
  auto context = fixture.context();
  context.configuration = TranslationUnitConfig{
      .descriptor_hash = "writer-config", .descriptor_json = "{}"};
  const auto initial = writer.apply(fixture.batch(), context);
  INFO(initial.error.value_or(""));
  REQUIRE(initial.ok());
  REQUIRE(initial.configuration_id >= 0);
  REQUIRE(fixture.storage.run_transform_pipeline().complete);
  REQUIRE(fixture.storage.pending_transform_changes().empty());

  // The republication adds facts that do not exist yet, so the pre-cleanup
  // capture of prior identities cannot possibly name their ids. They can only
  // reach the change set from the publication path.
  const auto republished = writer.apply(fixture.batch(true), context);
  INFO(republished.error.value_or(""));
  REQUIRE(republished.ok());

  auto added_symbol = fixture.storage.raw_db().prepare(
      "SELECT id FROM symbol WHERE usr='usr-header-added'");
  REQUIRE(added_symbol.step());
  const std::int64_t added_symbol_id = added_symbol.col_int64(0);
  auto added_edge =
      fixture.storage.raw_db().prepare("SELECT id FROM edge WHERE dst_id=?");
  added_edge.bind(1, added_symbol_id);
  REQUIRE(added_edge.step());
  const std::int64_t added_edge_id = added_edge.col_int64(0);
  auto added_definition = fixture.storage.raw_db().prepare(
      "SELECT id FROM definition WHERE symbol_id=?");
  added_definition.bind(1, added_symbol_id);
  REQUIRE(added_definition.step());
  const std::int64_t added_definition_id = added_definition.col_int64(0);

  const auto changes = fixture.storage.pending_transform_changes();
  CAPTURE(added_symbol_id);
  CAPTURE(added_edge_id);
  CAPTURE(added_definition_id);
  CHECK(std::ranges::find(changes.symbol_ids, added_symbol_id) !=
        changes.symbol_ids.end());
  CHECK(std::ranges::find(changes.edge_ids, added_edge_id) !=
        changes.edge_ids.end());
  CHECK(std::ranges::find(changes.definition_ids, added_definition_id) !=
        changes.definition_ids.end());
}

// Split out from C-1499 as C-1529: a compact matrix over the symbol upsert's
// three merge-rule classes, one representative per class, published in both
// orders across republications. This locks the semantics the deleted
// storage_sink_test.cpp field matrices (:204, :327, :439) covered without
// restoring the 844-line suite.
struct MergeFixture : Fixture {
  // Publishes one main-partition symbol with `usr-merge`, customised by the
  // caller, and returns the stored row. Same USR and partition on every call,
  // so every publication after the first takes the ON CONFLICT branch.
  template <typename Customise> auto publish(Customise customise) -> void {
    ast::FactBatchRecorder recorder("fact-batch-merge-test");
    const auto route = plan.serial_route("/tmp/cidx-s073-writer/main.cpp");
    const auto &main = route.partitions[route.main_partition];
    recorder.set_partition(main.partition, main.transient_file_handle);
    ast::SymbolRecord record = symbol(main.partition, "usr-merge", "merge");
    customise(record);
    recorder.emit(record);
    storage::FactBatchWriter writer(storage);
    const auto result = writer.apply(recorder.canonical_batch(), context());
    INFO(result.error.value_or(""));
    REQUIRE(result.ok());
  }

  auto column(const std::string &name) -> std::string {
    auto row = storage.raw_db().prepare("SELECT COALESCE(CAST(" + name +
                                        " AS TEXT),'<null>') FROM symbol "
                                        "WHERE usr='usr-merge'");
    REQUIRE(row.step());
    return row.col_text(0);
  }
};

TEST_CASE("FactBatchWriter merges COALESCE-preserved fields in both orders") {
  // Rich then poor: a later publication that knows nothing must not erase.
  {
    MergeFixture fixture;
    fixture.publish([](ast::SymbolRecord &record) {
      record.qual_name = "ns::merge";
      record.display_name = "merge()";
      record.type_info = "void ()";
      record.access = "public";
      record.parent_usr = "usr-parent";
      record.const_value = "42";
      record.callable_kind = "function";
      record.template_origin = "primary";
    });
    fixture.publish([](ast::SymbolRecord &record) {
      record.qual_name.reset();
      record.display_name.reset();
      record.type_info.reset();
      record.access.reset();
      record.parent_usr.reset();
      record.const_value.reset();
      record.callable_kind.reset();
      record.template_origin.reset();
    });
    CHECK(fixture.column("qual_name") == "ns::merge");
    CHECK(fixture.column("display_name") == "merge()");
    CHECK(fixture.column("type_info") == "void ()");
    CHECK(fixture.column("access") == "public");
    CHECK(fixture.column("parent_usr") == "usr-parent");
    CHECK(fixture.column("const_value") == "42");
    CHECK(fixture.column("callable_kind") == "function");
    CHECK(fixture.column("template_origin") == "primary");
  }
  // Poor then rich: a later publication that knows more must fill the gaps.
  {
    MergeFixture fixture;
    fixture.publish([](ast::SymbolRecord &record) {
      record.qual_name.reset();
      record.const_value.reset();
      record.callable_kind.reset();
    });
    CHECK(fixture.column("qual_name") == "<null>");
    fixture.publish([](ast::SymbolRecord &record) {
      record.qual_name = "ns::merge";
      record.const_value = "42";
      record.callable_kind = "function";
    });
    CHECK(fixture.column("qual_name") == "ns::merge");
    CHECK(fixture.column("const_value") == "42");
    CHECK(fixture.column("callable_kind") == "function");
  }
}

TEST_CASE(
    "FactBatchWriter keeps MAX-monotonic flags monotonic in both orders") {
  const auto flags = [](ast::SymbolRecord &record, bool on) {
    record.is_definition = on;
    record.is_pure = on;
    record.is_static = on;
    record.resolved = on;
    record.is_named_instance = on;
  };
  // Set then cleared: monotonic fields never fall back to 0.
  {
    MergeFixture fixture;
    fixture.publish([&](ast::SymbolRecord &record) { flags(record, true); });
    fixture.publish([&](ast::SymbolRecord &record) { flags(record, false); });
    for (const char *name : {"is_definition", "is_pure", "is_static",
                             "resolved", "is_named_instance"}) {
      CAPTURE(std::string(name));
      CHECK(fixture.column(name) == "1");
    }
  }
  // Cleared then set: they still rise.
  {
    MergeFixture fixture;
    fixture.publish([&](ast::SymbolRecord &record) { flags(record, false); });
    for (const char *name : {"is_pure", "is_static", "is_named_instance"}) {
      CAPTURE(std::string(name));
      CHECK(fixture.column(name) == "0");
    }
    fixture.publish([&](ast::SymbolRecord &record) { flags(record, true); });
    for (const char *name : {"is_definition", "is_pure", "is_static",
                             "resolved", "is_named_instance"}) {
      CAPTURE(std::string(name));
      CHECK(fixture.column(name) == "1");
    }
  }
}

TEST_CASE("FactBatchWriter ranks the location merge by definition and origin") {
  // A located definition is not displaced by a later location-less
  // declaration: the CASE requires excluded.file_id IS NOT NULL.
  {
    MergeFixture fixture;
    fixture.publish([](ast::SymbolRecord &record) {
      record.line = 11;
      record.col = 3;
      record.end_line = 11;
      record.end_col = 20;
      record.is_definition = true;
    });
    const std::string file_id = fixture.column("file_id");
    fixture.publish([](ast::SymbolRecord &record) {
      record.line = 0;
      record.col = 0;
      record.end_line = 0;
      record.end_col = 0;
      record.is_definition = false;
    });
    CHECK(fixture.column("line") == "11");
    CHECK(fixture.column("col") == "3");
    CHECK(fixture.column("end_line") == "11");
    CHECK(fixture.column("end_col") == "20");
    CHECK(fixture.column("file_id") == file_id);
  }
  // A located declaration IS displaced by a later located definition, because
  // excluded.is_definition >= symbol.is_definition.
  {
    MergeFixture fixture;
    fixture.publish([](ast::SymbolRecord &record) {
      record.line = 5;
      record.col = 1;
      record.is_definition = false;
    });
    CHECK(fixture.column("line") == "5");
    fixture.publish([](ast::SymbolRecord &record) {
      record.line = 11;
      record.col = 3;
      record.is_definition = true;
    });
    CHECK(fixture.column("line") == "11");
    CHECK(fixture.column("col") == "3");
  }
  // ...and a located declaration does NOT displace an existing definition's
  // location, because excluded.is_definition < symbol.is_definition.
  {
    MergeFixture fixture;
    fixture.publish([](ast::SymbolRecord &record) {
      record.line = 11;
      record.col = 3;
      record.is_definition = true;
    });
    fixture.publish([](ast::SymbolRecord &record) {
      record.line = 5;
      record.col = 1;
      record.is_definition = false;
    });
    CHECK(fixture.column("line") == "11");
    CHECK(fixture.column("col") == "3");
  }
}

TEST_CASE("FactBatchWriter merges declaration coordinates by direction") {
  // A location-less emission (excluded.file_id IS NULL) prefers the EXISTING
  // decl_* triple; a located one prefers its own. decl_path is
  // preserve-existing in both directions.
  {
    MergeFixture fixture;
    fixture.publish([](ast::SymbolRecord &record) {
      record.decl_line = 4;
      record.decl_col = 7;
      record.decl_path = "/tmp/cidx-s073-writer/first.hpp";
    });
    CHECK(fixture.column("decl_line") == "4");
    fixture.publish([](ast::SymbolRecord &record) {
      record.line = 0;
      record.col = 0;
      record.end_line = 0;
      record.end_col = 0;
      record.decl_line = 9;
      record.decl_col = 2;
      record.decl_path = "/tmp/cidx-s073-writer/second.hpp";
    });
    CHECK(fixture.column("decl_line") == "4");
    CHECK(fixture.column("decl_col") == "7");
    CHECK(fixture.column("decl_path") == "/tmp/cidx-s073-writer/first.hpp");
  }
  {
    MergeFixture fixture;
    fixture.publish([](ast::SymbolRecord &record) {
      record.decl_line = 4;
      record.decl_col = 7;
    });
    fixture.publish([](ast::SymbolRecord &record) {
      record.decl_line = 9;
      record.decl_col = 2;
    });
    CHECK(fixture.column("decl_line") == "9");
    CHECK(fixture.column("decl_col") == "2");
  }
}

// The other half of the change set: the ids a republication REMOVES. Those
// exist only before cleanup runs, so they can reach the change set only from
// the pre-cleanup capture. This locks the set-based capture that replaced the
// per-route SqliteStorageService::capture_transform_changes_for_file call, and
// asserts the capture is counted in the writer report rather than issued on
// raw prepare handles the report cannot see.
TEST_CASE("FactBatchWriter records removed prior facts as transform changes") {
  Fixture fixture;
  storage::FactBatchWriter writer(fixture.storage);
  auto context = fixture.context();
  context.configuration = TranslationUnitConfig{
      .descriptor_hash = "writer-config", .descriptor_json = "{}"};

  // Publish WITH the extra header symbol, edge and definition, then settle.
  const auto initial = writer.apply(fixture.batch(true), context);
  INFO(initial.error.value_or(""));
  REQUIRE(initial.ok());
  REQUIRE(fixture.storage.run_transform_pipeline().complete);
  REQUIRE(fixture.storage.pending_transform_changes().empty());

  auto prior_symbol = fixture.storage.raw_db().prepare(
      "SELECT id FROM symbol WHERE usr='usr-header-added'");
  REQUIRE(prior_symbol.step());
  const std::int64_t prior_symbol_id = prior_symbol.col_int64(0);
  auto prior_edge =
      fixture.storage.raw_db().prepare("SELECT id FROM edge WHERE dst_id=?");
  prior_edge.bind(1, prior_symbol_id);
  REQUIRE(prior_edge.step());
  const std::int64_t prior_edge_id = prior_edge.col_int64(0);
  auto prior_definition = fixture.storage.raw_db().prepare(
      "SELECT id FROM definition WHERE symbol_id=?");
  prior_definition.bind(1, prior_symbol_id);
  REQUIRE(prior_definition.step());
  const std::int64_t prior_definition_id = prior_definition.col_int64(0);

  // Republish WITHOUT them. Nothing the publication path sees names these ids.
  const auto republished = writer.apply(fixture.batch(), context);
  INFO(republished.error.value_or(""));
  REQUIRE(republished.ok());

  const auto changes = fixture.storage.pending_transform_changes();
  CAPTURE(prior_symbol_id);
  CAPTURE(prior_edge_id);
  CAPTURE(prior_definition_id);
  CHECK(std::ranges::find(changes.symbol_ids, prior_symbol_id) !=
        changes.symbol_ids.end());
  CHECK(std::ranges::find(changes.edge_ids, prior_edge_id) !=
        changes.edge_ids.end());
  CHECK(std::ranges::find(changes.definition_ids, prior_definition_id) !=
        changes.definition_ids.end());
}

// AC #1701: the change-set recording must not scale the number of DISTINCT
// SQL statements with the number of facts. Both halves -- the pre-cleanup
// capture and the post-publication record -- are set-based, so publishing a
// batch with three extra facts compiles exactly as many statements as
// publishing without them.
//
// statements_prepared is the right measure and the other two are not:
// statements_reused counts per-row rebinds of the staging loaders (one step
// per staged row on a single compiled statement) and statement_executions
// counts those steps, so both scale with rows by design. The row-at-a-time
// pattern this story removes would show up here as extra PREPARES.
TEST_CASE("FactBatchWriter change-set recording is constant in fact count") {
  const auto statements = [](bool additions) {
    Fixture fixture;
    storage::FactBatchWriter writer(fixture.storage);
    auto context = fixture.context();
    context.configuration = TranslationUnitConfig{
        .descriptor_hash = "writer-config", .descriptor_json = "{}"};
    const auto initial = writer.apply(fixture.batch(additions), context);
    REQUIRE(initial.ok());
    REQUIRE(fixture.storage.run_transform_pipeline().complete);
    // The republication runs both halves against an already-populated file.
    const auto republished = writer.apply(fixture.batch(additions), context);
    REQUIRE(republished.ok());
    return republished.report.statements_prepared;
  };
  CHECK(statements(true) == statements(false));
}

// PERF-002.6h: every declared failure point must roll the publication back
// completely, not only the one the e2e sweep happens to reach.
TEST_CASE("FactBatchWriter rolls back at every declared failure point") {
  constexpr std::array points{
      storage::FactBatchWriterFailurePoint::temporary_load,
      storage::FactBatchWriterFailurePoint::natural_key_resolution,
      storage::FactBatchWriterFailurePoint::entity_apply,
      storage::FactBatchWriterFailurePoint::annotation_apply,
      storage::FactBatchWriterFailurePoint::relation_apply,
      storage::FactBatchWriterFailurePoint::site_apply,
      storage::FactBatchWriterFailurePoint::publication,
      storage::FactBatchWriterFailurePoint::cleanup,
      storage::FactBatchWriterFailurePoint::before_commit,
      storage::FactBatchWriterFailurePoint::commit};
  for (const auto point : points) {
    CAPTURE(static_cast<int>(point));
    Fixture fixture;
    storage::FactBatchWriter writer(fixture.storage);
    const ast::FactBatch batch = fixture.batch();
    const auto failed = writer.apply(batch, fixture.context(point));
    CHECK_FALSE(failed.ok());
    CHECK_FALSE(failed.report.committed);
    // No main-table effect survives, and the connection-local staging tables
    // are left clean for the retry.
    CHECK(fixture.storage.lookup_symbols_by_usr("usr-main").empty());
    CHECK(fixture.storage.lookup_symbols_by_usr("usr-header").empty());
    CHECK(fixture.storage.edge_count() == 0);
    auto definitions =
        fixture.storage.raw_db().prepare("SELECT COUNT(*) FROM definition");
    REQUIRE(definitions.step());
    CHECK(definitions.col_int64(0) == 0);
    auto applicability = fixture.storage.raw_db().prepare(
        "SELECT COUNT(*) FROM fact_applicability");
    REQUIRE(applicability.step());
    CHECK(applicability.col_int64(0) == 0);

    // The same writer instance publishes cleanly straight afterwards.
    const auto retried = writer.apply(batch, fixture.context());
    INFO(retried.error.value_or(""));
    REQUIRE(retried.ok());
    CHECK(fixture.storage.lookup_symbols_by_usr("usr-main").size() == 1);
    CHECK(fixture.storage.edge_count() == 1);
  }
}

// Re-expresses the deleted tests/owned_header_plan_test.cpp "stale plans are
// rejected before lifecycle mutation" case against FactBatchWriter, which now
// owns the guard.
TEST_CASE("FactBatchWriter rejects a stale plan before any mutation") {
  Fixture fixture;
  storage::FactBatchWriter writer(fixture.storage);
  auto stale = fixture.context();
  stale.expected_generation = "generation-2";
  const auto rejected = writer.apply(fixture.batch(), stale);
  CHECK_FALSE(rejected.ok());
  REQUIRE(rejected.error.has_value());
  CHECK(rejected.error->find("generation is stale") != std::string::npos);
  CHECK(fixture.storage.lookup_symbols_by_usr("usr-main").empty());
  CHECK(fixture.storage.edge_count() == 0);
  CHECK_FALSE(
      fixture.storage.get_file("/tmp/cidx-s073-writer/header.hpp").has_value());
}

// A source that moved out from under the plan must be rejected the same way.
TEST_CASE("FactBatchWriter rejects a plan whose source is no longer current") {
  Fixture fixture;
  storage::FactBatchWriter writer(fixture.storage);
  auto moved = fixture.context();
  moved.source_is_current = [](const std::string &,
                               const ast::PlannedSourceSnapshot &) {
    return false;
  };
  const auto rejected = writer.apply(fixture.batch(), moved);
  CHECK_FALSE(rejected.ok());
  CHECK(fixture.storage.lookup_symbols_by_usr("usr-main").empty());
  CHECK(fixture.storage.edge_count() == 0);
}

// Re-expresses the coverage the deleted
// tests/storage_identity_scope_test.cpp "v39 carries translation-unit identity
// through header sinks" case and the deleted tests/storage_sink_test.cpp
// duplicate-bucket cases provided, against FactBatchWriter instead of the
// removed sinks. Both are acceptance evidence: AC #1702 (deterministic natural
// key resolution across duplicate USRs, local identities, multiple semantic
// universes and repeated declarations) and AC #1703 (per-file/configuration
// partitions stay intact through staging, cleanup and applicability).
struct MultiConfigFixture {
  MultiConfigFixture() {
    universe = storage.add_semantic_universe("workspace", "workspace");
    const std::int64_t component =
        storage.add_component("writer", "/tmp/cidx-s073-writer", "repo");
    storage.set_component_semantic_universe(component, universe);
    a_file = storage.add_file_path("/tmp/cidx-s073-writer/a.cpp");
    b_file = storage.add_file_path("/tmp/cidx-s073-writer/b.cpp");
  }

  // One TU, one owned header, under a named normalized configuration.
  static auto tu_partition(const std::string &tu, const std::string &config)
      -> ast::FactPartitionKey {
    ast::FactPartitionKey key = partition(tu, "/tmp/cidx-s073-writer/" + tu);
    key.configuration.translation_unit = "/tmp/cidx-s073-writer/" + tu;
    key.configuration.normalized_configuration = config;
    return key;
  }

  static auto header_partition(const std::string &tu, const std::string &config)
      -> ast::FactPartitionKey {
    ast::FactPartitionKey key =
        partition("shared.hpp", "/tmp/cidx-s073-writer/shared.hpp");
    key.configuration.translation_unit = "/tmp/cidx-s073-writer/" + tu;
    key.configuration.normalized_configuration = config;
    return key;
  }

  auto plan_for(const std::string &tu, const std::string &config,
                std::int64_t tu_file) const -> ast::OwnedHeaderRoutePlan {
    const auto main = tu_partition(tu, config);
    const auto header = header_partition(tu, config);
    return ast::plan_owned_header_routes(
        "generation-1",
        {{.role = ast::PlannedFileRole::translation_unit,
          .translation_unit = main.configuration.translation_unit,
          .translation_unit_file_id = tu_file,
          .path = main.file.portable_path(),
          .existing_file_id = tu_file,
          .snapshot = {.md5 = tu + "-md5"},
          .cleanup_symbols = true,
          .partition = main},
         {.role = ast::PlannedFileRole::owned_header,
          .translation_unit = main.configuration.translation_unit,
          .translation_unit_file_id = tu_file,
          .path = header.file.portable_path(),
          .discovery_ordinal = 1,
          .snapshot = {.md5 = "shared-md5"},
          .cleanup_symbols = true,
          .partition = header}});
  }

  // The header-local symbol both configurations declare, plus (optionally) a
  // partition of the SAME header under a DIFFERENT configuration that this
  // publication does not route. That foreign partition must never be
  // published: it is the C-1490 regression seam.
  static auto batch_for(const ast::OwnedHeaderRoutePlan &plan,
                        const std::string &tu,
                        const std::optional<std::string> &foreign_config)
      -> ast::FactBatch {
    ast::FactBatchRecorder recorder("fact-batch-writer-multi-config-test");
    const auto route = plan.serial_route("/tmp/cidx-s073-writer/" + tu);
    REQUIRE(route.partitions.size() == 2);
    const auto &main = route.partitions[route.main_partition];
    const auto &header = route.partitions[1];
    recorder.set_partition(main.partition, main.transient_file_handle);
    recorder.emit(symbol(main.partition, "usr-" + tu, tu));
    recorder.set_partition(header.partition, header.transient_file_handle);
    ast::SymbolRecord local =
        symbol(header.partition, "c:@F@header_local", "header_local");
    local.linkage = "internal";
    recorder.emit(local);
    // A repeated declaration of the same USR in the same partition must
    // collapse onto the first-seen identity, not mint a second row.
    recorder.emit(local);
    if (foreign_config) {
      const auto foreign = header_partition(tu, *foreign_config);
      recorder.set_partition(foreign, header.transient_file_handle);
      ast::SymbolRecord shadow =
          symbol(foreign, "c:@F@header_local", "header_local");
      shadow.linkage = "internal";
      shadow.spelling = "foreign_shadow";
      recorder.emit(shadow);
    }
    return recorder.canonical_batch();
  }

  static auto context_for(const ast::OwnedHeaderRoutePlan &plan,
                          const std::string &tu,
                          const cidx::TranslationUnitConfig &config)
      -> storage::FactBatchPublicationContext {
    return {.route_plan = plan,
            .translation_unit = "/tmp/cidx-s073-writer/" + tu,
            .expected_generation = "generation-1",
            .source_is_current =
                [](const std::string &, const ast::PlannedSourceSnapshot &) {
                  return true;
                },
            .configuration = config};
  }

  static auto tu_config(const std::string &macro)
      -> cidx::TranslationUnitConfig {
    cidx::TranslationUnitConfig config;
    config.driver = "clang++";
    config.working_dir = "/workspace";
    config.language = "c++";
    config.standard = "c++23";
    config.arguments = {"-std=c++23", "-D" + macro};
    config.macro_state = {macro};
    return config;
  }

  Storage storage;
  std::int64_t universe = -1;
  std::int64_t a_file = -1;
  std::int64_t b_file = -1;
};

TEST_CASE("FactBatchWriter keeps header identity separate per configuration") {
  MultiConfigFixture fixture;
  storage::FactBatchWriter writer(fixture.storage);

  const auto plan_a = fixture.plan_for("a.cpp", "config-a", fixture.a_file);
  const auto result_a = writer.apply(
      MultiConfigFixture::batch_for(plan_a, "a.cpp", std::nullopt),
      MultiConfigFixture::context_for(
          plan_a, "a.cpp", MultiConfigFixture::tu_config("CONFIG_A")));
  INFO(result_a.error.value_or(""));
  REQUIRE(result_a.ok());

  const auto plan_b = fixture.plan_for("b.cpp", "config-b", fixture.b_file);
  const auto result_b = writer.apply(
      MultiConfigFixture::batch_for(plan_b, "b.cpp", std::nullopt),
      MultiConfigFixture::context_for(
          plan_b, "b.cpp", MultiConfigFixture::tu_config("CONFIG_B")));
  INFO(result_b.error.value_or(""));
  REQUIRE(result_b.ok());

  // Same header-local USR, two translation-unit configurations: two rows with
  // distinct identity keys, exactly as the deleted v39 sink case asserted.
  const auto rows = fixture.storage.lookup_symbols_by_usr("c:@F@header_local",
                                                          fixture.universe);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].identity_key != rows[1].identity_key);
  CHECK_FALSE(rows[0].identity_key.empty());

  // Each row is applicable only under the configuration that published it.
  auto scoped = fixture.storage.raw_db().prepare(
      "SELECT COUNT(DISTINCT fa.config_id) FROM fact_applicability fa "
      "JOIN symbol s ON s.id=fa.fact_id WHERE fa.fact_kind='symbol' "
      "AND s.usr='c:@F@header_local'");
  REQUIRE(scoped.step());
  CHECK(scoped.col_int64(0) == 2);
  auto per_config = fixture.storage.raw_db().prepare(
      "SELECT fa.config_id,COUNT(*) FROM fact_applicability fa "
      "JOIN symbol s ON s.id=fa.fact_id WHERE fa.fact_kind='symbol' "
      "AND s.usr='c:@F@header_local' GROUP BY fa.config_id");
  while (per_config.step()) {
    CAPTURE(per_config.col_int64(0));
    CHECK(per_config.col_int64(1) == 1);
  }
}

TEST_CASE("FactBatchWriter never publishes an unrouted configuration's "
          "partition of a planned file") {
  MultiConfigFixture fixture;
  storage::FactBatchWriter writer(fixture.storage);

  // The batch carries a partition of shared.hpp under config-b while the
  // publication routes only config-a. Matching the planned flag on the file
  // path alone marks the foreign partition planned and upserts+publishes it.
  const auto plan_a = fixture.plan_for("a.cpp", "config-a", fixture.a_file);
  const auto result = writer.apply(
      MultiConfigFixture::batch_for(plan_a, "a.cpp", "config-b"),
      MultiConfigFixture::context_for(
          plan_a, "a.cpp", MultiConfigFixture::tu_config("CONFIG_A")));
  INFO(result.error.value_or(""));
  REQUIRE(result.ok());

  auto shadow = fixture.storage.raw_db().prepare(
      "SELECT COUNT(*) FROM symbol WHERE spelling='foreign_shadow'");
  REQUIRE(shadow.step());
  CHECK(shadow.col_int64(0) == 0);

  auto published = fixture.storage.raw_db().prepare(
      "SELECT COUNT(*) FROM fact_applicability fa JOIN symbol s "
      "ON s.id=fa.fact_id WHERE fa.fact_kind='symbol' AND "
      "s.usr='c:@F@header_local'");
  REQUIRE(published.step());
  CHECK(published.col_int64(0) == 1);
}

TEST_CASE(
    "FactBatchWriter suppresses duplicate declarations deterministically") {
  MultiConfigFixture fixture;
  storage::FactBatchWriter writer(fixture.storage);
  const auto plan = fixture.plan_for("a.cpp", "config-a", fixture.a_file);
  const auto config = MultiConfigFixture::tu_config("CONFIG_A");

  // The batch declares c:@F@header_local twice in the same partition.
  const auto first =
      writer.apply(MultiConfigFixture::batch_for(plan, "a.cpp", std::nullopt),
                   MultiConfigFixture::context_for(plan, "a.cpp", config));
  INFO(first.error.value_or(""));
  REQUIRE(first.ok());
  auto once = fixture.storage.raw_db().prepare(
      "SELECT COUNT(*) FROM symbol WHERE usr='c:@F@header_local'");
  REQUIRE(once.step());
  CHECK(once.col_int64(0) == 1);
  const auto first_id =
      fixture.storage
          .lookup_symbols_by_usr("c:@F@header_local", fixture.universe)
          .front()
          .id;

  // Republishing the identical batch must reuse the first-seen persistent id
  // rather than mint a second row.
  const auto second =
      writer.apply(MultiConfigFixture::batch_for(plan, "a.cpp", std::nullopt),
                   MultiConfigFixture::context_for(plan, "a.cpp", config));
  INFO(second.error.value_or(""));
  REQUIRE(second.ok());
  const auto after = fixture.storage.lookup_symbols_by_usr("c:@F@header_local",
                                                           fixture.universe);
  REQUIRE(after.size() == 1);
  CHECK(after.front().id == first_id);
}

struct IncludeDedupFixture {
  static constexpr std::size_t kTranslationUnits = 5;

  IncludeDedupFixture() {
    const std::int64_t universe =
        storage.add_semantic_universe("workspace", "workspace");
    const std::int64_t component =
        storage.add_component("include-dedup", root(), "repo");
    storage.set_component_semantic_universe(component, universe);
    shared_file = storage.add_file_path(root() + "/shared.hpp");
    leaf_file = storage.add_file_path(root() + "/leaf.hpp");
    for (std::size_t index = 0; index < kTranslationUnits; ++index) {
      tu_files.push_back(storage.add_file_path(tu_path(index)));
    }
  }

  [[nodiscard]] static auto root() -> std::string {
    return "/tmp/cidx-s116-include-dedup";
  }

  [[nodiscard]] static auto tu_path(std::size_t index) -> std::string {
    return root() + "/tu" + std::to_string(index) + ".cpp";
  }

  [[nodiscard]] static auto file_partition(std::string file, std::size_t index)
      -> ast::FactPartitionKey {
    ast::FactPartitionKey result;
    result.file.component_path = root();
    result.file.file_name = std::move(file);
    result.configuration.semantic_universe = "workspace";
    result.configuration.translation_unit = tu_path(index);
    result.configuration.normalized_configuration = "shared-config";
    result.configuration.identity_source = result.file.portable_path();
    return result;
  }

  [[nodiscard]] auto plan_for(std::size_t index, bool owns_shared) const
      -> ast::OwnedHeaderRoutePlan {
    const ast::FactPartitionKey main =
        file_partition("tu" + std::to_string(index) + ".cpp", index);
    ast::OwnedHeaderRouteCandidate main_route;
    main_route.role = ast::PlannedFileRole::translation_unit;
    main_route.translation_unit = tu_path(index);
    main_route.translation_unit_file_id = tu_files.at(index);
    main_route.path = main.file.portable_path();
    main_route.existing_file_id = tu_files.at(index);
    main_route.snapshot.md5 = "tu-" + std::to_string(index);
    main_route.cleanup_symbols = true;
    main_route.partition = main;
    std::vector<ast::OwnedHeaderRouteCandidate> routes;
    routes.push_back(std::move(main_route));
    if (owns_shared) {
      const ast::FactPartitionKey shared = file_partition("shared.hpp", index);
      ast::OwnedHeaderRouteCandidate header_route;
      header_route.role = ast::PlannedFileRole::owned_header;
      header_route.translation_unit = tu_path(index);
      header_route.translation_unit_file_id = tu_files.at(index);
      header_route.path = shared.file.portable_path();
      header_route.discovery_ordinal = 1;
      header_route.existing_file_id = shared_file;
      header_route.snapshot.md5 = "shared-md5";
      header_route.cleanup_symbols = true;
      header_route.partition = shared;
      routes.push_back(std::move(header_route));
    }
    return ast::plan_owned_header_routes("generation-1", std::move(routes));
  }

  [[nodiscard]] static auto batch_for(const ast::OwnedHeaderRoutePlan &plan,
                                      std::size_t index) -> ast::FactBatch {
    const ast::SerialFactRoute route = plan.serial_route(tu_path(index));
    REQUIRE_FALSE(route.partitions.empty());
    const ast::FactPartitionKey main =
        file_partition("tu" + std::to_string(index) + ".cpp", index);
    const ast::FactPartitionKey shared = file_partition("shared.hpp", index);
    const ast::FactPartitionKey leaf = file_partition("leaf.hpp", index);

    ast::FactBatchRecorder recorder("include-dedup-contract-test");
    recorder.set_partition(
        main, route.partitions.at(route.main_partition).transient_file_handle);
    recorder.set_partition(leaf);
    const auto planned_header = std::ranges::find_if(
        route.partitions, [&shared](const ast::FactRoutePartition &candidate) {
          return candidate.partition.file == shared.file;
        });
    recorder.set_partition(shared, planned_header == route.partitions.end()
                                       ? std::nullopt
                                       : planned_header->transient_file_handle);

    ast::IncludeDirectiveRecord shared_include;
    shared_include.partition = shared;
    shared_include.source = shared.file;
    shared_include.destination = leaf.file;
    shared_include.destination_path = leaf.file.portable_path();
    shared_include.spelling = "leaf.hpp";
    shared_include.line = 1;
    shared_include.col = 1;
    shared_include.begin_offset = 0;
    shared_include.end_offset = 20;
    shared_include.guarded = true;
    recorder.emit(shared_include);

    ast::MacroUseRecord shared_macro;
    shared_macro.partition = shared;
    shared_macro.source = shared.file;
    shared_macro.definition = leaf.file;
    shared_macro.definition_path = leaf.file.portable_path();
    shared_macro.name = "LEAF_MACRO";
    recorder.emit(shared_macro);

    ast::IncludeDirectiveRecord main_include;
    main_include.partition = main;
    main_include.source = main.file;
    main_include.destination = shared.file;
    main_include.destination_path = shared.file.portable_path();
    main_include.spelling = "shared.hpp";
    main_include.line = 1;
    main_include.col = 1;
    main_include.begin_offset = 0;
    main_include.end_offset = 22;
    main_include.guarded = true;
    recorder.emit(main_include);
    return recorder.canonical_batch();
  }

  [[nodiscard]] static auto context_for(const ast::OwnedHeaderRoutePlan &plan,
                                        std::size_t index)
      -> storage::FactBatchPublicationContext {
    cidx::TranslationUnitConfig configuration;
    configuration.driver = "clang++";
    configuration.working_dir = root();
    configuration.language = "c++";
    configuration.standard = "c++23";
    configuration.arguments = {"-std=c++23"};

    storage::FactBatchPublicationContext context;
    context.route_plan = plan;
    context.translation_unit = tu_path(index);
    context.expected_generation = "generation-1";
    context.source_is_current = [](const std::string &,
                                   const ast::PlannedSourceSnapshot &) {
      return true;
    };
    context.configuration = std::move(configuration);
    return context;
  }

  Storage storage;
  std::vector<std::int64_t> tu_files;
  std::int64_t shared_file = -1;
  std::int64_t leaf_file = -1;
};

TEST_CASE("FactBatchWriter persists shared include facts only for their "
          "claimed owner") {
  IncludeDedupFixture fixture;
  storage::FactBatchWriter writer(fixture.storage);
  std::int64_t normalized_config = -1;

  for (std::size_t index = 0; index < IncludeDedupFixture::kTranslationUnits;
       ++index) {
    const bool owns_shared = index == 0;
    const ast::OwnedHeaderRoutePlan plan = fixture.plan_for(index, owns_shared);
    const auto result =
        writer.apply(IncludeDedupFixture::batch_for(plan, index),
                     IncludeDedupFixture::context_for(plan, index));
    INFO(result.error.value_or(""));
    REQUIRE(result.ok());
    if (normalized_config < 0) {
      normalized_config = result.configuration_id;
    }
    CHECK(result.configuration_id == normalized_config);

    const auto &include_rows =
        result.report.families.at(ast::FactFamily::includes);
    const auto &macro_rows = result.report.families.at(ast::FactFamily::macros);
    CHECK(include_rows.inserted == (owns_shared ? 4 : 2));
    CHECK(macro_rows.inserted == (owns_shared ? 1 : 0));

    const auto configs =
        fixture.storage.include_configs_for_tu(fixture.tu_files.at(index));
    REQUIRE(configs.size() == 1);
    REQUIRE(configs.front().translation_unit_config_id.has_value());
    CHECK(configs.front().translation_unit_config_id.value_or(-1) ==
          normalized_config);
    const auto main_edges = fixture.storage.include_edges_from_config(
        fixture.tu_files.at(index), normalized_config, true);
    REQUIRE(main_edges.size() == 1);
    CHECK(main_edges.front().dst_path ==
          IncludeDedupFixture::root() + "/shared.hpp");
  }

  const auto shared_edges = fixture.storage.include_edges_from_config(
      fixture.shared_file, normalized_config, true);
  REQUIRE(shared_edges.size() == 1);
  CHECK(shared_edges.front().dst_path ==
        IncludeDedupFixture::root() + "/leaf.hpp");

  auto edge_owner = fixture.storage.raw_db().prepare(
      "SELECT c.tu_file_id,e.id FROM include_edge e JOIN include_config c "
      "ON c.id=e.config_id WHERE e.src_file_id=?");
  edge_owner.bind(1, fixture.shared_file);
  REQUIRE(edge_owner.step());
  CHECK(edge_owner.col_int64(0) == fixture.tu_files.front());
  const std::int64_t first_edge_id = edge_owner.col_int64(1);
  edge_owner.step_done();

  auto config_count =
      fixture.storage.raw_db().prepare("SELECT COUNT(*) FROM include_config");
  REQUIRE(config_count.step());
  CHECK(config_count.col_int64(0) ==
        static_cast<std::int64_t>(IncludeDedupFixture::kTranslationUnits));
  config_count.step_done();

  auto macro_count = fixture.storage.raw_db().prepare(
      "SELECT COUNT(*) FROM include_macro_use WHERE src_file_id=?");
  macro_count.bind(1, fixture.shared_file);
  REQUIRE(macro_count.step());
  CHECK(macro_count.col_int64(0) == 1);
  macro_count.step_done();

  const std::size_t repeat_index = IncludeDedupFixture::kTranslationUnits - 1;
  const ast::OwnedHeaderRoutePlan repeat_plan =
      fixture.plan_for(repeat_index, false);
  const auto repeated =
      writer.apply(IncludeDedupFixture::batch_for(repeat_plan, repeat_index),
                   IncludeDedupFixture::context_for(repeat_plan, repeat_index));
  INFO(repeated.error.value_or(""));
  REQUIRE(repeated.ok());
  CHECK(repeated.report.families.at(ast::FactFamily::includes).inserted == 2);
  CHECK(repeated.report.families.at(ast::FactFamily::macros).inserted == 0);

  auto shared_after = fixture.storage.raw_db().prepare(
      "SELECT id FROM include_edge WHERE src_file_id=?");
  shared_after.bind(1, fixture.shared_file);
  REQUIRE(shared_after.step());
  CHECK(shared_after.col_int64(0) == first_edge_id);
  shared_after.step_done();
}

} // namespace
