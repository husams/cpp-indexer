// v39 symbol identity scope acceptance tests.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <string>
#include <unistd.h>
#include <vector>

#include "ast/storage_edge_sink.hpp"
#include "ast/storage_symbol_sink.hpp"
#include "storage/ports.hpp"
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

} // namespace

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

  cidx::storage::AstStoragePorts ports{
      db.workspace_catalog_read(), db.source_read(), db.symbol_read(),
      db.symbol_write(),           db.type_write(),  db.fact_write(),
      db.definition_write(),       db.unit_of_work()};
  cidx::ast::StorageEdgeSink sink(ports);
  sink.set_current_file_id(banking_file);
  check_condition(sink.lookup_symbol_id("c:@N@collision") == banking_id);
  sink.set_current_file_id(composed_file);
  check_condition(sink.lookup_symbol_id("c:@N@collision") == composed_id);

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

TEST_CASE("v39 carries translation-unit identity through header sinks") {
  cidx::Storage db(":memory:");
  const auto universe = db.add_semantic_universe("program:banking");
  const auto repo =
      db.add_repository("banking", "repo", std::nullopt, universe);
  const auto component = db.add_component("banking", "/tmp/cidx-banking");
  db.set_component_repository(component, repo);
  const auto dir = db.add_directory(component, "");
  const auto header = db.add_file(dir, "shared.hpp");
  const auto tu_a =
      db.add_file(dir, "a.cpp", std::nullopt, std::nullopt,
                  std::vector<std::string>{"-DCONFIG_A"}, "clang++");
  const auto tu_b =
      db.add_file(dir, "b.cpp", std::nullopt, std::nullopt,
                  std::vector<std::string>{"-DCONFIG_B"}, "clang++");
  const auto header_path = db.file_abs_path(header);
  REQUIRE(header_path.has_value());
  cidx::TranslationUnitConfig config_a;
  config_a.driver = "clang++";
  config_a.working_dir = "/workspace";
  config_a.language = "c++";
  config_a.standard = "c++23";
  config_a.target = "x86_64-linux-gnu";
  config_a.sysroot = "/sdk/x86";
  config_a.resource_dir = "/clang/resource";
  config_a.include_paths = {"/workspace/include"};
  config_a.macro_state = {"CONFIG_A"};
  config_a.relevant_environment = {"SDKROOT=/sdk"};
  config_a.generated_inputs = {"generated/config.h"};
  config_a.diagnostics_policy = "error-limit=0";
  config_a.arguments = {"-std=c++23", "-DCONFIG_A"};
  auto config_b = config_a;
  config_b.target = "aarch64-linux-gnu";
  config_b.sysroot = "/sdk/arm64";
  config_b.macro_state = {"CONFIG_B"};
  config_b.generated_inputs = {"generated/config-arm.h"};
  const auto config_a_id = db.add_translation_unit_config(config_a);
  const auto config_b_id = db.add_translation_unit_config(config_b);

  cidx::ast::SymbolRecord record;
  record.file = *header_path;
  record.usr = "c:@F@header_local";
  record.spelling = "header_local";
  record.kind = 8; // CXCursor_FunctionDecl
  record.line = 1;
  record.col = 1;
  record.end_line = 1;
  record.end_col = 13;
  record.linkage = "internal";
  record.is_definition = true;
  record.resolved = true;

  cidx::storage::AstStoragePorts ports{
      db.workspace_catalog_read(), db.source_read(), db.symbol_read(),
      db.symbol_write(),           db.type_write(),  db.fact_write(),
      db.definition_write(),       db.unit_of_work()};
  cidx::ast::StorageSymbolSink symbols(ports);
  symbols.set_current_file_id(tu_a);
  symbols.set_identity_translation_unit_config_id(config_a_id, tu_a);
  symbols.emit(record);
  symbols.set_current_file_id(tu_b);
  symbols.set_identity_translation_unit_config_id(config_b_id, tu_b);
  symbols.emit(record);
  const auto rows = db.lookup_symbols_by_usr(record.usr, universe);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].identity_key != rows[1].identity_key);

  cidx::ast::StorageEdgeSink edges(ports);
  edges.set_current_file_id(tu_a);
  edges.set_identity_translation_unit_config_id(config_a_id, tu_a);
  const auto a_id = edges.lookup_symbol_id(record.usr, *header_path);
  edges.set_current_file_id(tu_b);
  edges.set_identity_translation_unit_config_id(config_b_id, tu_b);
  const auto b_id = edges.lookup_symbol_id(record.usr, *header_path);
  REQUIRE(a_id.has_value());
  REQUIRE(b_id.has_value());
  CHECK(*a_id != *b_id);
  CHECK(db.update_symbol(
      record.usr, {{"spelling", std::string("A")}}, universe, *header_path,
      db.portable_translation_unit_identity_for_config(config_a_id, tu_a)));
  CHECK(db.lookup_symbol_by_id(*a_id)->spelling == "A");
  CHECK(db.lookup_symbol_by_id(*b_id)->spelling == "header_local");

  cidx::ast::MintRequest request;
  request.usr = "c:@F@header_stub";
  request.spelling = "header_stub";
  request.kind_name = "function";
  request.decl_file_id = header;
  request.decl_line = 2;
  request.decl_col = 1;
  request.identity_source = *header_path;
  request.linkage = "no-linkage";
  edges.set_current_file_id(tu_a);
  edges.set_identity_translation_unit_config_id(config_a_id, tu_a);
  const auto stub_a = edges.mint_symbol(request);
  edges.set_current_file_id(tu_b);
  edges.set_identity_translation_unit_config_id(config_b_id, tu_b);
  const auto stub_b = edges.mint_symbol(request);
  CHECK(stub_a != stub_b);
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
  check_condition(version.col_text(0) == "42");
  auto edge = raw.prepare("SELECT src_id, dst_id FROM edge WHERE id = 11");
  REQUIRE(edge.step());
  check_condition(edge.col_int64(0) == 7);
  check_condition(edge.col_int64(1) == 7);
}
