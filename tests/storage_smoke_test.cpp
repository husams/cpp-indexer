// storage_smoke_test — assertion-for-assertion PORT of
// project/indexer/_storage_smoke.py (the executable spec for design §3.4/§3.5,
// G13–G18). Same order as the Python file, including the reopen-persistence
// check and the update_symbol unknown-column / bad-kind throws. Extra
// TEST_CASEs pin the fresh schema-v6 shape (story acceptance) and the SQL
// CHECK rejection of a bad symbol kind (§3.2: rejected by BOTH layers).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "storage/records.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"

namespace {

std::string make_temp_dir() {
  char tmpl[] = "/tmp/cidx_storage_XXXXXX";
  char *d = ::mkdtemp(tmpl);
  REQUIRE(d != nullptr);
  return d;
}

TEST_CASE("parent_id backfills when parent arrives after child") {
  cidx::Storage db(":memory:");
  cidx::Symbol child;
  child.usr = "late:child";
  child.spelling = "Child";
  child.kind = "struct";
  child.parent_usr = "late:parent";
  const auto child_id = db.add_symbol(child);

  auto before =
      db.raw_db().prepare("SELECT parent_id FROM symbol WHERE id = ?");
  before.bind(1, child_id);
  REQUIRE(before.step());
  CHECK(before.col_is_null(0));

  cidx::Symbol parent;
  parent.usr = "late:parent";
  parent.spelling = "Parent";
  parent.kind = "struct";
  const auto parent_id = db.add_symbol(parent);

  auto after = db.raw_db().prepare("SELECT parent_id FROM symbol WHERE id = ?");
  after.bind(1, child_id);
  REQUIRE(after.step());
  CHECK(after.col_int64(0) == parent_id);

  db.add_symbol(parent);
  after = db.raw_db().prepare("SELECT parent_id FROM symbol WHERE id = ?");
  after.bind(1, child_id);
  REQUIRE(after.step());
  CHECK(after.col_int64(0) == parent_id);
}

void makedirs(const std::string &path) {
  std::string cur;
  for (std::size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (!cur.empty()) {
        ::mkdir(cur.c_str(), 0755);
      }
    }
    if (i < path.size()) {
      cur += path[i];
    }
  }
}

std::vector<std::string> usrs_of(const std::vector<cidx::Symbol> &syms) {
  std::vector<std::string> out;
  for (const auto &s : syms) {
    out.push_back(s.usr);
  }
  return out;
}

} // namespace

TEST_CASE("storage smoke (port of _storage_smoke.py)") {
  const std::string tmp = make_temp_dir();
  const std::string repo = tmp + "/myrepo";
  makedirs(repo + "/src");
  const std::string db_path = tmp + "/index.db";

  cidx::Stats st;
  {
    cidx::Storage db(db_path);

    // -- components --------------------------------------------------
    const int64_t comp = db.add_component("myrepo", repo);
    CHECK_MESSAGE(db.add_component("myrepo", repo) == comp,
                  "idempotent on path");
    const int64_t ext = db.add_component("libc", "/usr/include", "external");
    CHECK(ext != comp);
    REQUIRE(db.get_component(repo).has_value());
    CHECK(db.get_component(repo)->name == "myrepo");
    REQUIRE(db.component_for_path(repo + "/src/a.c").has_value());
    CHECK(db.component_for_path(repo + "/src/a.c")->id == comp);

    // -- directories -------------------------------------------------
    const int64_t d_src = db.add_directory(comp, "src");
    CHECK_MESSAGE(db.add_directory(comp, "src") == d_src, "idempotent");
    db.add_directory(comp, ""); // d_root
    REQUIRE(db.get_directory(comp, "src").has_value());
    CHECK(db.get_directory(comp, "src")->id == d_src);

    // -- files ---------------------------------------------------------
    const std::vector<std::string> opts = {"-I.", "-DDEBUG"};
    const int64_t f1 =
        db.add_file(d_src, "a.c", 100.0, std::string("aaa"), opts);
    CHECK_MESSAGE(db.add_file(d_src, "a.c") == f1, "idempotent");
    const std::string a_c = repo + "/src/a.c";
    CHECK_MESSAGE(db.add_file_path(a_c) == f1,
                  "path convenience resolves to same row");
    REQUIRE(db.file_abs_path(f1).has_value());
    CHECK(*db.file_abs_path(f1) == a_c);

    auto rec = db.get_file(a_c);
    REQUIRE(rec.has_value());
    REQUIRE(rec->compile_options.has_value());
    CHECK_MESSAGE(*rec->compile_options == opts, "options round-trip");
    CHECK(rec->md5 == std::string("aaa"));
    CHECK_FALSE(rec->indexed);

    CHECK_MESSAGE(!db.is_file_indexed(a_c), "not indexed yet");
    db.mark_file_indexed(f1, 100.0);
    CHECK(db.is_file_indexed(a_c));
    CHECK_MESSAGE(db.is_file_indexed(a_c, 100.0), "fresh");
    CHECK_MESSAGE(!db.is_file_indexed(a_c, 200.0), "stale mtime -> reindex");
    CHECK_MESSAGE(db.is_file_indexed(a_c, std::nullopt, std::string("aaa")),
                  "same content");
    CHECK_MESSAGE(!db.is_file_indexed(a_c, std::nullopt, std::string("bbb")),
                  "changed content -> reindex");
    CHECK_MESSAGE(!db.is_file_indexed("/nowhere/else.c"), "unknown component");

    // re-import with a new md5 resets the indexed flag (G13)
    db.add_file(d_src, "a.c", std::nullopt, std::string("ccc"));
    CHECK_MESSAGE(!db.is_file_indexed(a_c), "content change clears indexed");
    db.mark_file_indexed(f1);
    CHECK(db.is_file_indexed(a_c));

    // -- symbols -------------------------------------------------------
    cidx::Symbol decl;
    decl.usr = "c:@F@multiply";
    decl.spelling = "multiply";
    decl.kind = "function";
    decl.type_info = "int (int, int)";
    decl.file_id = f1;
    decl.line = 3;
    decl.col = 5;
    decl.decl_file_id = f1;
    decl.decl_line = 3;
    decl.decl_col = 5;
    decl.is_definition = false;
    const int64_t sid = db.add_symbol(decl);
    REQUIRE(db.lookup_symbol("c:@F@multiply").has_value());
    CHECK_FALSE(db.lookup_symbol("c:@F@multiply")->is_definition);

    // definition upserts over the declaration (same USR, same row);
    // the declaration site recorded earlier survives alongside it
    cidx::Symbol defn;
    defn.usr = "c:@F@multiply";
    defn.spelling = "multiply";
    defn.kind = "function";
    defn.type_info = "int (int, int)";
    defn.file_id = f1;
    defn.line = 10;
    defn.col = 1;
    defn.is_definition = true;
    defn.resolved = true;
    CHECK_MESSAGE(db.add_symbol(defn) == sid, "USR upsert, not a new row");
    auto got = db.lookup_symbol("c:@F@multiply");
    REQUIRE(got.has_value());
    CHECK(got->is_definition);
    CHECK(got->resolved);
    CHECK(got->line == 10);
    CHECK_MESSAGE(got->decl_line == 3,
                  "decl site survives the definition upsert");

    // a later declaration must NOT downgrade the stored definition's location
    db.add_symbol(decl);
    got = db.lookup_symbol("c:@F@multiply");
    REQUIRE(got.has_value());
    CHECK_MESSAGE(got->line == 10, "definition wins");
    CHECK_MESSAGE(got->decl_line == 3, "decl site stays");

    // qual_name: stored, upsert-preserved, and fuzzy-searchable
    cidx::Symbol set_sym;
    set_sym.usr = "c:@N@rk@S@Conf@F@set";
    set_sym.spelling = "set";
    set_sym.kind = "method";
    set_sym.qual_name = "rk::Conf::set";
    set_sym.parent_usr = "c:@N@rk@S@Conf";
    set_sym.is_pure = true;
    set_sym.is_static = true;
    set_sym.resolved = true;
    db.add_symbol(set_sym);
    got = db.lookup_symbol("c:@N@rk@S@Conf@F@set");
    REQUIRE(got.has_value());
    CHECK(got->qual_name == std::string("rk::Conf::set"));
    CHECK_MESSAGE(got->is_pure, "is_pure round-trips");
    CHECK_MESSAGE(got->is_static, "is_static round-trips");
    cidx::Symbol set_again;
    set_again.usr = "c:@N@rk@S@Conf@F@set";
    set_again.spelling = "set";
    set_again.kind = "method";
    set_again.resolved = true;
    db.add_symbol(set_again);
    got = db.lookup_symbol("c:@N@rk@S@Conf@F@set");
    REQUIRE(got.has_value());
    CHECK_MESSAGE(got->qual_name == std::string("rk::Conf::set"),
                  "NULL must not clobber qual_name");
    CHECK_MESSAGE(usrs_of(db.search_symbols("conf::set")) ==
                      std::vector<std::string>{"c:@N@rk@S@Conf@F@set"},
                  "segment fuzzy match");
    CHECK(db.search_symbols("conf::set", std::string("function")).empty());
    CHECK(db.search_symbols("nosuchthing").empty());

    // update_symbol
    CHECK(db.update_symbol(
        "c:@F@multiply",
        {{"display_name", cidx::SqlValue{std::string("multiply(int, int)")}}}));
    CHECK(db.lookup_symbol("c:@F@multiply")->display_name ==
          std::string("multiply(int, int)"));
    CHECK_FALSE(db.update_symbol("c:@F@missing",
                                 {{"resolved", cidx::SqlValue{int64_t{1}}}}));
    CHECK_THROWS_AS(db.update_symbol("c:@F@multiply",
                                     {{"bogus", cidx::SqlValue{int64_t{1}}}}),
                    cidx::StorageError); // unknown column must raise
    {
      cidx::Symbol bad;
      bad.usr = "x";
      bad.spelling = "x";
      bad.kind = "not-a-kind";
      CHECK_THROWS_AS(db.add_symbol(bad),
                      cidx::StorageError); // unknown kind must raise
    }

    // name lookup returns every row with that spelling
    cidx::Symbol other_mul;
    other_mul.usr = "c:a.c@F@multiply";
    other_mul.spelling = "multiply";
    other_mul.kind = "function";
    other_mul.is_definition = true;
    db.add_symbol(other_mul);
    const auto hits = db.lookup_symbols_by_name("multiply");
    CHECK(hits.size() == 2);
    CHECK(std::all_of(hits.begin(), hits.end(), [](const cidx::Symbol &h) {
      return h.spelling == "multiply";
    }));
    CHECK(db.lookup_symbols_by_name("multiply", std::string("struct")).empty());

    // bulk insert inside one transaction (explicit commit required — R2)
    {
      auto txn = db.transaction();
      for (int i = 0; i < 50; ++i) {
        cidx::Symbol s;
        s.usr = "c:@S@T" + std::to_string(i);
        s.spelling = "T" + std::to_string(i);
        s.kind = "struct";
        s.resolved = true;
        db.add_symbol(s);
      }
      txn.commit();
    }

    // unresolved + per-file views
    {
      const auto unresolved_usrs = usrs_of(db.unresolved_symbols());
      const std::set<std::string> got_usrs(unresolved_usrs.begin(),
                                           unresolved_usrs.end());
      CHECK(got_usrs == std::set<std::string>{"c:a.c@F@multiply"});
    }
    CHECK(usrs_of(db.symbols_in_file(f1)) ==
          std::vector<std::string>{"c:@F@multiply"});

    // -- by-id getters -----------------------------------------------------
    REQUIRE(db.get_component_by_id(comp).has_value());
    CHECK(db.get_component_by_id(comp)->name == "myrepo");
    CHECK_FALSE(db.get_component_by_id(99999).has_value());
    REQUIRE(db.get_directory_by_id(d_src).has_value());
    CHECK(db.get_directory_by_id(d_src)->path == "src");
    CHECK(db.get_directory_by_id(d_src)->component_id == comp);
    CHECK_FALSE(db.get_directory_by_id(99999).has_value());
    REQUIRE(db.get_file_by_id(f1).has_value());
    CHECK(db.get_file_by_id(f1)->name == "a.c");
    CHECK(db.get_file_by_id(f1)->directory_id == d_src);
    CHECK_FALSE(db.get_file_by_id(99999).has_value());

    // -- list views --------------------------------------------------------
    {
      std::vector<std::string> names;
      for (const auto &c : db.list_components()) {
        names.push_back(c.name);
      }
      CHECK(names == std::vector<std::string>{"libc", "myrepo"});
    }
    {
      std::vector<std::string> names;
      for (const auto &c : db.list_components(std::string("myrp"))) {
        names.push_back(c.name);
      }
      CHECK_MESSAGE(names == std::vector<std::string>{"myrepo"},
                    "fuzzy: chars in order");
    }
    {
      std::vector<std::string> names;
      for (const auto &c :
           db.list_components(std::nullopt, std::string("external"))) {
        names.push_back(c.name);
      }
      CHECK(names == std::vector<std::string>{"libc"});
    }
    CHECK(db.list_components(std::string("zzz")).empty());

    {
      const auto dirs = db.list_directories(comp);
      std::vector<std::pair<std::string, std::string>> got_dirs;
      for (const auto &[d, n] : dirs) {
        got_dirs.emplace_back(d.path, n);
      }
      CHECK(got_dirs == std::vector<std::pair<std::string, std::string>>{
                            {"", "myrepo"}, {"src", "myrepo"}});
    }
    {
      std::vector<std::string> paths;
      for (const auto &[d, n] :
           db.list_directories(std::nullopt, std::string("sr"))) {
        (void)n;
        paths.push_back(d.path);
      }
      CHECK(paths == std::vector<std::string>{"src"});
    }

    const auto paths_of =
        [](const std::vector<std::pair<cidx::File, std::string>> &rows) {
          std::vector<std::string> out;
          for (const auto &[f, p] : rows) {
            (void)f;
            out.push_back(p);
          }
          return out;
        };
    CHECK(paths_of(db.list_files(comp)) == std::vector<std::string>{a_c});
    CHECK(paths_of(db.list_files(comp, std::string("src"))) ==
          std::vector<std::string>{a_c});
    CHECK_MESSAGE(paths_of(db.list_files(comp, std::string(""))) ==
                      std::vector<std::string>{a_c},
                  "root subtree covers everything");
    CHECK(db.list_files(comp, std::string("other")).empty());
    CHECK_MESSAGE(paths_of(db.list_files(std::nullopt, std::nullopt,
                                         std::string("ac"))) ==
                      std::vector<std::string>{a_c},
                  "fuzzy name");
    CHECK(
        db.list_files(std::nullopt, std::nullopt, std::nullopt, false).empty());
    CHECK(paths_of(db.list_files(std::nullopt, std::nullopt, std::nullopt,
                                 true)) == std::vector<std::string>{a_c});

    CHECK_MESSAGE(usrs_of(db.list_symbols(comp)) ==
                      std::vector<std::string>{"c:@F@multiply"},
                  "scoped by definition/declaration site");
    CHECK(usrs_of(db.list_symbols(comp, std::string("src"))) ==
          std::vector<std::string>{"c:@F@multiply"});
    CHECK(usrs_of(db.list_symbols(std::nullopt, std::nullopt, f1)) ==
          std::vector<std::string>{"c:@F@multiply"});
    CHECK_MESSAGE(
        usrs_of(db.list_symbols(std::nullopt, std::nullopt, std::nullopt,
                                std::string("cfset"))) ==
            std::vector<std::string>{"c:@N@rk@S@Conf@F@set"},
        "fuzzy hits the qualified name");
    CHECK(db.list_symbols(std::nullopt, std::nullopt, std::nullopt,
                          std::nullopt, std::string("struct"))
              .size() == 50);
    CHECK(db.list_symbols(comp, std::nullopt, std::nullopt, std::nullopt,
                          std::string("struct"))
              .empty());

    // -- stats -----------------------------------------------------------
    st = db.stats();
    CHECK(st.components == 2);
    CHECK(st.files == 1);
    CHECK(st.files_indexed == 1);
    CHECK(st.symbols == 53);
    CHECK(st.symbols_by_kind == std::map<std::string, int64_t>{{"function", 2},
                                                               {"method", 1},
                                                               {"struct", 50}});
    CHECK(st.symbols_unresolved == 1);
  }

  // data survives reopen
  {
    cidx::Storage db(db_path);
    REQUIRE(db.lookup_symbol("c:@F@multiply").has_value());
    CHECK(db.lookup_symbol("c:@F@multiply")->display_name ==
          std::string("multiply(int, int)"));
  }

  // §3.2 (v16): the SQL CHECK on symbol.kind was dropped (kind is now an
  // INTEGER == CXCursorKind); validation is app-side only (the add_symbol throw
  // was asserted above). A raw insert bypassing add_symbol is no longer
  // rejected by a constraint.
  {
    cidx::SqliteDb raw(db_path);
    auto ok = raw.prepare(
        "INSERT INTO symbol (usr, spelling, kind) VALUES ('y', 'y', 8)");
    CHECK_NOTHROW(ok.step_done());
  }
}

TEST_CASE("fresh Storage produces schema v39 (file-backed and :memory:)") {
  // :memory: exercises the skip-mkdir branch; raw_db() lets us assert the
  // schema shape on the same connection.
  cidx::Storage db(":memory:");
  auto &raw = db.raw_db();

  // tables — v7 adds edge_kind, edge, edge_site, template_param, template_arg
  std::set<std::string> tables;
  {
    auto st = raw.prepare("SELECT name FROM sqlite_master WHERE type = 'table' "
                          "AND name NOT LIKE 'sqlite_%'");
    while (st.step()) {
      tables.insert(st.col_text(0));
    }
  }
  // v14 adds label; v15 adds diagnostic; v17 adds entity_edge +
  // entity_edge_kind; v23 adds repository + clone; v26 adds decl_site; v30 adds
  // the signature/ type tier
  // (type_kind/type_node/type_edge_kind/type_edge/parameter/
  // symbol_type_kind/symbol_type)
  // v38 adds the manifest-governed artifact tables; v39 adds scoped identity.
  CHECK(tables == std::set<std::string>{"meta",
                                        "semantic_universe",
                                        "component",
                                        "directory",
                                        "file",
                                        "symbol",
                                        "symbol_kind",
                                        "edge_kind",
                                        "edge",
                                        "edge_site",
                                        "template_param",
                                        "template_arg",
                                        "call_arg",
                                        "label",
                                        "diagnostic",
                                        "entity_edge_kind",
                                        "entity_edge",
                                        "entity_kind",
                                        "entity_node",
                                        "repository",
                                        "clone",
                                        "decl_site",
                                        "definition",
                                        "def_edge",
                                        "possible_call",
                                        "type_kind",
                                        "type_node",
                                        "type_edge_kind",
                                        "type_edge",
                                        "parameter",
                                        "symbol_type_kind",
                                        "symbol_type",
                                        "translation_unit_config",
                                        "translation_unit",
                                        "file_config",
                                        "fact_applicability",
                                        "external_identity",
                                        "include_config",
                                        "include_edge",
                                        "include_directive_kind",
                                        "include_site",
                                        "include_macro_use",
                                        "artifact",
                                        "artifact_relation",
                                        "artifact_identity_map",
                                        "artifact_lease",
                                        "artifact_pin"});

  // columns, in declared order (byte-compatible v6 layout)
  const auto cols = [&raw](const char *table) {
    std::vector<std::string> out;
    auto st = raw.prepare(std::string("PRAGMA table_info(") + table + ")");
    while (st.step()) {
      out.push_back(st.col_text(1));
    }
    return out;
  };
  CHECK(cols("meta") == std::vector<std::string>{"key", "value"});
  // v14 adds the version column to component; v23 adds repository_id; v35
  // adds semantic_universe_id.
  CHECK(cols("component") ==
        std::vector<std::string>{"id", "name", "path", "kind", "version",
                                 "repository_id", "semantic_universe_id"});
  CHECK(cols("directory") ==
        std::vector<std::string>{"id", "component_id", "path"});
  CHECK(cols("file") ==
        std::vector<std::string>{"id", "directory_id", "name", "mtime", "md5",
                                 "compile_options", "driver", "indexed",
                                 "indexed_at", "args_overridden"});
  CHECK(cols("symbol") == std::vector<std::string>{"id",
                                                   "usr",
                                                   "spelling",
                                                   "qual_name",
                                                   "display_name",
                                                   "kind",
                                                   "type_info",
                                                   "file_id",
                                                   "line",
                                                   "col",
                                                   "end_line",
                                                   "end_col",
                                                   "decl_file_id",
                                                   "decl_line",
                                                   "decl_col",
                                                   "decl_path",
                                                   "is_definition",
                                                   "is_pure",
                                                   "is_static",
                                                   "is_instantiation",
                                                   "is_named_instance",
                                                   "linkage",
                                                   "access",
                                                   "parent_usr",
                                                   "parent_id",
                                                   "resolved",
                                                   "multi_def",
                                                   "const_value",
                                                   "semantic_universe_id",
                                                   "identity_key"});

  // the indexes (5 symbol + 2 edge + 1 call_arg + 1 diagnostic)
  std::set<std::string> indexes;
  {
    auto st = raw.prepare("SELECT name FROM sqlite_master WHERE type = 'index' "
                          "AND name LIKE 'idx_%'");
    while (st.step()) {
      indexes.insert(st.col_text(0));
    }
  }
  CHECK(indexes == std::set<std::string>{"idx_symbol_spelling",
                                         "idx_symbol_qual",
                                         "idx_symbol_file",
                                         "idx_symbol_parent",
                                         "idx_symbol_parent_id",
                                         "idx_symbol_kind",
                                         "idx_symbol_spelling_nc",
                                         "idx_symbol_qual_nc",
                                         "idx_symbol_usr",
                                         "idx_symbol_scope",
                                         "idx_symbol_identity",
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
                                         "idx_type_node_decl_id",
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
                                         "idx_include_macro_use_path",
                                         "idx_external_identity_symbol",
                                         "idx_external_identity_type",
                                         "idx_edge_site_recv_type_identity",
                                         "idx_edge_site_recv_decl_identity",
                                         "idx_call_arg_type_identity",
                                         "idx_call_arg_decl_identity",
                                         "idx_call_arg_callee_identity",
                                         "idx_artifact_current_logical",
                                         "idx_artifact_state",
                                         "idx_artifact_identity_stable"});

  // meta row + pragma parity (D25: foreign_keys ON, default journal mode)
  {
    auto st =
        raw.prepare("SELECT value FROM meta WHERE key = 'schema_version'");
    REQUIRE(st.step());
    CHECK(st.col_text(0) == std::to_string(cidx::kSchemaVersion));
  }
  {
    auto st = raw.prepare("PRAGMA foreign_keys");
    REQUIRE(st.step());
    CHECK(st.col_int64(0) == 1);
  }

  // FK actions are part of the byte-compatible DDL: spot-check via the stored
  // SQL text of the symbol table.
  {
    auto st = raw.prepare("SELECT sql FROM sqlite_master WHERE type = 'table' "
                          "AND name = 'symbol'");
    REQUIRE(st.step());
    const std::string ddl = st.col_text(0);
    CHECK(ddl.find("ON DELETE SET NULL") != std::string::npos);
    // v16: kind is an INTEGER (CXCursorKind); the old name CHECK list is gone.
    CHECK(ddl.find("kind         INTEGER NOT NULL") != std::string::npos);
  }
  {
    auto st = raw.prepare(
        "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'file'");
    REQUIRE(st.step());
    CHECK(st.col_text(0).find("ON DELETE CASCADE") != std::string::npos);
  }

  // all 17 kinds pass the CHECK (incl. the unreachable-but-stored 'macro')
  for (const char *kind :
       {"class", "struct", "union", "function", "method", "member",
        "constructor", "destructor", "enum", "enum-constant", "typedef",
        "type-alias", "class-template", "function-template", "variable",
        "namespace", "macro"}) {
    cidx::Symbol s;
    s.usr = std::string("c:kind@") + kind;
    s.spelling = "k";
    s.kind = kind;
    CHECK_NOTHROW(db.add_symbol(s));
  }
}

TEST_CASE("transaction rolls back on exception, commits on clean exit") {
  cidx::Storage db(":memory:");
  const int64_t comp = db.add_component("c", "/data/c");
  const int64_t dir = db.add_directory(comp, "");

  // rollback path: the Transaction dtor runs during unwind
  try {
    auto txn = db.transaction();
    db.add_file(dir, "rolled-back.c");
    throw std::runtime_error("boom");
  } catch (const std::runtime_error &) {
  }
  CHECK(db.list_files().empty());

  // commit path: explicit txn.commit() required (R2: dtor is rollback-only)
  {
    auto txn = db.transaction();
    db.add_file(dir, "kept.c");
    txn.commit();
  }
  CHECK(db.list_files().size() == 1);

  // explicit early commit
  {
    auto txn = db.transaction();
    db.add_file(dir, "kept2.c");
    txn.commit();
  }
  CHECK(db.list_files().size() == 2);
}

TEST_CASE("commit() propagates failure — not silently swallowed (R2)") {
  // Regression: before R2, Transaction::~Transaction swallowed all COMMIT
  // errors; a disk-full or busy COMMIT would return success and leave the DB
  // in an inconsistent state. Now ~Transaction is rollback-only; callers must
  // call txn.commit() explicitly, and a failing commit() throws.
  //
  // We trigger a synthetic COMMIT failure by manually issuing a ROLLBACK on
  // the underlying connection (bypassing Transaction::done_). SQLite then
  // returns SQLITE_ERROR ("cannot commit - no transaction is active") on the
  // subsequent COMMIT — identical in kind to a SQLITE_FULL / SQLITE_IOERR that
  // would occur on disk full.
  cidx::Storage db(":memory:");
  const int64_t comp = db.add_component("c2", "/data/c2");
  const int64_t dir = db.add_directory(comp, "");

  {
    auto txn = db.transaction();
    db.add_file(dir, "will-not-be-kept.c");

    // Forcibly roll back the underlying SQLite transaction, simulating a
    // COMMIT-time failure (disk full / I/O error) without relying on OS state.
    db.raw_db().exec("ROLLBACK");

    // txn.commit() must now throw, not silently succeed.
    CHECK_THROWS_AS(txn.commit(), cidx::StorageError);
    // txn falls out of scope; ~Transaction sees done_=false but no active
    // transaction — the ROLLBACK in the dtor is harmless (SQLite ignores it).
  }

  // The file must NOT have been persisted (the underlying txn was rolled back).
  CHECK(db.list_files().empty());

  // After the failed commit, the Storage object must still be usable.
  {
    auto txn2 = db.transaction();
    db.add_file(dir, "recovery.c");
    txn2.commit();
  }
  CHECK(db.list_files().size() == 1);
}

// delete_component (import --force): the component, its directories and files
// (ON DELETE CASCADE) and every symbol indexed from those files (explicit --
// symbol file refs are ON DELETE SET NULL) must vanish, leaving other
// components fully intact.
TEST_CASE("diagnostics: replace/get/counts, refresh, locationless, cascade") {
  cidx::Storage db(":memory:");
  const int64_t comp = db.add_component("d", "/repo/d");
  const int64_t dir = db.add_directory(comp, "");
  const int64_t fid = db.add_file(dir, "a.c");

  auto mk = [](int sev, std::string spelling, std::optional<std::string> path,
               std::optional<int64_t> line, std::optional<int64_t> col) {
    cidx::Diagnostic d;
    d.severity = sev;
    d.spelling = std::move(spelling);
    d.file_path = std::move(path);
    d.line = line;
    d.col = col;
    return d;
  };

  // Round-trip in TU (insertion) order; counts grouped by severity.
  db.replace_diagnostics(fid,
                         {mk(2, "unused 'x'", std::string("/r/a.c"), 3, 5),
                          mk(3, "implicit decl", std::string("/r/a.c"), 7, 1),
                          mk(2, "shadow", std::string("/r/a.c"), 9, 2)});
  auto got = db.get_diagnostics(fid);
  REQUIRE(got.size() == 3);
  CHECK(got[0].severity == 2);
  CHECK(got[0].spelling == "unused 'x'");
  CHECK(got[0].line == 3);
  CHECK(got[1].severity == 3);
  CHECK(got[0].id < got[1].id); // ids follow TU order
  CHECK(db.diagnostic_counts() ==
        std::map<int64_t, std::map<int, int64_t>>{{fid, {{2, 2}, {3, 1}}}});

  // Locationless diagnostic stores NULL file_path/line/col.
  db.replace_diagnostics(fid, {mk(2, "linker input unused", std::nullopt,
                                  std::nullopt, std::nullopt)});
  got = db.get_diagnostics(fid);
  REQUIRE(got.size() == 1); // wholesale refresh dropped the old three
  CHECK_FALSE(got[0].file_path.has_value());
  CHECK_FALSE(got[0].line.has_value());
  CHECK_FALSE(got[0].col.has_value());

  // Re-index of a now-clean file drops every row.
  db.replace_diagnostics(fid, {});
  CHECK(db.get_diagnostics(fid).empty());
  CHECK(db.diagnostic_counts().empty());

  // ON DELETE CASCADE: deleting the file removes its diagnostics.
  db.replace_diagnostics(
      fid, {mk(3, "e", std::nullopt, std::nullopt, std::nullopt)});
  db.delete_file(fid);
  CHECK(db.diagnostic_counts().empty());
}

TEST_CASE("delete_component removes files (cascade) and symbols (explicit)") {
  cidx::Storage db(":memory:");

  // Component A: one file, one symbol defined+declared in it.
  const int64_t a = db.add_component("a", "/repo/a");
  const int64_t da = db.add_directory(a, "");
  const int64_t fa = db.add_file(da, "a.c");
  cidx::Symbol sa;
  sa.usr = "c:@F@a_fn";
  sa.spelling = "a_fn";
  sa.kind = "function";
  sa.file_id = fa;
  sa.decl_file_id = fa;
  db.add_symbol(sa);

  // Component B: untouched bystander with its own file + symbol.
  const int64_t b = db.add_component("b", "/repo/b");
  const int64_t dbdir = db.add_directory(b, "");
  const int64_t fb = db.add_file(dbdir, "b.c");
  cidx::Symbol sb;
  sb.usr = "c:@F@b_fn";
  sb.spelling = "b_fn";
  sb.kind = "function";
  sb.file_id = fb;
  sb.decl_file_id = fb;
  db.add_symbol(sb);

  // Cross symbol: defined in B but DECLARED in A's file -> the decl_file_id
  // match means it is "related" to A and is removed when A is deleted.
  cidx::Symbol cross;
  cross.usr = "c:@F@cross";
  cross.spelling = "cross";
  cross.kind = "function";
  cross.file_id = fb;
  cross.decl_file_id = fa;
  db.add_symbol(cross);

  db.delete_component(a);

  CHECK_FALSE(db.get_component("/repo/a").has_value());
  CHECK_FALSE(db.get_file("/repo/a/a.c").has_value());
  CHECK_MESSAGE(!db.lookup_symbol("c:@F@a_fn").has_value(),
                "A's symbol deleted, not orphaned");
  CHECK_MESSAGE(!db.lookup_symbol("c:@F@cross").has_value(),
                "decl-site-in-A symbol deleted too");

  CHECK(db.get_component("/repo/b").has_value());
  REQUIRE(db.get_file("/repo/b/b.c").has_value());
  CHECK_MESSAGE(db.lookup_symbol("c:@F@b_fn").has_value(),
                "bystander component B untouched");
}

TEST_CASE(
    "translation-unit configurations are canonical and multi-applicable") {
  cidx::Storage db(":memory:");
  const int64_t component = db.add_component("configs", "/repo/configs");
  const int64_t directory = db.add_directory(component, "");
  const int64_t tu = db.add_file(directory, "main.cpp");
  const int64_t header = db.add_file(directory, "shared.hpp");

  cidx::TranslationUnitConfig first;
  first.driver = "clang++";
  first.language = "c++";
  first.arguments = {"-std=c++23", "-DFAST=1", "main.cpp"};
  first.diagnostics_policy = "error-limit=0";
  const int64_t first_id = db.add_translation_unit_config(first);
  cidx::TranslationUnitConfig repeat = first;
  CHECK(db.add_translation_unit_config(repeat) == first_id);
  const auto stored = db.translation_unit_config_by_id(first_id);
  REQUIRE(stored.has_value());
  CHECK(stored->descriptor_json ==
        "[\"clang++\",\"\",\"c++\",\"c++23\",\"\",[],\"\",\"\",[],[\"-DFAST="
        "1\"],[],[],\"error-"
        "limit=0\",[\"-std=c++23\",\"-DFAST=1\",\"main.cpp\"]]");

  cidx::IncludeConfig include;
  include.tu_file_id = tu;
  include.digest = "legacy-digest-1";
  include.driver = first.driver;
  include.lang_mode = first.language;
  include.arguments = first.arguments;
  const int64_t include_id = db.add_include_config(include);
  REQUIRE(db.include_config_by_id(include_id).has_value());
  const auto configs = db.translation_unit_configs_for_file(tu);
  REQUIRE(configs.size() == 1);
  CHECK(configs.front().descriptor_json == stored->descriptor_json);
  db.set_file_indexed(tu, true);
  db.add_file(directory, "main.cpp", std::nullopt, std::nullopt,
              std::vector<std::string>{"-DCHANGED"}, std::string("clang++"));
  REQUIRE(db.get_file_by_id(tu).has_value());
  CHECK_FALSE(db.get_file_by_id(tu)->indexed);
  CHECK(db.file_configs_for(tu).front().state ==
        cidx::TranslationUnitConfigState::stale);

  cidx::TranslationUnitConfig second = first;
  second.arguments.push_back("-DDEBUG=1");
  const int64_t second_id = db.add_translation_unit_config(second);
  CHECK(second_id != first_id);
  db.add_file_config({header, first_id, "header",
                      cidx::TranslationUnitConfigState::registered,
                      std::nullopt});
  db.add_file_config({header, second_id, "header",
                      cidx::TranslationUnitConfigState::ambiguous,
                      std::string("different compile worlds")});
  const auto applicability = db.file_configs_for(header);
  REQUIRE(applicability.size() == 2);
  CHECK(applicability[0].config_id == first_id);
  CHECK(applicability[1].config_id == second_id);
  CHECK(applicability[1].state == cidx::TranslationUnitConfigState::ambiguous);
  const auto unknown =
      db.invariant_include_edges(header, {first_id, second_id}, false);
  CHECK_FALSE(unknown.coverage_complete);
  db.add_file_config({header, second_id, "header",
                      cidx::TranslationUnitConfigState::registered,
                      std::nullopt});
  const auto invariant =
      db.invariant_include_edges(header, {first_id, second_id}, false);
  CHECK(invariant.coverage_complete);
  CHECK(invariant.edges.empty());
}

TEST_CASE("invariant include intersection keeps an empty first result empty") {
  cidx::Storage db(":memory:");
  const int64_t component = db.add_component("configs", "/repo/intersection");
  const int64_t directory = db.add_directory(component, "");
  const int64_t tu_empty = db.add_file(directory, "empty.cpp");
  const int64_t tu_edge = db.add_file(directory, "edge.cpp");
  const int64_t header = db.add_file(directory, "shared.hpp");

  cidx::IncludeConfig empty_cfg{
      .tu_file_id = tu_empty, .digest = "empty", .arguments = {"-DZERO"}};
  cidx::IncludeConfig edge_cfg{
      .tu_file_id = tu_edge, .digest = "edge", .arguments = {"-DONE"}};
  const int64_t empty_include = db.add_include_config(empty_cfg);
  const int64_t edge_include = db.add_include_config(edge_cfg);
  const auto configs = db.translation_unit_configs_for_file(tu_empty);
  REQUIRE(configs.size() == 1);
  const int64_t empty_id = configs.front().id;
  const auto other_configs = db.translation_unit_configs_for_file(tu_edge);
  REQUIRE(other_configs.size() == 1);
  const int64_t edge_id = other_configs.front().id;
  db.add_file_config({header, empty_id, "header",
                      cidx::TranslationUnitConfigState::registered,
                      std::nullopt});
  db.add_file_config({header, edge_id, "header",
                      cidx::TranslationUnitConfigState::registered,
                      std::nullopt});
  db.add_include_edge({.src_file_id = header,
                       .dst_path = "only-under-edge",
                       .config_id = edge_include});

  CHECK(db.invariant_include_edges(header, {empty_id, edge_id}, false)
            .edges.empty());
  CHECK(db.invariant_include_edges(header, {edge_id, empty_id}, false)
            .edges.empty());
  CHECK(db.invariant_include_edges(header, {empty_id, edge_id}, false)
            .coverage_complete);
  (void)empty_include;
}

TEST_CASE("descriptor golden captures every semantic dimension") {
  cidx::Storage db(":memory:");
  cidx::TranslationUnitConfig config;
  config.driver = "clang++";
  config.working_dir = ".";
  config.language = "c++";
  config.resource_dir = "/clang/resource";
  config.arguments = {"-std=c++23",     "--target=x86_64-unknown-linux-gnu",
                      "-mabi=lp64",     "-isysroot",
                      "/sdk",           "-I",
                      "/inc",           "-D",
                      "FEATURE=1",      "-include",
                      "/gen/header.hpp"};
  const auto stored =
      db.translation_unit_config_by_id(db.add_translation_unit_config(config));
  REQUIRE(stored.has_value());
  CHECK(stored->descriptor_json ==
        "[\"clang++\",\".\",\"c++\",\"c++23\","
        "\"x86_64-unknown-linux-gnu\",[\"-mabi=lp64\"],\"/sdk\","
        "\"/clang/resource\",[\"/inc\"],[\"-DFEATURE=1\"],[],"
        "[\"/gen/header.hpp\"],\"error-limit=0\",[\"-std=c++23\","
        "\"--target=x86_64-unknown-linux-gnu\",\"-mabi=lp64\","
        "\"-isysroot\",\"/sdk\",\"-I\",\"/inc\",\"-D\","
        "\"FEATURE=1\",\"-include\",\"/gen/header.hpp\"]]");
  CHECK(stored->descriptor_hash == "0e65af5d6defe83a2ea53aeac13ca9f6237c4a20");
}

TEST_CASE(
    "retiring a TU removes obsolete header applicability only when unused") {
  cidx::Storage db(":memory:");
  const int64_t component = db.add_component("configs", "/repo/retire");
  const int64_t directory = db.add_directory(component, "");
  const int64_t first_tu = db.add_file(directory, "first.cpp");
  const int64_t second_tu = db.add_file(directory, "second.cpp");
  const int64_t header = db.add_file(directory, "shared.hpp");
  cidx::IncludeConfig first{.tu_file_id = first_tu,
                            .digest = "shared",
                            .driver = std::nullopt,
                            .working_dir = std::nullopt,
                            .arguments = {},
                            .lang_mode = std::nullopt,
                            .resource_dir = std::nullopt};
  cidx::IncludeConfig second{.tu_file_id = second_tu,
                             .digest = "shared",
                             .driver = std::nullopt,
                             .working_dir = std::nullopt,
                             .arguments = {},
                             .lang_mode = std::nullopt,
                             .resource_dir = std::nullopt};
  db.add_include_config(first);
  db.add_include_config(second);
  const int64_t first_config =
      db.translation_unit_configs_for_file(first_tu).front().id;
  const int64_t second_config =
      db.translation_unit_configs_for_file(second_tu).front().id;
  db.add_file_config({header, first_config, "header",
                      cidx::TranslationUnitConfigState::registered,
                      std::nullopt});
  db.add_file_config({header, second_config, "header",
                      cidx::TranslationUnitConfigState::registered,
                      std::nullopt});
  db.delete_include_configs_for_tu(first_tu);
  auto rows = db.file_configs_for(header);
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().config_id == second_config);
  db.delete_include_configs_for_tu(second_tu);
  CHECK(db.file_configs_for(header).empty());
}

TEST_CASE("v35 occurrence identities are compact and lossless") {
  cidx::Storage db(":memory:");
  const int64_t component = db.add_component("c", "/repo/c");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "c.cpp");

  cidx::Symbol caller;
  caller.usr = "c:@F@caller";
  caller.spelling = "caller";
  caller.kind = "function";
  const int64_t caller_id = db.add_symbol(caller);
  cidx::Symbol target;
  target.usr = "c:@F@target";
  target.spelling = "target";
  target.kind = "function";
  const int64_t target_id = db.add_symbol(target);
  cidx::Symbol receiver;
  receiver.usr = "c:@S@Receiver";
  receiver.spelling = "Receiver";
  receiver.kind = "struct";
  const int64_t receiver_id = db.add_symbol(receiver);

  cidx::TypeNode receiver_type;
  receiver_type.type_key = "record:Receiver";
  receiver_type.spelling = "Receiver";
  receiver_type.kind = cidx::kTypeKindRecord;
  receiver_type.decl_usr = receiver.usr;
  const int64_t receiver_type_id = db.intern_type_node(receiver_type);
  cidx::Edge occurrence_edge;
  occurrence_edge.src_id = caller_id;
  occurrence_edge.dst_id = target_id;
  occurrence_edge.kind = 1;
  const int64_t edge_id = db.add_edge(occurrence_edge);

  cidx::EdgeSite resolved;
  resolved.edge_id = edge_id;
  resolved.file_id = file;
  resolved.line = 10;
  resolved.col = 2;
  resolved.recv_src_kind = "local";
  resolved.recv_type_usr = receiver.usr;
  resolved.recv_decl_usr = receiver.usr;
  db.add_edge_site(resolved);
  resolved.line = 11;
  db.add_edge_site(resolved);

  auto raw_resolved = db.raw_db().prepare(
      "SELECT recv_src_kind, recv_src_kind_id, recv_type_usr, recv_decl_usr, "
      "recv_type_id, recv_decl_id FROM edge_site ORDER BY line");
  REQUIRE(raw_resolved.step());
  CHECK(raw_resolved.col_is_null(0));
  CHECK(raw_resolved.col_int64(1) == 2);
  CHECK(raw_resolved.col_is_null(2));
  CHECK(raw_resolved.col_is_null(3));
  CHECK(raw_resolved.col_int64(4) == receiver_type_id);
  CHECK(raw_resolved.col_int64(5) == receiver_id);
  REQUIRE(raw_resolved.step());
  CHECK(raw_resolved.col_int64(4) == receiver_type_id);

  const auto sites = db.edge_sites_one(edge_id, 10);
  REQUIRE(sites.size() == 2);
  CHECK(sites[0].recv_src_kind == std::string("local"));
  CHECK(sites[0].recv_type_usr == receiver.usr);
  CHECK(sites[0].recv_decl_usr == receiver.usr);

  cidx::CallArg arg;
  arg.edge_id = edge_id;
  arg.file_id = file;
  arg.line = 20;
  arg.col = 4;
  arg.position = 0;
  arg.src_kind = "local";
  arg.type_usr = "external:type:Missing";
  arg.decl_usr = "external:symbol:Missing";
  arg.callee_usr = target.usr;
  db.add_call_arg(arg);

  auto raw_arg = db.raw_db().prepare(
      "SELECT src_kind, src_kind_id, type_usr, decl_usr, callee_usr, "
      "type_id, decl_id, callee_id, type_identity_id, decl_identity_id, "
      "callee_identity_id FROM call_arg");
  REQUIRE(raw_arg.step());
  CHECK(raw_arg.col_is_null(0));
  CHECK(raw_arg.col_int64(1) == 2);
  CHECK(raw_arg.col_is_null(2));
  CHECK(raw_arg.col_is_null(3));
  CHECK(raw_arg.col_is_null(4));
  CHECK(raw_arg.col_is_null(5));
  CHECK(raw_arg.col_is_null(6));
  CHECK(raw_arg.col_int64(7) == target_id);
  CHECK(raw_arg.col_int64(8) > 0);
  CHECK(raw_arg.col_int64(9) > 0);
  CHECK(raw_arg.col_is_null(10));

  auto readable = db.raw_db().prepare(
      "SELECT type_usr, decl_usr, callee_usr FROM call_arg_read");
  REQUIRE(readable.step());
  CHECK(readable.col_text(0) == "external:type:Missing");
  CHECK(readable.col_text(1) == "external:symbol:Missing");
  CHECK(readable.col_text(2) == target.usr);
  auto identities = db.raw_db().prepare(
      "SELECT identity_kind, identity_text, resolution_status "
      "FROM external_identity ORDER BY identity_kind, identity_text");
  REQUIRE(identities.step());
  CHECK(identities.col_int64(0) == 1);
  CHECK(identities.col_text(1) == "external:type:Missing");
  CHECK(identities.col_int64(2) == 0);
  REQUIRE(identities.step());
  CHECK(identities.col_int64(0) == 2);
  CHECK(identities.col_text(1) == "external:symbol:Missing");
  CHECK(identities.col_int64(2) == 0);
  CHECK_FALSE(identities.step());

  arg.position = 1;
  arg.line = 21;
  db.add_call_arg(arg);
  auto count = db.raw_db().prepare(
      "SELECT COUNT(*) FROM external_identity WHERE identity_text IN "
      "('external:type:Missing', 'external:symbol:Missing')");
  REQUIRE(count.step());
  CHECK(count.col_int64(0) == 2);
  arg.src_kind = "not-a-source-kind";
  CHECK_THROWS_AS(db.add_call_arg(arg), cidx::StorageError);
}

TEST_CASE("v35 identities reconcile independent of insertion order") {
  cidx::Storage db(":memory:");
  const auto component = db.add_component("c", "/repo/c");
  const auto directory = db.add_directory(component, "");
  const auto file = db.add_file(directory, "c.cpp");

  cidx::Symbol caller;
  caller.usr = "future:caller";
  caller.spelling = "caller";
  caller.kind = "function";
  const auto caller_id = db.add_symbol(caller);
  cidx::Symbol target;
  target.usr = "future:target";
  target.spelling = "target";
  target.kind = "function";
  const auto target_id = db.add_symbol(target);
  cidx::Edge edge;
  edge.src_id = caller_id;
  edge.dst_id = target_id;
  edge.kind = 1;
  const auto edge_id = db.add_edge(edge);

  cidx::EdgeSite site;
  site.edge_id = edge_id;
  site.file_id = file;
  site.line = 10;
  site.col = 1;
  site.recv_src_kind = std::nullopt;
  site.recv_type_usr = "future:type";
  site.recv_decl_usr = "future:symbol";
  db.add_edge_site(site);
  cidx::CallArg arg;
  arg.edge_id = edge_id;
  arg.file_id = file;
  arg.line = 10;
  arg.col = 1;
  arg.position = 0;
  arg.src_kind = "local";
  arg.type_usr = "future:type";
  arg.decl_usr = "future:symbol";
  arg.callee_usr = "future:callee";
  db.add_call_arg(arg);
  auto pending = db.raw_db().prepare(
      "SELECT recv_type_identity_id, recv_decl_identity_id FROM edge_site");
  REQUIRE(pending.step());
  CHECK(pending.col_is_null(0) == false);
  CHECK(pending.col_is_null(1) == false);

  cidx::Symbol late_symbol;
  late_symbol.usr = "future:symbol";
  late_symbol.spelling = "symbol";
  late_symbol.kind = "struct";
  const auto late_symbol_id = db.add_symbol(late_symbol);
  cidx::Symbol late_callee;
  late_callee.usr = "future:callee";
  late_callee.spelling = "callee";
  late_callee.kind = "function";
  const auto late_callee_id = db.add_symbol(late_callee);
  cidx::TypeNode late_type;
  late_type.type_key = "record:future:type";
  late_type.spelling = "Future";
  late_type.kind = cidx::kTypeKindRecord;
  late_type.decl_usr = "future:type";
  const auto late_type_id = db.intern_type_node(late_type);

  auto raw = db.raw_db().prepare(
      "SELECT recv_src_kind_id, recv_type_id, recv_decl_id FROM edge_site");
  REQUIRE(raw.step());
  CHECK(raw.col_is_null(0));
  CHECK(raw.col_int64(1) == late_type_id);
  CHECK(raw.col_int64(2) == late_symbol_id);
  raw = db.raw_db().prepare("SELECT src_kind, src_kind_id, type_id, decl_id, "
                            "callee_id FROM call_arg");
  REQUIRE(raw.step());
  CHECK(raw.col_is_null(0));
  CHECK(raw.col_int64(1) == 2);
  CHECK(raw.col_int64(2) == late_type_id);
  CHECK(raw.col_int64(3) == late_symbol_id);
  CHECK(raw.col_int64(4) == late_callee_id);
  auto unresolved = db.raw_db().prepare(
      "SELECT COUNT(*) FROM external_identity WHERE identity_text IN "
      "('future:type','future:symbol','future:callee') AND resolution_status = "
      "0");
  REQUIRE(unresolved.step());
  CHECK(unresolved.col_int64(0) == 0);
}
