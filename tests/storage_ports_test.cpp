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
