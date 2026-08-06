// v39 symbol identity scope acceptance tests.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "storage/sqlite.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"

namespace {

std::string temp_db() {
  std::array<char, 64> tmpl = {};
  std::string pattern = "/tmp/cidx_identity_XXXXXX";
  std::ranges::copy(pattern, tmpl.begin());
  const int fd = ::mkstemp(tmpl.data());
  const bool valid_fd = fd >= 0;
  REQUIRE(valid_fd);
  ::close(fd);
  return tmpl.data();
}

void check_condition(const bool condition) { CHECK(condition); }

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

using TableRows = std::vector<std::vector<std::optional<std::string>>>;

TableRows table_rows(cidx::Storage &db, std::string_view table) {
  TableRows rows;
  std::string order = "id";
  if (table == "edge_site") {
    order = "edge_id, file_id, line, col";
  } else if (table == "call_arg") {
    order = "edge_id, file_id, line, col, position";
  }
  auto statement = db.raw_db().prepare("SELECT * FROM " + std::string(table) +
                                       " ORDER BY " + order);
  while (statement.step()) {
    auto &row = rows.emplace_back();
    for (int column = 0; column < statement.column_count(); ++column) {
      row.push_back(statement.col_is_null(column)
                        ? std::nullopt
                        : std::optional(statement.col_text(column)));
    }
  }
  return rows;
}

struct ReconciliationFixtureResult {
  std::map<std::string, TableRows> tables;
  cidx::ExternalIdentityReconciliationMetrics metrics;
};

ReconciliationFixtureResult
run_reconciliation_fixture(cidx::ExternalIdentityReconciliationMode mode) {
  cidx::Storage db(":memory:");
  db.set_external_identity_reconciliation_mode_for_testing(mode);

  const auto first_universe = db.add_semantic_universe("fixture:first");
  const auto second_universe = db.add_semantic_universe("fixture:second");
  const auto first_repository =
      db.add_repository("fixture-first", "repo", std::nullopt, first_universe);
  const auto second_repository = db.add_repository(
      "fixture-second", "repo", std::nullopt, second_universe);
  const auto first_component =
      db.add_component("fixture-first", "/tmp/cidx-fixture-first");
  const auto second_component =
      db.add_component("fixture-second", "/tmp/cidx-fixture-second");
  db.set_component_repository(first_component, first_repository);
  db.set_component_repository(second_component, second_repository);
  const auto first_file = file_for(db, first_component, "fixture.cpp");
  const auto second_file = file_for(db, second_component, "fixture.cpp");

  auto caller = external_symbol("c:@F@fixture_caller#", first_file);
  caller.kind = "function";
  const auto caller_id = db.add_symbol(caller);
  auto target = external_symbol("c:@F@fixture_target#", first_file);
  target.kind = "function";
  const auto target_id = db.add_symbol(target);
  cidx::Edge edge;
  edge.src_id = caller_id;
  edge.dst_id = target_id;
  edge.kind = 1;
  const auto edge_id = db.add_edge(edge);

  constexpr std::string_view duplicate_usr = "c:@F@duplicate_fixture#";
  constexpr std::string_view multi_universe_usr =
      "c:@F@multi_universe_fixture#";
  cidx::EdgeSite site;
  site.edge_id = edge_id;
  site.file_id = first_file;
  site.line = 10;
  site.col = 1;
  site.recv_src_kind = std::nullopt;
  site.recv_decl_usr = std::string(duplicate_usr);
  db.add_edge_site(site);
  cidx::CallArg argument;
  argument.edge_id = edge_id;
  argument.file_id = first_file;
  argument.line = 10;
  argument.col = 1;
  argument.position = 0;
  argument.src_kind = "local";
  argument.decl_usr = std::string(duplicate_usr);
  argument.callee_usr = std::string(multi_universe_usr);
  db.add_call_arg(argument);
  cidx::TypeNode type;
  type.type_key = "record:duplicate_fixture";
  type.spelling = "duplicate_fixture";
  type.kind = cidx::kTypeKindRecord;
  type.decl_usr = std::string(duplicate_usr);
  db.intern_type_node(type);

  const auto emit = [&db](const cidx::Symbol &symbol) {
    const auto id = db.add_symbol(symbol);
    db.add_decl_site(id, symbol);
    return id;
  };
  {
    auto transaction = db.transaction();
    auto duplicate_first =
        external_symbol(std::string(duplicate_usr), first_file);
    duplicate_first.linkage = "no-linkage";
    duplicate_first.identity_translation_unit = "fixture:tu:first";
    duplicate_first.line = 20;
    duplicate_first.col = 1;
    emit(duplicate_first);

    auto multi_first =
        external_symbol(std::string(multi_universe_usr), first_file);
    multi_first.line = 21;
    multi_first.col = 1;
    emit(multi_first);

    auto duplicate_second =
        external_symbol(std::string(duplicate_usr), first_file);
    duplicate_second.linkage = "no-linkage";
    duplicate_second.identity_translation_unit = "fixture:tu:second";
    duplicate_second.line = 22;
    duplicate_second.col = 1;
    emit(duplicate_second);

    auto multi_second =
        external_symbol(std::string(multi_universe_usr), second_file);
    multi_second.line = 23;
    multi_second.col = 1;
    emit(multi_second);
    transaction.commit();
  }

  auto duplicate_count = db.raw_db().prepare(
      "SELECT COUNT(*), COUNT(DISTINCT file_id) FROM symbol WHERE usr = ?");
  duplicate_count.bind(1, duplicate_usr);
  REQUIRE(duplicate_count.step());
  CHECK(duplicate_count.col_int64(0) == 2);
  CHECK(duplicate_count.col_int64(1) == 1);
  auto universe_count = db.raw_db().prepare(
      "SELECT COUNT(*), COUNT(DISTINCT semantic_universe_id) FROM symbol "
      "WHERE usr = ?");
  universe_count.bind(1, multi_universe_usr);
  REQUIRE(universe_count.step());
  CHECK(universe_count.col_int64(0) == 2);
  CHECK(universe_count.col_int64(1) == 2);

  ReconciliationFixtureResult result;
  for (const std::string table :
       {"external_identity", "type_node", "edge_site", "call_arg"}) {
    result.tables.emplace(table, table_rows(db, table));
  }
  result.metrics = db.external_identity_reconciliation_metrics();
  return result;
}

} // namespace

TEST_CASE("batched identity reconciliation preserves schema-40 ordering") {
  const auto immediate = run_reconciliation_fixture(
      cidx::ExternalIdentityReconciliationMode::immediate);
  const auto batched = run_reconciliation_fixture(
      cidx::ExternalIdentityReconciliationMode::batched);

  CHECK(batched.tables == immediate.tables);
  CHECK(batched.metrics.emissions == immediate.metrics.emissions);
  CHECK(batched.metrics.distinct_identities ==
        immediate.metrics.distinct_identities);
  CHECK(batched.metrics.calls == 1);
  CHECK(batched.metrics.calls < immediate.metrics.calls);
  CHECK(batched.metrics.prepared_statements <
        immediate.metrics.prepared_statements);
  CHECK(batched.metrics.vdbe_steps < immediate.metrics.vdbe_steps);
}

TEST_CASE("batched identity reconciliation failure rolls back the whole TU") {
  cidx::Storage db(":memory:");
  const auto component = db.add_component("rollback", "/tmp/cidx-rollback");
  const auto file = file_for(db, component, "rollback.cpp");

  bool failed = false;
  try {
    auto transaction = db.transaction();
    auto symbol = external_symbol("c:@F@rollback_fixture#", file);
    symbol.kind = "function";
    symbol.line = 1;
    symbol.col = 1;
    db.add_symbol(symbol);
    db.raw_db().exec("UPDATE file SET indexed = 1 WHERE id = " +
                     std::to_string(file));
    db.inject_external_identity_reconciliation_failure_for_testing(4);
    transaction.commit();
  } catch (const cidx::StorageError &) {
    failed = true;
  }
  CHECK(failed);

  CHECK(db.lookup_symbols_by_usr("c:@F@rollback_fixture#").empty());
  auto indexed = db.raw_db().prepare("SELECT indexed FROM file WHERE id = ?");
  indexed.bind(1, file);
  REQUIRE(indexed.step());
  CHECK(indexed.col_int64(0) == 0);
  auto integrity = db.raw_db().prepare("PRAGMA integrity_check");
  REQUIRE(integrity.step());
  CHECK(integrity.col_text(0) == "ok");
  auto foreign_keys = db.raw_db().prepare("PRAGMA foreign_key_check");
  CHECK_FALSE(foreign_keys.step());
}

TEST_CASE("v39 isolates unrelated universes and merges declared sharing") {
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
  check_condition(banking_id != composed_id);
  check_condition(db.lookup_symbols_by_usr("c:@N@collision").size() == 2);
  CHECK_THROWS(db.lookup_symbol("c:@N@collision"));
  const auto banking_match = db.lookup_symbol("c:@N@collision", banking);
  const auto composed_match = db.lookup_symbol("c:@N@collision", composed);
  REQUIRE(banking_match.has_value());
  REQUIRE(composed_match.has_value());
  const auto banking_match_value = banking_match.value_or(cidx::Symbol{});
  const auto composed_match_value = composed_match.value_or(cidx::Symbol{});
  check_condition(banking_match_value.id == banking_id);
  check_condition(composed_match_value.id == composed_id);
  CHECK_THROWS(
      db.update_symbol("c:@N@collision", {{"spelling", std::string("X")}}));
  CHECK(db.update_symbol("c:@N@collision", {{"spelling", std::string("B")}},
                         banking));
  const auto banking_updated = db.lookup_symbol_by_id(banking_id);
  const auto composed_updated = db.lookup_symbol_by_id(composed_id);
  REQUIRE(banking_updated.has_value());
  REQUIRE(composed_updated.has_value());
  const auto banking_updated_value = banking_updated.value_or(cidx::Symbol{});
  const auto composed_updated_value = composed_updated.value_or(cidx::Symbol{});
  check_condition(banking_updated_value.spelling == "B");
  check_condition(composed_updated_value.spelling == "Thing");

  const auto composed_clone_repo =
      db.add_repository("composed-twin", "repo", std::nullopt, banking);
  const auto composed_clone =
      db.add_component("composed-twin", "/tmp/composed-twin");
  db.set_component_repository(composed_clone, composed_clone_repo);
  const auto shared_file = file_for(db, composed_clone, "collision.hpp");
  const auto shared_id =
      db.add_symbol(external_symbol("c:@N@collision", shared_file));
  check_condition(shared_id == banking_id);
  check_condition(db.lookup_symbols_by_usr("c:@N@collision").size() == 2);

  auto internal_a = external_symbol("c:@F@hidden", banking_file);
  internal_a.linkage = "internal";
  auto internal_b = external_symbol("c:@F@hidden", shared_file);
  internal_b.linkage = "internal";
  const auto internal_id_a = db.add_symbol(internal_a);
  const auto internal_id_b = db.add_symbol(internal_b);
  check_condition(internal_id_a != internal_id_b);
  check_condition(db.lookup_symbols_by_usr("c:@F@hidden", banking).size() == 2);

  auto no_linkage_a = external_symbol("c:@F@local", banking_file);
  no_linkage_a.linkage = "no-linkage";
  auto no_linkage_b = external_symbol("c:@F@local", shared_file);
  no_linkage_b.linkage = "no-linkage";
  check_condition(db.add_symbol(no_linkage_a) != db.add_symbol(no_linkage_b));

  const auto banking_symbol = db.lookup_symbol_by_id(banking_id);
  const auto composed_symbol = db.lookup_symbol_by_id(composed_id);
  REQUIRE(banking_symbol.has_value());
  REQUIRE(composed_symbol.has_value());
  if (banking_symbol && composed_symbol) {
    CHECK(banking_symbol->semantic_universe_id == banking);
    CHECK(composed_symbol->semantic_universe_id == composed);
    CHECK(banking_symbol->identity_key != composed_symbol->identity_key);
  }

  const auto banking_lookup = db.lookup_symbol("c:@N@collision", banking);
  const auto composed_lookup = db.lookup_symbol("c:@N@collision", composed);
  REQUIRE(banking_lookup.has_value());
  REQUIRE(composed_lookup.has_value());
  check_condition(banking_lookup->id == banking_id);
  check_condition(composed_lookup->id == composed_id);

  const auto preserved = db.add_repository("composed", "repo", std::nullopt);
  check_condition(preserved == composed_repo);
  const auto preserved_repo = db.get_repository_by_id(preserved);
  REQUIRE(preserved_repo.has_value());
  const auto preserved_repo_value = preserved_repo.value_or(cidx::Repository{});
  check_condition(preserved_repo_value.semantic_universe_id == composed);
}

TEST_CASE("v35 local identity is stable across file insertion order") {
  const auto make_key = [](bool add_filler) {
    cidx::Storage db(":memory:");
    const auto repo = db.add_repository("clone", "repo", "ssh://example/clone");
    const auto component = db.add_component("clone", "/tmp/cidx-stable-root");
    db.set_component_repository(component, repo);
    const auto dir = db.add_directory(component, "");
    if (add_filler) {
      db.add_file(dir, "unrelated.cpp");
    }
    const auto file = db.add_file(dir, "stable.cpp");
    auto sym = external_symbol("c:@F@hidden", file);
    sym.linkage = "internal";
    const auto id = db.add_symbol(sym);
    return db.lookup_symbol_by_id(id)->identity_key;
  };

  check_condition(make_key(false) == make_key(true));
}

TEST_CASE("source identity cache follows component repository ownership") {
  cidx::Storage db(":memory:");
  const std::string component_root = "/tmp/cidx-source-identity-cache";
  const std::string source_path = component_root + "/src/unit.cpp";
  const auto component = db.add_component("component", component_root, "repo");
  const auto repository = db.add_repository(
      "repo", "repo", std::string("https://example.test/repo.git"));

  const auto ungrouped = db.portable_source_identity_for_path(source_path);
  CHECK(ungrouped.starts_with("component:"));

  db.set_component_repository(component, repository);
  const auto attached = db.portable_source_identity_for_path(source_path);
  CHECK(attached.starts_with("remote:https://example.test/repo.git"));
  CHECK(attached != ungrouped);

  db.set_component_repository(component, std::nullopt);
  const auto detached = db.portable_source_identity_for_path(source_path);
  CHECK(detached == ungrouped);
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
      INSERT INTO symbol (id, usr, spelling, kind, file_id, linkage)
        VALUES (8, 'c:@F@legacy_local', 'legacy_local', 8, 42, 'internal');
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
    const auto local = db.lookup_symbol_by_id(8);
    REQUIRE(local.has_value());
    if (local) {
      CHECK(local->identity_key.find("file:") == std::string::npos);
      CHECK(local->identity_key ==
            std::string("legacy") + char(31) + "local:legacy" + char(31) +
                "source:unknown" + char(31) + "c:@F@legacy_local");
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
  check_condition(version.col_text(0) ==
                  std::to_string(cidx::kSchemaVersion));
  auto edge = raw.prepare("SELECT src_id, dst_id FROM edge WHERE id = 11");
  REQUIRE(edge.step());
  check_condition(edge.col_int64(0) == 7);
  check_condition(edge.col_int64(1) == 7);
}
