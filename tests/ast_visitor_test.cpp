// ast_visitor_test — focused fixtures for the src/ast visitor refactor
// (docs/improvements/refactoring.md, Phase 0). Each case indexes a tiny
// self-contained fixture through the real CLI (import + index, like
// index_golden_test) and asserts targeted rows via SQL, so the fixtures pin
// SEMANTIC behavior independent of traversal order.
//
// Two groups:
//   - pinning cases: current behavior that the refactor must preserve;
//   - correction cases, marked doctest::should_fail(): the contract the
//     refactor phases will make true. They assert the TARGET behavior, fail
//     today for the documented reason, and the decorator is removed by the
//     phase that lands the correction (should_fail turns the suite red the
//     moment the fix lands, forcing the decorator's removal).
//
// Correction cases and their phase:
//   - function/method-template local provenance     -> Phase 2
//     (classify_value_source treats locals in a template pattern as 'global',
//     bug-compatible with the retired libclang cursor kinds)
//   - receiver param_pos inside function templates  -> Phase 2
//     (receiver_provenance skips getFunctionScopeIndex on template parents)
//   - semantic unary operators must not be peeled   -> Phase 2
//     (unwrap_once strips every UnaryOperator; only & and * are provenance
//     preserving, !x / -x are derived values)
//   - one canonical template arg_kind mapping       -> Phase 3
//     (docs/improvements/template-arg-contract.md; the class-spec path stores
//     raw CXTemplateArgumentKind values 5/6/7/8 instead of 3/3/2/4)
//
// Real parse -> doctest suite "clang" (ctest label "clang").
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <sys/stat.h>

#include <algorithm>
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
  char tmpl[] = "/tmp/cidx_astvis_XXXXXX";
  char *d = ::mkdtemp(tmpl);
  REQUIRE(d != nullptr);
  return d;
}

void write_file(const std::string &path, const std::string &content) {
  std::ofstream f(path);
  REQUIRE(f.good());
  f << content;
}

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

// Index one C++ source as a single-TU component under a fresh temp cache and
// hand back the opened index. The fixture object owns the temp paths.
struct IndexedTu {
  std::string cache;
  cidx::Logger log;
  IndexedTu(const std::string &source) : cache(make_temp_dir()) {
    const std::string proj = cache + "/proj";
    ::mkdir(proj.c_str(), 0755);
    write_file(proj + "/tu.cpp", source);
    write_file(proj + "/compile_commands.json",
               "[{\"directory\": \"" + proj +
                   "\", \"command\": \"cc -c tu.cpp -o tu.o\", "
                   "\"file\": \"tu.cpp\"}]\n");
    log.set_file(cache + "/cidx.log");
    REQUIRE(run_cidx({"import", "--db", proj, "--name", "fixture"}, cache,
                     log) == 0);
    REQUIRE(run_cidx({"index"}, cache, log) == 0);
  }
  std::string db_path() const { return cache + "/index.db"; }
};

// All values of the first column of `sql`, sorted (deterministic compare).
std::vector<std::string> query_col(const std::string &db_path,
                                   const std::string &sql) {
  Storage store(db_path);
  SqliteStmt st = store.raw_db().prepare(sql);
  std::vector<std::string> out;
  while (st.step())
    out.push_back(st.col_text(0));
  std::sort(out.begin(), out.end());
  return out;
}

// call_arg src_kind / decl-USR emptiness for the position-0 argument of the
// unique call src->dst (by symbol spelling).
struct ArgProbe {
  std::string src_kind;
  bool has_decl_usr = false;
};
ArgProbe probe_arg0(const std::string &db_path, const std::string &src,
                    const std::string &dst) {
  const std::string sql =
      "SELECT ca.src_kind, COALESCE(ca.decl_usr,'') FROM call_arg ca "
      "JOIN edge e ON e.id = ca.edge_id "
      "JOIN symbol ss ON ss.id = e.src_id "
      "JOIN symbol ds ON ds.id = e.dst_id "
      "WHERE ss.spelling = '" +
      src + "' AND ds.spelling = '" + dst + "' AND ca.position = 0";
  Storage store(db_path);
  SqliteStmt st = store.raw_db().prepare(sql);
  ArgProbe p;
  int rows = 0;
  while (st.step()) {
    ++rows;
    p.src_kind = st.col_text(0);
    p.has_decl_usr = !std::string(st.col_text(1)).empty();
  }
  REQUIRE_MESSAGE(rows == 1, "expected exactly one arg row " << src << " -> "
                                                             << dst);
  return p;
}

const char *kProvenanceTu = R"cpp(
void sink(int);
void neg(int);
void lnot(bool);
void deref(int);
void addr(int *);

template <typename T> void ftlocal() {
  int x = 0;
  sink(x);
}
void use_ft() { ftlocal<int>(); }

struct W {
  template <typename T> void mtlocal() {
    int y = 0;
    sink(y);
  }
};

struct R {
  void go();
};
template <typename T> void call_via(R &r) { r.go(); }

void ordinary() {
  int z = 0;
  sink(z);
}

void statics() {
  static int s = 0;
  sink(s);
}

void lambdas() {
  auto lam = [] {
    int inlam = 0;
    sink(inlam);
  };
  lam();
}

void unary() {
  int x = 1;
  int *p = &x;
  bool f = false;
  neg(-x);
  lnot(!f);
  deref(*p);
  addr(&x);
}
)cpp";

const char *kTemplateArgsTu = R"cpp(
template <typename... Ts> struct PackS {};
template <> struct PackS<int, char> {};

template <template <typename> class C> struct TT {};
template <typename T> struct Vec {};
template <> struct TT<Vec> {};

template <int *P> struct PtrS {};
template <> struct PtrS<nullptr> {};

template <int N> struct NumS {};
template <> struct NumS<3> {};

template <typename... Ts> int count() { return 42; }
int use_count() { return count<int, char>(); }

struct Foo {};
template <typename T> struct Box {};
typedef Box<Foo *> FooPtrBox;
typedef Box<Foo &> FooRefBox;
)cpp";

const char *kConstructFormTu = R"cpp(
template <typename T> struct Holder {};

struct Taker {
  Taker(Holder<void(int &)> h);
};

struct Copyable {
  Copyable();
  Copyable(const Copyable &other);
  Copyable(Copyable &&other);
};

void use_taker() {
  Holder<void(int &)> h;
  Taker t(h);
}

void use_copyable() {
  Copyable a;
  Copyable b(a);
  Copyable c(static_cast<Copyable &&>(b));
}
)cpp";

// Construction-form edge kinds (10 construct-value / 11 construct-temp /
// 13 construct-copy / 14 construct-move) from `src` to the record `dst`.
std::vector<std::string> construct_forms(const std::string &db_path,
                                         const std::string &src,
                                         const std::string &dst) {
  return query_col(db_path,
                   "SELECT DISTINCT ek.name FROM edge e "
                   "JOIN symbol ss ON ss.id = e.src_id "
                   "JOIN symbol ds ON ds.id = e.dst_id "
                   "JOIN edge_kind ek ON ek.id = e.kind "
                   "WHERE ss.spelling = '" +
                       src + "' AND ds.spelling = '" + dst +
                       "' AND e.kind IN (10, 11, 13, 14)");
}

// arg_kinds recorded for every template_arg row owned by a symbol spelled
// `owner` (primaries carry template_param rows, not template_arg, so this
// selects the specialization's rows).
std::vector<std::string> arg_kinds_of(const std::string &db_path,
                                      const std::string &owner) {
  return query_col(db_path,
                   "SELECT DISTINCT ta.arg_kind FROM template_arg ta "
                   "JOIN symbol os ON os.id = ta.owner_id "
                   "WHERE os.spelling = '" +
                       owner + "'");
}

} // namespace

TEST_SUITE("clang") {

  // ---- provenance: pinned current-and-target behavior ----------------------

  TEST_CASE("provenance pin: ordinary function local is 'local'") {
    const IndexedTu tu(kProvenanceTu);
    const ArgProbe p = probe_arg0(tu.db_path(), "ordinary", "sink");
    CHECK(p.src_kind == "local");
    CHECK(p.has_decl_usr);
  }

  TEST_CASE("provenance pin: static local and lambda local are 'local'") {
    const IndexedTu tu(kProvenanceTu);
    CHECK(probe_arg0(tu.db_path(), "statics", "sink").src_kind == "local");
    // The lambda body's call is attributed to the enclosing function symbol.
    CHECK(query_col(tu.db_path(),
                    "SELECT DISTINCT ca.src_kind FROM call_arg ca "
                    "JOIN edge e ON e.id = ca.edge_id "
                    "JOIN symbol ss ON ss.id = e.src_id "
                    "JOIN symbol ds ON ds.id = e.dst_id "
                    "WHERE ss.spelling = 'lambdas' "
                    "AND ds.spelling = 'sink'") ==
          std::vector<std::string>{"local"});
  }

  TEST_CASE("provenance pin: & and * preserve the operand's provenance") {
    const IndexedTu tu(kProvenanceTu);
    const ArgProbe deref = probe_arg0(tu.db_path(), "unary", "deref");
    CHECK(deref.src_kind == "local");
    CHECK(deref.has_decl_usr);
    const ArgProbe addr = probe_arg0(tu.db_path(), "unary", "addr");
    CHECK(addr.src_kind == "local");
    CHECK(addr.has_decl_usr);
  }

  // ---- provenance: corrections landed in Phase 2 ---------------------------

  TEST_CASE("correction: function-template local is 'local'") {
    const IndexedTu tu(kProvenanceTu);
    const ArgProbe p = probe_arg0(tu.db_path(), "ftlocal", "sink");
    CHECK(p.src_kind == "local");
    CHECK(p.has_decl_usr);
  }

  TEST_CASE("correction: method-template local is 'local'") {
    const IndexedTu tu(kProvenanceTu);
    const ArgProbe p = probe_arg0(tu.db_path(), "mtlocal", "sink");
    CHECK(p.src_kind == "local");
    CHECK(p.has_decl_usr);
  }

  TEST_CASE("correction: receiver param_pos resolves in function templates") {
    const IndexedTu tu(kProvenanceTu);
    CHECK(query_col(tu.db_path(),
                    "SELECT COALESCE(es.recv_param_pos, '<null>') "
                    "FROM edge_site es "
                    "JOIN edge e ON e.id = es.edge_id "
                    "JOIN symbol ss ON ss.id = e.src_id "
                    "JOIN symbol ds ON ds.id = e.dst_id "
                    "WHERE ss.spelling = 'call_via' "
                    "AND ds.spelling = 'go'") ==
          std::vector<std::string>{"0"});
  }

  TEST_CASE("correction: !x and -x are derived values, not the operand") {
    const IndexedTu tu(kProvenanceTu);
    const ArgProbe negp = probe_arg0(tu.db_path(), "unary", "neg");
    CHECK(negp.src_kind == "unknown");
    CHECK(!negp.has_decl_usr);
    const ArgProbe lnotp = probe_arg0(tu.db_path(), "unary", "lnot");
    CHECK(lnotp.src_kind == "unknown");
    CHECK(!lnotp.has_decl_usr);
  }

  // ---- template arg_kind: pinned contract-conforming paths -----------------

  TEST_CASE("arg_kind pin: integral spec arg and callable pack arg") {
    const IndexedTu tu(kTemplateArgsTu);
    // NumS<3>: Integral -> 2 on the class-spec path (already in contract).
    CHECK(arg_kinds_of(tu.db_path(), "NumS") ==
          std::vector<std::string>{"2"});
    // count<int, char>: Pack -> 4 on the callable path (already in contract).
    CHECK(arg_kinds_of(tu.db_path(), "count") ==
          std::vector<std::string>{"4"});
  }

  // ---- template arg_kind: corrections landed in Phase 3 --------------------

  TEST_CASE("correction: class-spec pack arg uses contract kind 4") {
    const IndexedTu tu(kTemplateArgsTu);
    // PackS<int, char> stores raw CX kind 8 today.
    CHECK(arg_kinds_of(tu.db_path(), "PackS") ==
          std::vector<std::string>{"4"});
  }

  TEST_CASE("correction: pack arg_kind agrees across extraction paths") {
    const IndexedTu tu(kTemplateArgsTu);
    CHECK(arg_kinds_of(tu.db_path(), "PackS") ==
          arg_kinds_of(tu.db_path(), "count"));
  }

  TEST_CASE("correction: template-template spec arg uses contract kind 3") {
    const IndexedTu tu(kTemplateArgsTu);
    // TT<Vec> stores raw CX kind 5 today.
    CHECK(arg_kinds_of(tu.db_path(), "TT") == std::vector<std::string>{"3"});
  }

  TEST_CASE("correction: nullptr spec arg uses contract kind 2") {
    const IndexedTu tu(kTemplateArgsTu);
    // PtrS<nullptr> stores raw CX kind 3 (NullPtr) today, colliding with the
    // contract's template-template code.
    CHECK(arg_kinds_of(tu.db_path(), "PtrS") == std::vector<std::string>{"2"});
  }

  // ---- review 2026-07-14 blocking finding 1: pointer/reference ref_id ------

  TEST_CASE("review fix: pointer and reference type args keep their ref_id") {
    const IndexedTu tu(kTemplateArgsTu);
    // Box<Foo *> / Box<Foo &> instances: the type argument's underlying
    // record must resolve to Foo even through pointer/reference wrappers.
    CHECK(query_col(tu.db_path(),
                    "SELECT DISTINCT COALESCE(rs.usr, '<null>') "
                    "FROM template_arg ta "
                    "JOIN symbol os ON os.id = ta.owner_id "
                    "LEFT JOIN symbol rs ON rs.id = ta.ref_id "
                    "WHERE os.spelling = 'Box'") ==
          std::vector<std::string>{"c:@S@Foo"});
    // The literal stays the written spelling.
    CHECK(query_col(tu.db_path(),
                    "SELECT DISTINCT COALESCE(ta.literal, '') "
                    "FROM template_arg ta "
                    "JOIN symbol os ON os.id = ta.owner_id "
                    "WHERE os.spelling = 'Box'") ==
          (std::vector<std::string>{"Foo &", "Foo *"}));
  }

  // ---- review 2026-07-14 blocking finding 2: construction form -------------

  TEST_CASE("review fix: construction form uses the constructor category") {
    const IndexedTu tu(kConstructFormTu);
    // Taker's single-argument ctor takes Holder<void(int &)> BY VALUE: the
    // '&' inside the nested type spelling must not classify it copy.
    CHECK(construct_forms(tu.db_path(), "use_taker", "Taker") ==
          std::vector<std::string>{"construct-value"});
    // Real copy and move constructors still classify 13/14 (a's default
    // construction contributes the construct-value row).
    CHECK(construct_forms(tu.db_path(), "use_copyable", "Copyable") ==
          (std::vector<std::string>{"construct-copy", "construct-move",
                                    "construct-value"}));
  }

} // TEST_SUITE("clang")

int main(int argc, char **argv) {
  doctest::Context ctx;
  ctx.applyCommandLine(argc, argv);
  return ctx.run();
}
