#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "storage/ports.hpp"
#include "storage/sqlite_adapters.hpp"
#include "storage/storage.hpp"

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

  const auto component_id = catalog.add_component("ports", "/tmp/ports", "repo");
  const auto directory_id = db.add_directory(component_id, "");
  const auto file_id = source.add_file_path("/tmp/ports/main.cpp");
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
  CHECK(catalog.get_component_by_id(component_id)->name == "ports");

  auto &facade_catalog = db.workspace_catalog_write();
  const auto facade_component_id =
      facade_catalog.add_component("facade", "/tmp/facade", "repo");
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
    catalog.add_component("rolled-back", "/tmp/rolled-back", "repo");
    unit->rollback();
  }
  CHECK_FALSE(catalog.get_component("/tmp/rolled-back").has_value());

  {
    auto unit = units.begin();
    catalog.add_component("committed", "/tmp/committed", "repo");
    unit->commit();
  }
  CHECK(catalog.get_component("/tmp/committed").has_value());
}
