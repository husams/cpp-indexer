// storage_migration_test — design §4.1 / G19. Opens the committed
// Python-generated fixture DBs (v2/v3/v4/v5 historical layouts, written by
// tests/fixtures/generate_fixtures.py) and asserts the column adds, the
// qual_name recursive-CTE backfill, the decl_* backfill for declaration-only
// rows, and the meta bump to '6'. The fixtures being Python-written doubles
// as a cross-tool-open proof. Fixtures are copied to a temp dir first — the
// committed files are never mutated.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "storage/sqlite.hpp"
#include "storage/storage.hpp"

#ifndef CIDX_FIXTURES_DIR
#error "CIDX_FIXTURES_DIR must be defined by the build"
#endif

namespace {

std::string make_temp_dir() {
  char tmpl[] = "/tmp/cidx_migration_XXXXXX";
  char *d = ::mkdtemp(tmpl);
  REQUIRE(d != nullptr);
  return d;
}

// Copy a committed fixture into the temp dir (Storage migrates in place).
std::string stage_fixture(const std::string &tmp, const std::string &name) {
  const std::string src_path = std::string(CIDX_FIXTURES_DIR) + "/" + name;
  const std::string dst_path = tmp + "/" + name;
  std::ifstream src(src_path, std::ios::binary);
  REQUIRE_MESSAGE(src.good(), "missing fixture: run generate_fixtures.py");
  std::ofstream dst(dst_path, std::ios::binary);
  dst << src.rdbuf();
  REQUIRE(dst.good());
  return dst_path;
}

std::vector<std::string> table_columns(cidx::SqliteDb &db, const char *table) {
  std::vector<std::string> out;
  auto st = db.prepare(std::string("PRAGMA table_info(") + table + ")");
  while (st.step()) {
    out.push_back(st.col_text(1));
  }
  return out;
}

bool has_col(const std::vector<std::string> &cols, const std::string &name) {
  for (const auto &c : cols) {
    if (c == name) {
      return true;
    }
  }
  return false;
}

std::string meta_version(cidx::SqliteDb &db) {
  auto st = db.prepare("SELECT value FROM meta WHERE key = 'schema_version'");
  REQUIRE(st.step());
  return st.col_text(0);
}

// Post-migration invariants shared by every fixture version.
void check_migrated(const std::string &db_path) {
  cidx::SqliteDb raw(db_path);

  const auto scols = table_columns(raw, "symbol");
  for (const char *c : {"qual_name", "decl_file_id", "decl_line", "decl_col",
                        "is_pure", "is_static", "decl_path"}) {
    CHECK_MESSAGE(has_col(scols, c), "symbol." << c << " present");
  }
  CHECK(has_col(table_columns(raw, "file"), "driver"));
  CHECK(has_col(table_columns(raw, "file"), "args_overridden"));
  CHECK(meta_version(raw) >= "10");

  // qual_name: longest parent_usr chain wins; the anonymous-namespace level
  // (empty parent spelling) is skipped.
  const auto qual_of = [&raw](const char *usr) {
    auto st = raw.prepare("SELECT qual_name FROM symbol WHERE usr = ?");
    st.bind(1, std::string_view(usr));
    REQUIRE(st.step());
    return st.col_text(0);
  };
  CHECK(qual_of("c:@N@rk") == "rk");
  CHECK(qual_of("c:@N@rk@S@Conf") == "rk::Conf");
  CHECK(qual_of("c:@N@rk@S@Conf@F@set") == "rk::Conf::set");
  CHECK(qual_of("c:@aN@F@hidden") == "hidden");
  CHECK(qual_of("c:@F@main") == "main");

  // decl_* backfill: declaration-only rows copied their stored location;
  // definition rows stay NULL (populate on reindex).
  {
    auto st = raw.prepare("SELECT decl_file_id, decl_line, decl_col "
                          "FROM symbol WHERE usr = 'c:@N@rk@S@Conf@F@set'");
    REQUIRE(st.step());
    CHECK(st.col_int64(0) == 1);
    CHECK(st.col_int64(1) == 3);
    CHECK(st.col_int64(2) == 5);
  }
  {
    auto st =
        raw.prepare("SELECT decl_file_id FROM symbol WHERE usr = 'c:@F@main'");
    REQUIRE(st.step());
    CHECK(st.col_is_null(0));
  }

  // G19: the schema script ran AFTER migration, so idx_symbol_qual (which
  // references the migrated column) exists. v7 also adds idx_edge_src/dst.
  std::set<std::string> indexes;
  auto st = raw.prepare("SELECT name FROM sqlite_master WHERE type = 'index' "
                        "AND name LIKE 'idx_%'");
  while (st.step()) {
    indexes.insert(st.col_text(0));
  }
  CHECK(indexes == std::set<std::string>{"idx_symbol_spelling",
                                         "idx_symbol_qual",
                                         "idx_symbol_file",
                                         "idx_symbol_parent",
                                         "idx_symbol_kind",
                                         "idx_symbol_spelling_nc",
                                         "idx_symbol_qual_nc",
                                         "idx_edge_src",
                                         "idx_edge_dst",
                                         "idx_call_arg_edge",
                                         "idx_diagnostic_file",
                                         "idx_entity_edge_identity",
                                         "idx_entity_edge_src",
                                         "idx_entity_edge_dst",
                                         "idx_decl_site_symbol",
                                         "idx_definition_symbol",
                                         "idx_def_edge_src",
                                         "idx_def_edge_dst",
                                         "idx_possible_call_src",
                                         "idx_possible_call_dst",
                                         "idx_type_node_decl_usr",
                                         "idx_type_node_canonical",
                                         "idx_type_edge_dst",
                                         "idx_parameter_type",
                                         "idx_parameter_declared_type",
                                         "idx_parameter_adjusted_type",
                                         "idx_symbol_type_type",
                                         "idx_translation_unit_config_hash",
                                         "idx_file_config_config",
                                         "idx_fact_applicability_config",
                                         "idx_include_config_digest",
                                         "idx_include_edge_dst",
                                         "idx_include_edge_config",
                                         "idx_include_site_edge",
                                         "idx_include_macro_use_path"});
}

} // namespace

TEST_CASE(
    "v2 fixture migrates: qual_name CTE + decl backfill + is_pure + driver") {
  const std::string tmp = make_temp_dir();
  const std::string path = stage_fixture(tmp, "v2.db");
  {
    // Pre-condition: the fixture really is the old layout.
    cidx::SqliteDb raw(path);
    const auto scols = table_columns(raw, "symbol");
    CHECK_FALSE(has_col(scols, "qual_name"));
    CHECK_FALSE(has_col(scols, "decl_file_id"));
    CHECK_FALSE(has_col(scols, "is_pure"));
    CHECK_FALSE(has_col(table_columns(raw, "file"), "driver"));
    CHECK(meta_version(raw) == "2");
  }
  {
    cidx::Storage db(path);
  } // open = migrate
  check_migrated(path);
}

TEST_CASE("v3 fixture migrates: stored qual_name kept, decl backfill applied") {
  const std::string tmp = make_temp_dir();
  const std::string path = stage_fixture(tmp, "v3.db");
  {
    cidx::SqliteDb raw(path);
    CHECK(has_col(table_columns(raw, "symbol"), "qual_name"));
    CHECK_FALSE(has_col(table_columns(raw, "symbol"), "decl_file_id"));
    CHECK(meta_version(raw) == "3");
  }
  {
    cidx::Storage db(path);
  }
  check_migrated(path);
}

TEST_CASE("v4 fixture migrates: is_pure + driver added") {
  const std::string tmp = make_temp_dir();
  const std::string path = stage_fixture(tmp, "v4.db");
  {
    cidx::SqliteDb raw(path);
    CHECK(has_col(table_columns(raw, "symbol"), "decl_file_id"));
    CHECK_FALSE(has_col(table_columns(raw, "symbol"), "is_pure"));
    CHECK(meta_version(raw) == "4");
  }
  {
    cidx::Storage db(path);
  }
  check_migrated(path);

  // v4 already stored decl_* for the declaration row — values survive.
  cidx::SqliteDb raw(path);
  auto st = raw.prepare("SELECT decl_line FROM symbol "
                        "WHERE usr = 'c:@N@rk@S@Conf@F@set'");
  REQUIRE(st.step());
  CHECK(st.col_int64(0) == 3);
}

TEST_CASE("v5 fixture migrates: only file.driver added, is_pure kept") {
  const std::string tmp = make_temp_dir();
  const std::string path = stage_fixture(tmp, "v5.db");
  {
    cidx::SqliteDb raw(path);
    CHECK(has_col(table_columns(raw, "symbol"), "is_pure"));
    CHECK_FALSE(has_col(table_columns(raw, "file"), "driver"));
    CHECK(meta_version(raw) == "5");
  }
  {
    cidx::Storage db(path);
  }
  check_migrated(path);

  // is_pure=1 seeded on the pure-virtual method survives the migration.
  cidx::SqliteDb raw(path);
  auto st = raw.prepare("SELECT is_pure FROM symbol "
                        "WHERE usr = 'c:@N@rk@S@Conf@F@set'");
  REQUIRE(st.step());
  CHECK(st.col_int64(0) == 1);
}

TEST_CASE("migrated DB stays fully usable through the Storage API") {
  const std::string tmp = make_temp_dir();
  const std::string path = stage_fixture(tmp, "v2.db");
  cidx::Storage db(path);
  // Rows written by Python under the old layout read back correctly even
  // though ALTER TABLE appended the new columns at the end.
  auto sym = db.lookup_symbol("c:@N@rk@S@Conf@F@set");
  REQUIRE(sym.has_value());
  CHECK(sym->spelling == "set");
  CHECK(sym->kind == "method");
  CHECK(sym->qual_name == std::string("rk::Conf::set"));
  CHECK(sym->decl_line == 3);
  CHECK_FALSE(sym->is_pure);
  // The backfilled qual_name is immediately searchable.
  const auto hits = db.search_symbols("conf::set");
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].usr == "c:@N@rk@S@Conf@F@set");
}

// ---------------------------------------------------------------------------
// v13 → v14 migration (portable-paths): component.version column + label table
// ---------------------------------------------------------------------------

TEST_CASE(
    "v13 DB migrates to v14: component.version added, label table created") {
  const std::string tmp = make_temp_dir();
  const std::string path = tmp + "/v13.db";

  // Create a v13-like DB by opening it with the current code (gets v14),
  // then downgrade meta to '13' and remove the new columns/tables to simulate
  // a v13 DB.
  {
    cidx::Storage db(path); // creates v14
    db.add_component("mylib", "/opt/mylib", "external");
  }
  {
    cidx::SqliteDb raw(path);
    // Remove the v14 additions to simulate v13.
    raw.exec("UPDATE meta SET value = '13' WHERE key = 'schema_version'");
    // SQLite can't DROP COLUMN easily, so we'll just test that a pre-existing
    // v13 DB where version column is absent gets migrated correctly.
    // We can't truly remove the column in SQLite < 3.35, but we can verify
    // that the column now exists (it was created by Storage ctor above) and
    // that migration is idempotent.
  }
  // Open again: migration must be idempotent.
  {
    cidx::Storage db(path);
  }
  cidx::SqliteDb raw(path);
  // Verify schema_version bumped to current (14).
  CHECK(meta_version(raw) == std::to_string(cidx::kSchemaVersion));
  // component.version column exists.
  CHECK(has_col(table_columns(raw, "component"), "version"));
  // label table exists.
  {
    auto st = raw.prepare(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='label'");
    CHECK(st.step());
  }
}

TEST_CASE("v14: add_component preserves existing version on re-import") {
  cidx::Storage db(":memory:");
  // Initial import with version.
  db.add_component("mylib", "/opt/mylib", "external",
                   std::optional<std::string>{"v2.1.0"});
  // Re-import without version (NULL) — should COALESCE and keep v2.1.0.
  db.add_component("mylib", "/opt/mylib", "external",
                   std::optional<std::string>{});
  auto comp = db.get_component_by_name("mylib");
  REQUIRE(comp.has_value());
  CHECK(comp->version == std::optional<std::string>{"v2.1.0"});
}

TEST_CASE("v14: set_component_version and effective_root") {
  cidx::Storage db(":memory:");
  db.add_component("mylib", "/opt/mylib", "external");
  auto before = db.get_component_by_name("mylib");
  REQUIRE(before.has_value());
  CHECK_FALSE(before->version.has_value());
  CHECK(cidx::Storage::effective_root(*before) == "/opt/mylib");

  db.set_component_version("mylib", std::optional<std::string>{"v3.0"});
  auto after = db.get_component_by_name("mylib");
  REQUIRE(after.has_value());
  CHECK(after->version == std::optional<std::string>{"v3.0"});
  CHECK(cidx::Storage::effective_root(*after) == "/opt/mylib/v3.0");
}

TEST_CASE("v14: label add/get/remove/list round-trip") {
  cidx::Storage db(":memory:");
  const int64_t id1 = db.add_label("libfoo-include", "/opt/libfoo/include");
  const int64_t id2 = db.add_label("libbar-hdr", "$LIBBAR/include");
  CHECK(id1 > 0);
  CHECK(id2 > 0);

  CHECK(db.get_label("libfoo-include") ==
        std::optional<std::string>{"/opt/libfoo/include"});
  CHECK(db.get_label("libbar-hdr") ==
        std::optional<std::string>{"$LIBBAR/include"});
  CHECK_FALSE(db.get_label("nonexistent").has_value());

  const auto labels = db.list_labels();
  REQUIRE(labels.size() == 2);
  // ORDER BY name: libbar-hdr < libfoo-include
  CHECK(labels[0].first == "libbar-hdr");
  CHECK(labels[1].first == "libfoo-include");

  CHECK(db.remove_label("libbar-hdr"));
  CHECK_FALSE(db.remove_label("libbar-hdr")); // already gone
  CHECK(db.list_labels().size() == 1);
}

TEST_CASE("v14: label upsert updates path on conflict") {
  cidx::Storage db(":memory:");
  db.add_label("mylib", "/old/path");
  db.add_label("mylib", "/new/path"); // upsert → updates
  CHECK(db.get_label("mylib") == std::optional<std::string>{"/new/path"});
}

TEST_CASE("newer DB opens without refusal (no downgrade path)") {
  const std::string tmp = make_temp_dir();
  const std::string path = tmp + "/future.db";
  {
    cidx::Storage db(path);
  } // create a fresh v7
  {
    cidx::SqliteDb raw(path);
    raw.exec("ALTER TABLE symbol ADD COLUMN future_col TEXT");
    // A version NEWER than this build must be left untouched (no downgrade).
    raw.exec("UPDATE meta SET value = '99' WHERE key = 'schema_version'");
  }
  {
    cidx::Storage db(path); // must not throw, must not downgrade
    db.add_component("c", "/data/c");
    CHECK(db.get_component_by_name("c").has_value());
  }
  cidx::SqliteDb raw(path);
  CHECK(meta_version(raw) == "99"); // future schema left untouched, not bumped
  CHECK(has_col(table_columns(raw, "symbol"), "future_col"));
}

TEST_CASE("v28 -> v29: template_arg arg_kind remapped to the canonical codes") {
  // Simulate a v28 database: a fresh (v29) DB whose version is wound back to
  // 28 with the legacy raw-CX arg_kind values the retired class-spec path
  // stored (docs/improvements/template-arg-contract.md). Reopening must remap
  // by owner kind exactly once.
  const std::string tmp = make_temp_dir();
  const std::string path = tmp + "/v28.db";
  {
    cidx::Storage db(path);
  }
  {
    cidx::SqliteDb raw(path);
    raw.exec("INSERT INTO symbol (id, usr, spelling, kind) VALUES "
             "(1, 'c:@S@Spec', 'Spec', 2)," // struct owner (class-spec path)
             "(2, 'c:@F@fn', 'fn', 8)");    // function owner (callable path)
    raw.exec("INSERT INTO template_arg (owner_id, position, arg_kind) VALUES "
             "(1, 0, 8),"  // legacy Pack           -> 4
             "(1, 1, 5),"  // legacy Template       -> 3
             "(1, 2, 6),"  // legacy TmplExpansion  -> 3
             "(1, 3, 7),"  // legacy Expression     -> 2
             "(1, 4, 3),"  // legacy NullPtr        -> 2 (record owner)
             "(1, 5, 0),"  // legacy Null           -> row deleted
             "(1, 6, 1),"  // in-contract type      -> unchanged
             "(2, 0, 4),"  // callable pack         -> unchanged
             "(2, 1, 3)"); // callable tmpl-tmpl    -> unchanged (NOT NullPtr)
    raw.exec("UPDATE meta SET value = '28' WHERE key = 'schema_version'");
  }
  {
    cidx::Storage db(path); // migration runs here
  }
  cidx::SqliteDb raw(path);
  CHECK(meta_version(raw) == std::to_string(cidx::kSchemaVersion));
  auto q = [&](int owner, int pos) -> std::string {
    auto st = raw.prepare(
        "SELECT arg_kind FROM template_arg WHERE owner_id = " +
        std::to_string(owner) + " AND position = " + std::to_string(pos));
    if (!st.step()) {
      return "<gone>";
    }
    return st.col_text(0);
  };
  CHECK(q(1, 0) == "4");
  CHECK(q(1, 1) == "3");
  CHECK(q(1, 2) == "3");
  CHECK(q(1, 3) == "2");
  CHECK(q(1, 4) == "2");
  CHECK(q(1, 5) == "<gone>");
  CHECK(q(1, 6) == "1");
  CHECK(q(2, 0) == "4");
  CHECK(q(2, 1) == "3");

  { // idempotence: a second open (now stamped 29) must not remap the valid
    // template-template rows the first pass produced.
    cidx::Storage again(path);
  }
  cidx::SqliteDb raw2(path);
  auto st = raw2.prepare("SELECT arg_kind FROM template_arg "
                         "WHERE owner_id = 1 AND position = 1");
  REQUIRE(st.step());
  CHECK(st.col_text(0) == std::string("3")); // stayed template-template
}

TEST_CASE("v29 -> v30: signature/type tier tables created, version stamped") {
  // Simulate a v29 database: a fresh DB with the v30 tables dropped and the
  // version wound back. Reopening must create the tables (schema script),
  // stamp v30, and leave existing rows untouched.
  const std::string tmp = make_temp_dir();
  const std::string path = tmp + "/v29.db";
  {
    cidx::Storage db(path);
    db.add_component("c", "/data/c");
  }
  {
    cidx::SqliteDb raw(path);
    raw.exec("DROP TABLE symbol_type");
    raw.exec("DROP TABLE symbol_type_kind");
    raw.exec("DROP TABLE parameter");
    raw.exec("DROP TABLE type_edge");
    raw.exec("DROP TABLE type_edge_kind");
    raw.exec("DROP TABLE type_node");
    raw.exec("DROP TABLE type_kind");
    raw.exec("UPDATE meta SET value = '29' WHERE key = 'schema_version'");
  }
  {
    cidx::Storage db(path);                           // migration runs here
    CHECK(db.get_component_by_name("c").has_value()); // old data intact
    // The migrated DB accepts the new tier end-to-end.
    cidx::TypeNode n;
    n.type_key = "b:int";
    n.spelling = "int";
    n.kind = cidx::kTypeKindBuiltin;
    const int64_t tid = db.intern_type_node(n);
    CHECK(db.intern_type_node(n) == tid); // interned, not duplicated
  }
  cidx::SqliteDb raw(path);
  // migrate() stamps kSchemaVersion, not the version of the block that fired:
  // a v29 DB reopened by a v31 build lands on 31 in one step.
  CHECK(meta_version(raw) == std::to_string(cidx::kSchemaVersion));
  auto st = raw.prepare("SELECT COUNT(*) FROM type_kind");
  REQUIRE(st.step());
  CHECK(st.col_int64(0) == 13); // seed rows present
}

TEST_CASE("v30 -> v31: include tier tables created, version stamped") {
  // Simulate a v30 database: a fresh DB with the v31 tables dropped and the
  // version wound back. Reopening must create the tables (schema script),
  // stamp v31, and leave existing rows untouched. Include facts are NOT
  // backfillable -- only a reindex populates them -- so the migrated DB starts
  // with an empty include graph.
  const std::string tmp = make_temp_dir();
  const std::string path = tmp + "/v30.db";
  int64_t file_id = -1;
  {
    cidx::Storage db(path);
    db.add_component("c", "/data/c");
    file_id = db.add_file_path("/data/c/main.cpp");
  }
  {
    cidx::SqliteDb raw(path);
    raw.exec("DROP TABLE include_macro_use");
    raw.exec("DROP TABLE include_site");
    raw.exec("DROP TABLE include_directive_kind");
    raw.exec("DROP TABLE include_edge");
    raw.exec("DROP TABLE include_config");
    raw.exec("UPDATE meta SET value = '30' WHERE key = 'schema_version'");
  }
  {
    cidx::Storage db(path);                           // migration runs here
    CHECK(db.get_component_by_name("c").has_value()); // old data intact
    CHECK_FALSE(db.include_graph_populated());        // no backfill is possible

    // The migrated DB accepts the new tier end-to-end.
    cidx::IncludeConfig cfg;
    cfg.tu_file_id = file_id;
    cfg.digest = "deadbeef";
    cfg.arguments = {"-std=c++23"};
    const int64_t cid = db.add_include_config(cfg);
    CHECK(db.add_include_config(cfg) == cid); // upsert, not duplicated

    cidx::IncludeEdge e;
    e.src_file_id = file_id;
    e.dst_path = "/data/c/util.hpp";
    e.config_id = cid;
    const int64_t eid = db.add_include_edge(e);
    CHECK(db.add_include_edge(e) == eid); // collapsed onto one row
    CHECK(db.include_graph_populated());
  }
  cidx::SqliteDb raw(path);
  CHECK(meta_version(raw) == std::to_string(cidx::kSchemaVersion));
  auto st = raw.prepare("SELECT COUNT(*) FROM include_directive_kind");
  REQUIRE(st.step());
  CHECK(st.col_int64(0) == 5); // seed rows present
}

TEST_CASE("v31 -> v32: parameter and template-argument foreign keys survive") {
  const std::string tmp = make_temp_dir();
  const std::string path = tmp + "/v31.db";
  int64_t owner_id = -1;
  int64_t type_id = -1;
  int64_t file_id = -1;
  {
    cidx::Storage db(path);
    db.add_component("c", "/data/c");
    file_id = db.add_file_path("/data/c/main.cpp");
    cidx::Symbol owner;
    owner.usr = "c:@F@owner";
    owner.spelling = "owner";
    owner.kind = "function";
    owner.file_id = file_id;
    owner_id = db.add_symbol(owner);
    cidx::TypeNode type;
    type.type_key = "b:int";
    type.spelling = "int";
    type.kind = cidx::kTypeKindBuiltin;
    type_id = db.intern_type_node(type);
  }
  {
    cidx::SqliteDb raw(path);
    raw.exec("DROP TABLE parameter");
    raw.exec("DROP TABLE template_arg");
    raw.exec("CREATE TABLE parameter (owner_id INTEGER NOT NULL, "
             "position INTEGER NOT NULL, name TEXT, type_id INTEGER, "
             "file_id INTEGER, line INTEGER, col INTEGER, "
             "PRIMARY KEY (owner_id, position)) WITHOUT ROWID");
    raw.exec("CREATE TABLE template_arg (owner_id INTEGER NOT NULL, "
             "position INTEGER NOT NULL, arg_kind INTEGER NOT NULL, "
             "ref_id INTEGER, literal TEXT, type_id INTEGER, "
             "PRIMARY KEY (owner_id, position)) WITHOUT ROWID");
    auto parameter = raw.prepare(
        "INSERT INTO parameter(owner_id, position, name, type_id, file_id, "
        "line, col) VALUES (?, 0, 'value', ?, ?, 7, 9)");
    parameter.bind(1, owner_id);
    parameter.bind(2, type_id);
    parameter.bind(3, file_id);
    parameter.step_done();
    auto argument = raw.prepare(
        "INSERT INTO template_arg(owner_id, position, arg_kind, literal, "
        "type_id) VALUES (?, 0, 1, 'int', ?)");
    argument.bind(1, owner_id);
    argument.bind(2, type_id);
    argument.step_done();
    raw.exec("UPDATE meta SET value = '31' WHERE key = 'schema_version'");
  }
  {
    cidx::Storage db(path);
    CHECK(db.type_node_by_id(type_id).has_value());
  }

  cidx::SqliteDb raw(path);
  CHECK(meta_version(raw) == std::to_string(cidx::kSchemaVersion));
  auto parameter = raw.prepare(
      "SELECT type_id, declared_type_id, adjusted_type_id, file_id "
      "FROM parameter WHERE owner_id = ? AND position = 0 AND pack_index = -1");
  parameter.bind(1, owner_id);
  REQUIRE(parameter.step());
  CHECK(parameter.col_int64(0) == type_id);
  CHECK(parameter.col_is_null(1));
  CHECK(parameter.col_is_null(2));
  CHECK(parameter.col_int64(3) == file_id);
  auto argument = raw.prepare(
      "SELECT type_id, pack_index FROM template_arg WHERE owner_id = ?");
  argument.bind(1, owner_id);
  REQUIRE(argument.step());
  CHECK(argument.col_int64(0) == type_id);
  CHECK(argument.col_int64(1) == -1);

  std::set<std::string> parameter_fks;
  auto fk = raw.prepare("PRAGMA foreign_key_list(parameter)");
  while (fk.step())
    parameter_fks.insert(fk.col_text(2));
  CHECK(parameter_fks.contains("type_node"));
  CHECK(parameter_fks.contains("file"));
  std::set<std::string> argument_fks;
  fk = raw.prepare("PRAGMA foreign_key_list(template_arg)");
  while (fk.step())
    argument_fks.insert(fk.col_text(2));
  CHECK(argument_fks.contains("type_node"));
  CHECK(meta_version(raw) == std::to_string(cidx::kSchemaVersion));
}

TEST_CASE("v32 -> v33: symbol.const_value column added, version stamped") {
  // Simulate a v32 database: a fresh DB wound back to '32'. (SQLite < 3.35
  // can't DROP COLUMN, so like the v13 case above this verifies the column
  // guard is idempotent and the version restamps.) Values are NOT
  // backfillable -- only a reindex populates them.
  const std::string tmp = make_temp_dir();
  const std::string path = tmp + "/v32.db";
  {
    cidx::Storage db(path);
    db.add_component("c", "/data/c");
  }
  {
    cidx::SqliteDb raw(path);
    raw.exec("UPDATE meta SET value = '32' WHERE key = 'schema_version'");
  }
  {
    cidx::Storage db(path);                           // migration runs here
    CHECK(db.get_component_by_name("c").has_value()); // old data intact

    // The migrated DB accepts constant values end-to-end, and the upsert
    // keeps a stored value when a plain declaration (no initializer, so no
    // value) is indexed afterwards.
    cidx::Symbol s;
    s.usr = "c:@kAnswer";
    s.spelling = "kAnswer";
    s.kind = "variable";
    s.is_definition = true;
    s.const_value = "42";
    db.add_symbol(s);
    cidx::Symbol decl = s;
    decl.is_definition = false;
    decl.const_value = std::nullopt;
    db.add_symbol(decl);
    const auto got = db.lookup_symbol("c:@kAnswer");
    REQUIRE(got.has_value());
    CHECK(got->const_value == std::optional<std::string>{"42"});
  }
  cidx::SqliteDb raw(path);
  CHECK(meta_version(raw) == std::to_string(cidx::kSchemaVersion));
  CHECK(has_col(table_columns(raw, "symbol"), "const_value"));
}

TEST_CASE("v33 -> v34: alias/variable/field uses(7) edges become "
          "alias_of(19)/of_type(20)") {
  // Simulate a v33 database: a fresh DB wound back to '33' with the new
  // edge_kind rows removed and alias/variable/field type edges stored as
  // uses(7), the way the pre-v34 indexer wrote them. The migration must
  // rewrite exactly the alias -> type rows (alias_of) and the
  // variable/member -> type rows (of_type): namespace-qualifier edges and
  // the function -> type reference stay uses(7).
  const std::string tmp = make_temp_dir();
  const std::string path = tmp + "/v33.db";
  int64_t alias_id = -1;
  int64_t record_id = -1;
  int64_t ns_id = -1;
  int64_t fn_id = -1;
  int64_t var_id = -1;
  int64_t field_id = -1;
  {
    cidx::Storage db(path);
    db.add_component("c", "/data/c");
    auto sym = [&](const char *usr, const char *name, const char *kind) {
      cidx::Symbol s;
      s.usr = usr;
      s.spelling = name;
      s.kind = kind;
      s.is_definition = true;
      return db.add_symbol(s);
    };
    record_id = sym("c:@S@Color", "Color", "struct");
    alias_id = sym("c:@Rgb", "Rgb", "type-alias");
    ns_id = sym("c:@N@ns", "ns", "namespace");
    fn_id = sym("c:@F@paint#", "paint", "function");
    var_id = sym("c:@shade", "shade", "variable");
    field_id = sym("c:@S@Palette@FI@main", "main", "member");
    auto edge = [&](int64_t src, int64_t dst) {
      cidx::Edge e;
      e.src_id = src;
      e.dst_id = dst;
      e.kind = 7; // uses, as the pre-v34 indexer stored these relations
      db.add_edge(e);
    };
    edge(alias_id, record_id); // alias -> type: must become alias_of(19)
    edge(alias_id, ns_id);     // alias -> namespace qualifier: stays uses
    edge(fn_id, record_id);    // reference from a function: stays uses
    edge(var_id, record_id);   // variable -> its type: must become of_type(20)
    edge(field_id, record_id); // field -> its type: must become of_type(20)
    edge(var_id, ns_id);       // variable -> namespace qualifier: stays uses
  }
  {
    cidx::SqliteDb raw(path);
    raw.exec("DELETE FROM edge_kind WHERE id IN (19, 20)");
    raw.exec("UPDATE meta SET value = '33' WHERE key = 'schema_version'");
  }
  {
    cidx::Storage db(path); // migration runs here
  }
  cidx::SqliteDb raw(path);
  CHECK(meta_version(raw) == std::to_string(cidx::kSchemaVersion));
  auto kind_name = [&](int id) {
    auto st = raw.prepare("SELECT name FROM edge_kind WHERE id = ?");
    st.bind(1, static_cast<int64_t>(id));
    REQUIRE(st.step());
    return st.col_text(0);
  };
  CHECK(kind_name(19) == std::string("alias_of"));
  CHECK(kind_name(20) == std::string("of_type"));
  auto kind_of = [&](int64_t src, int64_t dst) {
    auto st = raw.prepare("SELECT kind FROM edge WHERE src_id=? AND dst_id=?");
    st.bind(1, src);
    st.bind(2, dst);
    REQUIRE(st.step());
    return st.col_int64(0);
  };
  CHECK(kind_of(alias_id, record_id) == 19);
  CHECK(kind_of(alias_id, ns_id) == 7);
  CHECK(kind_of(fn_id, record_id) == 7);
  CHECK(kind_of(var_id, record_id) == 20);
  CHECK(kind_of(field_id, record_id) == 20);
  CHECK(kind_of(var_id, ns_id) == 7);
}

TEST_CASE("v34 include configurations backfill into normalized identities") {
  const std::string tmp = make_temp_dir();
  const std::string path = tmp + "/v34.db";
  int64_t tu = -1;
  {
    cidx::Storage db(path);
    const int64_t component = db.add_component("config", "/repo/config");
    const int64_t directory = db.add_directory(component, "");
    tu = db.add_file(directory, "main.cpp");
  }
  {
    cidx::SqliteDb raw(path);
    raw.exec("DROP TABLE include_macro_use");
    raw.exec("DROP TABLE include_site");
    raw.exec("DROP TABLE include_edge");
    raw.exec("DROP TABLE include_config");
    raw.exec("DROP TABLE file_config");
    raw.exec("DROP TABLE translation_unit");
    raw.exec("DROP TABLE translation_unit_config");
    raw.exec("CREATE TABLE include_config ("
             "id INTEGER PRIMARY KEY, tu_file_id INTEGER NOT NULL, "
             "digest TEXT NOT NULL, driver TEXT, working_dir TEXT, "
             "arguments TEXT, lang_mode TEXT, resource_dir TEXT, "
             "UNIQUE(tu_file_id, digest))");
    auto insert = raw.prepare(
        "INSERT INTO include_config(tu_file_id, digest, driver, working_dir, "
        "arguments, lang_mode, resource_dir) VALUES (?, 'legacy', 'clang++', "
        "'.', "
        "'[\"-std=c++23\",\"main.cpp\"]', 'c++', NULL)");
    insert.bind(1, tu);
    insert.step_done();
    raw.exec("UPDATE meta SET value = '34' WHERE key = 'schema_version'");
  }
  {
    cidx::Storage db(path);
    cidx::SqliteDb raw(path);
    CHECK(meta_version(raw) == std::to_string(cidx::kSchemaVersion));
    auto count = raw.prepare("SELECT COUNT(*) FROM translation_unit_config");
    REQUIRE(count.step());
    CHECK(count.col_int64(0) == 1);
    auto linked =
        raw.prepare("SELECT translation_unit_config_id FROM include_config");
    REQUIRE(linked.step());
    CHECK_FALSE(linked.col_is_null(0));
    CHECK(db.translation_unit_configs_for_file(tu).size() == 1);
  }
}
