// index_golden_test — the repository-wide indexing oracle that replaced the
// retired byte-identical Python(libclang)-vs-C++ parity gate (S08). It indexes a
// small, self-contained fixture (a two-TU C project + one C++ template TU) with
// the LibTooling engine and compares NORMALIZED, ORDERED projections of the
// seven Layer-0 tables — file, symbol, decl_site, edge, edge_site, call_arg,
// template_arg — against a checked-in golden (tests/golden/index_layer0.txt).
//
// "Normalized, ordered" means: every projection resolves surrogate keys
// (symbol.id / edge.id / file.id) to stable semantic keys (USR, edge-kind name,
// basename) and ORDERs by them, and the temp fixture path is rewritten to
// {CACHE}. So the golden is invariant to AST-traversal insertion order and to
// where the fixture happens to live on disk, but pins the SEMANTIC content the
// indexer emits. Any drift in symbols/edges/sites/args is a diff.
//
// This is a real parse, so it lives in doctest suite "clang" (ctest label
// "clang"), excluded from the hermetic default gate.
//
// Regenerating the golden after an intentional change (or a Clang major bump,
// whose AST can legitimately shift): run the exe with CIDX_UPDATE_GOLDEN=1 (or
// `ctest -L clang` with it in the environment) to overwrite the golden, then
// review the diff before committing.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"
#include "util/logger.hpp"

using cidx::SqliteDb;
using cidx::SqliteStmt;
using cidx::Storage;
namespace cli = cidx::cli;

namespace {

std::string make_temp_dir() {
  char tmpl[] = "/tmp/cidx_golden_XXXXXX";
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

bool read_file(const std::string &path, std::string &out) {
  std::ifstream f(path);
  if (!f.good()) {
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

std::string replace_all(std::string text, const std::string &from,
                        const std::string &to) {
  if (from.empty()) {
    return text;
  }
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
  return text;
}

// Run one cidx invocation against `cache` exactly like the real CLI (parse +
// dispatch). Returns the exit code; stdout/stderr are captured and discarded
// (the assertions read the DB, not the transcript).
int run_cidx(const std::vector<std::string> &argv, const std::string &cache,
             cidx::Logger &log) {
  cli::ParsedArgs pa = cli::parse_args(argv);
  REQUIRE(!pa.help_text);
  std::ostringstream out;
  std::ostringstream err;
  cli::Context ctx;
  ctx.cache_dir = cache;
  ctx.index_path = cache + "/index.db";
  ctx.logger = &log;
  ctx.out = &out;
  ctx.err = &err;
  return cli::run_command(pa, ctx);
}

// One Layer-0 projection: a "== <name> ==" header followed by one tab-joined
// line per row, in the ORDER BY of `sql`. `ncols` columns are read as text.
std::string dump(SqliteDb &db, const char *name, const char *sql, int ncols) {
  std::string out = std::string("== ") + name + " ==\n";
  SqliteStmt st = db.prepare(sql);
  while (st.step()) {
    for (int i = 0; i < ncols; ++i) {
      if (i != 0) {
        out += '\t';
      }
      out += st.col_text(i);
    }
    out += '\n';
  }
  return out;
}

// Normalized, ordered projection of the Layer-0 tables (the original seven
// plus the v30 type_node/type_edge/parameter/symbol_type tier). Every surrogate
// key is resolved to a stable semantic key and every row set is ORDERed so the
// output is independent of AST-traversal insertion order.
std::string project_layer0(const std::string &index_path) {
  Storage store(index_path);
  SqliteDb &db = store.raw_db();
  std::string out;

  out += dump(db, "file",
              "SELECT f.name, COALESCE(f.compile_options,''), "
              "COALESCE(f.driver,''), f.indexed, COALESCE(f.md5,'') "
              "FROM file f ORDER BY f.name",
              5);

  out += dump(db, "symbol",
              "SELECT s.usr, s.spelling, COALESCE(s.qual_name,''), "
              "COALESCE(s.display_name,''), "
              "COALESCE(sk.name, CAST(s.kind AS TEXT)), "
              "COALESCE(s.type_info,''), COALESCE(ff.name,''), "
              "COALESCE(s.line,''), COALESCE(s.col,''), "
              "COALESCE(df.name,''), COALESCE(s.decl_line,''), "
              "COALESCE(s.decl_col,''), s.is_definition, s.is_pure, "
              "s.is_static, s.is_instantiation, COALESCE(s.linkage,''), "
              "COALESCE(s.access,''), COALESCE(s.parent_usr,''), s.resolved "
              "FROM symbol s "
              "LEFT JOIN symbol_kind sk ON sk.id = s.kind "
              "LEFT JOIN file ff ON ff.id = s.file_id "
              "LEFT JOIN file df ON df.id = s.decl_file_id "
              "ORDER BY s.usr",
              20);

  out += dump(db, "decl_site",
              "SELECT s.usr, COALESCE(f.name,''), COALESCE(d.line,''), "
              "COALESCE(d.col,''), d.is_definition "
              "FROM decl_site d JOIN symbol s ON s.id = d.symbol_id "
              "LEFT JOIN file f ON f.id = d.file_id "
              "ORDER BY s.usr, f.name, d.line, d.col",
              5);

  out += dump(db, "edge",
              "SELECT ss.usr, ds.usr, COALESCE(ek.name, CAST(e.kind AS TEXT)), "
              "e.count, COALESCE(e.base_access,''), COALESCE(e.is_virtual,'') "
              "FROM edge e JOIN symbol ss ON ss.id = e.src_id "
              "JOIN symbol ds ON ds.id = e.dst_id "
              "LEFT JOIN edge_kind ek ON ek.id = e.kind "
              "ORDER BY ss.usr, ds.usr, e.kind",
              6);

  out += dump(db, "edge_site",
              "SELECT ss.usr, ds.usr, COALESCE(ek.name, CAST(e.kind AS TEXT)), "
              "COALESCE(f.name,''), COALESCE(es.line,''), "
              "COALESCE(es.col,''), es.conditional, COALESCE(es.args_sig,''), "
              "COALESCE(es.recv_src_kind,''), COALESCE(es.recv_type_usr,''), "
              "COALESCE(es.recv_decl_usr,''), COALESCE(es.recv_param_pos,''), "
              "COALESCE(es.recv_type_is_value,'') "
              "FROM edge_site_read es JOIN edge e ON e.id = es.edge_id "
              "JOIN symbol ss ON ss.id = e.src_id "
              "JOIN symbol ds ON ds.id = e.dst_id "
              "LEFT JOIN edge_kind ek ON ek.id = e.kind "
              "LEFT JOIN file f ON f.id = es.file_id "
              "ORDER BY ss.usr, ds.usr, e.kind, f.name, es.line, es.col",
              13);

  out += dump(db, "call_arg",
              "SELECT ss.usr, ds.usr, COALESCE(ek.name, CAST(e.kind AS TEXT)), "
              "COALESCE(f.name,''), ca.line, ca.col, ca.position, ca.src_kind, "
              "COALESCE(ca.type_usr,''), COALESCE(ca.decl_usr,''), "
              "COALESCE(ca.callee_usr,''), COALESCE(ca.type_is_value,'') "
              "FROM call_arg_read ca JOIN edge e ON e.id = ca.edge_id "
              "JOIN symbol ss ON ss.id = e.src_id "
              "JOIN symbol ds ON ds.id = e.dst_id "
              "LEFT JOIN edge_kind ek ON ek.id = e.kind "
              "LEFT JOIN file f ON f.id = ca.file_id "
              "ORDER BY ss.usr, ds.usr, e.kind, f.name, ca.line, ca.col, "
              "ca.position",
              12);

  out += dump(db, "template_arg",
              "SELECT os.usr, ta.position, ta.arg_kind, COALESCE(rs.usr,''), "
              "COALESCE(ta.literal,'') "
              "FROM template_arg ta JOIN symbol os ON os.id = ta.owner_id "
              "LEFT JOIN symbol rs ON rs.id = ta.ref_id "
              "ORDER BY os.usr, ta.position",
              5);

  // v30 signature/type tier: type_node ids resolve to type_key (the stable
  // structural identity), symbols to USR, kinds to their seed names.
  out += dump(db, "type_node",
              "SELECT tn.type_key, tn.spelling, "
              "COALESCE(tk.name, CAST(tn.kind AS TEXT)), "
              "tn.is_const, tn.is_volatile, tn.is_restrict, "
              "COALESCE(tn.decl_usr,''), COALESCE(cn.type_key,'') "
              "FROM type_node tn LEFT JOIN type_kind tk ON tk.id = tn.kind "
              "LEFT JOIN type_node cn ON cn.id = tn.canonical_id "
              "ORDER BY tn.type_key",
              8);

  out += dump(db, "type_edge",
              "SELECT sn.type_key, "
              "COALESCE(tek.name, CAST(te.kind AS TEXT)), te.position, "
              "dn.type_key "
              "FROM type_edge te JOIN type_node sn ON sn.id = te.src_id "
              "JOIN type_node dn ON dn.id = te.dst_id "
              "LEFT JOIN type_edge_kind tek ON tek.id = te.kind "
              "ORDER BY sn.type_key, te.kind, te.position, dn.type_key",
              4);

  out += dump(db, "parameter",
              "SELECT s.usr, p.position, COALESCE(p.name,''), "
              "COALESCE(tn.type_key,''), COALESCE(f.name,''), "
              "COALESCE(p.line,''), COALESCE(p.col,'') "
              "FROM parameter p JOIN symbol s ON s.id = p.owner_id "
              "LEFT JOIN type_node tn ON tn.id = p.type_id "
              "LEFT JOIN file f ON f.id = p.file_id "
              "ORDER BY s.usr, p.position",
              7);

  out += dump(db, "symbol_type",
              "SELECT s.usr, COALESCE(stk.name, CAST(st.kind AS TEXT)), "
              "tn.type_key "
              "FROM symbol_type st JOIN symbol s ON s.id = st.symbol_id "
              "JOIN type_node tn ON tn.id = st.type_id "
              "LEFT JOIN symbol_type_kind stk ON stk.id = st.kind "
              "ORDER BY s.usr, st.kind",
              3);

  return out;
}

// Build the fixture project + its compile_commands.json under a fresh temp
// cache. Two C TUs (a header shared by the .c, exercising decl_site across
// files + a call with args) and one C++ TU (a template instantiation exercising
// template_arg + the instantiates edge). No system #includes, so the parse
// needs only the linked Clang's builtin headers.
struct Fixture {
  std::string cache; // temp root, doubles as INDEXER_CACHE
  std::string proj;  // <cache>/proj — component root
  Fixture() : cache(make_temp_dir()), proj(cache + "/proj") {
    makedirs(proj);
    write_file(proj + "/math.h", "#ifndef MATH_H\n"
                                 "#define MATH_H\n"
                                 "int add(int a, int b);\n"
                                 "int square(int x);\n"
                                 "#endif\n");
    write_file(proj + "/math.c", "#include \"math.h\"\n"
                                 "int add(int a, int b) { return a + b; }\n"
                                 "int square(int x) { return add(x, x); }\n");
    // A NAMED alias (typedef Box<int>) mints an is_named_instance symbol that
    // carries template_arg + an instantiates edge — so this TU exercises the
    // template_arg table (an unnamed Box<int> param would not mint an instance).
    write_file(proj + "/box.cpp",
               "template <typename T> struct Box { T item; };\n"
               "typedef Box<int> IntBox;\n"
               "int unbox(IntBox b) { return b.item; }\n");
    write_file(proj + "/compile_commands.json",
               "[\n  " + entry("math.c") + ",\n  " + entry("box.cpp") + "\n]\n");
  }
  std::string entry(const std::string &src) const {
    return "{\"directory\": \"" + proj + "\", \"command\": \"cc -I. -c " + src +
           " -o " + src + ".o\", \"file\": \"" + src + "\"}";
  }
};

} // namespace

TEST_SUITE("clang") {

  TEST_CASE("index golden: seven Layer-0 tables over a fixed fixture") {
    const Fixture fx;
    cidx::Logger log;
    log.set_file(fx.cache + "/cidx.log");

    // A stable component name keeps portable-path encodings (<name>/...) in
    // file.compile_options deterministic across runs.
    REQUIRE(run_cidx({"import", "--db", fx.proj, "--name", "fixture"}, fx.cache,
                     log) == 0);
    REQUIRE(run_cidx({"index"}, fx.cache, log) == 0);

    // Normalize the only run-varying token: the temp fixture path.
    std::string actual =
        replace_all(project_layer0(fx.cache + "/index.db"), fx.cache, "{CACHE}");

    const std::string golden_path = CIDX_GOLDEN_FILE;
    if (const char *upd = std::getenv("CIDX_UPDATE_GOLDEN");
        upd != nullptr && std::string(upd) == "1") {
      write_file(golden_path, actual);
      MESSAGE("CIDX_UPDATE_GOLDEN=1: wrote " << golden_path);
      return;
    }

    std::string golden;
    REQUIRE_MESSAGE(read_file(golden_path, golden),
                    "missing golden " << golden_path
                                      << " — regenerate with CIDX_UPDATE_GOLDEN=1");
    CHECK(actual == golden);
  }

} // TEST_SUITE("clang")

int main(int argc, char **argv) {
  doctest::Context ctx;
  ctx.applyCommandLine(argc, argv);
  return ctx.run();
}
