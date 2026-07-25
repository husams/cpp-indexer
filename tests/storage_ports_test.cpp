#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "storage/ports.hpp"
#include "storage/sqlite_adapters.hpp"
#include "storage/storage.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

using namespace cidx;
using namespace cidx::storage;

TEST_CASE("focused ports expose domain records without SQLite") {
  static_assert(std::is_abstract_v<WorkspaceCatalogReadPort>);
  static_assert(std::is_abstract_v<WorkspaceCatalogWritePort>);
  static_assert(std::is_abstract_v<SourceStoreReadPort>);
  static_assert(std::is_abstract_v<SymbolReadPort>);
  static_assert(std::is_abstract_v<SymbolWritePort>);
  static_assert(std::is_abstract_v<TypeReadPort>);
  static_assert(std::is_abstract_v<FactWritePort>);
  static_assert(std::is_abstract_v<DefinitionWritePort>);
  static_assert(std::is_abstract_v<IncludeReadPort>);
  static_assert(std::is_abstract_v<SchemaCatalogReadPort>);
  static_assert(std::is_abstract_v<UnitOfWork>);
}

TEST_CASE("SQLite adapters preserve domain records and separate writes") {
  Storage db(":memory:");
  SqliteWorkspaceCatalogAdapter catalog(db);
  SqliteSourceStoreAdapter source(db);
  SqliteSymbolStoreAdapter symbols(db);

  const ComponentWriteRecord component{"ports", "/tmp/ports", "repo", "v1"};
  const auto component_id = catalog.add_component(component);
  const auto directory_id = db.add_directory(component_id, "");
  const auto file_id = source.add_file(directory_id, "main.cpp");
  CHECK(file_id == db.add_file(directory_id, "main.cpp"));

  Symbol symbol;
  symbol.usr = "ports:@F@main";
  symbol.spelling = "main";
  symbol.kind = "function";
  symbol.file_id = file_id;
  symbol.is_definition = true;
  const auto symbol_id = symbols.add_symbol(symbol);

  const auto read_port = static_cast<SymbolReadPort *>(&symbols);
  REQUIRE(read_port->lookup_symbol_by_id(symbol_id).has_value());
  CHECK(read_port->lookup_symbol_by_id(symbol_id)->usr == symbol.usr);
  const auto stored_component = catalog.get_component_by_id(component_id);
  REQUIRE(stored_component.has_value());
  CHECK(stored_component->name == component.name);
  CHECK(stored_component->path == component.path);
  CHECK(stored_component->kind == component.kind);
  CHECK(stored_component->version == component.version);

  const auto universe_id =
      db.add_semantic_universe("ports-universe", "Ports", "explicit");
  const RepositoryWriteRecord repository{
      "ports-repo", "repo", "https://example.invalid/ports", universe_id};
  const auto repository_id = catalog.add_repository(repository);
  const auto stored_repository = db.get_repository_by_id(repository_id);
  REQUIRE(stored_repository.has_value());
  CHECK(stored_repository->name == repository.name);
  CHECK(stored_repository->kind == repository.kind);
  CHECK(stored_repository->remote_url == repository.remote_url);
  CHECK(stored_repository->semantic_universe_id ==
        repository.semantic_universe_id);

  const SymbolIdentityRecord identity{"ports:@F@minted",
                                      "minted",
                                      "ports::minted",
                                      "ports::minted()",
                                      "function",
                                      std::nullopt,
                                      11,
                                      7,
                                      "system/ports.hpp",
                                      true,
                                      true,
                                      "int ()",
                                      universe_id,
                                      "system/ports.hpp",
                                      "internal",
                                      "ports-tu"};
  const auto minted_id = symbols.mint_symbol_id(identity);
  const auto minted = db.lookup_symbol_by_id(minted_id);
  REQUIRE(minted.has_value());
  CHECK(minted->usr == identity.usr);
  CHECK(minted->spelling == identity.spelling);
  CHECK(minted->qual_name == identity.qual_name);
  CHECK(minted->display_name == identity.display_name);
  CHECK(minted->type_info == identity.type_info);
  CHECK(minted->decl_line == identity.decl_line);
  CHECK(minted->decl_col == identity.decl_col);
  CHECK(minted->decl_path == identity.decl_path);
  CHECK(minted->is_instantiation == identity.is_instantiation);
  CHECK(minted->semantic_universe_id == *identity.semantic_universe_id);
  CHECK(minted->identity_key.contains("system/ports.hpp"));
  CHECK(minted->identity_key.contains("ports-tu"));

  auto &facade_catalog = db.workspace_catalog_write();
  const auto facade_component_id = facade_catalog.add_component(
      ComponentWriteRecord{"facade", "/tmp/facade", "repo", std::nullopt});
  const auto facade_component =
      db.workspace_catalog_read().get_component_by_id(facade_component_id);
  REQUIRE(facade_component.has_value());
  CHECK(facade_component->name == "facade");
}

TEST_CASE("unit of work port commits and rolls back as a boundary") {
  Storage db(":memory:");
  SqliteWorkspaceCatalogAdapter catalog(db);
  SqliteUnitOfWorkFactory units(db);

  {
    auto unit = units.begin();
    catalog.add_component(
        ComponentWriteRecord{"rolled-back", "/tmp/rolled-back", "repo", {}});
    unit->rollback();
  }
  CHECK_FALSE(catalog.get_component("/tmp/rolled-back").has_value());

  {
    auto unit = units.begin();
    catalog.add_component(
        ComponentWriteRecord{"committed", "/tmp/committed", "repo", {}});
    unit->commit();
  }
  CHECK(catalog.get_component("/tmp/committed").has_value());
}

TEST_CASE("unit of work rolls back a failed one-TU publication") {
  Storage db(":memory:");
  SqliteWorkspaceCatalogAdapter catalog(db);
  SqliteUnitOfWorkFactory units(db);

  try {
    auto unit = units.begin();
    catalog.add_component(
        ComponentWriteRecord{"failed-tu", "/tmp/failed-tu", "repo", {}});
    throw std::runtime_error("injected publication failure");
  } catch (const std::runtime_error &) {
  }

  CHECK_FALSE(catalog.get_component("/tmp/failed-tu").has_value());
}

TEST_CASE("transform registry declares a deterministic dependency order") {
  TransformRegistry registry;
  TransformDescriptor source;
  source.id = "source";
  source.version = 1;
  source.input_facts = {"raw"};
  source.produced_facts = {"source.fact"};
  source.invalidation_keys = {"source"};
  source.options = {"deterministic-sql-v1"};
  source.input_queries = {"SELECT 1"};
  source.output_queries = {"SELECT 1"};
  source.output_count_query = "SELECT 1";
  registry.register_transform(source);
  TransformDescriptor derived;
  derived.id = "derived";
  derived.version = 1;
  derived.input_facts = {"source.fact"};
  derived.produced_facts = {"derived.fact"};
  derived.dependencies = {"source"};
  derived.invalidation_keys = {"derived"};
  derived.options = {"deterministic-sql-v1"};
  derived.input_queries = {"SELECT 1"};
  derived.output_queries = {"SELECT 1"};
  derived.output_count_query = "SELECT 1";
  registry.register_transform(derived);

  const auto order = registry.execution_order();
  REQUIRE(order.size() == 2);
  CHECK(order[0]->id == "source");
  CHECK(order[1]->id == "derived");
}

TEST_CASE("named transform pipeline reuses identical content identities") {
  Storage db(":memory:");

  Symbol source;
  source.usr = "transform:@F@nonempty-source";
  source.spelling = "nonempty-source";
  source.kind = "function";
  source.is_definition = true;
  source.resolved = true;
  const auto source_id = db.add_symbol(source);
  Symbol target;
  target.usr = "transform:@F@nonempty-target";
  target.spelling = "nonempty-target";
  target.kind = "function";
  target.is_definition = true;
  target.resolved = true;
  const auto target_id = db.add_symbol(target);
  Edge edge;
  edge.src_id = source_id;
  edge.dst_id = target_id;
  edge.kind = 1;
  db.add_edge(edge);

  const auto first = db.run_transform_pipeline();
  REQUIRE(first.runs.size() == 8);
  CHECK(first.still_stub_count == 0);
  for (const auto &run : first.runs) {
    CHECK((run.status == TransformRunStatus::ran ||
           run.status == TransformRunStatus::skipped));
    CHECK_FALSE(run.input_identity.empty());
    CHECK_FALSE(run.output_identity.empty());
  }

  const auto second = db.run_transform_pipeline();
  REQUIRE(second.runs.size() == first.runs.size());
  for (const auto &run : second.runs) {
    CHECK(run.status == TransformRunStatus::reused);
  }
}

TEST_CASE(
    "failed transform preserves prior derived rows and records stale input") {
  Storage db(":memory:");
  Symbol src;
  src.usr = "transform:@F@src";
  src.spelling = "src";
  src.kind = "function";
  src.is_definition = true;
  const auto src_id = db.add_symbol(src);
  Symbol dst;
  dst.usr = "transform:@F@dst";
  dst.spelling = "dst";
  dst.kind = "function";
  dst.is_definition = true;
  const auto dst_id = db.add_symbol(dst);
  Edge dispatch_edge;
  dispatch_edge.src_id = src_id;
  dispatch_edge.dst_id = dst_id;
  dispatch_edge.kind = 18;
  db.add_edge(dispatch_edge);

  const auto baseline = db.run_transform_pipeline();
  REQUIRE(baseline.runs.size() == 8);

  db.raw_db().exec("INSERT OR IGNORE INTO entity_node(id, kind) VALUES (" +
                   std::to_string(src_id) + ", 1), (" + std::to_string(dst_id) +
                   ", 1)");
  db.add_entity_edge(src_id, dst_id, 1);
  auto before = db.raw_db().prepare("SELECT COUNT(*) FROM entity_edge");
  REQUIRE(before.step());
  CHECK(before.col_int64(0) == 1);

  Edge override_edge;
  override_edge.src_id = src_id;
  override_edge.dst_id = dst_id;
  override_edge.kind = 6;
  db.add_edge(override_edge);
  db.inject_transform_failure_for_testing("entity-graph-rollup");
  const auto failed = db.run_transform_pipeline();
  REQUIRE(failed.runs.back().transform_id == "entity-graph-rollup");
  CHECK(failed.runs.back().status == TransformRunStatus::failed);
  CHECK_FALSE(failed.runs.back().diagnostic.empty());

  auto state = db.raw_db().prepare("SELECT value FROM meta WHERE key = "
                                   "'transform.entity-graph-rollup.status'");
  REQUIRE(state.step());
  CHECK(state.col_text(0) == "failed");
  auto published = db.raw_db().prepare(
      "SELECT value FROM meta WHERE key = "
      "'transform.entity-graph-rollup.published.status'");
  REQUIRE(published.step());
  CHECK(published.col_text(0) == "ran");
  auto attempt_input = db.raw_db().prepare(
      "SELECT value FROM meta WHERE key = "
      "'transform.entity-graph-rollup.attempt.input'");
  REQUIRE(attempt_input.step());
  CHECK_FALSE(attempt_input.col_text(0).empty());
  auto after = db.raw_db().prepare("SELECT COUNT(*) FROM entity_edge");
  REQUIRE(after.step());
  CHECK(after.col_int64(0) == 1);
}

TEST_CASE("transform invalidation is typed and has minimum closure") {
  Storage db(":memory:");
  const auto first = db.run_transform_pipeline();
  REQUIRE(first.complete);

  db.set_transform_invalidation_for_testing("catalog", "catalog-v2");
  const auto catalog_change = db.run_transform_pipeline();
  CHECK(catalog_change.runs[0].status == TransformRunStatus::reused);
  CHECK(catalog_change.runs[4].status == TransformRunStatus::ran);
  CHECK(catalog_change.runs[5].status == TransformRunStatus::reused);
  CHECK(catalog_change.runs[6].status == TransformRunStatus::skipped);
  CHECK(catalog_change.runs[7].status == TransformRunStatus::skipped);

  db.set_transform_invalidation_for_testing("catalog", "catalog-v1");
  db.set_transform_invalidation_for_testing("source", "source-v2");
  const auto source_change = db.run_transform_pipeline();
  CHECK(source_change.runs[0].status == TransformRunStatus::ran);
  CHECK(source_change.runs[1].status == TransformRunStatus::ran);
  CHECK(source_change.runs[2].status == TransformRunStatus::ran);
  CHECK(source_change.runs[3].status == TransformRunStatus::ran);
  CHECK(source_change.runs[4].status == TransformRunStatus::ran);
  CHECK(source_change.runs[5].status == TransformRunStatus::skipped);
  CHECK(source_change.runs[6].status == TransformRunStatus::skipped);
}

TEST_CASE("clean and incremental publication have equal identities") {
  const auto seed = [](Storage &db) {
    Symbol lhs;
    lhs.usr = "transform:@F@clean-lhs";
    lhs.spelling = "clean-lhs";
    lhs.kind = "function";
    lhs.is_definition = true;
    lhs.resolved = true;
    const auto lhs_id = db.add_symbol(lhs);
    Symbol rhs = lhs;
    rhs.usr = "transform:@F@clean-rhs";
    rhs.spelling = "clean-rhs";
    const auto rhs_id = db.add_symbol(rhs);
    Edge edge;
    edge.src_id = lhs_id;
    edge.dst_id = rhs_id;
    edge.kind = 1;
    db.add_edge(edge);
  };
  Storage clean(":memory:");
  Storage incremental(":memory:");
  seed(clean);
  seed(incremental);
  const auto clean_run = clean.run_transform_pipeline();
  const auto incremental_run = incremental.run_transform_pipeline();
  REQUIRE(clean_run.complete);
  REQUIRE(incremental_run.complete);
  REQUIRE(clean_run.runs.size() == incremental_run.runs.size());
  for (std::size_t i = 0; i < clean_run.runs.size(); ++i) {
    CHECK(clean_run.runs[i].output_identity ==
          incremental_run.runs[i].output_identity);
  }
  const auto reused = incremental.run_transform_pipeline();
  CHECK(std::ranges::all_of(reused.runs, [](const TransformRun &run) {
    return run.status == TransformRunStatus::reused;
  }));
}

TEST_CASE("legacy resolve propagates a failed transform") {
  Storage db(":memory:");
  REQUIRE(db.run_transform_pipeline().complete);
  db.set_transform_invalidation_for_testing("source", "resolve-failure");
  db.inject_transform_failure_for_testing("entity-graph-rollup");
  bool threw = false;
  try {
    (void)db.resolve_pass();
  } catch (const std::exception &) {
    threw = true;
  }
  CHECK(threw);
  CHECK_FALSE(db.graph_resolved());
}

TEST_CASE("pending transform publication stays stale until graph extraction") {
  Storage db(":memory:");
  REQUIRE(db.run_transform_pipeline().complete);
  db.stamp_graph_resolved();
  db.mark_transform_pipeline_pending("selected file remains pending");
  const auto status = db.transform_status();
  CHECK_FALSE(status.complete);
  CHECK_FALSE(db.graph_resolved());
  CHECK(std::ranges::all_of(status.runs, [](const TransformRun &run) {
    return run.status == TransformRunStatus::stale &&
           run.completeness == TransformCompleteness::pending;
  }));
}
