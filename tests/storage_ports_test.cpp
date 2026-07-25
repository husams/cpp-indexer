#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "storage/ports.hpp"
#include "storage/sqlite_adapters.hpp"
#include "storage/storage.hpp"

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
  registry.register_transform(TransformDescriptor{"source",
                                                  1,
                                                  {"raw"},
                                                  {"source.fact"},
                                                  {},
                                                  {"source"},
                                                  {},
                                                  {"SELECT 1"},
                                                  {"SELECT 1"},
                                                  "SELECT 1"});
  registry.register_transform(TransformDescriptor{"derived",
                                                  1,
                                                  {"source.fact"},
                                                  {"derived.fact"},
                                                  {"source"},
                                                  {"derived"},
                                                  {},
                                                  {"SELECT 1"},
                                                  {"SELECT 1"},
                                                  "SELECT 1"});

  const auto order = registry.execution_order();
  REQUIRE(order.size() == 2);
  CHECK(order[0]->id == "source");
  CHECK(order[1]->id == "derived");
}

TEST_CASE("named transform pipeline reuses identical content identities") {
  Storage db(":memory:");

  const auto first = db.run_transform_pipeline();
  REQUIRE(first.runs.size() == 5);
  CHECK(first.still_stub_count == 0);
  for (const auto &run : first.runs) {
    CHECK(run.status == TransformRunStatus::ran);
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
  REQUIRE(baseline.runs.size() == 5);

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
  auto after = db.raw_db().prepare("SELECT COUNT(*) FROM entity_edge");
  REQUIRE(after.step());
  CHECK(after.col_int64(0) == 1);
}
