// diff_test — cidx-diff plumbing (src/diff/, docs/diff.md). The suite "diff"
// is hermetic (no parse; label "default"): read-only Storage mode, per-side
// configuration resolution, option classification/delta, and the CLI11
// grammar driven in-process through diff::run(). Real-parse fixture cases
// live in TEST_SUITE("clang"), registered separately under the "clang" label.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "diff/compare.hpp"
#include "diff/driver.hpp"
#include "diff/report.hpp"
#include "diff/target.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/logger.hpp"

namespace diff = cidx::diff;

namespace {

std::string make_temp_dir() {
  char tmpl[] = "/tmp/cidx_diff_XXXXXX";
  char *d = ::mkdtemp(tmpl);
  REQUIRE(d != nullptr);
  return d;
}

std::string read_bytes(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// A minimal registered index: one TU with stored options + driver, one
// header without options. Hermetic — nothing is parsed.
struct Fixture {
  std::string dir;
  std::string db;
  std::string src;
  std::string hdr;
};

Fixture make_index() {
  Fixture f;
  f.dir = make_temp_dir();
  ::setenv("INDEXER_CACHE", f.dir.c_str(), 1);
  f.db = f.dir + "/index.db";
  f.src = f.dir + "/a.cpp";
  f.hdr = f.dir + "/a.hpp";
  cidx::Storage db(f.db);
  db.add_component("proj", f.dir);
  db.add_file_path(f.src, std::nullopt, std::nullopt,
                   std::vector<std::string>{"-std=c++17", "-I/opt/inc",
                                            "-DLEFT=1"},
                   std::string("c++"));
  db.add_file_path(f.hdr);
  return f;
}

int run_diff(const std::vector<std::string> &argv, std::string *out_text,
             std::string *err_text) {
  std::ostringstream out;
  std::ostringstream err;
  const int rc = diff::run(argv, out, err);
  if (out_text != nullptr) {
    *out_text = out.str();
  }
  if (err_text != nullptr) {
    *err_text = err.str();
  }
  return rc;
}

} // namespace

TEST_SUITE("diff") {

TEST_CASE("read-only Storage reads an existing index") {
  const Fixture f = make_index();
  cidx::Storage ro(f.db, cidx::Storage::OpenMode::read_only);
  const std::optional<cidx::File> rec = ro.get_file(f.src);
  REQUIRE(rec.has_value());
  CHECK(rec->name == "a.cpp");
  REQUIRE(rec->compile_options.has_value());
  CHECK(rec->compile_options->front() == "-std=c++17");
  CHECK(rec->driver == std::optional<std::string>("c++"));
  // Writes fail at the SQLite layer -- no per-writer guards.
  CHECK_THROWS_AS(ro.add_component("other", f.dir + "/x"),
                  cidx::StorageError);
}

TEST_CASE("read-only rejects a mismatched schema_version") {
  const Fixture f = make_index();
  {
    cidx::SqliteDb raw(f.db);
    raw.exec("UPDATE meta SET value = '29' WHERE key = 'schema_version'");
  }
  CHECK_THROWS_WITH_AS(
      cidx::Storage(f.db, cidx::Storage::OpenMode::read_only),
      doctest::Contains("schema_version"), cidx::CidxError);
}

TEST_CASE("read-only rejects a database without a meta table") {
  const std::string dir = make_temp_dir();
  const std::string db_path = dir + "/plain.db";
  {
    cidx::SqliteDb raw(db_path);
    raw.exec("CREATE TABLE t (x)");
  }
  CHECK_THROWS_WITH_AS(
      cidx::Storage(db_path, cidx::Storage::OpenMode::read_only),
      doctest::Contains("schema_version"), cidx::CidxError);
}

TEST_CASE("read-only open leaves the database byte-identical") {
  const Fixture f = make_index();
  const std::string before = read_bytes(f.db);
  {
    cidx::Storage ro(f.db, cidx::Storage::OpenMode::read_only);
    (void)ro.get_file(f.src);
    (void)ro.get_alias("proj");
  }
  CHECK(read_bytes(f.db) == before);
}

TEST_CASE("classify_options splits the semantic-affecting flags") {
  const diff::OptionClasses c = diff::classify_options(
      {"-std=c++20", "-target", "arm64-apple-macosx", "-DFOO=1", "-U", "BAR",
       "-I/a", "-isystem", "/b", "-iquote/q", "-F/Lib", "-fno-exceptions",
       "-D"});
  CHECK(c.standard == "c++20");
  CHECK(c.target == "arm64-apple-macosx");
  CHECK(c.definitions == std::vector<std::string>{"-DFOO=1", "-UBAR"});
  CHECK(c.includes == std::vector<std::string>{"-I /a", "-isystem /b",
                                               "-iquote /q", "-F /Lib"});
  // A dangling -D has no value to glue: classified as other, verbatim.
  CHECK(c.other == std::vector<std::string>{"-fno-exceptions", "-D"});
}

TEST_CASE("config_delta names every differing axis") {
  diff::ParseConfig l;
  diff::ParseConfig r;
  l.driver = "cc";
  r.driver = "c++";
  l.classes = diff::classify_options({"-std=c++17", "-DX=1", "-I/a", "-Wall"});
  r.classes =
      diff::classify_options({"-std=c++20", "-DY=2", "-I/b", "-Wextra"});
  const diff::ConfigDelta d = diff::config_delta(l, r);
  CHECK_FALSE(d.identical);
  REQUIRE(d.standard.has_value());
  CHECK(d.standard->first == "c++17");
  CHECK(d.standard->second == "c++20");
  CHECK_FALSE(d.target.has_value());
  REQUIRE(d.driver.has_value());
  CHECK(d.driver->first == "cc");
  CHECK(d.driver->second == "c++");
  CHECK(d.definitions_added == std::vector<std::string>{"-DY=2"});
  CHECK(d.definitions_removed == std::vector<std::string>{"-DX=1"});
  CHECK(d.includes_changed);
  CHECK(d.options_left_only == std::vector<std::string>{"-Wall"});
  CHECK(d.options_right_only == std::vector<std::string>{"-Wextra"});

  const diff::ConfigDelta same = diff::config_delta(l, l);
  CHECK(same.identical);
  CHECK_FALSE(same.standard.has_value());
  CHECK(same.definitions_added.empty());
  CHECK_FALSE(same.includes_changed);
}

TEST_CASE("config_delta: reordered -D/-U is not identical") {
  // Same multiset of definitions, opposite order -> opposite final macro
  // state (X defined vs undefined). Must not read as an identical config.
  diff::ParseConfig l;
  diff::ParseConfig r;
  l.classes = diff::classify_options({"-DX=1", "-UX"});
  r.classes = diff::classify_options({"-UX", "-DX=1"});
  const diff::ConfigDelta d = diff::config_delta(l, r);
  CHECK(d.definitions_added.empty());
  CHECK(d.definitions_removed.empty());
  CHECK(d.definitions_reordered);
  CHECK_FALSE(d.identical);

  // Identical order stays identical and unflagged.
  const diff::ConfigDelta same = diff::config_delta(l, l);
  CHECK_FALSE(same.definitions_reordered);
  CHECK(same.identical);
}

TEST_CASE("config_delta: reordered paired `other` options are not identical") {
  // `-include a.h -include b.h` and its reverse share an `other` multiset but
  // can leave the parser seeing different macro state. The sorted multiset
  // difference is empty, so the reorder must be flagged on its own axis.
  diff::ParseConfig l;
  diff::ParseConfig r;
  l.classes = diff::classify_options({"-include", "a.h", "-include", "b.h"});
  r.classes = diff::classify_options({"-include", "b.h", "-include", "a.h"});
  const diff::ConfigDelta d = diff::config_delta(l, r);
  CHECK(d.options_left_only.empty());
  CHECK(d.options_right_only.empty());
  CHECK(d.options_reordered);
  CHECK_FALSE(d.identical);

  // Identical order stays identical and unflagged; a genuine set difference
  // still surfaces through options_left_only/right_only, not the reorder axis.
  const diff::ConfigDelta same = diff::config_delta(l, l);
  CHECK_FALSE(same.options_reordered);
  CHECK(same.identical);
  diff::ParseConfig r2;
  r2.classes = diff::classify_options({"-include", "a.h", "-Xclang", "-foo"});
  const diff::ConfigDelta setdiff = diff::config_delta(l, r2);
  CHECK_FALSE(setdiff.options_reordered);
  CHECK_FALSE(setdiff.identical);
}

TEST_CASE("CLI misuse exits 2 with a usage message") {
  const Fixture f = make_index();
  std::string out;
  std::string err;

  CHECK(run_diff({}, &out, &err) == 2); // missing subcommand
  CHECK(err.find("error:") != std::string::npos);

  CHECK(run_diff({"file", f.src}, &out, &err) == 2); // missing RIGHT_FILE
  CHECK(err.find("cidx-diff file: error:") != std::string::npos);

  CHECK(run_diff({"file", f.src, f.src, "--bogus"}, &out, &err) == 2);
  CHECK(run_diff({"file", f.src, f.src, "--mode", "weird"}, &out, &err) == 2);
  CHECK(run_diff({"file", f.src, f.src, "--context", "-1"}, &out, &err) == 2);
  CHECK(run_diff({"symbol", f.src, f.src}, &out, &err) == 2); // no selectors
  CHECK(run_diff({"symbol", f.src, f.src, "--left", "x", "--right", "y",
                  "--kind", "enum"},
                 &out, &err) == 2); // enum is not a selectable kind
}

TEST_CASE("--help and --version exit 0 on stdout") {
  std::string out;
  std::string err;
  CHECK(run_diff({"--help"}, &out, &err) == 0);
  CHECK(out.find("cidx-diff") != std::string::npos);
  CHECK(err.empty());

  CHECK(run_diff({"--version"}, &out, &err) == 0);
  CHECK(out == std::string("cidx-diff ") + cidx::cli::kVersion + "\n");

  CHECK(run_diff({"file", "--help"}, &out, &err) == 0);
  CHECK(out.find("LEFT_FILE") != std::string::npos);
}

TEST_CASE("unregistered file exits 1 naming the path and DB") {
  const Fixture f = make_index();
  std::string err;
  const int rc =
      run_diff({"file", f.dir + "/nope.cpp", f.src, "--db", f.db}, nullptr,
               &err);
  CHECK(rc == 1);
  CHECK(err.find("error:") != std::string::npos);
  CHECK(err.find("not registered") != std::string::npos);
  CHECK(err.find(f.dir + "/nope.cpp") != std::string::npos);
  CHECK(err.find(f.db) != std::string::npos);
}

TEST_CASE("missing index exits 1") {
  const Fixture f = make_index();
  std::string err;
  const int rc = run_diff(
      {"file", f.src, f.src, "--db", f.dir + "/absent.db"}, nullptr, &err);
  CHECK(rc == 1);
  CHECK(err.find("left side: cidx index not found") != std::string::npos);
  CHECK(err.find(f.dir + "/absent.db") != std::string::npos);

  err.clear();
  CHECK(run_diff({"file", f.src, f.src, "--db", f.db, "--right-db",
                  f.dir + "/gone.db"},
                 nullptr, &err) == 1);
  CHECK(err.find("right side: cidx index not found") != std::string::npos);
  CHECK(err.find("--right-db") != std::string::npos);
}

TEST_CASE("older-schema index exits 1 through the CLI") {
  const Fixture f = make_index();
  {
    cidx::SqliteDb raw(f.db);
    raw.exec("UPDATE meta SET value = '1' WHERE key = 'schema_version'");
  }
  std::string err;
  const int rc = run_diff({"file", f.src, f.src, "--db", f.db}, nullptr, &err);
  CHECK(rc == 1);
  CHECK(err.find("schema_version") != std::string::npos);
}

TEST_CASE("a registered pair reaches the analyzer") {
  // The fixture registers a.cpp but never writes it: the resolved pair gets
  // all the way to the parse, which fails on the missing file.
  const Fixture f = make_index();
  std::string err;
  const int rc = run_diff({"file", f.src, f.src, "--db", f.db}, nullptr, &err);
  CHECK(rc == 1);
  CHECK(err.find("cannot parse") != std::string::npos);
}

TEST_CASE("--mode semantic and both reach the analyzer") {
  // The fixture registers a.cpp but never writes it: every mode gets past
  // option handling to the parse, which fails on the missing file.
  const Fixture f = make_index();
  std::string err;
  int rc = run_diff({"file", f.src, f.src, "--db", f.db, "--mode", "both"},
                    nullptr, &err);
  CHECK(rc == 1);
  CHECK(err.find("cannot parse") != std::string::npos);
  rc = run_diff({"file", f.src, f.src, "--db", f.db, "--mode", "semantic"},
                nullptr, &err);
  CHECK(rc == 1);
  CHECK(err.find("cannot parse") != std::string::npos);
}

TEST_CASE("a header needs --left-tu; a registered TU provides its context") {
  const Fixture f = make_index();
  std::string err;

  int rc = run_diff({"file", f.hdr, f.src, "--db", f.db}, nullptr, &err);
  CHECK(rc == 1);
  CHECK(err.find("--left-tu") != std::string::npos);

  rc = run_diff({"file", f.hdr, f.src, "--db", f.db, "--left-tu",
                 f.dir + "/ghost.cpp"},
                nullptr, &err);
  CHECK(rc == 1);
  CHECK(err.find("not registered") != std::string::npos);

  rc = run_diff({"file", f.hdr, f.src, "--db", f.db, "--left-tu", f.src},
                nullptr, &err);
  CHECK(rc == 1);
  CHECK(err.find("cannot parse") !=
        std::string::npos); // config resolved, parse attempted
}

TEST_CASE("resolve_parse_config resolves options, driver and TU scope") {
  const Fixture f = make_index();
  const diff::ParseConfig cfg = diff::resolve_parse_config(
      {"left", f.src, f.db, std::nullopt});
  CHECK(cfg.file == f.src);
  CHECK(cfg.parse_file == f.src);
  CHECK_FALSE(cfg.restrict_to_file);
  CHECK(cfg.driver == std::optional<std::string>("c++"));
  CHECK(cfg.args == std::vector<std::string>{"-std=c++17", "-I/opt/inc",
                                             "-DLEFT=1"});
  CHECK(cfg.classes.standard == "c++17");

  const diff::ParseConfig hdr = diff::resolve_parse_config(
      {"left", f.hdr, f.db, f.src});
  CHECK(hdr.file == f.hdr);
  CHECK(hdr.parse_file == f.src);
  CHECK(hdr.restrict_to_file);
  CHECK(hdr.args == cfg.args);
}

TEST_CASE("resolve_parse_config rejects stale normalized associations") {
  const Fixture f = make_index();
  cidx::Storage db(f.db);
  cidx::IncludeConfig include{.tu_file_id = db.get_file(f.src)->id,
                              .digest = "stale",
                              .driver = std::string("c++"),
                              .working_dir = std::string("."),
                              .arguments = {"-DOLD=1"},
                              .lang_mode = std::string("c++")};
  db.add_include_config(include);
  db.add_file_path(f.src, std::nullopt, std::nullopt,
                   std::vector<std::string>{"-DNEW=1"}, std::string("c++"));
  CHECK_THROWS_WITH_AS(
      diff::resolve_parse_config({"left", f.src, f.db, std::nullopt}),
      doctest::Contains("stale"), cidx::CidxError);
}
TEST_CASE("edit script past kEditCap sets truncated and stops at the cap") {
  // Hermetic: synthetic syntax trees drive compare_sides directly. The child
  // count also exceeds kLcsLimit, so the positional-pairing fallback and the
  // op cap are both exercised.
  diff::SideAnalysis left;
  diff::SideAnalysis right;
  diff::Entity le;
  diff::Entity re;
  le.kind = re.kind = "function";
  le.name = re.name = "big";
  le.usr = re.usr = "c:@F@big#";
  le.syntax.kind = re.syntax.kind = "FunctionDecl";
  for (int i = 0; i < diff::kEditCap + 100; ++i) {
    diff::SynNode a;
    a.kind = "IntegerLiteral";
    a.label = "1";
    a.detail = "literal 1";
    a.fingerprint = "l" + std::to_string(i);
    diff::SynNode b = a;
    b.label = "2";
    b.detail = "literal 2";
    b.fingerprint = "r" + std::to_string(i);
    le.syntax.children.push_back(std::move(a));
    re.syntax.children.push_back(std::move(b));
  }
  le.syntax.fingerprint = "lroot";
  re.syntax.fingerprint = "rroot";
  left.entities.push_back(std::move(le));
  right.entities.push_back(std::move(re));

  const diff::Comparison cmp = diff::compare_sides(
      left, right, "heuristic", diff::ConfigDelta{}, "file");
  CHECK(cmp.truncated);
  CHECK(cmp.edit_count == diff::kEditCap);
  REQUIRE(cmp.pairs.size() == 1);
  CHECK(cmp.pairs[0].truncated);
  CHECK(cmp.pairs[0].edits.size() ==
        static_cast<std::size_t>(diff::kEditCap));
  CHECK(cmp.pairs[0].verdict == "unknown");

  std::ostringstream text;
  diff::render_report({"file", "syntax", "heuristic", 0, false}, left, right,
                      diff::ConfigDelta{}, cmp, text);
  CHECK(text.str().find("syntax: changed (1000 edits, truncated)") !=
        std::string::npos);

  std::ostringstream json;
  diff::render_report({"file", "syntax", "heuristic", 0, true}, left, right,
                      diff::ConfigDelta{}, cmp, json);
  CHECK(json.str().find("\"truncated\": true") != std::string::npos);
  CHECK(json.str().find("\"edit_count\": 1000") != std::string::npos);
  CHECK(json.str().find("\"semantic\": null") != std::string::npos);
}

TEST_CASE("a cap-starved later entity still reports changed, never "
          "equivalent") {
  // The shared op budget is drained by a huge first pair; the second pair is
  // genuinely changed but gets no ops. Its truth must come from the subtree
  // fingerprints, not from the (empty) edit list.
  diff::SideAnalysis left;
  diff::SideAnalysis right;
  diff::Entity lbig;
  diff::Entity rbig;
  lbig.kind = rbig.kind = "function";
  lbig.name = rbig.name = "big";
  lbig.usr = rbig.usr = "c:@F@big#";
  lbig.syntax.kind = rbig.syntax.kind = "FunctionDecl";
  for (int i = 0; i < diff::kEditCap + 100; ++i) {
    diff::SynNode a;
    a.kind = "IntegerLiteral";
    a.label = "1";
    a.detail = "literal 1";
    a.fingerprint = "l" + std::to_string(i);
    diff::SynNode b = a;
    b.label = "2";
    b.detail = "literal 2";
    b.fingerprint = "r" + std::to_string(i);
    lbig.syntax.children.push_back(std::move(a));
    rbig.syntax.children.push_back(std::move(b));
  }
  lbig.syntax.fingerprint = "lbig";
  rbig.syntax.fingerprint = "rbig";
  diff::Entity lsmall;
  diff::Entity rsmall;
  lsmall.kind = rsmall.kind = "function";
  lsmall.name = rsmall.name = "small";
  lsmall.usr = rsmall.usr = "c:@F@small#";
  lsmall.syntax.kind = rsmall.syntax.kind = "FunctionDecl";
  diff::SynNode sa;
  sa.kind = "IntegerLiteral";
  sa.label = "3";
  sa.detail = "literal 3";
  sa.fingerprint = "sl";
  diff::SynNode sb = sa;
  sb.label = "4";
  sb.detail = "literal 4";
  sb.fingerprint = "sr";
  lsmall.syntax.children.push_back(std::move(sa));
  rsmall.syntax.children.push_back(std::move(sb));
  lsmall.syntax.fingerprint = "slroot";
  rsmall.syntax.fingerprint = "srroot";
  left.entities.push_back(std::move(lbig));
  left.entities.push_back(std::move(lsmall));
  right.entities.push_back(std::move(rbig));
  right.entities.push_back(std::move(rsmall));

  const diff::Comparison cmp = diff::compare_sides(
      left, right, "heuristic", diff::ConfigDelta{}, "file");
  CHECK(cmp.truncated);
  CHECK(cmp.edit_count == diff::kEditCap);
  CHECK(cmp.syntax_changed);
  REQUIRE(cmp.pairs.size() == 2);
  const diff::EntityPair *small = nullptr;
  for (const diff::EntityPair &p : cmp.pairs)
    if (p.left != nullptr && p.left->name == "small")
      small = &p;
  REQUIRE(small != nullptr);
  CHECK(small->subtree_differs);
  CHECK(small->edits.empty()); // budget exhausted by the big pair
  CHECK(small->truncated);
  CHECK(small->verdict == "unknown");
  CHECK(small->verdict != "equivalent");

  std::ostringstream json;
  diff::render_report({"file", "both", "heuristic", 0, true}, left, right,
                      diff::ConfigDelta{}, cmp, json);
  // Both entity rows report status changed even though the second has no ops.
  CHECK(json.str().find("\"status\": \"unchanged\"") == std::string::npos);
}

TEST_CASE("--context larger than INT_MAX-line clamps instead of overflowing") {
  const std::string dir = make_temp_dir();
  const std::string lpath = dir + "/l.cpp";
  const std::string rpath = dir + "/r.cpp";
  {
    std::ofstream l(lpath);
    l << "int a() {\n  return 3;\n}\n";
    std::ofstream r(rpath);
    r << "int a() {\n  return 4;\n}\n";
  }
  diff::SideAnalysis left;
  diff::SideAnalysis right;
  left.config.file = lpath;
  right.config.file = rpath;
  diff::Entity le;
  diff::Entity re;
  le.kind = re.kind = "function";
  le.name = re.name = "a";
  le.usr = re.usr = "c:@F@a#";
  le.syntax.kind = re.syntax.kind = "FunctionDecl";
  diff::SynNode a;
  a.kind = "IntegerLiteral";
  a.label = "3";
  a.detail = "literal 3";
  a.range = {2, 10, 2, 11};
  a.fingerprint = "cl";
  diff::SynNode b = a;
  b.label = "4";
  b.detail = "literal 4";
  b.fingerprint = "cr";
  le.syntax.children.push_back(std::move(a));
  re.syntax.children.push_back(std::move(b));
  le.syntax.fingerprint = "clroot";
  re.syntax.fingerprint = "crroot";
  left.entities.push_back(std::move(le));
  right.entities.push_back(std::move(re));
  const diff::Comparison cmp = diff::compare_sides(
      left, right, "heuristic", diff::ConfigDelta{}, "file");

  std::ostringstream huge;
  diff::render_report({"file", "syntax", "heuristic", 2147483647, false},
                      left, right, diff::ConfigDelta{}, cmp, huge);
  std::ostringstream sane;
  diff::render_report({"file", "syntax", "heuristic", 1000, false}, left,
                      right, diff::ConfigDelta{}, cmp, sane);
  CHECK(huge.str() == sane.str());
  CHECK(huge.str().find("L2 |   return 3;") != std::string::npos);
  CHECK(huge.str().find("R2 |   return 4;") != std::string::npos);
}

TEST_CASE("read-only open of a non-database file names the real failure") {
  const std::string dir = make_temp_dir();
  const std::string path = dir + "/garbage.db";
  {
    std::ofstream f(path);
    f << "this is not a sqlite database, not even close\n";
  }
  try {
    cidx::Storage bad(path, cidx::Storage::OpenMode::read_only);
    FAIL("expected the read-only open to throw");
  } catch (const cidx::CidxError &e) {
    // The real SQLite failure surfaces; not the "schema_version missing"
    // misreport (the probe SQL text in the message is fine).
    const std::string msg = e.what();
    CHECK(msg.find("not a database") != std::string::npos);
    CHECK(msg.find("schema_version missing") == std::string::npos);
    CHECK(msg.find("read-only open cannot migrate") == std::string::npos);
  }
}

} // TEST_SUITE("diff")

namespace {

void write_file(const std::string &path, const std::string &content) {
  std::ofstream f(path);
  REQUIRE(f.good());
  f << content;
}

int run_cidx_at(const std::string &cache, const std::string &db,
                cidx::Logger &log, const std::vector<std::string> &argv) {
  cidx::cli::ParsedArgs pa = cidx::cli::parse_args(argv);
  REQUIRE(!pa.help_text);
  std::ostringstream out;
  std::ostringstream err;
  cidx::cli::Context ctx;
  ctx.cache_dir = cache;
  ctx.index_path = db;
  ctx.logger = &log;
  ctx.out = &out;
  ctx.err = &err;
  return cidx::cli::run_command(pa, ctx);
}

// One shared indexed project holding every left/right fixture variant as its
// own TU (docs/diff.md fixture matrix, syntax half). Built once per process
// through the real CLI: import registers each file with its compile options,
// index proves the fixtures parse.
struct ClangFixture {
  std::string cache;
  std::string proj;
  std::string db;
  cidx::Logger log;

  ClangFixture() : cache(make_temp_dir()) {
    ::setenv("INDEXER_CACHE", cache.c_str(), 1);
    proj = cache + "/proj";
    db = cache + "/index.db";
    ::mkdir(proj.c_str(), 0755);

    const std::vector<std::pair<std::string, std::string>> sources = {
        {"fmt_left.cpp",
         "int add(int a, int b) { return a + b; }\n"
         "struct Box { int v; int twice() const { return v * 2; } };\n"},
        {"fmt_right.cpp",
         "int add(int a,\n"
         "        int b)\n"
         "{\n"
         "  return a + b;\n"
         "}\n"
         "struct Box {\n"
         "  int v;\n"
         "  int twice() const {\n"
         "    return v * 2;\n"
         "  }\n"
         "};\n"},
        {"cmt_left.cpp", "int mul(int a, int b) { return a * b; }\n"},
        {"cmt_right.cpp",
         "// multiply two ints\n"
         "int mul(int a, int b) { return a * b; /* product */ }\n"},
        {"call_left.cpp",
         "struct Vec {\n"
         "  void reserve(int n);\n"
         "  void resize(int n);\n"
         "};\n"
         "void fill(Vec &v, int n) { v.reserve(n); }\n"},
        {"call_right.cpp",
         "struct Vec {\n"
         "  void reserve(int n);\n"
         "  void resize(int n);\n"
         "};\n"
         "void fill(Vec &v, int n) { v.resize(n); }\n"},
        {"ret_left.cpp", "int answer() { return 3; }\n"},
        {"ret_right.cpp", "int answer() { return 4; }\n"},
        {"addrem_left.cpp",
         "int keep() { return 1; }\n"
         "int gone() { return 2; }\n"},
        {"addrem_right.cpp",
         "int keep() { return 1; }\n"
         "int fresh() { return 3; }\n"},
        {"ren_left.cpp",
         "int alpha(int x) { return x * 2; }\n"
         "int stable() { return 0; }\n"},
        {"ren_right.cpp",
         "int beta(int x) { return x * 2; }\n"
         "int stable() { return 0; }\n"},
        {"sel_left.cpp",
         "struct Cart {\n"
         "  int total() const { return 10; }\n"
         "  int total(int m) const { return 10 * m; }\n"
         "};\n"
         "int twice(int v) { return v + v; }\n"},
        {"sel_right.cpp",
         "struct Cart {\n"
         "  int total() const { return 11; }\n"
         "  int total(int m) const { return 10 * m; }\n"
         "};\n"
         "int twice(int v) { return v + v; }\n"},
        {"same_left.cpp", "int same(int a) { return a + 1; }\n"},
        {"same_right.cpp", "int same(int a) { return a + 1; }\n"},
        {"lren_left.cpp",
         "int calc(int a) { int tmp = a * 3; return tmp; }\n"},
        {"lren_right.cpp",
         "int calc(int a) { int val = a * 3; return val; }\n"},
        {"ltyp_left.cpp",
         "long calc2(int a) { int tmp = a; return tmp; }\n"},
        {"ltyp_right.cpp",
         "long calc2(int a) { long val = a; return val; }\n"},
        {"sig_left.cpp", "int scale(int f) { return f; }\n"},
        {"sig_right.cpp", "int scale(long f) { return f; }\n"},
        {"cls_left.cpp",
         "struct Widget {\n"
         "  int a;\n"
         "  int b;\n"
         "  int c;\n"
         "  void draw();\n"
         "};\n"},
        {"cls_right.cpp",
         "struct Widget {\n"
         "  int b;\n"
         "  int a;\n"
         "private:\n"
         "  int c;\n"
         "public:\n"
         "  virtual void draw();\n"
         "};\n"},
        {"vol_left.cpp", "int spin(volatile int *p) { return *p; }\n"},
        {"vol_right.cpp",
         "int spin(volatile int *p) { __asm__ volatile(\"nop\"); return *p; "
         "}\n"},
        {"cfg_left.cpp", "int flag() { return 7; }\n"},
        {"cfg_right.cpp", "int flag() { return 7; }\n"},
        {"hdrtu_left.cpp",
         "#include \"hdr_left.hpp\"\n"
         "int use() { return hval(1); }\n"},
        {"hdrtu_right.cpp",
         "#include \"hdr_right.hpp\"\n"
         "int use() { return hval(1); }\n"},
        {"frd_left.cpp",
         "struct S {\n"
         "  int v;\n"
         "  friend bool operator==(S a, S b) { return a.v == b.v; }\n"
         "};\n"},
        {"frd_right.cpp",
         "struct S {\n"
         "  int v;\n"
         "  friend bool operator==(S a, S b) { return a.v != b.v; }\n"
         "};\n"},
        {"mac_left.cpp", "#define BUF_SZ 512\n"},
        {"mac_right.cpp", "#define BUF_SZ 1024\n"},
        {"mac2_left.cpp", "#define BUF_SZ 512\n"},
        {"mac2_right.cpp", "#define BUF_SZ 512\n"},
        {"usg_left.cpp",
         "struct UB { void f(int); };\n"
         "struct UD : UB {\n"
         "  using UB::f;\n"
         "  void f(double);\n"
         "};\n"},
        {"usg_right.cpp",
         "struct UB { void f(int); };\n"
         "struct UD : UB {\n"
         "  void f(double);\n"
         "};\n"},
        {"def_left.cpp", "int scale2(int f = 1) { return f; }\n"},
        {"def_right.cpp", "int scale2(int f = 2) { return f; }\n"},
        {"sta_left.cpp", "int helper() { return 1; }\n"},
        {"sta_right.cpp", "static int helper() { return 1; }\n"},
        {"enm_left.cpp", "enum E { A, B };\n"},
        {"enm_right.cpp", "enum class E : long { A, B };\n"},
        {"env_left.cpp", "enum V { X = 1, Y };\n"},
        {"env_right.cpp", "enum V { X = 1, Y = 3 };\n"},
        {"aln_left.cpp", "struct AS { int x; };\n"},
        {"aln_right.cpp", "struct alignas(16) AS { int x; };\n"},
        {"ool_left.cpp",
         "struct Foo { int bar(); };\n"
         "int Foo::bar() { return 1; }\n"},
        {"ool_right.cpp",
         "struct Foo { int bar(); };\n"
         "int Foo::bar() { return 2; }\n"},
        {"dcl_left.cpp", "int ext(int);\n"},
        {"dcl_right.cpp", "int ext(int);\n"},
        {"vin_left.cpp",
         "void tick(volatile int *p) { (*p)++; }\n"
         "void bump(volatile int *p) { *p += 2; }\n"},
        {"vin_right.cpp",
         "void tick(volatile int *q) { (*q)++; }\n"
         "void bump(volatile int *q) { *q += 2; }\n"},
        {"vls_left.cpp",
         "int probe(volatile int *p) { (*p)++; return 1; }\n"},
        {"vls_right.cpp", "int probe(volatile int *p) { return 1; }\n"},
        {"anon_left.cpp",
         "struct Holder { struct { int x; } part; };\n"
         "struct { int x; } g_anon;\n"
         "int use_anon() { return g_anon.x; }\n"},
        {"anon_right.cpp",
         "struct Holder { struct { int x; } part; };\n"
         "struct { int x; } g_anon;\n"
         "int use_anon() { return g_anon.x; }\n"},
        // sizeof of a type: the operand rides as a type, not a child node.
        {"szt_left.cpp",
         "unsigned long tsz() { unsigned long n = sizeof(int); return n; }\n"},
        {"szt_right.cpp",
         "unsigned long tsz() { unsigned long n = sizeof(long); return n; }\n"},
        // public vs private, otherwise identical method (symbol scope).
        {"acc_left.cpp", "struct Api { int m() const { return 1; } };\n"},
        {"acc_right.cpp",
         "struct Api {\n"
         "private:\n"
         "  int m() const { return 1; }\n"
         "};\n"},
        // postfix vs prefix increment: same opcode, different fixity.
        {"fix_left.cpp", "void inc(int x) { x++; }\n"},
        {"fix_right.cpp", "void inc(int x) { ++x; }\n"},
        // identical source, reversed `-include a -include b` order: the two
        // forced headers leave a different final macro state, so the ordered
        // `other` options must register as a configuration delta even though
        // the callable itself is unchanged.
        {"cfgi_left.cpp", "int fi() { return 0; }\n"},
        {"cfgi_right.cpp", "int fi() { return 0; }\n"},
        // written `override` keyword removed: D::f still overrides B::f, so
        // size_overridden_methods() stays non-zero and only the written
        // OverrideAttr distinguishes the two in the syntax fingerprint.
        {"ovr_left.cpp", "struct B { virtual void f() {} };\n"
                         "struct D : B { void f() override {} };\n"},
        {"ovr_right.cpp", "struct B { virtual void f() {} };\n"
                          "struct D : B { void f() {} };\n"},
        // written `explicit(false)` vs a plain constructor: both are implicitly
        // convertible (isExplicit() is false for each), so only the written
        // explicit-specifier distinguishes them. Needs C++20 for explicit(bool).
        {"exp_left.cpp", "struct E { explicit(false) E(int) {} };\n"},
        {"exp_right.cpp", "struct E { E(int) {} };\n"},
        // `explicit(true)` vs a bare `explicit`: both resolve to explicit, so
        // getKind() collapses them -- only the written condition expression
        // distinguishes them in the syntax fingerprint.
        {"ex2_left.cpp", "struct V { explicit(true) V(int) {} };\n"},
        {"ex2_right.cpp", "struct V { explicit V(int) {} };\n"},
        // `explicit(0)` vs `explicit(false)`: both resolve to non-explicit;
        // getKind() maps each to ResolvedFalse, so only the preserved written
        // condition keeps them distinct.
        {"ex3_left.cpp", "struct W { explicit(0) W(int) {} };\n"},
        {"ex3_right.cpp", "struct W { explicit(false) W(int) {} };\n"},
    };
    // The headers are registered by `cidx index` through their TUs; they are
    // not compile_commands entries themselves.
    write_file(proj + "/hdr_left.hpp",
               "inline int hval(int x) { return x + 1; }\n");
    write_file(proj + "/hdr_right.hpp",
               "inline int hval(int x) { return x + 2; }\n");
    // Forced-include headers with an order-sensitive final macro value.
    write_file(proj + "/inc_a.hpp", "#undef IV\n#define IV 1\n");
    write_file(proj + "/inc_b.hpp", "#undef IV\n#define IV 2\n");
    std::string cc = "[";
    for (std::size_t i = 0; i < sources.size(); ++i) {
      write_file(proj + "/" + sources[i].first, sources[i].second);
      if (i != 0)
        cc += ",\n ";
      // The config-delta fixture pair has identical sources; only the left
      // side's stored compile options carry -DX=1. The cfgi pair shares one
      // source but stores reversed `-include` order.
      // -include is not an include-search flag, so import does not rewrite it
      // to an absolute path; use absolute header paths so the forced include
      // resolves regardless of the parse CWD.
      const std::string inc_a = "-include " + proj + "/inc_a.hpp";
      const std::string inc_b = "-include " + proj + "/inc_b.hpp";
      std::string extra;
      if (sources[i].first == "cfg_left.cpp")
        extra = "-DX=1 ";
      else if (sources[i].first == "cfgi_left.cpp")
        extra = inc_a + " " + inc_b + " ";
      else if (sources[i].first == "cfgi_right.cpp")
        extra = inc_b + " " + inc_a + " ";
      // explicit(bool) is C++20; the trailing -std overrides the -std=c++17 in
      // the base command (clang takes the last -std).
      else if (sources[i].first == "exp_left.cpp" ||
               sources[i].first == "exp_right.cpp" ||
               sources[i].first == "ex2_left.cpp" ||
               sources[i].first == "ex2_right.cpp" ||
               sources[i].first == "ex3_left.cpp" ||
               sources[i].first == "ex3_right.cpp")
        extra = "-std=c++20 ";
      cc += "{\"directory\": \"" + proj +
            "\", \"command\": \"c++ -std=c++17 " + extra + "-c " +
            sources[i].first + " -o " + sources[i].first +
            ".o\", \"file\": \"" + sources[i].first + "\"}";
    }
    cc += "]\n";
    write_file(proj + "/compile_commands.json", cc);
    log.set_file(cache + "/cidx.log");
    REQUIRE(run_cidx({"import", "--db", proj, "--name", "fixture"}) == 0);
    REQUIRE(run_cidx({"index"}) == 0);
  }

  int run_cidx(const std::vector<std::string> &argv) {
    return run_cidx_at(cache, db, log, argv);
  }

  std::string path(const std::string &name) const { return proj + "/" + name; }
};

ClangFixture &fx() {
  static ClangFixture f;
  return f;
}

// file-scope diff of one left/right fixture pair, with the shared index.
int diff_pair(const std::string &left, const std::string &right,
              std::vector<std::string> extra, std::string *out,
              std::string *err) {
  std::vector<std::string> argv = {"file", fx().path(left), fx().path(right),
                                   "--db", fx().db};
  argv.insert(argv.end(), extra.begin(), extra.end());
  return run_diff(argv, out, err);
}

int diff_symbol(const std::string &sel_l, const std::string &sel_r,
                std::vector<std::string> extra, std::string *out,
                std::string *err) {
  std::vector<std::string> argv = {"symbol",  fx().path("sel_left.cpp"),
                                   fx().path("sel_right.cpp"), "--db",
                                   fx().db,   "--left",
                                   sel_l,     "--right",
                                   sel_r};
  argv.insert(argv.end(), extra.begin(), extra.end());
  return run_diff(argv, out, err);
}

} // namespace

TEST_SUITE("clang") {

TEST_CASE("formatting-only edit: syntax unchanged") {
  std::string out;
  std::string err;
  const int rc = diff_pair("fmt_left.cpp", "fmt_right.cpp", {}, &out, &err);
  CHECK(rc == 0);
  CHECK(err.empty());
  CHECK(out.find("syntax: unchanged") != std::string::npos);
  CHECK(out.find("config: identical") != std::string::npos);

  // Whole file all-equivalent: formatting differs, so the source slices are
  // not byte-equal -- evidence is normalized-ir, never
  // identical-source-and-config.
  CHECK(out.find("semantic: equivalent (all entities equivalent)") !=
        std::string::npos);
  CHECK(out.find("assumptions: same-standard-library no-undefined-behavior") !=
        std::string::npos);

  std::string json;
  CHECK(diff_pair("fmt_left.cpp", "fmt_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"status\": \"unchanged\"") != std::string::npos);
  CHECK(json.find("\"edit_count\": 0") != std::string::npos);
  CHECK(json.find("\"status\": \"changed\"") == std::string::npos);
  CHECK(json.find("\"verdict\": \"equivalent\"") != std::string::npos);
  CHECK(json.find("\"evidence\": \"normalized-ir\"") != std::string::npos);
  CHECK(json.find("\"evidence\": \"identical-source-and-config\"") ==
        std::string::npos);
  CHECK(json.find("\"same-standard-library\"") != std::string::npos);
  CHECK(json.find("\"no-undefined-behavior\"") != std::string::npos);
  CHECK(json.find("\"unsupported_count\": 0") != std::string::npos);
}

TEST_CASE("comment-only edit: syntax unchanged, semantic equivalent") {
  std::string out;
  std::string err;
  const int rc = diff_pair("cmt_left.cpp", "cmt_right.cpp", {}, &out, &err);
  CHECK(rc == 0);
  CHECK(out.find("syntax: unchanged") != std::string::npos);
  CHECK(out.find("semantic: equivalent") != std::string::npos);
}

TEST_CASE("byte-identical sources: identical-source-and-config evidence") {
  std::string json;
  std::string err;
  CHECK(diff_pair("same_left.cpp", "same_right.cpp", {"--json"}, &json,
                  &err) == 0);
  CHECK(json.find("\"verdict\": \"equivalent\"") != std::string::npos);
  CHECK(json.find("\"evidence\": \"identical-source-and-config\"") !=
        std::string::npos);

  // --mode semantic nulls the syntax blocks but keeps the semantic ones.
  std::string sem;
  CHECK(diff_pair("same_left.cpp", "same_right.cpp",
                  {"--json", "--mode", "semantic"}, &sem, &err) == 0);
  CHECK(sem.find("\"syntax\": null") != std::string::npos);
  CHECK(sem.find("\"verdict\": \"equivalent\"") != std::string::npos);
}

TEST_CASE("local rename: semantic equivalent via normalized IR") {
  std::string json;
  std::string err;
  CHECK(diff_pair("lren_left.cpp", "lren_right.cpp", {"--json"}, &json,
                  &err) == 0);
  CHECK(json.find("\"op\": \"renamed\"") != std::string::npos);
  CHECK(json.find("\"verdict\": \"equivalent\"") != std::string::npos);
  CHECK(json.find("\"evidence\": \"normalized-ir\"") != std::string::npos);
  CHECK(json.find("\"verdict\": \"unknown\"") == std::string::npos);
}

TEST_CASE("local rename with type change is not equivalent") {
  std::string json;
  std::string err;
  CHECK(diff_pair("ltyp_left.cpp", "ltyp_right.cpp", {"--json"}, &json,
                  &err) == 0);
  CHECK(json.find("\"verdict\": \"equivalent\"") == std::string::npos);
  CHECK(json.find("\"verdict\": \"unknown\"") != std::string::npos);
  CHECK(json.find("\"evidence\": \"unsupported-or-incomplete\"") !=
        std::string::npos);
}

TEST_CASE("signature param-type change: different via summary contradiction") {
  std::string json;
  std::string err;
  CHECK(diff_pair("sig_left.cpp", "sig_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"verdict\": \"different\"") != std::string::npos);
  CHECK(json.find("\"evidence\": \"summary-contradiction\"") !=
        std::string::npos);
  CHECK(json.find("\"parameter 1 type: int -> long\"") != std::string::npos);

  std::string out;
  CHECK(diff_pair("sig_left.cpp", "sig_right.cpp", {}, &out, &err) == 0);
  CHECK(out.find("semantic: different") != std::string::npos);
  CHECK(out.find("change: parameter 1 type: int -> long") !=
        std::string::npos);
}

TEST_CASE("class profile contradiction names every change") {
  std::string out;
  std::string err;
  const int rc = run_diff({"symbol", fx().path("cls_left.cpp"),
                           fx().path("cls_right.cpp"), "--db", fx().db,
                           "--left", "Widget", "--right", "Widget"},
                          &out, &err);
  CHECK(rc == 0);
  CHECK(out.find("semantic: different (class profile contradiction)") !=
        std::string::npos);
  CHECK(out.find("change: field reordered") != std::string::npos);
  CHECK(out.find("change: field removed: public c : int") !=
        std::string::npos);
  CHECK(out.find("change: field added: private c : int") !=
        std::string::npos);
  CHECK(out.find("change: method removed: public void draw()") !=
        std::string::npos);
  CHECK(out.find("change: method added: public void draw() virtual") !=
        std::string::npos);
  CHECK(out.find("change: polymorphic: false -> true") != std::string::npos);
  // Class target: the member rows ride along, judged individually.
  CHECK(out.find("method Widget::draw  different (signature contradiction)") !=
        std::string::npos);
  CHECK(out.find("change: virtual: false -> true") != std::string::npos);
}

TEST_CASE("sizeof(int) vs sizeof(long): not falsely equivalent") {
  // The type operand is not a child node; before it was encoded, both bodies
  // lowered to identical syntax and IR and reported equivalent.
  std::string json;
  std::string err;
  CHECK(diff_pair("szt_left.cpp", "szt_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"status\": \"changed\"") != std::string::npos);
  CHECK(json.find("\"verdict\": \"equivalent\"") == std::string::npos);
  CHECK(json.find("\"verdict\": \"unknown\"") != std::string::npos);
}

TEST_CASE("method access change (public -> private): different") {
  // Selected directly, the access specifier is outside the method extent;
  // it must still be part of the callable's compared API state -- in both the
  // semantic IR and the syntax fingerprint.
  std::string out;
  std::string err;
  const int rc = run_diff({"symbol", fx().path("acc_left.cpp"),
                           fx().path("acc_right.cpp"), "--db", fx().db,
                           "--left", "Api::m", "--right", "Api::m"},
                          &out, &err);
  CHECK(rc == 0);
  CHECK(out.find("semantic: different") != std::string::npos);
  CHECK(out.find("change: access: public -> private") != std::string::npos);

  // Syntax mode must also see the API-state change: status changed, edits > 0.
  std::string json;
  const int rcj = run_diff({"symbol", fx().path("acc_left.cpp"),
                            fx().path("acc_right.cpp"), "--db", fx().db,
                            "--left", "Api::m", "--right", "Api::m", "--mode",
                            "syntax", "--json"},
                           &json, &err);
  CHECK(rcj == 0);
  CHECK(json.find("\"status\": \"changed\"") != std::string::npos);
  CHECK(json.find("\"edit_count\": 0") == std::string::npos);
}

TEST_CASE("removed override keyword: syntax changed") {
  // D::f overrides B::f in both revisions, so size_overridden_methods() stays
  // non-zero; only the written `override` keyword (OverrideAttr) differs. The
  // syntax fingerprint must still register the edit.
  std::string json;
  std::string err;
  const int rc = run_diff({"symbol", fx().path("ovr_left.cpp"),
                           fx().path("ovr_right.cpp"), "--db", fx().db, "--left",
                           "D::f", "--right", "D::f", "--mode", "syntax",
                           "--json"},
                          &json, &err);
  CHECK(rc == 0);
  CHECK(json.find("\"status\": \"changed\"") != std::string::npos);
  CHECK(json.find("\"edit_count\": 0") == std::string::npos);
}

TEST_CASE("explicit(false) vs plain constructor: syntax changed") {
  // Both constructors are implicitly convertible -- isExplicit() is false for
  // each -- so only the written explicit-specifier distinguishes them. The
  // syntax fingerprint must preserve the written explicit(false).
  std::string json;
  std::string err;
  const int rc = run_diff({"symbol", fx().path("exp_left.cpp"),
                           fx().path("exp_right.cpp"), "--db", fx().db, "--left",
                           "E::E", "--right", "E::E", "--mode", "syntax",
                           "--json"},
                          &json, &err);
  CHECK(rc == 0);
  CHECK(json.find("\"status\": \"changed\"") != std::string::npos);
  CHECK(json.find("\"edit_count\": 0") == std::string::npos);
}

TEST_CASE("explicit(true) vs bare explicit: syntax changed") {
  // Both resolve to explicit (getKind() == ResolvedTrue), so a kind-only token
  // collapses them; only the preserved written condition distinguishes the two.
  std::string json;
  std::string err;
  const int rc = run_diff({"symbol", fx().path("ex2_left.cpp"),
                           fx().path("ex2_right.cpp"), "--db", fx().db, "--left",
                           "V::V", "--right", "V::V", "--mode", "syntax",
                           "--json"},
                          &json, &err);
  CHECK(rc == 0);
  CHECK(json.find("\"status\": \"changed\"") != std::string::npos);
  CHECK(json.find("\"edit_count\": 0") == std::string::npos);
}

TEST_CASE("explicit(0) vs explicit(false): syntax changed") {
  // Both resolve to non-explicit (getKind() == ResolvedFalse); only the
  // preserved written condition keeps `0` distinct from `false`.
  std::string json;
  std::string err;
  const int rc = run_diff({"symbol", fx().path("ex3_left.cpp"),
                           fx().path("ex3_right.cpp"), "--db", fx().db, "--left",
                           "W::W", "--right", "W::W", "--mode", "syntax",
                           "--json"},
                          &json, &err);
  CHECK(rc == 0);
  CHECK(json.find("\"status\": \"changed\"") != std::string::npos);
  CHECK(json.find("\"edit_count\": 0") == std::string::npos);
}

TEST_CASE("postfix vs prefix increment: syntax changed") {
  // x++ and ++x share an opcode string; only the fixity differs. Before, the
  // label dropped it and syntax mode reported unchanged.
  std::string json;
  std::string err;
  CHECK(diff_pair("fix_left.cpp", "fix_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"status\": \"changed\"") != std::string::npos);
  CHECK(json.find("\"edit_count\": 0") == std::string::npos);
}

TEST_CASE("volatile and inline asm are unsupported markers, not verdicts") {
  std::string json;
  std::string err;
  CHECK(diff_pair("vol_left.cpp", "vol_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"verdict\": \"unknown\"") != std::string::npos);
  CHECK(json.find("\"evidence\": \"unsupported-or-incomplete\"") !=
        std::string::npos);
  CHECK(json.find("\"what\": \"volatile access\"") != std::string::npos);
  CHECK(json.find("\"what\": \"inline assembly\"") != std::string::npos);
  CHECK(json.find("\"side\": \"right\"") != std::string::npos);
  CHECK(json.find("\"verdict\": \"equivalent\"") == std::string::npos);
}

TEST_CASE("config delta downgrades identical source to unknown") {
  std::string json;
  std::string err;
  CHECK(diff_pair("cfg_left.cpp", "cfg_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"identical\": false") != std::string::npos);
  CHECK(json.find("\"-DX=1\"") != std::string::npos);
  // The callable pair itself stays equivalent (normalized-ir), but the
  // whole-file verdict downgrades on the configuration delta.
  CHECK(json.find("\"verdict\": \"equivalent\"") != std::string::npos);
  CHECK(json.find("\"evidence\": \"identical-source-and-config\"") ==
        std::string::npos);
  CHECK(json.find(
            "\"detail\": \"configuration differs; no behavioral difference "
            "established\"") != std::string::npos);

  std::string out;
  CHECK(diff_pair("cfg_left.cpp", "cfg_right.cpp", {}, &out, &err) == 0);
  CHECK(out.find("config: different") != std::string::npos);
  CHECK(out.find("definitions removed: -DX=1") != std::string::npos);
  CHECK(out.find("semantic: unknown (configuration differs; no behavioral "
                 "difference established)") != std::string::npos);
}

TEST_CASE("reversed -include order is a config delta, not an identical config") {
  // Identical source, reversed `-include a -include b`: before the fix the
  // sorted `other` multiset matched and the whole-file verdict read as an
  // identical-source-and-config equivalence. It must now downgrade.
  std::string json;
  std::string err;
  CHECK(diff_pair("cfgi_left.cpp", "cfgi_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"identical\": false") != std::string::npos);
  CHECK(json.find("\"options_reordered\": true") != std::string::npos);
  CHECK(json.find("\"evidence\": \"identical-source-and-config\"") ==
        std::string::npos);

  std::string out;
  CHECK(diff_pair("cfgi_left.cpp", "cfgi_right.cpp", {}, &out, &err) == 0);
  CHECK(out.find("config: different") != std::string::npos);
  CHECK(out.find("options: reordered") != std::string::npos);
}

TEST_CASE("callee change reserve -> resize: one changed CallExpr op") {
  std::string out;
  std::string err;
  const int rc = diff_pair("call_left.cpp", "call_right.cpp", {}, &out, &err);
  CHECK(rc == 0);
  CHECK(out.find("syntax: changed (1 edit)") != std::string::npos);
  CHECK(out.find("function fill  matched (usr 100)") != std::string::npos);
  CHECK(out.find("changed  CXXMemberCallExpr  callee reserve -> resize  L") !=
        std::string::npos);
  // A callee change is a real IR difference but not a proven behavioral one.
  CHECK(out.find("semantic: unknown (no behavioral difference established)") !=
        std::string::npos);
  CHECK(out.find("function fill  unknown (normalized IR differs; no "
                 "behavioral difference established)") != std::string::npos);

  std::string json;
  CHECK(diff_pair("call_left.cpp", "call_right.cpp", {"--json"}, &json,
                  &err) == 0);
  CHECK(json.find("\"op\": \"changed\"") != std::string::npos);
  CHECK(json.find("\"node\": \"CXXMemberCallExpr\"") != std::string::npos);
  CHECK(json.find("\"detail\": \"callee reserve -> resize\"") !=
        std::string::npos);
  CHECK(json.find("\"edit_count\": 1") != std::string::npos);
}

TEST_CASE("return-value literal change: one changed IntegerLiteral op") {
  std::string out;
  std::string err;
  const int rc = diff_pair("ret_left.cpp", "ret_right.cpp", {}, &out, &err);
  CHECK(rc == 0);
  CHECK(out.find("syntax: changed (1 edit)") != std::string::npos);
  CHECK(out.find("changed  IntegerLiteral  literal 3 -> 4  L") !=
        std::string::npos);
}

TEST_CASE("added and removed functions at file scope") {
  std::string json;
  std::string err;
  const int rc = diff_pair("addrem_left.cpp", "addrem_right.cpp", {"--json"},
                           &json, &err);
  CHECK(rc == 0);
  CHECK(json.find("\"name\": \"fresh\"") != std::string::npos);
  CHECK(json.find("\"status\": \"added\"") != std::string::npos);
  CHECK(json.find("\"name\": \"gone\"") != std::string::npos);
  CHECK(json.find("\"status\": \"removed\"") != std::string::npos);
  CHECK(json.find("\"status\": \"matched\"") != std::string::npos); // keep
  CHECK(json.find("\"detail\": \"function fresh\"") != std::string::npos);
  CHECK(json.find("\"detail\": \"function gone\"") != std::string::npos);

  std::string out;
  CHECK(diff_pair("addrem_left.cpp", "addrem_right.cpp", {}, &out, &err) == 0);
  CHECK(out.find("function fresh  added") != std::string::npos);
  CHECK(out.find("function gone  removed") != std::string::npos);
}

TEST_CASE("rename: heuristic fingerprint pairs it, strict does not") {
  std::string json;
  std::string err;
  CHECK(diff_pair("ren_left.cpp", "ren_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"status\": \"renamed\"") != std::string::npos);
  CHECK(json.find("\"match\": \"fingerprint\"") != std::string::npos);
  CHECK(json.find("\"confidence\": 70") != std::string::npos);
  CHECK(json.find("\"op\": \"renamed\"") != std::string::npos);
  CHECK(json.find("\"detail\": \"name alpha -> beta\"") != std::string::npos);

  std::string strict;
  CHECK(diff_pair("ren_left.cpp", "ren_right.cpp",
                  {"--json", "--match", "strict"}, &strict, &err) == 0);
  CHECK(strict.find("\"status\": \"renamed\"") == std::string::npos);
  CHECK(strict.find("\"status\": \"added\"") != std::string::npos);
  CHECK(strict.find("\"status\": \"removed\"") != std::string::npos);
}

TEST_CASE("selector by unique qualified name") {
  std::string out;
  std::string err;
  const int rc = diff_symbol("twice", "twice", {}, &out, &err);
  CHECK(rc == 0);
  CHECK(out.find(" :: twice(int)") != std::string::npos);
  CHECK(out.find("syntax: unchanged") != std::string::npos);
}

TEST_CASE("selector by qualified signature among overloads") {
  std::string out;
  std::string err;
  const int rc = diff_symbol("Cart::total() const", "Cart::total() const", {},
                             &out, &err);
  CHECK(rc == 0);
  CHECK(out.find(" :: Cart::total() const") != std::string::npos);
  CHECK(out.find("syntax: changed (1 edit)") != std::string::npos);
  CHECK(out.find("changed  IntegerLiteral  literal 10 -> 11  L") !=
        std::string::npos);
}

TEST_CASE("ambiguous overloaded selector lists the candidates") {
  std::string out;
  std::string err;
  const int rc = diff_symbol("Cart::total", "Cart::total", {}, &out, &err);
  CHECK(rc == 1);
  CHECK(err.find("ambiguous") != std::string::npos);
  CHECK(err.find(fx().path("sel_left.cpp")) != std::string::npos);
  // Every candidate line carries kind, qualified signature, USR and extent.
  CHECK(err.find("method Cart::total() const") != std::string::npos);
  CHECK(err.find("method Cart::total(int) const") != std::string::npos);
  CHECK(err.find("c:@S@Cart@F@total") != std::string::npos);
  CHECK(err.find(" L2:3-2:") != std::string::npos);
}

TEST_CASE("selector line:N picks the innermost entity") {
  std::string out;
  std::string err;
  const int rc = diff_symbol("line:2", "line:2", {}, &out, &err);
  CHECK(rc == 0);
  CHECK(out.find(" :: Cart::total() const") != std::string::npos);
  CHECK(out.find("literal 10 -> 11") != std::string::npos);
}

TEST_CASE("unmatched selector exits 1") {
  std::string out;
  std::string err;
  const int rc = diff_symbol("nosuch", "nosuch", {}, &out, &err);
  CHECK(rc == 1);
  CHECK(err.find("matches nothing") != std::string::npos);
}

TEST_CASE("separate --left-db and --right-db resolve both configs") {
  // The right side lives in a second, fully independent index.
  const std::string cache2 = make_temp_dir();
  const std::string proj2 = cache2 + "/proj";
  const std::string db2 = cache2 + "/index.db";
  ::mkdir(proj2.c_str(), 0755);
  write_file(proj2 + "/flag.cpp", "int flag() { return 8; }\n");
  write_file(proj2 + "/compile_commands.json",
             "[{\"directory\": \"" + proj2 +
                 "\", \"command\": \"c++ -std=c++20 -c flag.cpp -o flag.o\", "
                 "\"file\": \"flag.cpp\"}]\n");
  cidx::Logger log2;
  log2.set_file(cache2 + "/cidx.log");
  REQUIRE(run_cidx_at(cache2, db2, log2,
                      {"import", "--db", proj2, "--name", "fixture2"}) == 0);
  REQUIRE(run_cidx_at(cache2, db2, log2, {"index"}) == 0);

  std::string json;
  std::string err;
  const int rc = run_diff({"file", fx().path("cfg_right.cpp"),
                           proj2 + "/flag.cpp", "--left-db", fx().db,
                           "--right-db", db2, "--json"},
                          &json, &err);
  CHECK(rc == 0);
  CHECK(err.empty());
  CHECK(json.find("\"db\": \"" + fx().db + "\"") != std::string::npos);
  CHECK(json.find("\"db\": \"" + db2 + "\"") != std::string::npos);
  CHECK(json.find("\"std\": [\n      \"c++17\",\n      \"c++20\"\n    ]") !=
        std::string::npos);
  CHECK(json.find("\"detail\": \"literal 7 -> 8\"") != std::string::npos);
}

TEST_CASE("header comparison through --left-tu and --right-tu") {
  std::string json;
  std::string err;
  const int rc = run_diff(
      {"file", fx().path("hdr_left.hpp"), fx().path("hdr_right.hpp"), "--db",
       fx().db, "--left-tu", fx().path("hdrtu_left.cpp"), "--right-tu",
       fx().path("hdrtu_right.cpp"), "--json"},
      &json, &err);
  CHECK(rc == 0);
  CHECK(err.empty());
  CHECK(json.find("\"tu\": \"" + fx().path("hdrtu_left.cpp") + "\"") !=
        std::string::npos);
  CHECK(json.find("\"tu\": \"" + fx().path("hdrtu_right.cpp") + "\"") !=
        std::string::npos);
  CHECK(json.find("\"name\": \"hval\"") != std::string::npos);
  CHECK(json.find("\"detail\": \"literal 1 -> 2\"") != std::string::npos);
  // The diff scope is the header: the TU's own decls stay out of the report.
  CHECK(json.find("\"name\": \"use\"") == std::string::npos);
}

TEST_CASE("--context prints the affected source lines") {
  std::string out;
  std::string err;
  const int rc =
      diff_pair("ret_left.cpp", "ret_right.cpp", {"--context", "1"}, &out,
                &err);
  CHECK(rc == 0);
  CHECK(out.find("changed  IntegerLiteral  literal 3 -> 4  L") !=
        std::string::npos);
  CHECK(out.find("L1 | int answer() { return 3; }") != std::string::npos);
  CHECK(out.find("R1 | int answer() { return 4; }") != std::string::npos);

  std::string plain;
  CHECK(diff_pair("ret_left.cpp", "ret_right.cpp", {}, &plain, &err) == 0);
  CHECK(plain.find("L1 |") == std::string::npos);
}

TEST_CASE("reports are deterministic and the index stays byte-identical") {
  const std::string before = read_bytes(fx().db);
  std::string a;
  std::string b;
  std::string err;
  CHECK(diff_pair("call_left.cpp", "call_right.cpp", {"--json"}, &a, &err) ==
        0);
  CHECK(diff_pair("call_left.cpp", "call_right.cpp", {"--json"}, &b, &err) ==
        0);
  CHECK(a == b);
  CHECK(!a.empty());
  CHECK(a.front() == '{');
  CHECK(a.back() == '\n');
  CHECK(a.find("\"tool\": \"cidx-diff\"") != std::string::npos);
  CHECK(a.find("\"report_version\": 1") != std::string::npos);
  CHECK(a.find("\"verdict\": \"unknown\"") != std::string::npos);
  CHECK(a.find("\"assumptions\"") != std::string::npos);

  std::string t1;
  std::string t2;
  CHECK(diff_pair("call_left.cpp", "call_right.cpp", {}, &t1, &err) == 0);
  CHECK(diff_pair("call_left.cpp", "call_right.cpp", {}, &t2, &err) == 0);
  CHECK(t1 == t2);
  CHECK(read_bytes(fx().db) == before);
}

TEST_CASE("hidden friend body change is visible: syntax changed, semantic "
          "not equivalent") {
  std::string json;
  std::string err;
  CHECK(diff_pair("frd_left.cpp", "frd_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"status\": \"changed\"") != std::string::npos);
  CHECK(json.find("\"verdict\": \"equivalent\"") == std::string::npos);
  CHECK(json.find("\"verdict\": \"unknown\"") != std::string::npos);

  std::string out;
  CHECK(diff_pair("frd_left.cpp", "frd_right.cpp", {}, &out, &err) == 0);
  CHECK(out.find("syntax: changed") != std::string::npos);
  CHECK(out.find("semantic: equivalent") == std::string::npos);
}

TEST_CASE("zero-entity pair: differing bytes are unknown, identical bytes "
          "equivalent") {
  std::string json;
  std::string err;
  CHECK(diff_pair("mac_left.cpp", "mac_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"verdict\": \"unknown\"") != std::string::npos);
  CHECK(json.find("\"detail\": \"no indexed entities to compare\"") !=
        std::string::npos);
  CHECK(json.find("\"evidence\": \"unsupported-or-incomplete\"") !=
        std::string::npos);
  CHECK(json.find("identical-source-and-config") == std::string::npos);

  std::string same;
  CHECK(diff_pair("mac2_left.cpp", "mac2_right.cpp", {"--json"}, &same,
                  &err) == 0);
  CHECK(same.find("\"verdict\": \"equivalent\"") != std::string::npos);
  CHECK(same.find("\"evidence\": \"identical-source-and-config\"") !=
        std::string::npos);
}

TEST_CASE("removing a class-scope using-declaration contradicts the "
          "profile") {
  std::string out;
  std::string err;
  const int rc = run_diff({"symbol", fx().path("usg_left.cpp"),
                           fx().path("usg_right.cpp"), "--db", fx().db,
                           "--left", "UD", "--right", "UD"},
                          &out, &err);
  CHECK(rc == 0);
  CHECK(out.find("semantic: different (class profile contradiction)") !=
        std::string::npos);
  CHECK(out.find("change: declaration removed: using UB::f") !=
        std::string::npos);
}

TEST_CASE("default-argument change on a matched callable is different") {
  std::string json;
  std::string err;
  CHECK(diff_pair("def_left.cpp", "def_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"verdict\": \"different\"") != std::string::npos);
  CHECK(json.find("\"evidence\": \"summary-contradiction\"") !=
        std::string::npos);
  CHECK(json.find("\"default argument of parameter 1 changed\"") !=
        std::string::npos);
}

TEST_CASE("adding static to a file-scope function is different") {
  std::string json;
  std::string err;
  CHECK(diff_pair("sta_left.cpp", "sta_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"verdict\": \"different\"") != std::string::npos);
  CHECK(json.find("\"storage class: - -> static\"") != std::string::npos);
}

TEST_CASE("enum scoped-ness, underlying type and enumerator values are "
          "compared") {
  std::string json;
  std::string err;
  CHECK(diff_pair("enm_left.cpp", "enm_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"status\": \"changed\"") != std::string::npos);
  CHECK(json.find("\"verdict\": \"equivalent\"") == std::string::npos);

  std::string shift;
  CHECK(diff_pair("env_left.cpp", "env_right.cpp", {"--json"}, &shift,
                  &err) == 0);
  CHECK(shift.find("\"status\": \"changed\"") != std::string::npos);
  CHECK(shift.find("\"verdict\": \"equivalent\"") == std::string::npos);
}

TEST_CASE("alignas changes the record layout row: class different") {
  std::string out;
  std::string err;
  const int rc = run_diff({"symbol", fx().path("aln_left.cpp"),
                           fx().path("aln_right.cpp"), "--db", fx().db,
                           "--left", "AS", "--right", "AS"},
                          &out, &err);
  CHECK(rc == 0);
  CHECK(out.find("semantic: different (class profile contradiction)") !=
        std::string::npos);
  CHECK(out.find("declaration removed: layout size:4 align:4") !=
        std::string::npos);
  CHECK(out.find("declaration added: layout size:16 align:16") !=
        std::string::npos);
}

TEST_CASE("parse failure exits 1 with the first diagnostic, no report") {
  const std::string cache3 = make_temp_dir();
  const std::string proj3 = cache3 + "/proj";
  const std::string db3 = cache3 + "/index.db";
  ::mkdir(proj3.c_str(), 0755);
  write_file(proj3 + "/bad.cpp",
             "#include \"gone.hpp\"\nint f() { return 0; }\n");
  write_file(proj3 + "/compile_commands.json",
             "[{\"directory\": \"" + proj3 +
                 "\", \"command\": \"c++ -std=c++17 -c bad.cpp -o bad.o\", "
                 "\"file\": \"bad.cpp\"}]\n");
  cidx::Logger log3;
  log3.set_file(cache3 + "/cidx.log");
  REQUIRE(run_cidx_at(cache3, db3, log3,
                      {"import", "--db", proj3, "--name", "fixture3"}) == 0);

  std::string out;
  std::string err;
  const int rc = run_diff(
      {"file", proj3 + "/bad.cpp", proj3 + "/bad.cpp", "--db", db3}, &out,
      &err);
  CHECK(rc == 1);
  CHECK(out.empty());
  CHECK(err.find("error: cannot parse") != std::string::npos);
  CHECK(err.find("gone.hpp") != std::string::npos);
}

TEST_CASE("volatile increment and compound assignment are unsupported "
          "markers") {
  std::string json;
  std::string err;
  CHECK(diff_pair("vin_left.cpp", "vin_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"verdict\": \"unknown\"") != std::string::npos);
  CHECK(json.find("\"what\": \"volatile access\"") != std::string::npos);
  CHECK(json.find("\"verdict\": \"equivalent\"") == std::string::npos);
}

TEST_CASE("unsupported markers carry the side that owns them") {
  std::string json;
  std::string err;
  CHECK(diff_pair("vls_left.cpp", "vls_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"what\": \"volatile access\"") != std::string::npos);
  CHECK(json.find("\"side\": \"left\"") != std::string::npos);
  CHECK(json.find("\"side\": \"right\"") == std::string::npos);
}

TEST_CASE("out-of-line member bodies are bound and compared") {
  std::string out;
  std::string err;
  const int rc = run_diff({"symbol", fx().path("ool_left.cpp"),
                           fx().path("ool_right.cpp"), "--db", fx().db,
                           "--left", "Foo", "--right", "Foo"},
                          &out, &err);
  CHECK(rc == 0);
  CHECK(out.find("semantic: equivalent") == std::string::npos);
  CHECK(out.find("semantic: unknown") != std::string::npos);
}

TEST_CASE("matched declaration-only pair is equivalent with a transparent "
          "detail") {
  std::string json;
  std::string err;
  CHECK(diff_pair("dcl_left.cpp", "dcl_right.cpp", {"--json"}, &json, &err) ==
        0);
  CHECK(json.find("\"verdict\": \"equivalent\"") != std::string::npos);
  CHECK(json.find(
            "\"detail\": \"declaration only (no body in this translation "
            "unit)\"") != std::string::npos);
  CHECK(json.find("\"evidence\": \"identical-source-and-config\"") !=
        std::string::npos);
}

TEST_CASE("--match strict skips the name tier") {
  std::string json;
  std::string err;
  CHECK(diff_pair("sig_left.cpp", "sig_right.cpp",
                  {"--json", "--match", "strict"}, &json, &err) == 0);
  CHECK(json.find("\"match\": \"name\"") == std::string::npos);
  CHECK(json.find("\"status\": \"added\"") != std::string::npos);
  CHECK(json.find("\"status\": \"removed\"") != std::string::npos);
  CHECK(json.find("\"status\": \"matched\"") == std::string::npos);
}

TEST_CASE("identical unnamed types do not embed the side's file path") {
  std::string json;
  std::string err;
  CHECK(diff_pair("anon_left.cpp", "anon_right.cpp", {"--json"}, &json,
                  &err) == 0);
  CHECK(json.find("\"verdict\": \"equivalent\"") != std::string::npos);
  CHECK(json.find("\"verdict\": \"different\"") == std::string::npos);
  CHECK(json.find("\"verdict\": \"unknown\"") == std::string::npos);
  CHECK(json.find("\"status\": \"changed\"") == std::string::npos);
  CHECK(json.find("anon_left.cpp:") == std::string::npos);
}

TEST_CASE("selector tier 1: exact USR wins") {
  std::string json;
  std::string err;
  REQUIRE(diff_symbol("Cart::total() const", "Cart::total() const",
                      {"--json"}, &json, &err) == 0);
  const std::string key = "\"left_usr\": \"";
  const std::size_t at = json.find(key);
  REQUIRE(at != std::string::npos);
  const std::size_t begin = at + key.size();
  const std::string usr = json.substr(begin, json.find('"', begin) - begin);
  REQUIRE(!usr.empty());

  std::string out;
  const int rc = diff_symbol(usr, usr, {}, &out, &err);
  CHECK(rc == 0);
  CHECK(out.find(" :: Cart::total() const") != std::string::npos);
  CHECK(out.find("literal 10 -> 11") != std::string::npos);
}

TEST_CASE("without --db the index resolves from INDEXER_CACHE") {
  ::setenv("INDEXER_CACHE", fx().cache.c_str(), 1);
  std::string out;
  std::string err;
  const int rc = run_diff(
      {"file", fx().path("ret_left.cpp"), fx().path("ret_right.cpp")}, &out,
      &err);
  CHECK(rc == 0);
  CHECK(out.find("literal 3 -> 4") != std::string::npos);

  const std::string empty_cache = make_temp_dir();
  ::setenv("INDEXER_CACHE", empty_cache.c_str(), 1);
  err.clear();
  const int rc2 = run_diff(
      {"file", fx().path("ret_left.cpp"), fx().path("ret_right.cpp")},
      nullptr, &err);
  CHECK(rc2 == 1);
  CHECK(err.find(empty_cache + "/index.db") != std::string::npos);
  ::setenv("INDEXER_CACHE", fx().cache.c_str(), 1);
}

} // TEST_SUITE("clang")

int main(int argc, char **argv) {
  doctest::Context ctx(argc, argv);
  return ctx.run();
}
