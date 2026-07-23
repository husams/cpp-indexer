// v35 symbol identity scope acceptance tests.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <string>
#include <unistd.h>

#include "storage/sqlite.hpp"
#include "storage/storage.hpp"

namespace {

std::string temp_db() {
  std::array<char, 64> tmpl = {};
  std::string pattern = "/tmp/cidx_identity_XXXXXX";
  std::ranges::copy(pattern, tmpl.begin());
  const int fd = ::mkstemp(tmpl.data());
  REQUIRE(fd >= 0);
  ::close(fd);
  return tmpl.data();
}

int64_t file_for(cidx::Storage &db, int64_t component,
                 const std::string &name) {
  const auto dir = db.add_directory(component, "");
  return db.add_file(dir, name);
}

cidx::Symbol external_symbol(const std::string &usr, int64_t file_id) {
  cidx::Symbol sym;
  sym.usr = usr;
  sym.spelling = "Thing";
  sym.kind = "struct";
  sym.file_id = file_id;
  sym.linkage = "external";
  return sym;
}

} // namespace

TEST_CASE("v35 isolates unrelated universes and merges declared sharing") {
  cidx::Storage db(":memory:");
  const auto banking = db.add_semantic_universe("program:banking");
  const auto composed = db.add_semantic_universe("program:composed");
  const auto banking_repo =
      db.add_repository("banking", "repo", std::nullopt, banking);
  const auto composed_repo =
      db.add_repository("composed", "repo", std::nullopt, composed);
  const auto banking_component = db.add_component("banking", "/tmp/banking");
  const auto composed_component = db.add_component("composed", "/tmp/composed");
  db.set_component_repository(banking_component, banking_repo);
  db.set_component_repository(composed_component, composed_repo);
  const auto banking_file = file_for(db, banking_component, "collision.hpp");
  const auto composed_file = file_for(db, composed_component, "collision.hpp");

  const auto banking_id =
      db.add_symbol(external_symbol("c:@N@collision", banking_file));
  const auto composed_id =
      db.add_symbol(external_symbol("c:@N@collision", composed_file));
  CHECK(banking_id != composed_id);
  CHECK(db.lookup_symbols_by_usr("c:@N@collision").size() == 2);
  const auto bare = db.lookup_symbol("c:@N@collision");
  REQUIRE(bare.has_value());
  if (bare) {
    CHECK(bare->id == std::min(banking_id, composed_id));
  }

  const auto composed_clone_repo =
      db.add_repository("composed-twin", "repo", std::nullopt, banking);
  const auto composed_clone =
      db.add_component("composed-twin", "/tmp/composed-twin");
  db.set_component_repository(composed_clone, composed_clone_repo);
  const auto shared_file = file_for(db, composed_clone, "collision.hpp");
  const auto shared_id =
      db.add_symbol(external_symbol("c:@N@collision", shared_file));
  CHECK(shared_id == banking_id);
  CHECK(db.lookup_symbols_by_usr("c:@N@collision").size() == 2);

  auto internal_a = external_symbol("c:@F@hidden", banking_file);
  internal_a.linkage = "internal";
  auto internal_b = external_symbol("c:@F@hidden", shared_file);
  internal_b.linkage = "internal";
  const auto internal_id_a = db.add_symbol(internal_a);
  const auto internal_id_b = db.add_symbol(internal_b);
  CHECK(internal_id_a != internal_id_b);
  CHECK(db.lookup_symbols_by_usr("c:@F@hidden", banking).size() == 2);

  auto no_linkage_a = external_symbol("c:@F@local", banking_file);
  no_linkage_a.linkage = "no-linkage";
  auto no_linkage_b = external_symbol("c:@F@local", shared_file);
  no_linkage_b.linkage = "no-linkage";
  CHECK(db.add_symbol(no_linkage_a) != db.add_symbol(no_linkage_b));

  const auto banking_symbol = db.lookup_symbol_by_id(banking_id);
  const auto composed_symbol = db.lookup_symbol_by_id(composed_id);
  REQUIRE(banking_symbol.has_value());
  REQUIRE(composed_symbol.has_value());
  if (banking_symbol && composed_symbol) {
    CHECK(banking_symbol->semantic_universe_id == banking);
    CHECK(composed_symbol->semantic_universe_id == composed);
    CHECK(banking_symbol->identity_key != composed_symbol->identity_key);
  }
}

TEST_CASE(
    "v34 migration preserves ids, graph references, and legacy identity") {
  const std::string path = temp_db();
  {
    cidx::SqliteDb raw(path);
    raw.exec(R"sql(
      CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
      INSERT INTO meta VALUES ('schema_version', '34');
      CREATE TABLE symbol (
        id INTEGER PRIMARY KEY, usr TEXT NOT NULL UNIQUE, spelling TEXT NOT NULL,
        qual_name TEXT, display_name TEXT, kind INTEGER NOT NULL, type_info TEXT,
        file_id INTEGER, line INTEGER, col INTEGER, end_line INTEGER,
        end_col INTEGER, decl_file_id INTEGER, decl_line INTEGER, decl_col INTEGER,
        decl_path TEXT, is_definition INTEGER NOT NULL DEFAULT 0,
        is_pure INTEGER NOT NULL DEFAULT 0, is_static INTEGER NOT NULL DEFAULT 0,
        is_instantiation INTEGER NOT NULL DEFAULT 0,
        is_named_instance INTEGER NOT NULL DEFAULT 0, linkage TEXT, access TEXT,
        parent_usr TEXT, resolved INTEGER NOT NULL DEFAULT 0,
        multi_def INTEGER NOT NULL DEFAULT 0, const_value TEXT
      );
      CREATE TABLE repository (
        id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE,
        kind TEXT NOT NULL DEFAULT 'repo', remote_url TEXT,
        active_clone_id INTEGER
      );
      INSERT INTO repository (id, name) VALUES (3, 'legacy-repo');
      CREATE TABLE component (
        id INTEGER PRIMARY KEY, name TEXT NOT NULL, path TEXT NOT NULL,
        kind TEXT NOT NULL DEFAULT 'repo', version TEXT, repository_id INTEGER,
        UNIQUE(path)
      );
      INSERT INTO component (id, name, path, repository_id)
        VALUES (4, 'legacy-component', '.', 3);
      CREATE TABLE edge (
        id INTEGER PRIMARY KEY, src_id INTEGER NOT NULL, dst_id INTEGER NOT NULL,
        kind INTEGER NOT NULL, count INTEGER NOT NULL DEFAULT 1,
        base_access INTEGER, is_virtual INTEGER, vtable_slot INTEGER,
        UNIQUE(src_id, dst_id, kind)
      );
      INSERT INTO symbol (id, usr, spelling, kind, linkage)
        VALUES (7, 'c:@N@legacy', 'legacy', 22, 'external');
      INSERT INTO edge (id, src_id, dst_id, kind) VALUES (11, 7, 7, 1);
    )sql");
  }
  {
    cidx::Storage db(path);
    const auto symbol = db.lookup_symbol_by_id(7);
    REQUIRE(symbol.has_value());
    if (symbol) {
      CHECK(symbol->semantic_universe_id == 1);
      CHECK(symbol->identity_key ==
            std::string("legacy") + char(31) + "c:@N@legacy");
    }
    REQUIRE(db.get_repository_by_id(3).has_value());
    const auto repository = db.get_repository_by_id(3);
    if (repository) {
      CHECK(repository->semantic_universe_id == 1);
    }
  }
  cidx::SqliteDb raw(path);
  auto version =
      raw.prepare("SELECT value FROM meta WHERE key = 'schema_version'");
  REQUIRE(version.step());
  CHECK(version.col_text(0) == "35");
  auto edge = raw.prepare("SELECT src_id, dst_id FROM edge WHERE id = 11");
  REQUIRE(edge.step());
  CHECK(edge.col_int64(0) == 7);
  CHECK(edge.col_int64(1) == 7);
}
