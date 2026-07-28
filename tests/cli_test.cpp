// S07 tests — args grammar (CLI11-based parser: usage/help/error text is
// CLI11's native formatting; the Python-argparse transcription is retired),
// cli/format, add-source, and the query commands' golden outputs (hermetic,
// label "default"); cmd_import needs CompileDb::load
// (clang::tooling::JSONCompilationDatabase) and lives in doctest suite "clang"
// (label "clang") using the linked Clang C++ API.
//
// Args-grammar expectations were captured from the CLI11-based cidx binary
// (vendored third_party/CLI11.hpp). Command-OUTPUT expectations (tables,
// show/list formats) are still the Python-tool goldens: captured from
// python3 -m indexer (Python 3.14, COLUMNS=80) against a DB seeded with
// EXACTLY the rows seed_gold() writes; {ROOT}/{T} placeholders are
// substituted with the runtime temp paths.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ast/index_engine.hpp"
#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "cli/format.hpp"
#include "query/exec.hpp"
#include "storage/records.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/logger.hpp"

using cidx::Diagnostic;
using cidx::Storage;
using cidx::Symbol;
using cidx::UsageError;
namespace cli = cidx::cli;

namespace {

bool g_clang_skipped = false;

// Returns true when CIDX_MANIFESTS_DIR points at an existing directory.
// On a host without the lab checkout (e.g. the e2e box that only rsyncs
// cidx-cpp/) the fixture cases should SKIP rather than fail.
bool require_manifests() {
  struct stat st{};
  if (::stat(CIDX_MANIFESTS_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
    g_clang_skipped = true;
    MESSAGE("SKIP: lab fixtures not found at " << CIDX_MANIFESTS_DIR);
    return false;
  }
  return true;
}

std::string make_temp_dir() {
  char tmpl[] = "/tmp/cidx_cli_XXXXXX";
  char *d = ::mkdtemp(tmpl);
  REQUIRE(d != nullptr);
  return d;
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

void write_file(const std::string &path, const std::string &content) {
  std::ofstream f(path);
  REQUIRE(f.good());
  f << content;
}

bool path_exists(const std::string &path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0;
}

std::string replace_all(std::string text, const std::string &from,
                        const std::string &to) {
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
  return text;
}

class ScopedEnv {
public:
  ScopedEnv(const char *name, const char *value) : name_(name) {
    const char *prev = std::getenv(name);
    if (prev != nullptr) {
      prev_ = prev;
    }
    ::setenv(name, value, 1);
  }
  ~ScopedEnv() {
    if (prev_) {
      ::setenv(name_, prev_->c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }

private:
  const char *name_;
  std::optional<std::string> prev_;
};

// -- parse helpers ------------------------------------------------------------

struct ParseFail {
  int code = 0;
  std::string msg;
};

ParseFail parse_fail(const std::vector<std::string> &argv) {
  ParseFail out;
  try {
    cli::parse_args(argv);
    FAIL("expected UsageError for: ", doctest::toString(argv.size()));
  } catch (const UsageError &e) {
    out.code = e.exit_code();
    out.msg = e.what();
  }
  return out;
}

// -- command runner -----------------------------------------------------------

struct CmdResult {
  int rc = -1;
  std::string out;
  std::string err;
};

// `logger` lets index tests use a per-case Logger: the warning counter is
// cumulative per Logger instance (Python's module-global _warnings), so a
// fresh one keeps the "N warning(s)/error(s)" assertions deterministic.
CmdResult run_cli(const std::vector<std::string> &argv,
                  const std::string &cache, cidx::Logger *logger = nullptr,
                  cli::IndexOutcomeSink outcome_sink = {}) {
  cli::ParsedArgs pa = cli::parse_args(argv);
  REQUIRE(!pa.help_text);
  std::ostringstream out;
  std::ostringstream err;
  cli::Context ctx;
  ctx.cache_dir = cache;
  ctx.index_path = cache + "/index.db";
  ctx.logger = logger != nullptr ? logger : &cidx::Logger::root();
  ctx.out = &out;
  ctx.err = &err;
  ctx.index_outcome_sink = std::move(outcome_sink);
  CmdResult r;
  r.rc = cli::run_command(pa, ctx);
  r.out = out.str();
  r.err = err.str();
  return r;
}

std::string read_file(const std::string &path) {
  std::ifstream f(path);
  REQUIRE(f.good());
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct TuPublicationSnapshot {
  std::map<std::string, int64_t> table_counts;
  std::vector<std::string> rows;
  std::vector<std::string> meta;
  std::string file_state;

  friend bool operator==(const TuPublicationSnapshot &a,
                         const TuPublicationSnapshot &b) {
    return a.table_counts == b.table_counts && a.rows == b.rows &&
           a.meta == b.meta && a.file_state == b.file_state;
  }
};

TuPublicationSnapshot snapshot_tu_publication(Storage &db,
                                              const std::string &path) {
  static constexpr const char *kTables[] = {"translation_unit_config",
                                            "file_config",
                                            "include_config",
                                            "include_edge",
                                            "include_site",
                                            "include_macro_use",
                                            "symbol",
                                            "edge",
                                            "edge_site",
                                            "call_arg",
                                            "template_param",
                                            "template_arg",
                                            "type_node",
                                            "type_edge",
                                            "symbol_type",
                                            "parameter",
                                            "definition",
                                            "def_edge",
                                            "entity_edge",
                                            "diagnostic"};
  TuPublicationSnapshot snapshot;
  for (const char *table : kTables) {
    auto statement =
        db.raw_db().prepare("SELECT COUNT(*) FROM " + std::string(table));
    REQUIRE(statement.step());
    snapshot.table_counts.emplace(table, statement.col_int64(0));
    std::vector<std::string> column_names;
    auto table_info =
        db.raw_db().prepare("PRAGMA table_info(" + std::string(table) + ")");
    while (table_info.step()) {
      column_names.push_back(table_info.col_text(1));
    }
    std::string row_sql = "SELECT * FROM " + std::string(table);
    if (!column_names.empty()) {
      row_sql += " ORDER BY \"" + column_names.front() + "\"";
      for (std::size_t i = 1; i < column_names.size(); ++i) {
        row_sql += ",\"" + column_names[i] + "\"";
      }
    }
    auto rows = db.raw_db().prepare(row_sql);
    const int columns = rows.column_count();
    while (rows.step()) {
      std::string encoded = std::string(table) + "\x1e";
      for (int column = 0; column < columns; ++column) {
        if (column != 0) {
          encoded.push_back('\x1f');
        }
        encoded += rows.col_is_null(column) ? "<null>" : rows.col_text(column);
      }
      snapshot.rows.push_back(std::move(encoded));
    }
  }
  auto meta = db.raw_db().prepare("SELECT key,value FROM meta ORDER BY key");
  while (meta.step()) {
    snapshot.meta.push_back(meta.col_text(0) + "\x1f" + meta.col_text(1));
  }
  const auto file = db.get_file(path);
  if (file) {
    snapshot.file_state = std::to_string(file->id) + "\x1f" +
                          (file->indexed ? "1" : "0") + "\x1f" +
                          file->md5.value_or("") + "\x1f" +
                          file->indexed_at.value_or("");
  }
  return snapshot;
}

// -- golden DB seed -----------------------------------------------------------
// Mirror of the Python seeding script used for the capture (same rows, same
// order, same ids 1..6); root is a path that does NOT exist on disk.

void seed_gold(const std::string &cache, const std::string &root) {
  Storage db(cache + "/index.db");
  const int64_t cid = db.add_component("gold", root, "repo");
  const int64_t f1 = db.add_file_path(
      root + "/src/a.c", 1718000000.0,
      std::string("0123456789abcdef0123456789abcdef"),
      std::vector<std::string>{"-I" + root + "/include", "-DX=1"},
      std::string("gcc"));
  const int64_t f2 = db.add_file_path(root + "/include/a.h");
  REQUIRE(cid == 1);
  REQUIRE(f1 == 1);
  REQUIRE(f2 == 2);

  Symbol s;
  s.usr = "c:@F@multiply";
  s.spelling = "multiply";
  s.kind = "function";
  s.qual_name = "multiply";
  s.display_name = "multiply(int, int)";
  s.type_info = "int (int, int)";
  s.file_id = f1;
  s.line = 12;
  s.col = 5;
  s.decl_file_id = f2;
  s.decl_line = 3;
  s.decl_col = 5;
  s.is_definition = true;
  s.linkage = "external";
  s.resolved = true;
  db.add_symbol(s);

  s = Symbol{};
  s.usr = "c:@F@square";
  s.spelling = "square";
  s.kind = "function";
  s.qual_name = "square";
  s.display_name = "square(int)";
  s.type_info = "int (int)";
  s.file_id = f2;
  s.line = 4;
  s.col = 5;
  s.decl_file_id = f2;
  s.decl_line = 4;
  s.decl_col = 5;
  s.linkage = "external";
  db.add_symbol(s);

  s = Symbol{};
  s.usr = "c:@N@NS";
  s.spelling = "NS";
  s.kind = "namespace";
  s.qual_name = "NS";
  s.display_name = "NS";
  s.file_id = f2;
  s.line = 8;
  s.col = 11;
  s.is_definition = true;
  s.resolved = true;
  db.add_symbol(s);

  s = Symbol{};
  s.usr = "c:@N@NS@S@Shape";
  s.spelling = "Shape";
  s.kind = "class";
  s.qual_name = "NS::Shape";
  s.display_name = "Shape";
  s.type_info = "NS::Shape";
  s.file_id = f2;
  s.line = 10;
  s.col = 7;
  s.is_definition = true;
  s.resolved = true;
  s.linkage = "external";
  s.parent_usr = "c:@N@NS";
  db.add_symbol(s);

  s = Symbol{};
  s.usr = "c:@N@NS@S@Shape@F@area#";
  s.spelling = "area";
  s.kind = "method";
  s.qual_name = "NS::Shape::area";
  s.display_name = "area()";
  s.type_info = "double ()";
  s.file_id = f2;
  s.line = 12;
  s.col = 18;
  s.is_pure = true;
  s.linkage = "external";
  s.access = "public";
  s.parent_usr = "c:@N@NS@S@Shape";
  db.add_symbol(s);

  s = Symbol{};
  s.usr = "c:a.c@counter";
  s.spelling = "counter";
  s.kind = "variable";
  s.qual_name = "counter";
  s.display_name = "counter";
  s.type_info = "int";
  s.file_id = f1;
  s.line = 7;
  s.col = 12;
  s.is_definition = true;
  s.resolved = true;
  s.linkage = "internal";
  db.add_symbol(s);

  // Deterministic indexed state (datetime('now') would not be).
  db.raw_db().exec(
      "UPDATE file SET indexed=1, indexed_at='2026-06-12 10:00:00' "
      "WHERE id=1");
}

struct GoldFixture {
  std::string cache;
  std::string root;
  GoldFixture() : cache(make_temp_dir()), root(cache + "/gold") {
    seed_gold(cache, root); // root deliberately NOT created on disk
  }
  std::string expect(const std::string &tmpl) const {
    return replace_all(tmpl, "{ROOT}", root);
  }
};

// Usage lines shared by several expected error messages (captured from the
// CLI11-based parser; UsageError messages are "<Usage line>\n<prog>: error:
// <detail>\n").
const char kTopUsage[] = "Usage: cidx [OPTIONS] SUBCOMMAND\n";

const char kSetUsage[] = "Usage: cidx file set [OPTIONS] ASSIGNMENT...\n";

const char kSearchUsage[] = "Usage: cidx search [OPTIONS] pattern\n";

const char kListFilesUsage[] = "Usage: cidx file list [OPTIONS] [pattern]\n";

} // namespace

// ---------------------------------------------------------------------------
// Args grammar (default label)
// ---------------------------------------------------------------------------

TEST_CASE("args: no command -> exit 2, subcommand required") {
  const ParseFail f = parse_fail({});
  CHECK(f.code == 2);
  CHECK(f.msg ==
        std::string(kTopUsage) + "cidx: error: A subcommand is required\n");
}

TEST_CASE("args: unknown command -> exit 2, named as unrecognized") {
  const ParseFail f = parse_fail({"bogus"});
  CHECK(f.code == 2);
  CHECK(f.msg == std::string(kTopUsage) +
                     "cidx: error: A subcommand is required "
                     "(unrecognized: bogus)\n");
}

TEST_CASE("args: file — REMAINDER captures the op tail verbatim") {
  // $ cidx file demo://src/a.c -set-flag -I/x -DFOO
  cli::ParsedArgs pa = cli::parse_args(
      {"file", "flags", "demo://src/a.c", "-set-flag", "-I/x", "-DFOO"});
  CHECK(pa.command == "file");
  CHECK(pa.target == "demo://src/a.c");
  CHECK(pa.op == std::vector<std::string>{"-set-flag", "-I/x", "-DFOO"});

  // No op -> empty REMAINDER (the handler defaults to -dump-args).
  pa = cli::parse_args({"file", "flags", "demo://src/a.c"});
  CHECK(pa.target == "demo://src/a.c");
  CHECK(pa.op.empty());

  // --db before the target is parsed as an option; the rest is the tail.
  pa = cli::parse_args(
      {"file", "flags", "--db", "/tmp/x.db", "demo://a.c", "-dump-args"});
  CHECK(pa.index_db == "/tmp/x.db");
  CHECK(pa.target == "demo://a.c");
  CHECK(pa.op == std::vector<std::string>{"-dump-args"});
}

TEST_CASE("args: file — missing target -> exit 2, required positional") {
  const ParseFail f = parse_fail({"file", "flags"});
  CHECK(f.code == 2);
  CHECK(f.msg == "Usage: cidx file flags [OPTIONS] TARGET\n"
                 "cidx file flags: error: TARGET is required\n");
}

TEST_CASE("args: file -h returns help text") {
  const cli::ParsedArgs pa = cli::parse_args({"file", "flags", "-h"});
  REQUIRE(pa.help_text);
  CHECK(pa.help_text->find("Usage: cidx file flags [OPTIONS] TARGET") !=
        std::string::npos);
  CHECK(pa.help_text->find("file address, e.g. 'mylib://src/foo.c'") !=
        std::string::npos);
  CHECK(pa.help_text->find("--db TEXT") != std::string::npos);
}

TEST_CASE("args: dump-compile-commands parses the component positional") {
  cli::ParsedArgs pa = cli::parse_args(
      {"component", "compile-commands", "--db", "/tmp/x.db", "demo"});
  CHECK(pa.command == "dump-compile-commands");
  CHECK(pa.component == "demo");
  CHECK(pa.index_db == "/tmp/x.db");

  const ParseFail f = parse_fail({"component", "compile-commands"});
  CHECK(f.code == 2);
  CHECK(f.msg ==
        "Usage: cidx component compile-commands [OPTIONS] COMPONENT\n"
        "cidx component compile-commands: error: COMPONENT is required\n");
}

TEST_CASE("args: unknown flag -> exit 2, unexpected argument") {
  const ParseFail f = parse_fail({"search", "foo", "--bogus"});
  CHECK(f.code == 2);
  CHECK(f.msg == std::string(kSearchUsage) +
                     "cidx search: error: The following argument was not "
                     "expected: --bogus\n");
}

TEST_CASE("args: extra positional -> exit 2, unexpected argument") {
  const ParseFail f = parse_fail({"symbol", "show", "5", "extra"});
  CHECK(f.code == 2);
  CHECK(f.msg == "Usage: cidx symbol show [OPTIONS] symbol\n"
                 "cidx symbol show: error: The following argument was not "
                 "expected: extra\n");
}

TEST_CASE("args: no prefix abbreviation — --lim is rejected") {
  // CLI11 matches option names exactly; the argparse allow_abbrev behavior
  // (--lim expanding to --limit) is retired with the hand-rolled parser.
  const ParseFail f = parse_fail({"search", "--lim", "5", "foo"});
  CHECK(f.code == 2);
  CHECK(f.msg.find("--lim") != std::string::npos);
}

TEST_CASE("args: missing required positional -> subparser exit 2") {
  const ParseFail f = parse_fail({"search"});
  CHECK(f.code == 2);
  CHECK(f.msg == std::string(kSearchUsage) +
                     "cidx search: error: pattern is required\n");
}

TEST_CASE("args: unknown flag with missing positional -> both reported") {
  const ParseFail f = parse_fail({"search", "--bogus"});
  CHECK(f.code == 2);
  CHECK(f.msg == std::string(kSearchUsage) +
                     "cidx search: error: pattern is required "
                     "(unrecognized: --bogus)\n");
}

TEST_CASE("args: missing required option -> exit 2 (add-source, import)") {
  ParseFail f = parse_fail({"component", "add"});
  CHECK(f.code == 2);
  CHECK(f.msg == "Usage: cidx component add [OPTIONS]\n"
                 "cidx component add: error: --path is required\n");
  f = parse_fail({"import"});
  CHECK(f.code == 2);
  CHECK(f.msg == "Usage: cidx import [OPTIONS]\n"
                 "cidx import: error: --db is required\n");
}

TEST_CASE("args: option missing its value -> exit 2") {
  ParseFail f = parse_fail({"search", "foo", "--limit"});
  CHECK(f.code == 2);
  CHECK(f.msg == std::string(kSearchUsage) +
                     "cidx search: error: --limit: 1 required INT missing\n");
  f = parse_fail({"dir", "list", "--component"});
  CHECK(f.code == 2);
  CHECK(f.msg == "Usage: cidx dir list [OPTIONS] [pattern]\n"
                 "cidx dir list: error: --component: 1 required TEXT "
                 "missing\n");
}

TEST_CASE("args: invalid int -> exit 2") {
  const ParseFail f = parse_fail({"search", "foo", "--limit", "xx"});
  CHECK(f.code == 2);
  CHECK(f.msg == std::string(kSearchUsage) +
                     "cidx search: error: Could not convert: --limit = xx\n");
}

TEST_CASE("args: invalid choice -> exit 2 (both kind sets)") {
  ParseFail f = parse_fail({"search", "foo", "--kind", "bogus"});
  CHECK(f.code == 2);
  CHECK(f.msg ==
        std::string(kSearchUsage) +
            "cidx search: error: --kind: bogus not in "
            "{class,class-template,constructor,destructor,enum,enum-constant,"
            "function,function-template,macro,member,method,namespace,struct,"
            "type-alias,typedef,union,variable}\n");
  f = parse_fail({"component", "add", "--path", "/tmp", "--kind", "bogus"});
  CHECK(f.code == 2);
  CHECK(f.msg == "Usage: cidx component add [OPTIONS]\n"
                 "cidx component add: error: --kind: bogus not in "
                 "{repo,external}\n");
}

TEST_CASE("args: symbol/file need a sub-command; invalid what -> exit 2") {
  ParseFail f = parse_fail({"symbol"});
  CHECK(f.code == 2);
  CHECK(f.msg == "Usage: cidx symbol [OPTIONS] SUBCOMMAND\n"
                 "cidx symbol: error: A subcommand is required\n");
  f = parse_fail({"symbol", "bogus"});
  CHECK(f.msg == "Usage: cidx symbol [OPTIONS] SUBCOMMAND\n"
                 "cidx symbol: error: A subcommand is required "
                 "(unrecognized: bogus)\n");
  f = parse_fail({"file", "bogus"});
  CHECK(f.msg == "Usage: cidx file [OPTIONS] SUBCOMMAND\n"
                 "cidx file: error: A subcommand is required "
                 "(unrecognized: bogus)\n");
}

TEST_CASE("args: --indexed and --pending are mutually exclusive (exit 2)") {
  ParseFail f = parse_fail({"file", "list", "--indexed", "--pending"});
  CHECK(f.code == 2);
  CHECK(f.msg == std::string(kListFilesUsage) +
                     "cidx file list: error: --indexed excludes --pending\n");
  f = parse_fail({"file", "list", "--pending", "--indexed"});
  CHECK(f.msg == std::string(kListFilesUsage) +
                     "cidx file list: error: --indexed excludes --pending\n");
}

TEST_CASE("args: dir needs a sub-command; invalid what -> exit 2") {
  ParseFail f = parse_fail({"dir"});
  CHECK(f.code == 2);
  CHECK(f.msg == "Usage: cidx dir [OPTIONS] SUBCOMMAND\n"
                 "cidx dir: error: A subcommand is required\n");
  f = parse_fail({"dir", "bogus"});
  CHECK(f.msg == "Usage: cidx dir [OPTIONS] SUBCOMMAND\n"
                 "cidx dir: error: A subcommand is required "
                 "(unrecognized: bogus)\n");
}

TEST_CASE("args: delete requires one selector (required option group)") {
  ParseFail f = parse_fail({"symbol", "rm"});
  CHECK(f.code == 2);
  CHECK(f.msg == "Usage: cidx symbol rm [OPTIONS]\n"
                 "cidx symbol rm: error: Exactly 1 option from "
                 "[--id,--name,--usr] is required\n");
  // dir's group is just --id | --path
  f = parse_fail({"dir", "rm"});
  CHECK(f.msg == "Usage: cidx dir rm [OPTIONS]\n"
                 "cidx dir rm: error: Exactly 1 option from "
                 "[--id,--path] is required\n");
}

TEST_CASE("args: delete selectors are mutually exclusive (exit 2)") {
  ParseFail f = parse_fail({"component", "rm", "--id", "1", "--name", "x"});
  CHECK(f.code == 2);
  CHECK(f.msg == "Usage: cidx component rm [OPTIONS]\n"
                 "cidx component rm: error: Exactly 1 option from "
                 "[--id,--name,--path] is required but 2 were given\n");
}

TEST_CASE("args: delete --id is int-typed (exit 2 on non-int)") {
  const ParseFail f = parse_fail({"file", "rm", "--id", "notanint"});
  CHECK(f.code == 2);
  CHECK(f.msg == "Usage: cidx file rm [OPTIONS]\n"
                 "cidx file rm: error: Could not convert: --id = notanint\n");
}

TEST_CASE("args: delete parses each selector + --component + --dry-run") {
  cli::ParsedArgs pa = cli::parse_args({"symbol", "rm", "--id", "7"});
  CHECK(pa.command == "delete");
  CHECK(pa.what == "symbol");
  REQUIRE(pa.del_id.has_value());
  CHECK(*pa.del_id == 7);
  CHECK_FALSE(pa.dry_run);

  pa = cli::parse_args(
      {"symbol", "rm", "--usr", "c:@F@multiply", "-c", "proj", "--dry-run"});
  REQUIRE(pa.usr.has_value());
  CHECK(*pa.usr == "c:@F@multiply");
  REQUIRE(pa.component.has_value());
  CHECK(*pa.component == "proj");
  CHECK(pa.dry_run);

  pa = cli::parse_args({"component", "rm", "--path", "/tmp/repo"});
  REQUIRE(pa.del_path.has_value());
  CHECK(*pa.del_path == "/tmp/repo");

  pa = cli::parse_args({"file", "rm", "--name", "a.c"});
  REQUIRE(pa.name.has_value());
  CHECK(*pa.name == "a.c");
}

TEST_CASE("delete: functional — multi-match list+delete, dry-run, cascade") {
  GoldFixture g; // seeded: component 'gold' (id 1), files a.c/a.h, 6 symbols
  // dry-run previews without mutating
  CmdResult r =
      run_cli({"symbol", "rm", "--name", "multiply", "--dry-run"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == "  #1  function  multiply\n"
                 "would delete 1 symbol\n");
  // symbol still present after dry-run
  r = run_cli({"symbol", "rm", "--name", "multiply", "--dry-run"}, g.cache);
  CHECK(r.out == "  #1  function  multiply\n"
                 "would delete 1 symbol\n");
  // real delete by name
  r = run_cli({"symbol", "rm", "--name", "multiply"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == "  #1  function  multiply\n"
                 "deleted 1 symbol\n");
  // gone now -> 0 match -> exit 1, stderr
  r = run_cli({"symbol", "rm", "--name", "multiply"}, g.cache);
  CHECK(r.rc == 1);
  CHECK(r.err == "error: no symbol matches --name multiply\n");
  // delete file by basename cascades to its symbols (FK SET NULL -> purged)
  r = run_cli({"file", "rm", "--name", "a.h"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("  #2  {ROOT}/include/a.h\n") + "deleted 1 file\n");
  // delete the whole component (full cascade)
  r = run_cli({"component", "rm", "--name", "gold"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out ==
        g.expect("  #1  gold (repo)  {ROOT}\n") + "deleted 1 component\n");
  r = run_cli({"component", "list"}, g.cache);
  CHECK(r.out == "0 component(s)\n");
}

TEST_CASE("args: defaults — search 25, list symbols 50, add-source repo") {
  cli::ParsedArgs pa = cli::parse_args({"search", "foo"});
  CHECK(pa.command == "search");
  CHECK(pa.limit == 25);
  CHECK(!pa.kind);
  CHECK(*pa.pattern == "foo");

  pa = cli::parse_args({"symbol", "list"});
  CHECK(pa.what == "symbols");
  CHECK(pa.limit == 50);
  CHECK(!pa.pattern);

  pa = cli::parse_args({"component", "add", "--path", "/x"});
  CHECK(*pa.kind == "repo");
}

TEST_CASE("args: ls aliases list") {
  const cli::ParsedArgs pa = cli::parse_args({"component", "ls"});
  CHECK(pa.command == "list");
  CHECK(pa.what == "components");
}

TEST_CASE("args: --flag=value, glued -cVALUE, negative limit value") {
  cli::ParsedArgs pa = cli::parse_args({"search", "--kind=function", "foo"});
  CHECK(*pa.kind == "function");
  pa = cli::parse_args({"dir", "list", "-cmycomp"});
  CHECK(*pa.component == "mycomp");
  pa = cli::parse_args({"search", "foo", "--limit", "-5"});
  CHECK(pa.limit == -5);
  // Python int()'s whitespace stripping is retired with the hand-rolled
  // parser: --limit ' 12 ' is now a conversion error.
  const ParseFail f = parse_fail({"search", "foo", "--limit", " 12 "});
  CHECK(f.code == 2);
}

TEST_CASE("args: --limit is range-checked, not saturated") {
  // The hand-rolled parser saturated over-INT_MAX limits; CLI11 rejects
  // them as conversion errors (exit 2).
  ParseFail f = parse_fail(
      {"search", "foo", "--limit", "999999999999999999999999999999"});
  CHECK(f.code == 2);
  CHECK(f.msg.find("Could not convert") != std::string::npos);

  // INT_MAX exactly (2^31 - 1) still parses.
  cli::ParsedArgs pa =
      cli::parse_args({"search", "foo", "--limit", "2147483647"});
  CHECK(pa.limit == 2147483647);

  // Negative limits are preserved unchanged (existing negative-slice path).
  pa = cli::parse_args({"search", "foo", "--limit", "-5"});
  CHECK(pa.limit == -5);
}

TEST_CASE("args: index collects FILE... and --source") {
  const cli::ParsedArgs pa =
      cli::parse_args({"index", "a.c", "b.c", "--source", "comp"});
  CHECK(pa.files == std::vector<std::string>{"a.c", "b.c"});
  CHECK(*pa.source == "comp");
}

TEST_CASE("args: index accepts profiling paths and documents the opt-in flag") {
  const cli::ParsedArgs parsed = cli::parse_args(
      {"index", "a.cpp", "--profile-json", "/tmp/cidx-profile.json",
       "--profile-sqlite-config", "/tmp/cidx-sqlite.json"});
  CHECK(parsed.profile_json ==
        std::optional<std::string>{"/tmp/cidx-profile.json"});
  CHECK(parsed.profile_sqlite_configuration ==
        std::optional<std::string>{"/tmp/cidx-sqlite.json"});

  const cli::ParsedArgs help = cli::parse_args({"index", "--help"});
  REQUIRE(help.help_text);
  CHECK(help.help_text->find("--profile-json") != std::string::npos);

  const ParseFail missing_profile =
      parse_fail({"index", "--profile-sqlite-config", "/tmp/sqlite.json"});
  CHECK(missing_profile.code == 2);
  CHECK(missing_profile.msg.find("requires --profile-json") !=
        std::string::npos);
  CHECK(parse_fail({"index", "--profile-json"}).code == 2);

  const cli::ParsedArgs resolve = cli::parse_args(
      {"resolve", "--profile-json", "/tmp/cidx-resolve-profile.json"});
  CHECK(resolve.profile_json ==
        std::optional<std::string>{"/tmp/cidx-resolve-profile.json"});
  CHECK(parse_fail({"resolve", "--profile-sqlite-config", "/tmp/sqlite.json"})
            .code == 2);
}

TEST_CASE("args: index status and explain expose fact-set readiness") {
  cli::ParsedArgs pa =
      cli::parse_args({"index", "status", "--fact-set", "entity-graph"});
  CHECK(pa.command == "index");
  CHECK(pa.index_status);
  CHECK(*pa.index_fact_set == "entity-graph");
  pa = cli::parse_args({"index", "explain"});
  CHECK(pa.command == "index");
  CHECK(pa.index_explain);
}

TEST_CASE("index status and explain filter named fact-set readiness") {
  const std::string cache = make_temp_dir();
  {
    Storage db(cache + "/index.db");
    REQUIRE(db.run_transform_pipeline().complete);
    REQUIRE(db.run_transform_pipeline().complete);
  }
  const CmdResult status =
      run_cli({"index", "status", "--fact-set", "entity-graph"}, cache);
  CHECK(status.rc == 0);
  CHECK(status.out.find("fact-set entity-graph") != std::string::npos);
  CHECK(status.out.find("reused") != std::string::npos);
  CHECK(status.out.find("edge-site-count-rollup") == std::string::npos);
  const CmdResult explain =
      run_cli({"index", "explain", "--fact-set", "entity-graph"}, cache);
  CHECK(explain.rc == 0);
  CHECK(explain.out.find("entity-graph-rollup") != std::string::npos);
  const CmdResult unknown =
      run_cli({"index", "status", "--fact-set", "missing"}, cache);
  CHECK(unknown.rc == 1);
  CHECK(unknown.out.find("unknown") != std::string::npos);
}

TEST_CASE("index status and explain preserve pending state after reopen") {
  const std::string cache = make_temp_dir();
  {
    Storage db(cache + "/index.db");
    REQUIRE(db.run_transform_pipeline().complete);
    REQUIRE(db.run_transform_pipeline().complete);
    db.mark_transform_pipeline_pending("selected file remains pending");
  }
  const CmdResult status = run_cli({"index", "status"}, cache);
  CHECK(status.rc == 0);
  std::size_t pending_lines = 0;
  std::size_t cursor = 0;
  while ((cursor = status.out.find(" stale pending\n", cursor)) !=
         std::string::npos) {
    ++pending_lines;
    cursor += std::string(" stale pending\n").size();
  }
  CHECK(pending_lines == 9);
  CHECK(status.out.find("readiness: stale") != std::string::npos);

  const CmdResult named_status =
      run_cli({"index", "status", "--fact-set", "entity-graph"}, cache);
  CHECK(named_status.rc == 0);
  CHECK(named_status.out.find("fact-set entity-graph stale unknown") !=
        std::string::npos);

  const CmdResult explain =
      run_cli({"index", "explain", "--fact-set", "entity-graph"}, cache);
  CHECK(explain.rc == 0);
  CHECK(explain.out.find("entity-graph-rollup: stale, pending") !=
        std::string::npos);
  CHECK(explain.out.find("cause=selected file remains pending") !=
        std::string::npos);
}

TEST_CASE("resolve compatibility adapter reports transform failure") {
  const std::string cache = make_temp_dir();
  {
    Storage db(cache + "/index.db");
    const auto baseline = db.run_transform_pipeline();
    if (!baseline.complete) {
      for (const auto &run : baseline.runs) {
        MESSAGE(run.transform_id << " " << transform_run_status_name(run.status)
                                 << " " << run.diagnostic);
      }
    }
    REQUIRE(baseline.complete);
    db.set_transform_invalidation_for_testing("source", "compat-failure");
    db.inject_transform_failure_for_testing("entity-graph-rollup");
  }
  const CmdResult result = run_cli({"resolve"}, cache);
  CHECK(result.rc == 1);
  CHECK(result.err.find("resolve failed") != std::string::npos);
  Storage db(cache + "/index.db");
  CHECK_FALSE(db.graph_resolved());
}

TEST_CASE("resolve profiling records transform wall time") {
  const std::string cache = make_temp_dir();
  const std::string profile_path = cache + "/resolve-profile.json";
  const CmdResult result =
      run_cli({"resolve", "--profile-json", profile_path}, cache);
  REQUIRE(result.rc == 0);
  const std::string profile = read_file(profile_path);
  const std::string key = "\"transforms\": ";
  const std::size_t position = profile.find(key);
  REQUIRE(position != std::string::npos);
  CHECK(std::stod(profile.substr(position + key.size())) > 0.0);
}

TEST_CASE("args: --version sets the version flag (top level only)") {
  // $ python3 -m indexer --version   -> "cidx 0.13.0" on stdout, exit 0
  cli::ParsedArgs pa = cli::parse_args({"--version"});
  CHECK(pa.version);
  CHECK(!pa.help_text);
  CHECK(pa.command.empty()); // fires before the required-subcommand check
  CHECK(std::string(cli::kVersion) == "0.53.0");

  // --version wins over a following (would-be) command, like argparse.
  pa = cli::parse_args({"--version", "search", "foo"});
  CHECK(pa.version);

  // -h before --version: help wins (encounter order).
  pa = cli::parse_args({"-h", "--version"});
  CHECK(pa.help_text);
  CHECK(!pa.version);
}

TEST_CASE("args: -h returns help text; validation beats help") {
  // $ cidx -h    (full top help, exit 0)
  cli::ParsedArgs pa = cli::parse_args({"-h"});
  REQUIRE(pa.help_text);
  CHECK(pa.help_text->find("cidx command-line skeleton") != std::string::npos);
  CHECK(pa.help_text->find(kTopUsage) != std::string::npos);
  for (const char *sub :
       {"init", "import", "index", "resolve", "search", "analyze", "db",
        "component", "repo", "dir", "file", "symbol", "graph"}) {
    CHECK(pa.help_text->find(std::string("\n  ") + sub + " ") !=
          std::string::npos);
  }
  CHECK(pa.help_text->find("--version") != std::string::npos);

  // An invalid option value errors even when -h is present (CLI11 validates
  // before processing help), regardless of encounter order.
  ParseFail f = parse_fail({"search", "-h", "--kind", "bogus", "foo"});
  CHECK(f.code == 2);
  f = parse_fail({"search", "--kind", "bogus", "-h"});
  CHECK(f.code == 2);

  // A well-formed invocation with -h yields the subcommand's help.
  pa = cli::parse_args({"search", "-h"});
  REQUIRE(pa.help_text);
  CHECK(pa.help_text->find("show at most N matches (0 = all; default 25)") !=
        std::string::npos);

  // resolve takes no options other than -h (the destructive --rebuild flag
  // was removed in v0.4.1 — it cleared all edges with no re-extract path).
  pa = cli::parse_args({"resolve", "-h"});
  REQUIRE(pa.help_text);
  CHECK(*pa.help_text ==
        "finalize cross-repo edges and roll up edge counts\n"
        "Usage: cidx resolve [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -h,--help                   Print this help message and exit\n");

  // Subcommand help carries the full "cidx file list" usage line.
  pa = cli::parse_args({"file", "list", "-h"});
  REQUIRE(pa.help_text);
  CHECK(pa.help_text->find(kListFilesUsage) != std::string::npos);
}

// ---------------------------------------------------------------------------
// set grammar (cli.py cmd_set) — parity with the Python `cidx set` subcommand
// ---------------------------------------------------------------------------

TEST_CASE("args: set grammar — assignment positional + component/file/db") {
  // $ cidx set pending=False --component demo --file sub/b.c
  cli::ParsedArgs pa =
      cli::parse_args({"file", "set", "pending=False", "--component", "demo",
                       "--file", "sub/b.c"});
  CHECK(pa.command == "set");
  REQUIRE(pa.assignment.size() == 1);
  CHECK(pa.assignment[0] == "pending=False");
  REQUIRE(pa.component);
  CHECK(*pa.component == "demo");
  REQUIRE(pa.file_filter);
  CHECK(*pa.file_filter == "sub/b.c");
  CHECK_FALSE(pa.dry_run);

  // spaced form 'pending = True' -> three positional tokens (nargs="+")
  pa = cli::parse_args({"file", "set", "pending", "=", "True", "-c", "demo",
                        "--db", "/tmp/i.db", "--dry-run"});
  REQUIRE(pa.assignment.size() == 3);
  CHECK(pa.assignment[0] == "pending");
  CHECK(pa.assignment[1] == "=");
  CHECK(pa.assignment[2] == "True");
  REQUIRE(pa.index_db);
  CHECK(*pa.index_db == "/tmp/i.db");
  CHECK(pa.dry_run);

  // missing positional -> exit 2, required ASSIGNMENT
  const ParseFail f = parse_fail({"file", "set", "--component", "demo"});
  CHECK(f.code == 2);
  CHECK(f.msg == std::string(kSetUsage) +
                     "cidx file set: error: ASSIGNMENT is required\n");
}

TEST_CASE("args: set -h lists the assignment positional and options") {
  cli::ParsedArgs pa = cli::parse_args({"file", "set", "-h"});
  REQUIRE(pa.help_text);
  CHECK(pa.help_text->find(kSetUsage) != std::string::npos);
  CHECK(pa.help_text->find("attribute assignment, e.g. 'pending=False' "
                           "(fields: pending, indexed)") != std::string::npos);
  for (const char *opt : {"-c,--component", "--file", "--db", "--dry-run"}) {
    CHECK(pa.help_text->find(opt) != std::string::npos);
  }
}

TEST_CASE("args: verify grammar — --component/-c, --all, --db") {
  // $ cidx verify --component demo --all --db /tmp/i.db
  cli::ParsedArgs pa = cli::parse_args(
      {"db", "verify", "--component", "demo", "--all", "--db", "/tmp/i.db"});
  CHECK(pa.command == "verify");
  REQUIRE(pa.component);
  CHECK(*pa.component == "demo");
  CHECK(pa.all);
  REQUIRE(pa.index_db);
  CHECK(*pa.index_db == "/tmp/i.db");

  // bare verify: no component, --all off
  pa = cli::parse_args({"db", "verify"});
  CHECK_FALSE(pa.component);
  CHECK_FALSE(pa.all);

  // glued short option -cNAME
  pa = cli::parse_args({"db", "verify", "-cgraphlab"});
  REQUIRE(pa.component);
  CHECK(*pa.component == "graphlab");

  // unknown flag -> exit 2
  const ParseFail f = parse_fail({"db", "verify", "--bogus"});
  CHECK(f.code == 2);
}

TEST_CASE("args: verify -h lists its options") {
  cli::ParsedArgs pa = cli::parse_args({"db", "verify", "-h"});
  REQUIRE(pa.help_text);
  CHECK(pa.help_text->find("Usage: cidx db verify [OPTIONS]") !=
        std::string::npos);
  CHECK(pa.help_text->find("-c,--component") != std::string::npos);
  CHECK(pa.help_text->find("--all") != std::string::npos);
  CHECK(pa.help_text->find("--db") != std::string::npos);
}

// ---------------------------------------------------------------------------
// format helpers (default label)
// ---------------------------------------------------------------------------

TEST_CASE("format: mtime renders in LOCAL time (G31/D14)") {
  namespace fmt = cli::format;
  {
    ScopedEnv tz("TZ", "UTC");
    ::tzset();
    CHECK(fmt::format_mtime(1718000000.0) == "2024-06-10 06:13:20");
  }
  {
    ScopedEnv tz("TZ", "America/New_York"); // UTC-4 on 2024-06-10 (EDT)
    ::tzset();
    CHECK(fmt::format_mtime(1718000000.0) == "2024-06-10 02:13:20");
  }
  ::tzset();
}

TEST_CASE("format: py_str / py_repr / just helpers") {
  namespace fmt = cli::format;
  CHECK(fmt::py_str(std::optional<int64_t>{}) == "None");
  CHECK(fmt::py_str(std::optional<int64_t>{12}) == "12");
  CHECK(fmt::py_str(std::optional<std::string>{}) == "None");
  CHECK(fmt::py_repr("nope") == "'nope'");
  CHECK(fmt::py_repr("it's") == "\"it's\"");
  CHECK(fmt::rjust("1", 6) == "     1");
  CHECK(fmt::ljust("repo", 8) == "repo    ");
}

// ---------------------------------------------------------------------------
// R1/R9 exception-handler contract (default label)
// ---------------------------------------------------------------------------
// R9: makedirs() now checks errno and throws CidxError on any failure other
//   than EEXIST.  We verify the shape of that error propagates as CidxError
//   (which main catches with "error: …" + exit 1) and is NOT swallowed
//   silently.  makedirs itself lives in main.cpp's anonymous namespace and
//   cannot be called from tests, but Storage::open() throws CidxError on a
//   bad DB path — same catch-site chain as makedirs — so that path is used
//   as a proxy to confirm the error type and message shape.
//
// R1: main() previously lacked a catch(std::exception) handler.  Because
//   CidxError : std::runtime_error : std::exception, the EXISTING handlers
//   already cover CidxError subtypes; R1 adds coverage for non-CidxError
//   std::exception types (bad_alloc, regex_error, …).  We assert the type
//   hierarchy and simulate the new handler with a try/catch that mirrors
//   main()'s catch chain.

TEST_CASE("main: CidxError propagation shape (R9 proxy) + "
          "std::exception IS-A chain (R1)") {
  // --- R1: type-system assertion -------------------------------------------
  // CidxError IS-A std::exception; before R1 a bare std::runtime_error (not
  // CidxError) thrown from, say, a SQLite driver would escape both handlers.
  static_assert(std::is_base_of<std::exception, cidx::CidxError>::value,
                "CidxError must derive from std::exception");
  // A type that is std::exception but NOT CidxError (simulates third-party
  // throws that R1's catch(std::exception) must catch).
  static_assert(!std::is_base_of<cidx::CidxError, std::runtime_error>::value,
                "plain std::runtime_error must NOT be-a CidxError");

  // Runtime: simulate main()'s R1-extended catch chain on a plain
  // std::runtime_error — prior to R1 this would terminate(); now exit 1.
  int simulated_rc = -1;
  try {
    throw std::runtime_error("simulated third-party failure");
  } catch (const cidx::UsageError &) {
    simulated_rc = 2;
  } catch (const cidx::CidxError &) {
    simulated_rc = 1;
  } catch (const std::exception &) { // R1 new handler
    simulated_rc = 1;
  } catch (...) { // R1 new handler
    simulated_rc = 1;
  }
  CHECK(simulated_rc == 1);

  // --- R9 proxy: Storage open on bad path throws CidxError -----------------
  // On macOS /dev/null/bad.db is not creatable; on Linux same.
  const std::string t = make_temp_dir();
  write_file(t + "/index.db", ""); // ensure open fails via bad parent dir
  bool threw_cidx_error = false;
  std::string cidx_msg;
  try {
    // Construct a context with a DB path whose parent is unwritable.
    cidx::cli::Context ctx;
    ctx.cache_dir = t;
    ctx.index_path = "/dev/null/cidx-r9-test.db";
    ctx.logger = &cidx::Logger::root();
    std::ostringstream out, err;
    ctx.out = &out;
    ctx.err = &err;
    cidx::cli::ParsedArgs pa = cidx::cli::parse_args({"component", "list"});
    // run_command opens Storage; opening /dev/null/cidx-r9-test.db must throw.
    cidx::cli::run_command(pa, ctx);
  } catch (const cidx::CidxError &e) {
    threw_cidx_error = true;
    cidx_msg = e.what();
  }
  CHECK(threw_cidx_error);
  CHECK(!cidx_msg.empty()); // message carries the path/reason
}

// ---------------------------------------------------------------------------
// add-source (default label)
// ---------------------------------------------------------------------------

TEST_CASE("add-source: repo walks to git root, name from .git/config") {
  const std::string t = make_temp_dir();
  makedirs(t + "/repo/.git");
  makedirs(t + "/repo/sub");
  write_file(
      t + "/repo/.git/config",
      "[remote \"origin\"]\n\turl = https://example.com/gold-repo.git\n");
  // $ python3 -m indexer add-source --path <t>/repo/sub
  // component #1: gold-repo (repo) at <t>/repo
  CmdResult r = run_cli({"component", "add", "--path", t + "/repo/sub"}, t);
  CHECK(r.rc == 0);
  CHECK(r.out == "component #1: gold-repo (repo) at " + t + "/repo\n");
  CHECK(r.err.empty());

  // external: path as-is, name = basename; no git walk
  makedirs(t + "/ext");
  // $ python3 -m indexer add-source --path <t>/ext --kind external
  r = run_cli({"component", "add", "--path", t + "/ext", "--kind", "external"},
              t);
  CHECK(r.rc == 0);
  CHECK(r.out == "component #2: ext (external) at " + t + "/ext\n");

  // --name override; same path upserts to the same id
  r = run_cli({"component", "add", "--path", t + "/ext", "--kind", "external",
               "--name", "mylib"},
              t);
  CHECK(r.rc == 0);
  CHECK(r.out == "component #2: mylib (external) at " + t + "/ext\n");

  // repo kind without any .git up the tree: name = basename via repo_name
  makedirs(t + "/norepo");
  r = run_cli({"component", "add", "--path", t + "/norepo"}, t);
  CHECK(r.rc == 0);
  CHECK(r.out == "component #3: norepo (repo) at " + t + "/norepo\n");
}

TEST_CASE("add-source: --path not a directory -> exit 1") {
  const std::string t = make_temp_dir();
  // $ python3 -m indexer add-source --path <t>/missing
  const CmdResult r =
      run_cli({"component", "add", "--path", t + "/missing"}, t);
  CHECK(r.rc == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: " + t + "/missing is not a directory\n");
}

// ---------------------------------------------------------------------------
// query commands — golden outputs (default label)
// ---------------------------------------------------------------------------

TEST_CASE("query: read-only execution and nonexistent database safety") {
  const GoldFixture g;
  const CmdResult result =
      run_cli({"query", "codebase() | nodes() | limit(1)"}, g.cache);
  CHECK(result.rc == 0);
  CHECK(result.err.empty());
  CHECK(result.out.find("\"shape\": \"nodes\"") != std::string::npos);
  CHECK(result.out.find("\"index\":") != std::string::npos);

  const std::string missing = g.cache + "/does-not-exist.db";
  cli::ParsedArgs args = cli::parse_args({"query", "codebase() | nodes()"});
  cli::Context ctx;
  std::ostringstream out;
  std::ostringstream err;
  ctx.cache_dir = g.cache;
  ctx.index_path = missing;
  ctx.logger = &cidx::Logger::root();
  ctx.out = &out;
  ctx.err = &err;
  CHECK_THROWS(cli::run_command(args, ctx));
  CHECK_FALSE(path_exists(missing));
}

TEST_CASE("ui export rejects typed failures before publishing an artifact") {
  const std::string cache = make_temp_dir();
  const std::string index_path = cache + "/index.db";
  {
    Storage db(index_path);
    const int64_t component = db.add_component("ui", cache);
    const int64_t directory = db.add_directory(component, "");
    const int64_t file = db.add_file(directory, "main.cpp");
    Symbol exact;
    exact.usr = "USR::ui-exact";
    exact.spelling = "exact";
    exact.qual_name = "exact";
    exact.kind = "function";
    exact.file_id = file;
    exact.is_definition = true;
    exact.resolved = true;
    db.add_symbol(exact);
    for (const char *usr : {"USR::ui-ambiguous-a", "USR::ui-ambiguous-b"}) {
      Symbol ambiguous = exact;
      ambiguous.usr = usr;
      ambiguous.spelling = "ambiguous";
      ambiguous.qual_name = "ambiguous";
      db.add_symbol(ambiguous);
    }
    Symbol oversized = exact;
    oversized.usr = "USR::ui-oversized";
    oversized.spelling = std::string(4000, 'x');
    oversized.qual_name = oversized.spelling;
    db.add_symbol(oversized);
  }

  const std::string output = cache + "/snapshot.html";
  const auto check_rejected = [&](const std::vector<std::string> &input,
                                  const std::string &code) {
    ::unlink(output.c_str());
    const CmdResult result =
        run_cli({"ui", "export", "--output", output, "--input-kind", input[0],
                 "--input", input[1]},
                cache);
    CHECK(result.rc == 2);
    CHECK(result.err.find(code) != std::string::npos);
    CHECK_FALSE(path_exists(output));
  };

  check_rejected({"symbol", "USR::ui-missing"}, "E_UI_UNKNOWN_IDENTITY");
  check_rejected({"symbol", "ambiguous"}, "E_UI_AMBIGUOUS_IDENTITY");
  check_rejected({"cxq", "codebase() | nodes() | count()"},
                 "E_UI_UNSUPPORTED_INPUT");

  ::unlink(output.c_str());
  const CmdResult oversized =
      run_cli({"ui", "export", "--output", output, "--input-kind", "symbol",
               "--input", "USR::ui-oversized", "--byte-limit", "1024"},
              cache);
  CHECK(oversized.rc == 2);
  CHECK(oversized.err.find("E_UI_OVERSIZED") != std::string::npos);
  CHECK_FALSE(path_exists(output));
}

TEST_CASE("search: def row + second decl row; zero matches exit 1") {
  const GoldFixture g;
  // $ python3 -m indexer search multiply
  CmdResult r = run_cli({"search", "multiply"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("     1  multiply  function          def   "
                          "{ROOT}/src/a.c:12\n"
                          "                                    decl  "
                          "{ROOT}/include/a.h:3\n"
                          "1 match(es)\n"));

  // $ python3 -m indexer search Shape::area   ('::'-segment match, pure)
  r = run_cli({"search", "Shape::area"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("     5  NS::Shape::area  method            pure  "
                          "{ROOT}/include/a.h:12\n"
                          "1 match(es)\n"));

  // $ python3 -m indexer search zz
  r = run_cli({"search", "zz"}, g.cache);
  CHECK(r.rc == 1);
  CHECK(r.out == "0 match(es)\n");
}

TEST_CASE("search: --limit slicing, 0 = all, --kind filter") {
  const GoldFixture g;
  // $ python3 -m indexer search a --limit 2
  CmdResult r = run_cli({"search", "a", "--limit", "2"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("     2  square     function          decl  "
                          "{ROOT}/include/a.h:4\n"
                          "     4  NS::Shape  class             def   "
                          "{ROOT}/include/a.h:10\n"
                          "3 match(es) (showing 2)\n"));

  // $ python3 -m indexer search a --limit 0
  r = run_cli({"search", "a", "--limit", "0"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("     2  square           function          decl  "
                          "{ROOT}/include/a.h:4\n"
                          "     4  NS::Shape        class             def   "
                          "{ROOT}/include/a.h:10\n"
                          "     5  NS::Shape::area  method            pure  "
                          "{ROOT}/include/a.h:12\n"
                          "3 match(es)\n"));

  // $ python3 -m indexer search square --kind function
  r = run_cli({"search", "square", "--kind", "function"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("     2  square  function          decl  "
                          "{ROOT}/include/a.h:4\n"
                          "1 match(es)\n"));

  // $ python3 -m indexer search counter --kind class
  r = run_cli({"search", "counter", "--kind", "class"}, g.cache);
  CHECK(r.rc == 1);
  CHECK(r.out == "0 match(es)\n");
}

TEST_CASE("show symbol: by id and USR; None fields omitted; glosses") {
  const GoldFixture g;
  // $ python3 -m indexer show symbol 1
  CmdResult r = run_cli({"symbol", "show", "1"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("id           1\n"
                          "usr          c:@F@multiply\n"
                          "name         multiply\n"
                          "qualified    multiply\n"
                          "display      multiply(int, int)\n"
                          "kind         function\n"
                          "type         int (int, int)\n"
                          "visibility   program-wide (usable from any .cpp)\n"
                          "definition   {ROOT}/src/a.c:12:5\n"
                          "declaration  {ROOT}/include/a.h:3:5\n"
                          "resolved     yes\n"));

  // $ python3 -m indexer show symbol 'c:@F@square'   (USR lookup)
  r = run_cli({"symbol", "show", "c:@F@square"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("id           2\n"
                          "usr          c:@F@square\n"
                          "name         square\n"
                          "qualified    square\n"
                          "display      square(int)\n"
                          "kind         function\n"
                          "type         int (int)\n"
                          "visibility   program-wide (usable from any .cpp)\n"
                          "declaration  {ROOT}/include/a.h:4:5\n"
                          "resolved     no (definition not seen)\n"));

  // $ python3 -m indexer show symbol 5   (pure virtual + parent + access)
  r = run_cli({"symbol", "show", "5"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("id           5\n"
                          "usr          c:@N@NS@S@Shape@F@area#\n"
                          "name         area\n"
                          "qualified    NS::Shape::area\n"
                          "display      area()\n"
                          "kind         method\n"
                          "type         double ()\n"
                          "visibility   program-wide (usable from any .cpp)\n"
                          "access       public\n"
                          "parent       NS::Shape  [c:@N@NS@S@Shape]\n"
                          "pure         yes (pure virtual; implemented by "
                          "overriders)\n"
                          "resolved     n/a (pure virtual)\n"));

  // $ python3 -m indexer show symbol 6   (internal linkage gloss)
  r = run_cli({"symbol", "show", "6"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out ==
        g.expect("id           6\n"
                 "usr          c:a.c@counter\n"
                 "name         counter\n"
                 "qualified    counter\n"
                 "display      counter\n"
                 "kind         variable\n"
                 "type         int\n"
                 "visibility   file-local (static / anonymous namespace)\n"
                 "definition   {ROOT}/src/a.c:7:12\n"
                 "resolved     yes\n"));

  // $ python3 -m indexer show symbol 3   (no linkage stored: visibility,
  // type, parent all omitted)
  r = run_cli({"symbol", "show", "3"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("id           3\n"
                          "usr          c:@N@NS\n"
                          "name         NS\n"
                          "qualified    NS\n"
                          "display      NS\n"
                          "kind         namespace\n"
                          "definition   {ROOT}/include/a.h:8:11\n"
                          "resolved     yes\n"));

  // $ python3 -m indexer show symbol 99
  r = run_cli({"symbol", "show", "99"}, g.cache);
  CHECK(r.rc == 1);
  CHECK(r.err == "error: no symbol with id/USR '99'\n");
}

TEST_CASE("show file: by path and id; G31 time formats; G20 placeholder") {
  const GoldFixture g;
  ScopedEnv tz("TZ", "UTC"); // mtime is local-time formatted; pin it
  ::tzset();

  // $ TZ=UTC python3 -m indexer show file <root>/src/a.c
  CmdResult r = run_cli({"file", "show", g.root + "/src/a.c"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("id           1\n"
                          "path         {ROOT}/src/a.c\n"
                          "component    gold (repo)  {ROOT}\n"
                          "directory    src\n"
                          "mtime        2024-06-10 06:13:20\n"
                          "md5          0123456789abcdef0123456789abcdef\n"
                          "driver       gcc\n"
                          "options      -I{ROOT}/include -DX=1\n"
                          "indexed      no (content changed since import)\n"
                          "indexed at   2026-06-12 10:00:00 UTC\n"
                          "symbols      2 (2 defined here, 0 declared here)\n"
                          "by kind      function: 1, variable: 1\n"));

  // $ python3 -m indexer show file 2   (header row: NULL options/driver)
  r = run_cli({"file", "show", "2"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out ==
        g.expect("id           2\n"
                 "path         {ROOT}/include/a.h\n"
                 "component    gold (repo)  {ROOT}\n"
                 "directory    include\n"
                 "options      (none -- header indexed via an including "
                 "TU)\n"
                 "indexed      no (never indexed)\n"
                 "symbols      5 (2 defined here, 2 declared here)\n"
                 "by kind      class: 1, function: 2, method: 1, "
                 "namespace: 1\n"));

  // $ python3 -m indexer show file bogus.c -c gold   (error names the REF)
  r = run_cli({"file", "show", "bogus.c", "-c", "gold"}, g.cache);
  CHECK(r.rc == 1);
  CHECK(r.err == "error: not in index database: bogus.c\n");

  // $ python3 -m indexer show file 99
  r = run_cli({"file", "show", "99"}, g.cache);
  CHECK(r.rc == 1);
  CHECK(r.err == "error: not in index database: 99\n");
  ::tzset();
}

TEST_CASE("diagnostics: list-files indicator + show-file section (v15)") {
  const GoldFixture g;
  // Inject one error + one warning on file 1 (src/a.c) via the storage API.
  {
    Storage db(g.cache + "/index.db");
    std::vector<Diagnostic> diags;
    Diagnostic e;
    e.severity = 3;
    e.spelling = "boom";
    e.file_path = g.root + "/src/a.c";
    e.line = 10;
    e.col = 2;
    diags.push_back(e);
    Diagnostic w;
    w.severity = 2;
    w.spelling = "meh";
    w.file_path = g.root + "/src/a.c";
    w.line = 12;
    w.col = 3;
    diags.push_back(w);
    db.replace_diagnostics(1, diags);
  }

  // list files: a.c shows the '1E1W' indicator; the clean header shows '-'.
  CmdResult r = run_cli({"file", "list"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out.find("1E1W") != std::string::npos);
  CHECK(r.out.find(g.expect("{ROOT}/src/a.c")) != std::string::npos);

  // show file 1: summary field + one line per diagnostic, in TU order.
  r = run_cli({"file", "show", "1"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out.find("diagnostics  1 error(s), 1 warning(s)\n") !=
        std::string::npos);
  CHECK(r.out.find(g.expect("  error   {ROOT}/src/a.c:10:2: boom\n")) !=
        std::string::npos);
  CHECK(r.out.find(g.expect("  warning {ROOT}/src/a.c:12:3: meh\n")) !=
        std::string::npos);

  // A clean file shows no diagnostics field.
  r = run_cli({"file", "show", "2"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out.find("diagnostics") == std::string::npos);
}

TEST_CASE("list components: table, kind filter, fuzzy pattern, ls alias") {
  const GoldFixture g;
  // $ python3 -m indexer list components
  CmdResult r = run_cli({"component", "list"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out ==
        g.expect("   1  gold  repo      -  -  {ROOT}\n1 component(s)\n"));

  // $ python3 -m indexer ls components   (alias, same output)
  r = run_cli({"component", "ls"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out ==
        g.expect("   1  gold  repo      -  -  {ROOT}\n1 component(s)\n"));

  // $ python3 -m indexer list components --kind external   (0 rows, exit 1)
  r = run_cli({"component", "list", "--kind", "external"}, g.cache);
  CHECK(r.rc == 1);
  CHECK(r.out == "0 component(s)\n");

  // $ python3 -m indexer list components gld   (char-in-order fuzzy)
  r = run_cli({"component", "list", "gld"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out ==
        g.expect("   1  gold  repo      -  -  {ROOT}\n1 component(s)\n"));
}

TEST_CASE("list dirs: table + unknown component error") {
  const GoldFixture g;
  // $ python3 -m indexer list dirs
  CmdResult r = run_cli({"dir", "list"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == "   2  gold  include\n"
                 "   1  gold  src\n"
                 "2 directory(ies)\n");

  // $ python3 -m indexer list dirs -c gold
  r = run_cli({"dir", "list", "-c", "gold"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == "   2  gold  include\n"
                 "   1  gold  src\n"
                 "2 directory(ies)\n");

  // $ python3 -m indexer list dirs -c nope
  r = run_cli({"dir", "list", "-c", "nope"}, g.cache);
  CHECK(r.rc == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: no component named 'nope'\n");
}

TEST_CASE("list files: idx/pend marks, --indexed/--pending, --dir scope") {
  const GoldFixture g;
  // $ python3 -m indexer list files
  CmdResult r = run_cli({"file", "list"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("   2  pend  -  -  {ROOT}/include/a.h\n"
                          "   1  idx   -  -  {ROOT}/src/a.c\n"
                          "2 file(s)\n"));

  // $ python3 -m indexer list files --pending
  r = run_cli({"file", "list", "--pending"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("   2  pend  -  -  {ROOT}/include/a.h\n1 file(s)\n"));

  // $ python3 -m indexer list files --indexed
  r = run_cli({"file", "list", "--indexed"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("   1  idx   -  -  {ROOT}/src/a.c\n1 file(s)\n"));

  // $ python3 -m indexer list files -c gold -d src
  r = run_cli({"file", "list", "-c", "gold", "-d", "src"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("   1  idx   -  -  {ROOT}/src/a.c\n1 file(s)\n"));
}

TEST_CASE("list files/symbols: --dir without --component -> exit 1") {
  const GoldFixture g;
  const char kMsg[] = "error: --dir requires --component (directory paths "
                      "are relative to a component root)\n";
  // $ python3 -m indexer list files -d src
  CmdResult r = run_cli({"file", "list", "-d", "src"}, g.cache);
  CHECK(r.rc == 1);
  CHECK(r.out.empty());
  CHECK(r.err == kMsg);
  // $ python3 -m indexer list symbols -d src
  r = run_cli({"symbol", "list", "-d", "src"}, g.cache);
  CHECK(r.rc == 1);
  CHECK(r.err == kMsg);
}

TEST_CASE("list symbols: full table, limit, fuzzy, scopes, kind, file") {
  const GoldFixture g;
  const std::string full_table =
      g.expect("     3  NS               namespace         def   "
               "{ROOT}/include/a.h:8\n"
               "     2  square           function          decl  "
               "{ROOT}/include/a.h:4\n"
               "     6  counter          variable          def   "
               "{ROOT}/src/a.c:7\n"
               "     1  multiply         function          def   "
               "{ROOT}/src/a.c:12\n"
               "                                           decl  "
               "{ROOT}/include/a.h:3\n"
               "     4  NS::Shape        class             def   "
               "{ROOT}/include/a.h:10\n"
               "     5  NS::Shape::area  method            pure  "
               "{ROOT}/include/a.h:12\n"
               "6 match(es)\n");
  // $ python3 -m indexer list symbols
  CmdResult r = run_cli({"symbol", "list"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == full_table);

  // $ python3 -m indexer list symbols --limit 2
  r = run_cli({"symbol", "list", "--limit", "2"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("     3  NS      namespace         def   "
                          "{ROOT}/include/a.h:8\n"
                          "     2  square  function          decl  "
                          "{ROOT}/include/a.h:4\n"
                          "6 match(es) (showing 2)\n"));

  // $ python3 -m indexer list symbols ar   (char-in-order fuzzy, G18)
  r = run_cli({"symbol", "list", "ar"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("     2  square           function          decl  "
                          "{ROOT}/include/a.h:4\n"
                          "     5  NS::Shape::area  method            pure  "
                          "{ROOT}/include/a.h:12\n"
                          "2 match(es)\n"));

  const std::string include_scope =
      g.expect("     3  NS               namespace         def   "
               "{ROOT}/include/a.h:8\n"
               "     2  square           function          decl  "
               "{ROOT}/include/a.h:4\n"
               "     1  multiply         function          def   "
               "{ROOT}/src/a.c:12\n"
               "                                           decl  "
               "{ROOT}/include/a.h:3\n"
               "     4  NS::Shape        class             def   "
               "{ROOT}/include/a.h:10\n"
               "     5  NS::Shape::area  method            pure  "
               "{ROOT}/include/a.h:12\n"
               "5 match(es)\n");
  // $ python3 -m indexer list symbols -c gold -d include   (decl OR def
  // site in scope — multiply's def lives in src/ but its decl is here)
  r = run_cli({"symbol", "list", "-c", "gold", "-d", "include"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == include_scope);

  // $ python3 -m indexer list symbols -f <root>/include/a.h  (same rows)
  r = run_cli({"symbol", "list", "-f", g.root + "/include/a.h"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == include_scope);

  // $ python3 -m indexer list symbols --kind method
  r = run_cli({"symbol", "list", "--kind", "method"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(r.out == g.expect("     5  NS::Shape::area  method            pure  "
                          "{ROOT}/include/a.h:12\n"
                          "1 match(es)\n"));

  // $ python3 -m indexer list symbols -f a.h -c gold   (resolved against
  // the component root; error names the resolved path)
  r = run_cli({"symbol", "list", "-f", "a.h", "-c", "gold"}, g.cache);
  CHECK(r.rc == 1);
  CHECK(r.err == g.expect("error: not in index database: {ROOT}/a.h\n"));

  // $ python3 -m indexer list symbols zz
  r = run_cli({"symbol", "list", "zz"}, g.cache);
  CHECK(r.rc == 1);
  CHECK(r.out == "0 match(es)\n");
}

// ---------------------------------------------------------------------------
// index — hermetic paths (default label; no parse happens, so no Clang runtime)
// ---------------------------------------------------------------------------

TEST_CASE("index: empty DB, unknown --source, unknown FILE — hermetic") {
  const std::string t = make_temp_dir();
  cidx::Logger log;
  log.set_file(t + "/cidx.log");

  // $ python3 -m indexer index   (empty index: nothing pending, exit 0)
  CmdResult r = run_cli({"index"}, t, &log);
  CHECK(r.rc == 0);
  CHECK(r.out == "index: 0 indexed, 0 failed, 0 already indexed\n");
  CHECK(r.err.empty());

  // $ python3 -m indexer index --source nope   (LookupError path: exit 1,
  // no summary line, no warning-count line)
  r = run_cli({"index", "--source", "nope"}, t, &log);
  CHECK(r.rc == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: no component named 'nope'\n");

  // $ python3 -m indexer index /no/such/file.c   (unknown FILE: exit 1)
  r = run_cli({"index", "/no/such/file.c"}, t, &log);
  CHECK(r.rc == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: not in index database: /no/such/file.c\n");

  CHECK(!path_exists(t + "/cidx.log")); // nothing was ever logged (G27)
}

TEST_CASE("init: blank DB, already-exists error, --force recreate — hermetic") {
  const std::string t = make_temp_dir();
  const std::string db = t + "/index.db";

  // $ python3 -m indexer init   (fresh: materialize blank schema-v6 DB)
  CHECK(!path_exists(db));
  CmdResult r = run_cli({"init"}, t);
  CHECK(r.rc == 0);
  CHECK(r.out == "initialized empty index database at " + db + "\n");
  CHECK(r.err.empty());
  CHECK(path_exists(db));

  // Blank: schema present (a component can be added), zero rows.
  {
    Storage check(db);
    CHECK(check.list_components().empty());
  }

  // $ python3 -m indexer init   (again: refuse to clobber, exit 1)
  r = run_cli({"init"}, t);
  CHECK(r.rc == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: index database already exists at " + db +
                     " (use --force to recreate)\n");

  // Put a row in, then prove --force wipes it back to blank.
  {
    Storage seed(db);
    seed.add_component("gone", "/no/such/root", "repo");
    CHECK(seed.list_components().size() == 1);
  }
  // $ python3 -m indexer init --force   (recreate: drop + reapply schema)
  r = run_cli({"init", "--force"}, t);
  CHECK(r.rc == 0);
  CHECK(r.out == "recreated empty index database at " + db + "\n");
  CHECK(r.err.empty());
  {
    Storage check(db);
    CHECK(check.list_components().empty()); // the seeded row is gone
  }
}

TEST_CASE("args: init grammar — --force flag, no positionals") {
  // happy: bare init
  cli::ParsedArgs pa = cli::parse_args({"init"});
  CHECK(pa.command == "init");
  CHECK(pa.force == false);

  pa = cli::parse_args({"init", "--force"});
  CHECK(pa.command == "init");
  CHECK(pa.force == true);

  // -h returns help text (exit 0 path)
  pa = cli::parse_args({"init", "-h"});
  REQUIRE(pa.help_text.has_value());
  CHECK(*pa.help_text ==
        "create a blank index database\n"
        "Usage: cidx init [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -h,--help                   Print this help message and exit\n"
        "  --force                     overwrite an existing index database\n");

  // unknown flag -> unexpected argument, exit 2
  ParseFail f = parse_fail({"init", "--bogus"});
  CHECK(f.code == 2);
  CHECK(f.msg == "Usage: cidx init [OPTIONS]\n"
                 "cidx init: error: The following argument was not expected: "
                 "--bogus\n");

  // stray positional -> unexpected argument, exit 2
  f = parse_fail({"init", "extra"});
  CHECK(f.code == 2);
  CHECK(f.msg == "Usage: cidx init [OPTIONS]\n"
                 "cidx init: error: The following argument was not expected: "
                 "extra\n");
}

TEST_CASE("migrate: missing DB errors; already-current is a no-op (hermetic)") {
  const std::string t = make_temp_dir();
  const std::string db = t + "/index.db";

  // No index yet -> error, exit 1.
  CmdResult r = run_cli({"db", "migrate"}, t);
  CHECK(r.rc == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: no index database at " + db +
                     " (run `cidx init` / `cidx import` first)\n");

  // Fresh init is already current -> no-op message, exit 0.
  run_cli({"init"}, t);
  r = run_cli({"db", "migrate"}, t);
  CHECK(r.rc == 0);
  CHECK(r.err.empty());
  CHECK(r.out == db + " already at schema v" +
                     std::to_string(cidx::kSchemaVersion) +
                     "; nothing to migrate\n");
}

TEST_CASE("migrate: upgrades a v15 DB in place (kind TEXT->int), no re-index") {
  const std::string t = make_temp_dir();
  const std::string db = t + "/index.db";

  // Build a minimal v15 DB: symbol.kind is a TEXT name with the old CHECK.
  {
    cidx::SqliteDb raw(db);
    raw.exec(
        "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);"
        "INSERT INTO meta VALUES ('schema_version','15');"
        "CREATE TABLE component (id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
        " path TEXT NOT NULL UNIQUE, kind TEXT NOT NULL DEFAULT 'repo',"
        " version TEXT);"
        "CREATE TABLE directory (id INTEGER PRIMARY KEY, component_id INTEGER,"
        " path TEXT);"
        "CREATE TABLE file (id INTEGER PRIMARY KEY, directory_id INTEGER,"
        " name TEXT NOT NULL, mtime REAL, md5 TEXT, compile_options TEXT,"
        " driver TEXT, indexed INTEGER NOT NULL DEFAULT 0, indexed_at TEXT,"
        " args_overridden INTEGER NOT NULL DEFAULT 0,"
        " UNIQUE(directory_id,name));"
        "CREATE TABLE symbol (id INTEGER PRIMARY KEY, usr TEXT NOT NULL UNIQUE,"
        " spelling TEXT NOT NULL, qual_name TEXT, display_name TEXT,"
        " kind TEXT NOT NULL CHECK (kind IN ('class','struct','function',"
        "'method','macro')), type_info TEXT, file_id INTEGER, line INTEGER,"
        " col INTEGER, decl_file_id INTEGER, decl_line INTEGER,"
        " decl_col INTEGER, decl_path TEXT,"
        " is_definition INTEGER NOT NULL DEFAULT 0,"
        " is_pure INTEGER NOT NULL DEFAULT 0,"
        " is_static INTEGER NOT NULL DEFAULT 0,"
        " is_instantiation INTEGER NOT NULL DEFAULT 0, linkage TEXT,"
        " access TEXT, parent_usr TEXT, resolved INTEGER NOT NULL DEFAULT 0);"
        "INSERT INTO symbol (usr,spelling,kind) VALUES "
        "('c:@F@f','f','function'),('c:@S@S','S','struct');");
  }

  CmdResult r = run_cli({"db", "migrate"}, t);
  CHECK(r.rc == 0);
  CHECK(r.err.empty());
  CHECK(r.out == "migrated " + db + ": schema v15 -> v" +
                     std::to_string(cidx::kSchemaVersion) + "\n");

  // kind is now the CXCursorKind int, symbol_kind seeded, rows preserved.
  {
    cidx::SqliteDb raw(db);
    auto st =
        raw.prepare("SELECT typeof(kind), kind FROM symbol WHERE usr='c:@F@f'");
    REQUIRE(st.step());
    CHECK(st.col_text(0) == "integer");
    CHECK(st.col_int64(1) == 8); // function == CXCursor_FunctionDecl
    auto m = raw.prepare("SELECT COUNT(*) FROM symbol_kind");
    REQUIRE(m.step());
    CHECK(m.col_int64(0) == 17);
  }

  // Idempotent: a second migrate is a no-op.
  r = run_cli({"db", "migrate"}, t);
  CHECK(r.rc == 0);
  CHECK(r.out == db + " already at schema v" +
                     std::to_string(cidx::kSchemaVersion) +
                     "; nothing to migrate\n");
}

TEST_CASE("migrate: v17 -> v19 drops nests edges and renumbers befriends") {
  const std::string t = make_temp_dir();
  const std::string db = t + "/index.db";

  // Build a current DB, then mutate it to look like v17: the old kind table
  // (nests=10, befriends=11) plus one nests(10) edge and one befriends(11) edge
  // between two entity symbols.
  {
    cidx::Storage s(db);
    auto &raw = s.raw_db();
    raw.exec(
        "INSERT INTO symbol (usr,spelling,kind) VALUES "
        "('c:@S@A','A',4),('c:@S@B','B',4);"
        "DELETE FROM entity_edge_kind;"
        "INSERT INTO entity_edge_kind (id,name) VALUES "
        "(1,'generalizes'),(2,'realizes'),(3,'specializes'),(4,'composes'),"
        "(5,'aggregates'),(6,'associates'),(7,'creates'),(8,'uses'),"
        "(9,'destroys'),(10,'nests'),(11,'befriends');"
        "INSERT INTO entity_edge (src_id,dst_id,kind) SELECT a.id,b.id,10 "
        "FROM (SELECT id FROM symbol WHERE usr='c:@S@A') a,"
        "(SELECT id FROM symbol WHERE usr='c:@S@B') b;"
        "INSERT INTO entity_edge (src_id,dst_id,kind) SELECT a.id,b.id,11 "
        "FROM (SELECT id FROM symbol WHERE usr='c:@S@A') a,"
        "(SELECT id FROM symbol WHERE usr='c:@S@B') b;"
        "UPDATE meta SET value='17' WHERE key='schema_version';");
  }

  // migrate via the CLI: reports the in-place upgrade.
  CmdResult r = run_cli({"db", "migrate"}, t);
  CHECK(r.rc == 0);
  CHECK(r.err.empty());
  CHECK(r.out == "migrated " + db + ": schema v17 -> v" +
                     std::to_string(cidx::kSchemaVersion) + "\n");

  {
    cidx::SqliteDb raw(db);
    {
      auto st =
          raw.prepare("SELECT value FROM meta WHERE key='schema_version'");
      REQUIRE(st.step());
      CHECK(st.col_text(0) == std::to_string(cidx::kSchemaVersion));
    }
    // the nests row is gone; only befriends survives, renumbered 11 -> 10.
    {
      auto st = raw.prepare("SELECT COUNT(*), MIN(kind) FROM entity_edge");
      REQUIRE(st.step());
      CHECK(st.col_int64(0) == 1);
      CHECK(st.col_int64(1) == 10);
    }
    // entity_edge_kind reseeded: 12 rows (old (11,'befriends') renumbered to
    // 10, freeing id 11 for the reseeded instantiates; v26 adds declares(12)),
    // id 10 = befriends, no 'nests'.
    {
      auto st = raw.prepare("SELECT COUNT(*) FROM entity_edge_kind");
      REQUIRE(st.step());
      CHECK(st.col_int64(0) == 12);
    }
    {
      auto st = raw.prepare("SELECT name FROM entity_edge_kind WHERE id=10");
      REQUIRE(st.step());
      CHECK(st.col_text(0) == "befriends");
    }
    {
      auto st = raw.prepare("SELECT name FROM entity_edge_kind WHERE id=11");
      REQUIRE(st.step());
      CHECK(st.col_text(0) == "instantiates");
    }
    {
      auto st = raw.prepare("SELECT name FROM entity_edge_kind WHERE id=12");
      REQUIRE(st.step());
      CHECK(st.col_text(0) == "declares");
    }
    {
      auto st = raw.prepare(
          "SELECT COUNT(*) FROM entity_edge_kind WHERE name='nests'");
      REQUIRE(st.step());
      CHECK(st.col_int64(0) == 0);
    }
    // realizes(2) renamed to implements(2).
    {
      auto st = raw.prepare("SELECT name FROM entity_edge_kind WHERE id=2");
      REQUIRE(st.step());
      CHECK(st.col_text(0) == "implements");
    }
  }

  // Idempotent: a second migrate is a no-op.
  r = run_cli({"db", "migrate"}, t);
  CHECK(r.rc == 0);
  CHECK(r.out == db + " already at schema v" +
                     std::to_string(cidx::kSchemaVersion) +
                     "; nothing to migrate\n");
}

TEST_CASE(
    "migrate: cleans a DB already stamped current but still carrying nests") {
  // Regression: an earlier build bumped schema_version WITHOUT cleaning, so the
  // migration must be gated on the stale 'nests' marker, not the version.
  const std::string t = make_temp_dir();
  const std::string db = t + "/index.db";

  {
    cidx::Storage s(db);
    auto &raw = s.raw_db();
    raw.exec(
        "INSERT INTO symbol (usr,spelling,kind) VALUES "
        "('c:@S@A','A',4),('c:@S@B','B',4);"
        "DELETE FROM entity_edge_kind;"
        "INSERT INTO entity_edge_kind (id,name) VALUES "
        "(1,'generalizes'),(2,'realizes'),(3,'specializes'),(4,'composes'),"
        "(5,'aggregates'),(6,'associates'),(7,'creates'),(8,'uses'),"
        "(9,'destroys'),(10,'nests'),(11,'befriends');"
        "INSERT INTO entity_edge (src_id,dst_id,kind) SELECT a.id,b.id,10 "
        "FROM (SELECT id FROM symbol WHERE usr='c:@S@A') a,"
        "(SELECT id FROM symbol WHERE usr='c:@S@B') b;"
        "INSERT INTO entity_edge (src_id,dst_id,kind) SELECT a.id,b.id,11 "
        "FROM (SELECT id FROM symbol WHERE usr='c:@S@A') a,"
        "(SELECT id FROM symbol WHERE usr='c:@S@B') b;");
    // NOTE: schema_version stays at the fresh stamp (== kSchemaVersion) -- the
    // dirty case where the version is current but the data was never cleaned.
  }

  // migrate reports the cleanup even though the version does not change.
  CmdResult r = run_cli({"db", "migrate"}, t);
  CHECK(r.rc == 0);
  CHECK(r.err.empty());
  CHECK(r.out == "migrated " + db +
                     ": refreshed entity relation kinds (schema v" +
                     std::to_string(cidx::kSchemaVersion) + ")\n");

  {
    cidx::SqliteDb raw(db);
    auto st = raw.prepare("SELECT COUNT(*), MIN(kind) FROM entity_edge");
    REQUIRE(st.step());
    CHECK(st.col_int64(0) == 1);  // only befriends survives
    CHECK(st.col_int64(1) == 10); // renumbered 11 -> 10
    auto k =
        raw.prepare("SELECT COUNT(*) FROM entity_edge_kind WHERE name='nests'");
    REQUIRE(k.step());
    CHECK(k.col_int64(0) == 0);
    // realizes(2) renamed to implements(2).
    auto im = raw.prepare("SELECT name FROM entity_edge_kind WHERE id=2");
    REQUIRE(im.step());
    CHECK(im.col_text(0) == "implements");
  }

  // Now clean -> second migrate is a true no-op.
  r = run_cli({"db", "migrate"}, t);
  CHECK(r.rc == 0);
  CHECK(r.out == db + " already at schema v" +
                     std::to_string(cidx::kSchemaVersion) +
                     "; nothing to migrate\n");
}

TEST_CASE("args: migrate grammar — --db option, -h help") {
  cli::ParsedArgs pa = cli::parse_args({"db", "migrate"});
  CHECK(pa.command == "migrate");
  CHECK(!pa.index_db.has_value());

  pa = cli::parse_args({"db", "migrate", "--db", "/tmp/x.db"});
  CHECK(pa.command == "migrate");
  REQUIRE(pa.index_db.has_value());
  CHECK(*pa.index_db == "/tmp/x.db");

  pa = cli::parse_args({"db", "migrate", "-h"});
  REQUIRE(pa.help_text.has_value());
  CHECK(pa.help_text->find("Usage: cidx db migrate [OPTIONS]") !=
        std::string::npos);
  CHECK(pa.help_text->find("index database (default: the standard cache "
                           "index)") != std::string::npos);
}

TEST_CASE("query-only invocations never create cidx.log (G27/D7)") {
  const GoldFixture g;
  const std::string log = g.cache + "/cidx.log";
  cidx::Logger::root().set_file(log); // what main() does — lazy open
  CmdResult r = run_cli({"search", "multiply"}, g.cache);
  CHECK(r.rc == 0);
  r = run_cli({"file", "list"}, g.cache);
  CHECK(r.rc == 0);
  r = run_cli({"file", "show", "2"}, g.cache);
  CHECK(r.rc == 0);
  CHECK(!path_exists(log));
}

// ---------------------------------------------------------------------------
// import — needs CompileDb::load (label "clang")
// ---------------------------------------------------------------------------

TEST_SUITE("clang") {

  TEST_CASE("index TU publication is atomic at every injected failure point") {
    const std::string dir = make_temp_dir();
    const std::string source = dir + "/source.cpp";
    const std::string header = dir + "/header.hpp";
    write_file(header, "struct PublishedHeader { int value; };\n");
    write_file(source, "#include \"header.hpp\"\n"
                       "int published_pipeline_symbol() { return 1; }\n");

    Storage db(":memory:");
    db.add_component("pipeline", dir);
    const int64_t file_id = db.add_file_path(
        source, std::nullopt, std::nullopt,
        std::vector<std::string>{"-std=c++23"}, std::string("clang++"));
    const auto file = db.get_file_by_id(file_id);
    REQUIRE(file.has_value());
    db.stamp_graph_resolved();
    db.stamp_index_identity();
    REQUIRE_NOTHROW(cidx::ast::run_index_one(
        db, *file, source, true, cidx::ast::IndexFailurePoint::none));
    const auto published = snapshot_tu_publication(db, source);
    write_file(source, "#include \"header.hpp\"\n"
                       "int changed_pipeline_symbol() { return 2; }\n");

    for (const auto failure : {cidx::ast::IndexFailurePoint::begin,
                               cidx::ast::IndexFailurePoint::adapter,
                               cidx::ast::IndexFailurePoint::partial_transform,
                               cidx::ast::IndexFailurePoint::commit}) {
      CHECK_THROWS(cidx::ast::run_index_one(db, *file, source, true, failure));
      CHECK(snapshot_tu_publication(db, source) == published);
    }

    REQUIRE_NOTHROW(cidx::ast::run_index_one(
        db, *file, source, true, cidx::ast::IndexFailurePoint::none));
    CHECK(db.find_symbols("changed_pipeline_symbol", {}, 10).size() == 1);
    CHECK(db.find_symbols("published_pipeline_symbol", {}, 10).empty());
  }

  TEST_CASE("index TU publishes unsupported-call evidence and metrics") {
    const std::string dir = make_temp_dir();
    const std::string source = dir + "/unsupported.cpp";
    write_file(source, "template <typename T> void unsupported(T value) {\n"
                       "  value();\n"
                       "}\n"
                       "void indirect() { void (*callable)(); callable(); }\n");

    Storage db(":memory:");
    db.add_component("unsupported", dir);
    const int64_t file_id = db.add_file_path(
        source, std::nullopt, std::nullopt,
        std::vector<std::string>{"-std=c++23"}, std::string("clang++"));
    const auto file = db.get_file_by_id(file_id);
    REQUIRE(file.has_value());
    db.stamp_graph_resolved();
    db.stamp_index_identity();

    const cidx::ast::IndexOneOutcome outcome =
        cidx::ast::run_index_one(db, *file, source, true);
    REQUIRE(!outcome.parse_failed);
    bool found_evidence = false;
    for (const auto &evidence : outcome.evidence) {
      found_evidence |= evidence.construct == "CallExpr";
    }
    CHECK(found_evidence);
    bool found_unknown_metric = false;
    for (const auto &metrics : outcome.pass_metrics) {
      if (metrics.id == "statements.main") {
        found_unknown_metric = metrics.unknown_constructs > 0;
      }
    }
    CHECK(found_unknown_metric);
    for (const auto &metrics : outcome.pass_metrics) {
      if (metrics.id == "statements.main") {
        CHECK(metrics.unknown_constructs >= 2);
      }
    }
  }

  TEST_CASE("index plan exposes the complete main and header lifecycle") {
    const std::string dir = make_temp_dir();
    const std::string source = dir + "/sequence.cpp";
    const std::string header = dir + "/sequence.hpp";
    write_file(header, "struct SequenceHeader { int value; };\n");
    write_file(source, "#include \"sequence.hpp\"\n"
                       "int sequence_main() { return 1; }\n");

    Storage db(":memory:");
    db.add_component("sequence", dir);
    const int64_t file_id = db.add_file_path(
        source, std::nullopt, std::nullopt,
        std::vector<std::string>{"-std=c++23"}, std::string("clang++"));
    const auto file = db.get_file_by_id(file_id);
    REQUIRE(file.has_value());
    db.stamp_graph_resolved();
    db.stamp_index_identity();

    const auto outcome = cidx::ast::run_index_one(db, *file, source, true);
    REQUIRE(!outcome.parse_failed);
    const std::vector<std::string> expected{
        "symbols.main",         "symbols.headers",     "lifecycle.headers",
        "declarations.headers", "definitions.headers", "statements.headers",
        "namespaces.headers",   "headers.associate",   "lifecycle.main",
        "declarations.main",    "definitions.main",    "statements.main",
        "namespaces.main",      "main.associate",      "presentation.persist",
        "includes.persist",     "evidence.persist"};
    REQUIRE(outcome.pass_metrics.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
      CHECK(outcome.pass_metrics[index].id == expected[index]);
      CHECK(!outcome.pass_metrics[index].produced_fact_families.empty());
    }
    const std::vector<std::vector<cidx::ast::FrontendCapability>> capabilities{
        {cidx::ast::FrontendCapability::ast},
        {cidx::ast::FrontendCapability::ast,
         cidx::ast::FrontendCapability::preprocessor},
        {},
        {cidx::ast::FrontendCapability::ast},
        {cidx::ast::FrontendCapability::ast},
        {cidx::ast::FrontendCapability::ast,
         cidx::ast::FrontendCapability::templates},
        {cidx::ast::FrontendCapability::ast},
        {},
        {},
        {cidx::ast::FrontendCapability::ast},
        {cidx::ast::FrontendCapability::ast},
        {cidx::ast::FrontendCapability::ast,
         cidx::ast::FrontendCapability::templates},
        {cidx::ast::FrontendCapability::ast},
        {},
        {},
        {cidx::ast::FrontendCapability::preprocessor},
        {}};
    REQUIRE(outcome.pass_metrics.size() == capabilities.size());
    for (std::size_t index = 0; index < capabilities.size(); ++index) {
      CHECK(outcome.pass_metrics[index].required_capabilities ==
            capabilities[index]);
    }
    const std::vector<
        std::tuple<std::vector<std::string>, std::vector<std::string>,
                   std::vector<std::string>>>
        contracts{
            {{}, {"symbols"}, {}},
            {{"includes"}, {"symbols"}, {"symbols.main"}},
            {{"symbols"}, {"fact_lifecycle"}, {"symbols.headers"}},
            {{"symbols", "fact_lifecycle"},
             {"relations", "types", "definitions", "presentation_intents"},
             {"symbols.headers", "lifecycle.headers"}},
            {{"symbols"}, {"definitions"}, {"declarations.headers"}},
            {{"definitions", "relations", "types"},
             {"relations", "types", "evidence", "definitions", "symbols"},
             {"definitions.headers"}},
            {{"symbols", "relations"}, {"relations"}, {"statements.headers"}},
            {{"symbols", "relations", "definitions"},
             {"file_associations"},
             {"symbols.headers", "statements.headers", "namespaces.headers"}},
            {{}, {"fact_lifecycle"}, {"headers.associate"}},
            {{"symbols", "fact_lifecycle"},
             {"relations", "types", "definitions", "presentation_intents"},
             {"headers.associate", "lifecycle.main"}},
            {{"symbols"}, {"definitions"}, {"declarations.main"}},
            {{"definitions", "relations", "types"},
             {"relations", "types", "evidence", "definitions", "symbols"},
             {"definitions.main"}},
            {{"symbols", "relations"}, {"relations"}, {"statements.main"}},
            {{"symbols", "relations", "definitions"},
             {"file_associations"},
             {"symbols.main", "lifecycle.main", "statements.main",
              "namespaces.main"}},
            {{"presentation_intents"},
             {"display_names"},
             {"declarations.headers", "declarations.main"}},
            {{"preprocessor_facts"}, {"include_facts"}, {"main.associate"}},
            {{"evidence"},
             {"evidence_artifact"},
             {"statements.headers", "statements.main", "includes.persist"}},
        };
    REQUIRE(outcome.pass_metrics.size() == contracts.size());
    for (std::size_t index = 0; index < contracts.size(); ++index) {
      CHECK(std::tie(outcome.pass_metrics[index].consumed_fact_families,
                     outcome.pass_metrics[index].produced_fact_families,
                     outcome.pass_metrics[index].dependencies) ==
            contracts[index]);
    }
    const auto main_associate = std::ranges::find_if(
        outcome.pass_metrics, [](const cidx::ast::IndexPassMetrics &metrics) {
          return metrics.id == "main.associate";
        });
    REQUIRE(main_associate != outcome.pass_metrics.end());
    CHECK(std::ranges::find(main_associate->dependencies, "symbols.main") !=
          main_associate->dependencies.end());
    CHECK(std::ranges::find(main_associate->dependencies, "statements.main") !=
          main_associate->dependencies.end());
    CHECK(std::ranges::find(main_associate->consumed_fact_families,
                            "definitions") !=
          main_associate->consumed_fact_families.end());
    const auto statements_main = std::ranges::find_if(
        outcome.pass_metrics, [](const cidx::ast::IndexPassMetrics &metrics) {
          return metrics.id == "statements.main";
        });
    REQUIRE(statements_main != outcome.pass_metrics.end());
    CHECK(statements_main->completeness ==
          cidx::ast::FactCompleteness::partial);
    CHECK(statements_main->trust == cidx::ast::FactTrust::inferred);
    CHECK(std::ranges::find(main_associate->produced_fact_families,
                            "file_associations") !=
          main_associate->produced_fact_families.end());
    const auto presentation = std::ranges::find_if(
        outcome.pass_metrics, [](const cidx::ast::IndexPassMetrics &metrics) {
          return metrics.id == "presentation.persist";
        });
    REQUIRE(presentation != outcome.pass_metrics.end());
    CHECK(presentation->consumed_fact_families ==
          std::vector<std::string>{"presentation_intents"});
    CHECK(presentation->produced_fact_families ==
          std::vector<std::string>{"display_names"});
  }

  TEST_CASE(
      "CLI index publishes evidence and pass metrics to its outcome sink") {
    const std::string dir = make_temp_dir();
    const std::string source = dir + "/observable.cpp";
    write_file(source,
               "void observable() { void (*callable)(); callable(); }\n");
    Storage db(dir + "/index.db");
    db.add_component("observable", dir);
    const int64_t file_id = db.add_file_path(
        source, std::nullopt, std::nullopt,
        std::vector<std::string>{"-std=c++23"}, std::string("clang++"));
    REQUIRE(db.get_file_by_id(file_id).has_value());
    db.stamp_graph_resolved();
    db.stamp_index_identity();

    bool observed = false;
    std::size_t metric_count = 0;
    std::size_t evidence_count = 0;
    const CmdResult result =
        run_cli({"index"}, dir, nullptr,
                [&](const cidx::ast::IndexOneOutcome &outcome) {
                  observed = true;
                  metric_count = outcome.pass_metrics.size();
                  evidence_count = outcome.evidence.size();
                });
    CHECK(result.rc == 0);
    CHECK(observed);
    CHECK(metric_count >= 17);
    CHECK(evidence_count > 0);
  }

  TEST_CASE("frontend session providers receive focused services") {
    const std::string dir = make_temp_dir();
    const std::string source = dir + "/extension.cpp";
    write_file(source, "int extension_symbol() { return 1; }\n");

    Storage db(":memory:");
    db.add_component("extension", dir);
    const int64_t file_id = db.add_file_path(
        source, std::nullopt, std::nullopt,
        std::vector<std::string>{"-std=c++23"}, std::string("clang++"));
    const auto file = db.get_file_by_id(file_id);
    REQUIRE(file.has_value());
    db.stamp_graph_resolved();
    db.stamp_index_identity();

    bool provider_saw_session = false;
    bool runner_saw_session = false;
    cidx::ast::register_frontend_pass_provider(
        [&](cidx::ast::FrontendSession &session,
            cidx::ast::ExtractionPassRegistry &registry,
            cidx::ast::IndexingPlan &plan) {
          provider_saw_session =
              session.ast_context != nullptr &&
              session.preprocessor != nullptr &&
              session.statement_ports != nullptr &&
              session.declaration_ports != nullptr &&
              session.namespace_ports != nullptr &&
              session.evidence != nullptr &&
              session.supports(cidx::ast::FrontendCapability::cfg) &&
              session.supports(cidx::ast::FrontendCapability::templates);
          auto descriptor = cidx::ast::ExtractionPassDescriptor{
              .id = "synthetic.extension",
              .version = 1,
              .required_capabilities =
                  {cidx::ast::FrontendCapability::ast,
                   cidx::ast::FrontendCapability::preprocessor},
              .consumed_fact_families = {"ast", "preprocessor"},
              .produced_fact_families = {"extension"},
              .catalog_versions = {1},
              .dependencies = {"main.associate"},
              .scope = cidx::ast::PassScope::translation_unit,
              .traversal = cidx::ast::TraversalMode::lifecycle,
              .budget = {.max_visited_constructs = 2,
                         .max_emitted_facts = 2,
                         .max_diagnostics = 2,
                         .declared = true}};
          registry.register_pass(
              std::move(descriptor),
              [&](cidx::ast::PassExecutionContext &context) {
                runner_saw_session =
                    context.session != nullptr &&
                    context.session->ast_context != nullptr &&
                    context.session->preprocessor != nullptr &&
                    context.session->statement_ports != nullptr;
                context.metrics.note_visited();
                context.session->statement_ports->emit(
                    cidx::ast::EvidenceRecord{
                        .producer = "extension",
                        .construct = "Synthetic",
                        .file = source,
                        .completeness = cidx::ast::FactCompleteness::partial,
                        .trust = cidx::ast::FactTrust::inferred,
                        .detail = "extension"});
              });
          plan.insert_before("evidence.persist", "synthetic.extension");
        });
    const auto outcome = cidx::ast::run_index_one(db, *file, source, true);
    cidx::ast::clear_frontend_pass_providers();
    CHECK(!outcome.parse_failed);
    CHECK(provider_saw_session);
    CHECK(runner_saw_session);
    CHECK(std::ranges::find_if(outcome.pass_metrics,
                               [](const cidx::ast::IndexPassMetrics &metrics) {
                                 return metrics.id == "synthetic.extension";
                               }) != outcome.pass_metrics.end());
  }

  TEST_CASE("over-budget frontend storage emission cannot commit") {
    const std::string dir = make_temp_dir();
    const std::string source = dir + "/budget.cpp";
    write_file(source, "int budget_symbol() { return 1; }\n");

    Storage db(":memory:");
    db.add_component("budget", dir);
    const int64_t file_id = db.add_file_path(
        source, std::nullopt, std::nullopt,
        std::vector<std::string>{"-std=c++23"}, std::string("clang++"));
    const auto file = db.get_file_by_id(file_id);
    REQUIRE(file.has_value());
    db.stamp_graph_resolved();
    db.stamp_index_identity();
    const auto before = snapshot_tu_publication(db, source);

    cidx::ast::register_frontend_pass_provider(
        [&db](cidx::ast::FrontendSession &,
              cidx::ast::ExtractionPassRegistry &registry,
              cidx::ast::IndexingPlan &plan) {
          auto descriptor = cidx::ast::ExtractionPassDescriptor{
              .id = "synthetic.over-budget",
              .version = 1,
              .required_capabilities = {cidx::ast::FrontendCapability::ast},
              .consumed_fact_families = {"symbols"},
              .produced_fact_families = {"relations"},
              .catalog_versions = {1},
              .dependencies = {"main.associate"},
              .scope = cidx::ast::PassScope::main_file,
              .traversal = cidx::ast::TraversalMode::body,
              .budget = {.max_visited_constructs = 1,
                         .max_emitted_facts = 1,
                         .max_diagnostics = 1,
                         .declared = true}};
          registry.register_pass(
              std::move(descriptor),
              [&db](cidx::ast::PassExecutionContext &context) {
                const auto symbols = db.find_symbols("budget_symbol", {}, 10);
                if (symbols.empty()) {
                  return;
                }
                const cidx::ast::EdgeRecord edge{.src_id = symbols.front().id,
                                                 .dst_id = symbols.front().id,
                                                 .kind = 1};
                context.session->statement_ports->add_edge(edge);
                context.session->statement_ports->add_edge(edge);
              });
          plan.add("synthetic.over-budget");
        });
    const auto outcome = cidx::ast::run_index_one(db, *file, source, true);
    cidx::ast::clear_frontend_pass_providers();
    CHECK(outcome.parse_failed);
    CHECK(outcome.error.find("budget exceeded") != std::string::npos);
    CHECK(snapshot_tu_publication(db, source) == before);
  }

  TEST_CASE(
      "production association preflight rejects low budgets before writes") {
    const std::string dir = make_temp_dir();
    const std::string source = dir + "/association_budget.cpp";
    write_file(source, "int association_budget_symbol() { return 1; }\n");
    Storage db(":memory:");
    db.add_component("association-budget", dir);
    const int64_t file_id = db.add_file_path(
        source, std::nullopt, std::nullopt,
        std::vector<std::string>{"-std=c++23"}, std::string("clang++"));
    const auto file = db.get_file_by_id(file_id);
    REQUIRE(file.has_value());
    db.stamp_graph_resolved();
    db.stamp_index_identity();
    const auto before = snapshot_tu_publication(db, source);
    cidx::ast::register_frontend_pass_provider(
        [](cidx::ast::FrontendSession &session,
           cidx::ast::ExtractionPassRegistry &, cidx::ast::IndexingPlan &) {
          session.budget_overrides["main.associate"] =
              cidx::ast::PassBudget{.max_emitted_facts = 1, .declared = true};
        });
    const auto outcome = cidx::ast::run_index_one(db, *file, source, true);
    cidx::ast::clear_frontend_pass_providers();
    CHECK(outcome.parse_failed);
    CHECK(outcome.error.find("budget exceeded") != std::string::npos);
    CHECK(snapshot_tu_publication(db, source) == before);
  }

  TEST_CASE("production include preflight rejects low budgets before writes") {
    const std::string dir = make_temp_dir();
    const std::string header = dir + "/include_budget.hpp";
    const std::string source = dir + "/include_budget.cpp";
    write_file(header, "int include_budget_value = 1;\n");
    write_file(source, "#include \"include_budget.hpp\"\n"
                       "int include_budget_symbol() { return 1; }\n");
    Storage db(":memory:");
    db.add_component("include-budget", dir);
    const int64_t file_id =
        db.add_file_path(source, std::nullopt, std::nullopt,
                         std::vector<std::string>{"-std=c++23", "-I", dir},
                         std::string("clang++"));
    const auto file = db.get_file_by_id(file_id);
    REQUIRE(file.has_value());
    db.stamp_graph_resolved();
    db.stamp_index_identity();
    const auto before = snapshot_tu_publication(db, source);
    cidx::ast::register_frontend_pass_provider(
        [](cidx::ast::FrontendSession &session,
           cidx::ast::ExtractionPassRegistry &, cidx::ast::IndexingPlan &) {
          session.budget_overrides["includes.persist"] =
              cidx::ast::PassBudget{.max_emitted_facts = 1, .declared = true};
        });
    const auto outcome = cidx::ast::run_index_one(db, *file, source, true);
    cidx::ast::clear_frontend_pass_providers();
    CHECK(outcome.parse_failed);
    CHECK(outcome.error.find("budget exceeded") != std::string::npos);
    CHECK(snapshot_tu_publication(db, source) == before);
  }

  TEST_CASE(
      "production association and include budgets accept N and reject N-1") {
    struct Prepared {
      std::unique_ptr<Storage> db;
      cidx::File file;
      std::string source;
    };
    const auto prepare_association = [] {
      Prepared prepared;
      const std::string dir = make_temp_dir();
      prepared.source = dir + "/exact_association.cpp";
      write_file(prepared.source,
                 "int exact_association_symbol() { return 1; }\n");
      prepared.db = std::make_unique<Storage>(":memory:");
      prepared.db->add_component("exact-association", dir);
      const int64_t file_id = prepared.db->add_file_path(
          prepared.source, std::nullopt, std::nullopt,
          std::vector<std::string>{"-std=c++23"}, std::string("clang++"));
      prepared.file = *prepared.db->get_file_by_id(file_id);
      prepared.db->stamp_graph_resolved();
      prepared.db->stamp_index_identity();
      return prepared;
    };
    const auto measured_association = prepare_association();
    const auto measured_association_outcome = cidx::ast::run_index_one(
        *measured_association.db, measured_association.file,
        measured_association.source, true);
    REQUIRE(!measured_association_outcome.parse_failed);
    const auto association_metrics =
        std::ranges::find_if(measured_association_outcome.pass_metrics,
                             [](const cidx::ast::IndexPassMetrics &metrics) {
                               return metrics.id == "main.associate";
                             });
    REQUIRE(association_metrics !=
            measured_association_outcome.pass_metrics.end());
    const std::size_t association_n = association_metrics->emitted_facts;
    REQUIRE(association_n > 0);

    const auto run_association = [&](std::size_t budget) {
      Prepared prepared = prepare_association();
      const auto before =
          snapshot_tu_publication(*prepared.db, prepared.source);
      cidx::ast::register_frontend_pass_provider(
          [budget](cidx::ast::FrontendSession &session,
                   cidx::ast::ExtractionPassRegistry &,
                   cidx::ast::IndexingPlan &) {
            session.budget_overrides["main.associate"] = cidx::ast::PassBudget{
                .max_emitted_facts = budget, .declared = true};
          });
      const auto outcome = cidx::ast::run_index_one(*prepared.db, prepared.file,
                                                    prepared.source, true);
      cidx::ast::clear_frontend_pass_providers();
      if (budget < association_n) {
        CHECK(outcome.parse_failed);
        CHECK(snapshot_tu_publication(*prepared.db, prepared.source) == before);
      } else {
        CHECK(!outcome.parse_failed);
      }
      return outcome;
    };
    run_association(association_n);
    run_association(association_n - 1);

    const auto prepare_include = [] {
      Prepared prepared;
      const std::string dir = make_temp_dir();
      const std::string header = dir + "/exact_include.hpp";
      prepared.source = dir + "/exact_include.cpp";
      write_file(header, "int exact_include_value = 1;\n");
      write_file(
          prepared.source,
          "#include \"exact_include.hpp\"\n"
          "#include \"exact_include.hpp\"\n"
          "int exact_include_symbol() { return exact_include_value; }\n");
      prepared.db = std::make_unique<Storage>(":memory:");
      prepared.db->add_component("exact-include", dir);
      const int64_t file_id = prepared.db->add_file_path(
          prepared.source, std::nullopt, std::nullopt,
          std::vector<std::string>{"-std=c++23", "-I", dir},
          std::string("clang++"));
      prepared.file = *prepared.db->get_file_by_id(file_id);
      prepared.db->stamp_graph_resolved();
      prepared.db->stamp_index_identity();
      return prepared;
    };
    const auto measured_include = prepare_include();
    const auto measured_include_outcome =
        cidx::ast::run_index_one(*measured_include.db, measured_include.file,
                                 measured_include.source, true);
    REQUIRE(!measured_include_outcome.parse_failed);
    const auto include_metrics =
        std::ranges::find_if(measured_include_outcome.pass_metrics,
                             [](const cidx::ast::IndexPassMetrics &metrics) {
                               return metrics.id == "includes.persist";
                             });
    REQUIRE(include_metrics != measured_include_outcome.pass_metrics.end());
    const std::size_t include_n = include_metrics->emitted_facts;
    REQUIRE(include_n > 0);
    CHECK(include_metrics->duplicates >= 3);
    const auto run_include = [&](std::size_t budget) {
      Prepared prepared = prepare_include();
      const auto before =
          snapshot_tu_publication(*prepared.db, prepared.source);
      cidx::ast::register_frontend_pass_provider(
          [budget](cidx::ast::FrontendSession &session,
                   cidx::ast::ExtractionPassRegistry &,
                   cidx::ast::IndexingPlan &) {
            session.budget_overrides["includes.persist"] =
                cidx::ast::PassBudget{.max_emitted_facts = budget,
                                      .declared = true};
          });
      const auto outcome = cidx::ast::run_index_one(*prepared.db, prepared.file,
                                                    prepared.source, true);
      cidx::ast::clear_frontend_pass_providers();
      if (budget < include_n) {
        CHECK(outcome.parse_failed);
        CHECK(snapshot_tu_publication(*prepared.db, prepared.source) == before);
      } else {
        CHECK(!outcome.parse_failed);
      }
      return outcome;
    };
    run_include(include_n);
    run_include(include_n - 1);
  }

  TEST_CASE(
      "production nested include preflight counts shared applicability once") {
    struct Prepared {
      std::unique_ptr<Storage> db;
      cidx::File file;
      std::string source;
    };
    const auto prepare = [] {
      Prepared prepared;
      const std::string dir = make_temp_dir();
      write_file(dir + "/inner.hpp", "int nested_include_value = 1;\n");
      write_file(dir + "/outer.hpp", "#include \"inner.hpp\"\n");
      prepared.source = dir + "/nested.cpp";
      write_file(
          prepared.source,
          "#include \"outer.hpp\"\n"
          "int nested_include_user() { return nested_include_value; }\n");
      prepared.db = std::make_unique<Storage>(":memory:");
      prepared.db->add_component("nested-include", dir);
      const int64_t file_id = prepared.db->add_file_path(
          prepared.source, std::nullopt, std::nullopt,
          std::vector<std::string>{"-std=c++23", "-I", dir},
          std::string("clang++"));
      prepared.file = *prepared.db->get_file_by_id(file_id);
      prepared.db->stamp_graph_resolved();
      prepared.db->stamp_index_identity();
      return prepared;
    };

    const auto measured = prepare();
    const auto measured_outcome = cidx::ast::run_index_one(
        *measured.db, measured.file, measured.source, true);
    REQUIRE(!measured_outcome.parse_failed);
    const auto include_metrics =
        std::ranges::find_if(measured_outcome.pass_metrics,
                             [](const cidx::ast::IndexPassMetrics &metrics) {
                               return metrics.id == "includes.persist";
                             });
    REQUIRE(include_metrics != measured_outcome.pass_metrics.end());
    const std::size_t include_n = include_metrics->emitted_facts;
    REQUIRE(include_n > 0);
    CHECK(include_metrics->duplicates == 1);

    const auto run = [&](std::size_t budget) {
      Prepared prepared = prepare();
      const auto before =
          snapshot_tu_publication(*prepared.db, prepared.source);
      cidx::ast::register_frontend_pass_provider(
          [budget](cidx::ast::FrontendSession &session,
                   cidx::ast::ExtractionPassRegistry &,
                   cidx::ast::IndexingPlan &) {
            session.budget_overrides["includes.persist"] =
                cidx::ast::PassBudget{.max_emitted_facts = budget,
                                      .declared = true};
          });
      const auto outcome = cidx::ast::run_index_one(*prepared.db, prepared.file,
                                                    prepared.source, true);
      cidx::ast::clear_frontend_pass_providers();
      if (budget < include_n) {
        CHECK(outcome.parse_failed);
        CHECK(outcome.error.find("budget exceeded") != std::string::npos);
        CHECK(snapshot_tu_publication(*prepared.db, prepared.source) == before);
      } else {
        CHECK(!outcome.parse_failed);
      }
    };
    run(include_n);
    run(include_n - 1);
  }

  TEST_CASE(
      "production statement body-edge preflight rejects N-1 before mutation") {
    struct Prepared {
      std::unique_ptr<Storage> db;
      cidx::File file;
      std::string source;
    };
    const auto prepare = [] {
      Prepared prepared;
      const std::string dir = make_temp_dir();
      prepared.source = dir + "/body_budget.cpp";
      write_file(prepared.source,
                 "int body_budget_callee() { return 1; }\n"
                 "int body_budget_caller() { return body_budget_callee(); }\n");
      prepared.db = std::make_unique<Storage>(":memory:");
      prepared.db->add_component("body-budget", dir);
      const int64_t file_id = prepared.db->add_file_path(
          prepared.source, std::nullopt, std::nullopt,
          std::vector<std::string>{"-std=c++23"}, std::string("clang++"));
      prepared.file = *prepared.db->get_file_by_id(file_id);
      prepared.db->stamp_graph_resolved();
      prepared.db->stamp_index_identity();
      return prepared;
    };
    const auto measured = prepare();
    const auto measured_outcome = cidx::ast::run_index_one(
        *measured.db, measured.file, measured.source, true);
    REQUIRE(!measured_outcome.parse_failed);
    const auto statement_metrics =
        std::ranges::find_if(measured_outcome.pass_metrics,
                             [](const cidx::ast::IndexPassMetrics &metrics) {
                               return metrics.id == "statements.main";
                             });
    REQUIRE(statement_metrics != measured_outcome.pass_metrics.end());
    const std::size_t statement_n = statement_metrics->emitted_facts;
    REQUIRE(statement_n > 0);
    for (const std::size_t budget : {statement_n, statement_n - 1}) {
      Prepared prepared = prepare();
      const auto before =
          snapshot_tu_publication(*prepared.db, prepared.source);
      cidx::ast::register_frontend_pass_provider(
          [budget](cidx::ast::FrontendSession &session,
                   cidx::ast::ExtractionPassRegistry &,
                   cidx::ast::IndexingPlan &) {
            session.budget_overrides["statements.main"] = cidx::ast::PassBudget{
                .max_emitted_facts = budget, .declared = true};
          });
      const auto outcome = cidx::ast::run_index_one(*prepared.db, prepared.file,
                                                    prepared.source, true);
      cidx::ast::clear_frontend_pass_providers();
      if (budget == statement_n) {
        CHECK(!outcome.parse_failed);
      } else {
        CHECK(outcome.parse_failed);
        CHECK(snapshot_tu_publication(*prepared.db, prepared.source) == before);
      }
    }
  }

  TEST_CASE(
      "production presentation budget rejects N+1 before display writes") {
    struct Prepared {
      std::unique_ptr<Storage> db;
      cidx::File file;
      std::string source;
    };
    const auto prepare = [] {
      Prepared prepared;
      const std::string dir = make_temp_dir();
      prepared.source = dir + "/presentation_budget.cpp";
      write_file(prepared.source,
                 "template <typename T> void presentation_budget(T) {}\n"
                 "template <> void presentation_budget<int>(int) {}\n"
                 "template <typename T> void presentation_budget_extra(T) {}\n"
                 "template <> void presentation_budget_extra<int>(int) {}\n");
      prepared.db = std::make_unique<Storage>(":memory:");
      prepared.db->add_component("presentation-budget", dir);
      const int64_t file_id = prepared.db->add_file_path(
          prepared.source, std::nullopt, std::nullopt,
          std::vector<std::string>{"-std=c++23"}, std::string("clang++"));
      prepared.file = *prepared.db->get_file_by_id(file_id);
      prepared.db->stamp_graph_resolved();
      prepared.db->stamp_index_identity();
      return prepared;
    };
    const auto install_presentation_provider = [](Storage &db,
                                                  std::size_t budget,
                                                  bool extra) {
      cidx::ast::register_frontend_pass_provider(
          [&db, budget, extra](cidx::ast::FrontendSession &session,
                               cidx::ast::ExtractionPassRegistry &registry,
                               cidx::ast::IndexingPlan &plan) {
            session.budget_overrides["presentation.persist"] =
                cidx::ast::PassBudget{.max_emitted_facts = budget,
                                      .declared = true};
            auto descriptor = cidx::ast::ExtractionPassDescriptor{
                .id = "synthetic.presentation_intent",
                .version = 1,
                .required_capabilities = {cidx::ast::FrontendCapability::ast},
                .consumed_fact_families = {"symbols"},
                .produced_fact_families = {"presentation_intents"},
                .catalog_versions = {1},
                .dependencies = {"declarations.main"},
                .scope = cidx::ast::PassScope::main_file,
                .traversal = cidx::ast::TraversalMode::lifecycle,
                .budget = {.max_visited_constructs = 4,
                           .max_emitted_facts = 4,
                           .max_diagnostics = 4,
                           .declared = true}};
            registry.register_pass(
                std::move(descriptor),
                [&db, extra](cidx::ast::PassExecutionContext &context) {
                  const auto symbols =
                      db.lookup_symbols_by_name("presentation_budget");
                  const auto extra_symbols =
                      db.lookup_symbols_by_name("presentation_budget_extra");
                  if (symbols.empty() || (extra && extra_symbols.empty())) {
                    return;
                  }
                  const auto emit_intent = [&db,
                                            &context](const Symbol &symbol,
                                                      std::string_view name) {
                    db.update_symbol_by_id(
                        symbol.id,
                        {{"display_name", std::string(name) + "<old>"}});
                    context.session->presentation_intents->emit(
                        cidx::ast::PresentationIntent{.symbol_id = symbol.id,
                                                      .display_args = {"int"}});
                  };
                  emit_intent(symbols.front(), "presentation_budget");
                  if (extra) {
                    emit_intent(extra_symbols.front(),
                                "presentation_budget_extra");
                  }
                });
            plan.insert_before("presentation.persist",
                               "synthetic.presentation_intent");
          });
    };
    const auto measured = prepare();
    install_presentation_provider(*measured.db, 100, false);
    const auto measured_outcome = cidx::ast::run_index_one(
        *measured.db, measured.file, measured.source, true);
    cidx::ast::clear_frontend_pass_providers();
    REQUIRE(!measured_outcome.parse_failed);
    const auto presentation_metrics =
        std::ranges::find_if(measured_outcome.pass_metrics,
                             [](const cidx::ast::IndexPassMetrics &metrics) {
                               return metrics.id == "presentation.persist";
                             });
    REQUIRE(presentation_metrics != measured_outcome.pass_metrics.end());
    const std::size_t presentation_n = presentation_metrics->emitted_facts;
    REQUIRE(presentation_n > 0);
    {
      Prepared prepared = prepare();
      install_presentation_provider(*prepared.db, presentation_n, false);
      const auto outcome = cidx::ast::run_index_one(*prepared.db, prepared.file,
                                                    prepared.source, true);
      cidx::ast::clear_frontend_pass_providers();
      CHECK(!outcome.parse_failed);
    }
    {
      Prepared prepared = prepare();
      const auto before =
          snapshot_tu_publication(*prepared.db, prepared.source);
      install_presentation_provider(*prepared.db, presentation_n, true);
      const auto outcome = cidx::ast::run_index_one(*prepared.db, prepared.file,
                                                    prepared.source, true);
      cidx::ast::clear_frontend_pass_providers();
      CHECK(outcome.parse_failed);
      CHECK(snapshot_tu_publication(*prepared.db, prepared.source) == before);
    }
  }

  TEST_CASE("index TU publication enforces read-only storage") {
    const std::string dir = make_temp_dir();
    const std::string source = dir + "/source.cpp";
    const std::string database = dir + "/index.db";
    write_file(source, "int read_only_pipeline_symbol() { return 1; }\n");
    {
      Storage writable(database);
      writable.add_component("readonly", dir);
      writable.add_file_path(source, std::nullopt, std::nullopt,
                             std::vector<std::string>{"-std=c++23"},
                             std::string("clang++"));
      writable.stamp_graph_resolved();
      writable.stamp_index_identity();
    }

    Storage readonly(database, Storage::OpenMode::read_only);
    const auto file = readonly.get_file(source);
    REQUIRE(file.has_value());
    const auto before = snapshot_tu_publication(readonly, source);
    CHECK_THROWS(cidx::ast::run_index_one(readonly, *file, source, true,
                                          cidx::ast::IndexFailurePoint::none));
    CHECK(snapshot_tu_publication(readonly, source) == before);
  }

  TEST_CASE("import: synthetic compile DB — strip, driver, skip counter") {
    const std::string t = make_temp_dir();
    makedirs(t + "/proj/sub");
    makedirs(t + "/other");
    write_file(t + "/proj/a.c", "int a;\n");
    write_file(t + "/proj/sub/b.c", "int b;\n");
    write_file(t + "/other/c.c", "int c;\n");
    // No .git anywhere: the component root falls back to the directory holding
    // compile_commands.json (here <t>/proj), and its basename names the
    // component. Sources outside that dir (other/c.c) fall outside the
    // component and are skipped.
    write_file(t + "/proj/compile_commands.json",
               "[\n"
               "  {\"directory\": \"" +
                   t +
                   "/proj\", \"command\": "
                   "\"cc -I. -c a.c -o a.o\", \"file\": \"a.c\"},\n"
                   "  {\"directory\": \"" +
                   t +
                   "/proj\", \"command\": "
                   "\"gcc -Iinclude -DFOO -c sub/b.c -o sub/b.o\", "
                   "\"file\": \"sub/b.c\"},\n"
                   "  {\"directory\": \"" +
                   t +
                   "/other\", \"command\": "
                   "\"cc -c c.c\", \"file\": \"c.c\"}\n"
                   "]\n");

    // $ python3 -m indexer import --db <t>/proj/compile_commands.json
    // component #1: proj at <t>/proj
    // imported 2 file(s), skipped 1
    //   skip (outside any component): <t>/other/c.c        (stderr)
    const CmdResult r =
        run_cli({"import", "--db", t + "/proj/compile_commands.json"}, t);
    CHECK(r.rc == 0);
    CHECK(r.out == "component #1: proj at " + t +
                       "/proj\n"
                       "repository 'proj': 1 component(s)\n"
                       "imported 2 file(s), skipped 1\n");
    CHECK(r.err == "  skip (outside any component): " + t + "/other/c.c\n");

    // Stored rows: stripped options (G10/G12), driver, md5/mtime captured,
    // indexed = 0 (pending).
    Storage db(t + "/index.db");
    const std::optional<cidx::File> a = db.get_file(t + "/proj/a.c");
    REQUIRE(a);
    // The lazily-created "proj" component is now in the alias registry, so its
    // own -I paths encode to <proj> (v0.28.1: rebuild the registry after the
    // lazy creation so a fresh component's includes are portable, not
    // absolute).
    CHECK(*a->compile_options == std::vector<std::string>{"-I<proj>"});
    CHECK(*a->driver == "cc");
    CHECK(a->md5);
    CHECK(a->mtime);
    CHECK(!a->indexed);
    const std::optional<cidx::File> b = db.get_file(t + "/proj/sub/b.c");
    REQUIRE(b);
    CHECK(*b->compile_options ==
          std::vector<std::string>{"-I<proj>/include", "-DFOO"});
    CHECK(*b->driver == "gcc");
    CHECK(!db.get_file(t + "/other/c.c"));
  }

  TEST_CASE("import: --db accepts the directory; git root wins as the "
            "component root") {
    const std::string t = make_temp_dir();
    makedirs(t + "/proj/.git");
    makedirs(t + "/proj/src");
    makedirs(t + "/proj/build");
    write_file(t + "/proj/.git/config",
               "[remote \"origin\"]\n\turl = git@host:team/widget.git\n");
    write_file(t + "/proj/src/m.c", "int m;\n");
    write_file(t + "/proj/build/compile_commands.json",
               "[{\"directory\": \"" + t +
                   "/proj/src\", \"command\": "
                   "\"cc -c m.c\", \"file\": \"m.c\"}]\n");

    // $ python3 -m indexer import --db <t>/proj/build   (directory form;
    // component root = git root, name from .git/config origin url)
    const CmdResult r = run_cli({"import", "--db", t + "/proj/build"}, t);
    CHECK(r.rc == 0);
    CHECK(r.out == "component #1: widget at " + t +
                       "/proj\n"
                       "repository 'widget': 1 component(s)\n"
                       "imported 1 file(s), skipped 0\n");
    CHECK(r.err.empty());
  }

  TEST_CASE("import: manifests unified compile DB (READ-ONLY fixture)") {
    if (!require_manifests()) {
      return;
    }
    const std::string t = make_temp_dir();
    // Single unified DB at manifests/ (sub-project DBs were consolidated).
    // compile_commands.json is generated per-checkout from the committed .in
    // template (CMake configure), so imported file paths match the fixtures on
    // THIS machine — see tests/CMakeLists.txt.
    const std::string db_path =
        std::string(CIDX_MANIFESTS_DIR) + "/compile_commands.json";
    const std::string project = std::string(CIDX_MANIFESTS_DIR) + "/project";

    const CmdResult r = run_cli({"import", "--db", db_path}, t);
    CHECK(r.rc == 0);
    // Unified DB: assert the project TUs imported with their flags rather than
    // a fixed total count (fixtures grow over time — see CLAUDE.md).
    CHECK(r.out.find("skipped 0\n") != std::string::npos);
    CHECK(r.err.empty());

    Storage db(t + "/index.db");
    const std::optional<cidx::File> mathlib =
        db.get_file(project + "/mathlib.c");
    REQUIRE(mathlib);
    // The lazily-created component (named after the manifests' git root) is in
    // the alias registry, so its project-dir -I encodes to <name>/manifests/
    // project (v0.28.1). Derive the name from the DB for parity-robustness.
    const std::optional<cidx::Component> comp =
        db.component_for_path(project + "/mathlib.c");
    REQUIRE(comp);
    CHECK(*mathlib->compile_options ==
          std::vector<std::string>{"-I<" + comp->name + ">/manifests/project"});
    CHECK(*mathlib->driver == "cc");
    REQUIRE(db.get_file(project + "/app.c"));
  }

  TEST_CASE("import: load failure -> exit 1 with the Python-parity message") {
    const std::string t = make_temp_dir();
    // $ python3 -m indexer import --db <t>/nope
    // error: cannot load compilation database from <t>/nope: Error 1:
    // CompilationDatabase loading failed
    // (libclang additionally prints LIBCLANG TOOLING ERROR lines straight
    // to fd 2 — both tools share that noise.)
    const CmdResult r = run_cli({"import", "--db", t + "/nope"}, t);
    CHECK(r.rc == 1);
    CHECK(r.out.empty());
    CHECK(r.err == "error: cannot load compilation database from " + t +
                       "/nope: Error 1: CompilationDatabase loading "
                       "failed\n");
  }

  TEST_CASE("import: empty compilation database -> exit 1") {
    const std::string t = make_temp_dir();
    makedirs(t + "/empty");
    write_file(t + "/empty/compile_commands.json", "[]\n");
    // KNOWN DELTA: Python crashes into the load-error message here
    // ("'NoneType' object is not iterable"); cidx-cpp prints the intended
    // empty-DB message. Exit code matches (1).
    const CmdResult r = run_cli({"import", "--db", t + "/empty"}, t);
    CHECK(r.rc == 1);
    CHECK(r.err == "error: compilation database is empty\n");
  }

  // -------------------------------------------------------------------------
  // index — end-to-end on a tmp two-TU project (S08). The fixture mirrors
  // manifests/project/ (mathlib.h/.c + app.c) but is SYNTHESIZED in a temp
  // dir — manifests/ stays read-only. Expected lines were captured from the
  // Python tool ($ python3 -m indexer index ...) on an identical fixture.
  // -------------------------------------------------------------------------

  struct TwoTuProject {
    std::string cache; // temp root, doubles as INDEXER_CACHE
    std::string proj;  // <cache>/proj — component root (no .git: dirname)
    explicit TwoTuProject(bool with_bad_tu = false)
        : cache(make_temp_dir()), proj(cache + "/proj") {
      makedirs(proj);
      write_file(proj + "/mathlib.h", "#ifndef MATHLIB_H\n"
                                      "#define MATHLIB_H\n"
                                      "int add(int a, int b);\n"
                                      "int multiply(int a, int b);\n"
                                      "int square(int x);\n"
                                      "#endif\n");
      write_file(proj + "/mathlib.c",
                 "#include \"mathlib.h\"\n"
                 "int add(int a, int b) { return a + b; }\n"
                 "int multiply(int a, int b) { return a * b; }\n"
                 "int square(int x) { return multiply(x, x); }\n");
      write_file(proj + "/app.c",
                 "#include \"mathlib.h\"\n"
                 "int main(void) { return square(5) + add(1, 2); }\n");
      std::string db = "[\n  " + entry("mathlib.c") + ",\n  " + entry("app.c");
      if (with_bad_tu) {
        write_file(proj + "/bad.c", "#include \"missing.h\"\nint bad;\n");
        db += ",\n  " + entry("bad.c");
      }
      db += "\n]\n";
      write_file(proj + "/compile_commands.json", db);
    }
    std::string entry(const std::string &src) const {
      return "{\"directory\": \"" + proj + "\", \"command\": \"cc -I. -c " +
             src + " -o " + src + ".o\", \"file\": \"" + src + "\"}";
    }
  };

  struct FreshnessProject {
    std::string cache;
    std::string proj;
    std::string a;
    std::string b;

    FreshnessProject()
        : cache(make_temp_dir()), proj(cache + "/proj"), a(proj + "/a.cpp"),
          b(proj + "/b.cpp") {
      makedirs(proj);
      write_file(a, "int old_symbol() { return 1; }\n");
      write_file(b, "int other_symbol() { return 2; }\n");
      write_file(proj + "/compile_commands.json",
                 "[\n"
                 "  {\"directory\": \"" +
                     proj +
                     "\", \"command\": \"c++ -std=c++23 -c a.cpp -o a.o\", "
                     "\"file\": \"a.cpp\"},\n"
                     "  {\"directory\": \"" +
                     proj +
                     "\", \"command\": \"c++ -std=c++23 -c b.cpp -o b.o\", "
                     "\"file\": \"b.cpp\"}\n"
                     "]\n");
    }
  };

  struct HeaderRaceProject {
    std::string cache;
    std::string proj;
    std::string header;
    std::string parse_gate_one;
    std::string source;

    HeaderRaceProject()
        : cache(make_temp_dir()), proj(cache + "/proj"),
          header(proj + "/header.hpp"),
          parse_gate_one(cache + "/parse_gate_one.hpp"),
          source(proj + "/main.cpp") {
      makedirs(proj);
      write_file(header, "int initial_header_symbol() { return 1; }\n");
      write_file(source,
                 "#include \"header.hpp\"\nint main_symbol() { return 2; }\n");
      write_file(proj + "/compile_commands.json",
                 "[{\"directory\": \"" + proj +
                     "\", \"command\": \"c++ -std=c++23 -c main.cpp -o "
                     "main.o\", \"file\": \"main.cpp\"}]\n");
    }
  };

  TEST_CASE("index: partial/no-op run preserves stale identity until reindex") {
    const FreshnessProject p;
    CmdResult r = run_cli({"import", "--db", p.proj}, p.cache);
    REQUIRE(r.rc == 0);

    {
      Storage db(p.cache + "/index.db");
      CHECK(db.index_identity().freshness == "unverifiable");
    }

    cidx::Logger log;
    log.set_file(p.cache + "/cidx.log");
    r = run_cli({"index"}, p.cache, &log);
    REQUIRE(r.rc == 0);
    {
      Storage db(p.cache + "/index.db");
      CHECK(db.index_identity().freshness == "current");
      REQUIRE(db.find_symbols("old_symbol", {}, 10).size() == 1);
      using namespace cidx::query;
      SqliteQueryReadAdapter read(db);
      Executor executor(read);
      const auto rows = executor.run((start(codebase()) | nodes()).plan());
      const std::string row_json =
          cidx::json_out::dumps_indent2(rows.to_json());
      CHECK(row_json.find("\"shape\"") < row_json.find("\"view\""));
      CHECK(row_json.find("\"view\"") < row_json.find("\"count\""));
      CHECK(row_json.find("\"count\"") < row_json.find("\"truncated\""));
      CHECK(row_json.find("\"truncated\"") < row_json.find("\"index\""));
      CHECK(row_json.find("\"index\"") < row_json.find("\"rows\""));
      const auto scalar =
          executor.run((start(codebase()) | nodes() | count()).plan());
      const std::string scalar_json =
          cidx::json_out::dumps_indent2(scalar.to_json());
      CHECK(scalar_json.find("\"truncated\"") < scalar_json.find("\"index\""));
    }

    write_file(p.a, "int new_symbol() { return 3; }\n");
    r = run_cli({"index", p.b}, p.cache, &log);
    REQUIRE(r.rc == 0);
    {
      Storage db(p.cache + "/index.db");
      CHECK(db.index_identity().freshness == "stale");
      CHECK(db.find_symbols("old_symbol", {}, 10).size() == 1);
      CHECK(db.find_symbols("new_symbol", {}, 10).empty());
    }

    r = run_cli({"index", p.a}, p.cache, &log);
    REQUIRE(r.rc == 0);
    Storage db(p.cache + "/index.db");
    CHECK(db.index_identity().freshness == "current");
    CHECK(db.find_symbols("old_symbol", {}, 10).empty());
    const auto replacement = db.find_symbols("new_symbol", {}, 10);
    REQUIRE(replacement.size() == 1);
    CHECK(replacement[0].line == 1);
  }

  TEST_CASE("index: source snapshot detects replacement after parse") {
    const std::string dir = make_temp_dir();
    const std::string source = dir + "/source.cpp";
    write_file(source, "int intermediate_symbol() { return 1; }\n");
    const cidx::ast::SourceSnapshot parsed =
        cidx::ast::SourceSnapshot::capture(source);
    CHECK(parsed.matches(source));

    write_file(source, "int final_symbol() { return 2; }\n");
    CHECK_FALSE(parsed.matches(source));
  }

  TEST_CASE("index: owned header A to B to A mutation stays pending") {
    const HeaderRaceProject p;
    const std::string initial_header =
        "int initial_header_symbol() { return 1; }\n";
    CmdResult r = run_cli({"import", "--db", p.proj}, p.cache);
    REQUIRE(r.rc == 0);

    cidx::Logger log;
    log.set_file(p.cache + "/cidx.log");
    r = run_cli({"index"}, p.cache, &log);
    REQUIRE(r.rc == 0);
    {
      Storage db(p.cache + "/index.db");
      CHECK(db.index_identity().freshness == "current");
      CHECK(db.find_symbols("initial_header_symbol", {}, 10).size() == 1);
    }

    const std::string intermediate_header =
        "int intermediate_header_symbol() { return 3; }\n"
        "#include \"" +
        p.parse_gate_one + "\"\n";
    write_file(p.header, intermediate_header);
    REQUIRE(::mkfifo(p.parse_gate_one.c_str(), 0600) == 0);
    write_file(p.source, "#include \"header.hpp\"\n"
                         "int changed_main_symbol() { return 5; }\n");
    std::atomic<bool> indexing_done = false;
    std::atomic<bool> restorer_failed = false;
    std::atomic<bool> restored = false;
    std::thread restorer([&] {
      int gate = -1;
      while (!indexing_done.load(std::memory_order_acquire)) {
        gate = ::open(p.parse_gate_one.c_str(), O_WRONLY | O_NONBLOCK);
        if (gate >= 0) {
          break;
        }
        std::this_thread::yield();
      }
      if (gate < 0) {
        return;
      }
      if (::unlink(p.header.c_str()) != 0) {
        restorer_failed.store(true, std::memory_order_release);
        ::close(gate);
        return;
      }
      std::ofstream restored_header(p.header);
      if (!restored_header.good()) {
        restorer_failed.store(true, std::memory_order_release);
        ::close(gate);
        return;
      }
      restored_header << initial_header;
      restored_header.close();
      const char release = '\n';
      if (::write(gate, &release, 1) != 1) {
        restorer_failed.store(true, std::memory_order_release);
        ::close(gate);
        return;
      }
      ::close(gate);
      restored.store(true, std::memory_order_release);
    });
    r = run_cli({"index"}, p.cache, &log);
    indexing_done.store(true, std::memory_order_release);
    restorer.join();
    REQUIRE_FALSE(restorer_failed.load(std::memory_order_acquire));
    REQUIRE(restored.load(std::memory_order_acquire));
    CHECK(r.rc == 1);
    CHECK(r.err.find("source changed during indexing") != std::string::npos);

    {
      Storage db(p.cache + "/index.db");
      CHECK(db.index_identity().freshness == "stale");
      CHECK(db.find_symbols("intermediate_header_symbol", {}, 10).empty());
      CHECK(db.find_symbols("initial_header_symbol", {}, 10).size() == 1);
      const auto header = db.get_file(p.header);
      REQUIRE(header.has_value());
      CHECK(header->indexed);
    }

    r = run_cli({"index"}, p.cache, &log);
    REQUIRE(r.rc == 0);
    Storage db(p.cache + "/index.db");
    CHECK(db.index_identity().freshness == "current");
    CHECK(db.find_symbols("initial_header_symbol", {}, 10).size() == 1);
    CHECK(db.find_symbols("intermediate_header_symbol", {}, 10).empty());
    CHECK(db.find_symbols("changed_main_symbol", {}, 10).size() == 1);
  }

  TEST_CASE("index: two-TU pending flow — header counters, md5 skip, "
            "content change re-indexes") {
    const TwoTuProject p;
    const std::string &t = p.cache;
    const std::string &proj = p.proj;
    CmdResult r = run_cli({"import", "--db", proj}, t);
    REQUIRE(r.rc == 0);

    cidx::Logger log;
    log.set_file(t + "/cidx.log");

    // $ python3 -m indexer index   (pending order: c.path, d.path, f.name —
    // app.c first; its TU indexes mathlib.h, mathlib.c then finds it current)
    r = run_cli({"index"}, t, &log);
    CHECK(r.rc == 0);
    CHECK(r.err.empty());
    CHECK(r.out ==
          "indexing " + proj +
              "/app.c\n"
              "  -> 1 symbols; headers: 1 indexed (+3 symbols), 0 already, "
              "0 system, 0 unowned\n"
              "indexing " +
              proj +
              "/mathlib.c\n"
              "  -> 3 symbols; headers: 0 indexed (+0 symbols), 1 already, "
              "0 system, 0 unowned\n"
              "index: 2 indexed, 0 failed, 0 already indexed\n");

    // Header row written via the including TU (app.c): indexed, md5 captured,
    // and stamped with that TU's compile_options + driver so the header is
    // standalone-reparseable (v0.13.0; was NULL options/driver pre-G20 fix).
    {
      Storage db(t + "/index.db");
      const std::optional<cidx::File> h = db.get_file(proj + "/mathlib.h");
      REQUIRE(h);
      CHECK(h->indexed);
      CHECK(h->md5);
      const std::optional<cidx::File> tu = db.get_file(proj + "/app.c");
      REQUIRE(tu);
      CHECK(h->compile_options == tu->compile_options);
      CHECK(h->driver == tu->driver);
    }

    // $ python3 -m indexer index   (second run: md5-current — the header row
    // joined the snapshot, so 3 skips, nothing parsed)
    r = run_cli({"index"}, t, &log);
    CHECK(r.rc == 0);
    CHECK(r.out == "index: 0 indexed, 0 failed, 3 already indexed\n");

    // $ python3 -m indexer index <proj>/app.c   (FILE arg, already indexed)
    r = run_cli({"index", proj + "/app.c"}, t, &log);
    CHECK(r.rc == 0);
    CHECK(r.out == "file: " + proj + "/app.c\n  already indexed\n");

    // $ python3 -m indexer index app.c --source proj   (relative FILE
    // resolves against the --source component root)
    r = run_cli({"index", "app.c", "--source", "proj"}, t, &log);
    CHECK(r.rc == 0);
    CHECK(r.out == "file: " + proj + "/app.c\n  already indexed\n");

    // Content change -> md5 mismatch -> only app.c re-indexed; the header is
    // still current ("1 already"). Reindex replacement removes the previous
    // file-owned symbols before storing the new AST output.
    write_file(proj + "/app.c",
               "#include \"mathlib.h\"\n"
               "int main(void) { return add(square(2), 1); }\n");
    r = run_cli({"index"}, t, &log);
    CHECK(r.rc == 0);
    CHECK(r.out ==
          "indexing " + proj +
              "/app.c\n"
              "  -> 1 symbols; headers: 0 indexed (+0 symbols), 1 already, "
              "0 system, 0 unowned\n"
              "index: 1 indexed, 0 failed, 2 already indexed\n");

    // No warnings anywhere on the happy path: the lazy log never appeared
    // and no warning-count line was printed (G27).
    CHECK(!path_exists(t + "/cidx.log"));
  }

  TEST_CASE("index: fatal include error — exit 1, rest indexed, flag dump "
            "only in cidx.log") {
    const TwoTuProject p(/*with_bad_tu=*/true);
    const std::string &t = p.cache;
    const std::string &proj = p.proj;
    CmdResult r = run_cli({"import", "--db", proj}, t);
    REQUIRE(r.rc == 0);

    cidx::Logger log;
    log.set_file(t + "/cidx.log");

    // $ python3 -m indexer index   (bad.c aborts FATAL, the others index;
    // the ERROR flag-dump record makes the warning counter 1 -> summary line)
    r = run_cli({"index"}, t, &log);
    CHECK(r.rc == 1);
    CHECK(r.out ==
          "indexing " + proj +
              "/app.c\n"
              "  -> 1 symbols; headers: 1 indexed (+3 symbols), 0 already, "
              "0 system, 0 unowned\n"
              "indexing " +
              proj +
              "/bad.c\n"
              "indexing " +
              proj +
              "/mathlib.c\n"
              "  -> 3 symbols; headers: 0 indexed (+0 symbols), 1 already, "
              "0 system, 0 unowned\n"
              "index: 2 indexed, 1 failed, 0 already indexed\n"
              "1 warning(s)/error(s) logged to " +
              t + "/cidx.log\n");
    // Terminal gets ONLY the short summary (G28)…
    CHECK(r.err == "error: " + proj + "/bad.c: 1 fatal diagnostic(s): " + proj +
                       "/bad.c:1: 'missing.h' file not found\n");
    // …the flag dump and per-diagnostic lines live in the log.
    REQUIRE(path_exists(t + "/cidx.log"));
    const std::string logged = read_file(t + "/cidx.log");
    CHECK(logged.find("failed parse flags:") != std::string::npos);
    CHECK(logged.find("-ferror-limit=0") != std::string::npos);
    CHECK(logged.find("'missing.h' file not found") != std::string::npos);

    // The failed TU stays pending (never marked indexed).
    {
      Storage db(t + "/index.db");
      const std::optional<cidx::File> bad = db.get_file(proj + "/bad.c");
      REQUIRE(bad);
      CHECK(!bad->indexed);
    }

    // Re-run: only bad.c is retried (and fails again) — exit stays 1.
    cidx::Logger log2;
    log2.set_file(t + "/cidx.log");
    r = run_cli({"index"}, t, &log2);
    CHECK(r.rc == 1);
    CHECK(r.out == "indexing " + proj +
                       "/bad.c\n"
                       "index: 0 indexed, 1 failed, 3 already indexed\n"
                       "1 warning(s)/error(s) logged to " +
                       t + "/cidx.log\n");
  }

  TEST_CASE("index: static member definition spanning an X-macro .def include "
            "indexes without crashing (cross-buffer init-slice guard)") {
    // Regression (found indexing llvm-project: clang::FPOptions::TotalWidth).
    // The out-of-line static data member's initializer is assembled by an
    // #include inside the declarator, so the definition's source range BEGINS
    // in the .cpp and ENDS inside the .def file — two different file buffers.
    // Slicing raw text between getCharacterData() pointers from those two
    // buffers computed a garbage length and crashed (SIGSEGV) pre-guard; the
    // TU must index cleanly, merely without a static_var_init_text.
    const std::string t = make_temp_dir();
    const std::string proj = t + "/proj";
    makedirs(proj);
    write_file(proj + "/widths.def", "+ 1\n+ 2\n");
    write_file(proj + "/s.h", "struct S { static const int total; };\n");
    write_file(proj + "/s.cpp", "#include \"s.h\"\n"
                                "const int S::total = 0\n"
                                "#include \"widths.def\"\n"
                                "    ;\n");
    write_file(proj + "/compile_commands.json",
               "[{\"directory\": \"" + proj +
                   "\", \"command\": \"c++ -I. -c s.cpp -o s.o\", "
                   "\"file\": \"s.cpp\"}]\n");
    CmdResult r = run_cli({"import", "--db", proj}, t);
    REQUIRE(r.rc == 0);

    cidx::Logger log;
    log.set_file(t + "/cidx.log");
    r = run_cli({"index"}, t, &log);
    CHECK(r.rc == 0);
    CHECK(r.err.empty());
    CHECK(r.out.find("index: 1 indexed, 0 failed") != std::string::npos);

    // The symbol landed as a definition despite the skipped init slice.
    Storage db(t + "/index.db");
    const std::vector<cidx::Symbol> hits = db.find_symbols("S::total", {}, 10);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].is_definition);
  }

  TEST_CASE("index: conditional header facts are configuration-qualified") {
    const std::string t = make_temp_dir();
    const std::string proj = t + "/proj";
    makedirs(proj);
    write_file(proj + "/conditional.hpp",
               "#pragma once\n"
               "#ifdef ONLY_A\nint only_a() { return 1; }\n#endif\n"
               "#ifdef ONLY_B\nint only_b() { return 2; }\n#endif\n");
    write_file(proj + "/a.cpp", "#include \"conditional.hpp\"\n"
                                "int a() { return only_a(); }\n");
    write_file(proj + "/b.cpp", "#include \"conditional.hpp\"\n"
                                "int b() { return only_b(); }\n");
    write_file(proj + "/compile_commands.json",
               "[{\"directory\": \"" + proj +
                   "\", \"command\": \"c++ -I. -DONLY_A -c a.cpp -o a.o\", "
                   "\"file\": \"a.cpp\"}, {\"directory\": \"" +
                   proj +
                   "\", \"command\": \"c++ -I. -DONLY_B -c b.cpp -o b.o\", "
                   "\"file\": \"b.cpp\"}]\n");
    REQUIRE(run_cli({"import", "--db", proj}, t).rc == 0);
    cidx::Logger log;
    log.set_file(t + "/cidx.log");
    REQUIRE(run_cli({"index"}, t, &log).rc == 0);

    Storage db(t + "/index.db");
    const auto header = db.get_file(proj + "/conditional.hpp");
    REQUIRE(header.has_value());
    std::vector<int64_t> configs;
    for (const auto &row : db.file_configs_for(header->id)) {
      if (row.role == "header" &&
          row.state == cidx::TranslationUnitConfigState::registered) {
        configs.push_back(row.config_id);
      }
    }
    REQUIRE(configs.size() == 2);
    std::ranges::sort(configs);

    const bool first_is_a =
        db.search_symbols("only_a", std::nullopt, configs[0]).size() == 1;
    const bool first_is_b =
        db.search_symbols("only_b", std::nullopt, configs[0]).size() == 1;
    CHECK(first_is_a != first_is_b);
    const auto all =
        db.symbols_for_config(header->id, configs, cidx::FactCoverage::all);
    REQUIRE(all.coverage_complete);
    CHECK(all.symbols.size() >= 2);
    const auto invariant = db.symbols_for_config(header->id, configs,
                                                 cidx::FactCoverage::invariant);
    CHECK(invariant.coverage_complete);
    CHECK(invariant.symbols.empty());
    const auto definitions_a = db.fact_ids_for_config(
        header->id, "definition", {configs[0]}, cidx::FactCoverage::one);
    const auto definitions_b = db.fact_ids_for_config(
        header->id, "definition", {configs[1]}, cidx::FactCoverage::one);
    CHECK(definitions_a.coverage_complete);
    CHECK(definitions_b.coverage_complete);
    REQUIRE(definitions_a.ids.size() == 1);
    REQUIRE(definitions_b.ids.size() == 1);
    CHECK(definitions_a.ids != definitions_b.ids);
    CHECK_FALSE(db.symbols_for_config(header->id, {configs[0], -1},
                                      cidx::FactCoverage::one)
                    .coverage_complete);
  }

} // TEST_SUITE("clang")

// -- analyze (Souffle Datalog)
// -------------------------------------------------

namespace {

// True when the standalone souffle interpreter is reachable the same way
// cmd_analyze resolves it (CIDX_SOUFFLE, else PATH).
bool souffle_available() {
  const char *env = std::getenv("CIDX_SOUFFLE");
  if (env != nullptr && *env != '\0') {
    return ::access(env, X_OK) == 0;
  }
  return std::system("command -v souffle >/dev/null 2>&1") == 0;
}

// Minimal call graph: alpha -> beta -> alpha (cycle) and beta -> gamma.
void seed_analyze(const std::string &cache) {
  Storage db(cache + "/index.db");
  db.add_component("lab", "/nonexistent/lab", "repo");
  const int64_t f1 = db.add_file_path("/nonexistent/lab/a.c");
  Symbol s;
  s.kind = "function";
  s.is_definition = true;
  s.file_id = f1;
  s.col = 1;
  s.usr = "c:@F@alpha";
  s.spelling = "alpha";
  s.qual_name = "alpha";
  s.line = 1;
  const int64_t a = db.add_symbol(s);
  s.usr = "c:@F@beta";
  s.spelling = "beta";
  s.qual_name = "beta";
  s.line = 10;
  const int64_t b = db.add_symbol(s);
  s.usr = "c:@F@gamma";
  s.spelling = "gamma";
  s.qual_name = "gamma";
  s.line = 20;
  const int64_t c = db.add_symbol(s);
  cidx::Edge e;
  e.kind = 1; // calls
  e.src_id = a;
  e.dst_id = b;
  db.add_edge(e);
  e.src_id = b;
  e.dst_id = a;
  db.add_edge(e);
  e.src_id = b;
  e.dst_id = c;
  db.add_edge(e);
}

} // namespace

TEST_CASE("args: analyze grammar — flags, defaults, -h help") {
  cli::ParsedArgs pa = cli::parse_args(
      {"analyze", "--rule", "cycles", "--jobs", "2", "--db", "/x/i.db"});
  CHECK(pa.command == "analyze");
  CHECK(pa.analyze_rule == "cycles");
  CHECK(pa.analyze_jobs == 2);
  CHECK(pa.index_db == "/x/i.db");
  CHECK(!pa.analyze_list);

  pa = cli::parse_args({"analyze", "--list"});
  CHECK(pa.analyze_list);
  CHECK(pa.analyze_jobs == 1); // default
  CHECK(!pa.analyze_rule);

  pa = cli::parse_args({"analyze", "--export-facts", "/tmp/f"});
  CHECK(pa.analyze_export == "/tmp/f");

  pa = cli::parse_args({"analyze", "--rules-file", "prog.dl"});
  CHECK(pa.analyze_rules_file == "prog.dl");

  pa = cli::parse_args({"analyze", "-h"});
  REQUIRE(pa.help_text);
  CHECK(pa.help_text->find("Usage: cidx analyze [OPTIONS]") !=
        std::string::npos);

  const ParseFail f1 = parse_fail({"analyze", "--jobs"});
  CHECK(f1.code == 2);
  const ParseFail f2 = parse_fail({"analyze", "--jobs", "x"});
  CHECK(f2.code == 2);
}

// python3 -m indexer analyze --list
TEST_CASE("analyze: --list prints the built-in rules as JSON") {
  const std::string t = make_temp_dir();
  const CmdResult r = run_cli({"analyze", "--list"}, t);
  CHECK(r.rc == 0);
  CHECK(r.err.empty());
  CHECK(r.out ==
        "{\n"
        "  \"rules\": [\n"
        "    {\n"
        "      \"name\": \"callgraph\",\n"
        "      \"description\": \"direct and transitive call graph (outputs: "
        "call, call_transitive)\"\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"cycles\",\n"
        "      \"description\": \"call cycles over calls/dispatch_calls edges "
        "(outputs: cycle_member, cycle_edge)\"\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"unused\",\n"
        "      \"description\": \"defined functions with no incoming call or "
        "override edge (outputs: unused)\"\n"
        "    }\n"
        "  ]\n"
        "}\n");
}

TEST_CASE("analyze: mode and jobs validation (exit 2), unknown rule and "
          "missing index (exit 1)") {
  const std::string t = make_temp_dir();
  seed_analyze(t);

  CmdResult r = run_cli({"analyze"}, t);
  CHECK(r.rc == 2);
  CHECK(r.err == "error: exactly one of --list, --export-facts, --rule, or "
                 "--rules-file is required\n");

  r = run_cli({"analyze", "--list", "--rule", "cycles"}, t);
  CHECK(r.rc == 2);

  r = run_cli({"analyze", "--rule", "cycles", "--jobs", "0"}, t);
  CHECK(r.rc == 2);
  CHECK(r.err == "error: --jobs must be at least 1\n");

  r = run_cli({"analyze", "--rule", "nope"}, t);
  CHECK(r.rc == 1);
  CHECK(r.err == "error: unknown rule: nope (see cidx analyze --list)\n");

  r = run_cli({"analyze", "--rules-file", t + "/absent.dl"}, t);
  CHECK(r.rc == 1);
  CHECK(r.err == "error: rules file not found: " + t + "/absent.dl\n");

  const std::string empty = make_temp_dir();
  r = run_cli({"analyze", "--rule", "cycles"}, empty);
  CHECK(r.rc == 1);
  CHECK(r.err == "error: index not found at " + empty +
                     "/index.db (run 'cidx import' first, or pass --db)\n");
}

TEST_CASE("analyze: --export-facts writes TSV facts plus the prelude") {
  const std::string t = make_temp_dir();
  seed_analyze(t);
  const std::string out_dir = t + "/facts";
  const CmdResult r = run_cli({"analyze", "--export-facts", out_dir}, t);
  CHECK(r.rc == 0);
  CHECK(r.err.empty());
  // 10 relations; the exact row total depends only on the seeded rows.
  CHECK(r.out.find(out_dir + ": 10 fact files, ") == 0);

  const std::string symbols = read_file(out_dir + "/symbol.facts");
  CHECK(symbols == "1\tc:@F@alpha\talpha\talpha\t8\t1\t1\t1\n"
                   "2\tc:@F@beta\tbeta\tbeta\t8\t1\t1\t10\n"
                   "3\tc:@F@gamma\tgamma\tgamma\t8\t1\t1\t20\n");
  const std::string edges = read_file(out_dir + "/edge.facts");
  CHECK(edges == "1\t1\t2\t1\t1\n"
                 "2\t2\t1\t1\t1\n"
                 "3\t2\t3\t1\t1\n");
  const std::string prelude = read_file(out_dir + "/cidx_facts.dl");
  CHECK(prelude.find(".decl symbol(") != std::string::npos);
  CHECK(prelude.find(".input file") != std::string::npos);
}

// python3 -m indexer analyze --rule cycles --db <seeded>: alpha and beta form
// the only call cycle; gamma stays out.
TEST_CASE("analyze: --rule cycles finds the seeded recursion (needs souffle)") {
  if (!souffle_available()) {
    MESSAGE("SKIP: souffle interpreter not installed");
    return;
  }
  const std::string t = make_temp_dir();
  seed_analyze(t);
  const CmdResult r = run_cli({"analyze", "--rule", "cycles"}, t);
  CHECK(r.rc == 0);
  CHECK(r.err.empty());
  CHECK(r.out == "{\n"
                 "  \"rule\": \"cycles\",\n"
                 "  \"db\": \"" +
                     t +
                     "/index.db\",\n"
                     "  \"relations\": {\n"
                     "    \"cycle_edge\": [\n"
                     "      [\n"
                     "        \"c:@F@alpha\",\n"
                     "        \"alpha\",\n"
                     "        \"c:@F@beta\",\n"
                     "        \"beta\"\n"
                     "      ],\n"
                     "      [\n"
                     "        \"c:@F@beta\",\n"
                     "        \"beta\",\n"
                     "        \"c:@F@alpha\",\n"
                     "        \"alpha\"\n"
                     "      ]\n"
                     "    ],\n"
                     "    \"cycle_member\": [\n"
                     "      [\n"
                     "        \"c:@F@alpha\",\n"
                     "        \"alpha\"\n"
                     "      ],\n"
                     "      [\n"
                     "        \"c:@F@beta\",\n"
                     "        \"beta\"\n"
                     "      ]\n"
                     "    ]\n"
                     "  }\n"
                     "}\n");
}

TEST_CASE("analyze: --rules-file runs user Datalog against the exported facts "
          "(needs souffle)") {
  if (!souffle_available()) {
    MESSAGE("SKIP: souffle interpreter not installed");
    return;
  }
  const std::string t = make_temp_dir();
  seed_analyze(t);
  const std::string prog = t + "/leaves.dl";
  {
    std::ofstream f(prog);
    f << ".decl leaf(usr:symbol, name:symbol)\n"
         "leaf(u, n) :- symbol(s, u, n, _, _, _, _, _), !edge(_, s, _, _, _).\n"
         ".output leaf\n";
  }
  const CmdResult r = run_cli({"analyze", "--rules-file", prog}, t);
  CHECK(r.rc == 0);
  CHECK(r.out == "{\n"
                 "  \"rule\": \"" +
                     prog +
                     "\",\n"
                     "  \"db\": \"" +
                     t +
                     "/index.db\",\n"
                     "  \"relations\": {\n"
                     "    \"leaf\": [\n"
                     "      [\n"
                     "        \"c:@F@gamma\",\n"
                     "        \"gamma\"\n"
                     "      ]\n"
                     "    ]\n"
                     "  }\n"
                     "}\n");
}

TEST_CASE("analyze: broken user Datalog surfaces the souffle error (needs "
          "souffle)") {
  if (!souffle_available()) {
    MESSAGE("SKIP: souffle interpreter not installed");
    return;
  }
  const std::string t = make_temp_dir();
  seed_analyze(t);
  const std::string prog = t + "/broken.dl";
  {
    std::ofstream f(prog);
    f << ".decl broken(\n";
  }
  const CmdResult r = run_cli({"analyze", "--rules-file", prog}, t);
  CHECK(r.rc == 1);
  CHECK(r.err.find("error: souffle failed (exit ") == 0);
}

int main(int argc, char **argv) {
  doctest::Context ctx(argc, argv);
  const int res = ctx.run();
  if (ctx.shouldExit()) {
    return res;
  }
  if (res == 0 && g_clang_skipped) {
    return 77; // CTest SKIP_RETURN_CODE — missing lab fixtures is a skip
  }
  return res;
}
