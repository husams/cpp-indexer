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

void scast(int);
void ccast(int);
void rcast(long);

void named_casts() {
  int x = 1;
  const int cx = 2;
  scast(static_cast<int &>(x));
  ccast(const_cast<int &>(cx));
  rcast(reinterpret_cast<long &>(x));
}

void vcast(int);
void cvalue(long);

void value_casts() {
  int x = 1;
  vcast(static_cast<int>(x));
  cvalue((long)x);
}

struct VBase {
  virtual void act();
};
struct VDerived : VBase {
  void act() override;
};

void recv_value_cast(VDerived &d) { static_cast<VBase>(d).act(); }
void recv_ref_cast(VDerived &d) { static_cast<VBase &>(d).act(); }
void recv_ptr_cast(VDerived *d) { static_cast<VBase *>(d)->act(); }
void recv_intptr_cast(long raw) { reinterpret_cast<VBase *>(raw)->act(); }

void sink_ref(int &);
using IntRef = int &;

void alias_ref_cast() {
  int x = 1;
  sink_ref(IntRef(x));
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

// Template specialization handling (docs/improvements/fix-spec.md): every
// acceptance-matrix construct in one TU. Declaration-driven for classes AND
// callables; call sites only add as-written spellings.
const char *kSpecializationTu = R"cpp(
template <class T> struct Box { T v; };
template <class T> struct Box<T *> { T *p; };
template <> struct Box<bool> { bool b; };
template struct Box<int>;
extern template struct Box<long>;
Box<int *> value;
Box<bool> flag_box;

template <class T> T twice(T x) { return x + x; }
template <> int twice<int>(int x) { return x; }
template double twice<double>(double);

struct Worker {
  template <class T> int convert(T v) { return static_cast<int>(v); }
};
template <> int Worker::convert<int>(int v) { return v; }
void use(Worker &w, float f) {
  w.convert(f);
  w.convert<char>('a');
}

template <int N> int nth() { return N; }
template <> int nth<7>() { return 0; }
template int nth<9>();

template <typename... Ts> int cnt() { return 0; }
template <> int cnt<int, char>() { return 1; }

template <typename T> struct Vec {};
template <template <typename> class C> int pick() { return 0; }
template <> int pick<Vec>() { return 1; }
)cpp";

// Explicit instantiations of members of class templates — never lexical
// decls, reachable only through specialization lists (PR #16 review).
const char *kMemberInstantiationTu = R"cpp(
template <class T> struct PlainMember { void run(); };
template <class T> void PlainMember<T>::run() {}
template void PlainMember<int>::run();

template <class T> struct Gadget {
  template <class U> int conv(U u) { return static_cast<int>(u); }
};
template int Gadget<char>::conv<long>(long);

template <class T> struct Outer { struct Inner { void run(); }; };
template <class T> void Outer<T>::Inner::run() {}
template void Outer<int>::Inner::run();
)cpp";

// Two-file fixture: a template header plus the TU holding the explicit
// instantiation statements. reindex_with_source() rewrites the TU and re-runs
// `index` — only the changed TU is reparsed (md5 skip keeps the header).
struct IndexedProject {
  std::string cache;
  std::string proj;
  cidx::Logger log;
  IndexedProject(const std::string &header, const std::string &source)
      : cache(make_temp_dir()), proj(cache + "/proj") {
    ::mkdir(proj.c_str(), 0755);
    write_file(proj + "/templates.hpp", header);
    write_file(proj + "/instantiate.cpp", source);
    write_file(proj + "/compile_commands.json",
               "[{\"directory\": \"" + proj +
                   "\", \"command\": \"cc -I. -c instantiate.cpp -o "
                   "instantiate.o\", \"file\": \"instantiate.cpp\"}]\n");
    log.set_file(cache + "/cidx.log");
    REQUIRE(run_cidx({"import", "--db", proj, "--name", "fixture"}, cache,
                     log) == 0);
    REQUIRE(run_cidx({"index"}, cache, log) == 0);
  }
  void reindex_with_source(const std::string &source) {
    write_file(proj + "/instantiate.cpp", source);
    REQUIRE(run_cidx({"index"}, cache, log) == 0);
  }
  // Rewrite BOTH files (a header-only edit never reparses the unchanged TU,
  // and headers are indexed only through their including TU).
  void reindex_with(const std::string &header, const std::string &source) {
    write_file(proj + "/templates.hpp", header);
    reindex_with_source(source);
  }
  std::string db_path() const { return cache + "/index.db"; }
};

// "<file-basename>:<line>" owning the symbol with this USR.
std::vector<std::string> owner_probe(const std::string &db_path,
                                     const std::string &usr) {
  return query_col(db_path,
                   "SELECT f.name || ':' || s.line FROM symbol s "
                   "JOIN file f ON f.id = s.file_id "
                   "WHERE s.usr = '" +
                       usr + "'");
}

// "<kind-name>/<is_instantiation>" of the unique symbol with this USR.
std::vector<std::string> sym_probe(const std::string &db_path,
                                   const std::string &usr) {
  return query_col(db_path,
                   "SELECT sk.name || '/' || s.is_instantiation FROM symbol s "
                   "JOIN symbol_kind sk ON sk.id = s.kind "
                   "WHERE s.usr = '" +
                       usr + "'");
}

// "<edge-kind-name>/<count>" of every specializes/instantiates/method_of edge
// src_usr -> dst_usr. One row with count 1 = the relationship exists exactly
// once and later emissions did not re-count it.
std::vector<std::string> structural_edges(const std::string &db_path,
                                          const std::string &src_usr,
                                          const std::string &dst_usr) {
  return query_col(db_path,
                   "SELECT ek.name || '/' || e.count FROM edge e "
                   "JOIN symbol ss ON ss.id = e.src_id "
                   "JOIN symbol ds ON ds.id = e.dst_id "
                   "JOIN edge_kind ek ON ek.id = e.kind "
                   "WHERE e.kind IN (4, 5, 9) AND ss.usr = '" +
                       src_usr + "' AND ds.usr = '" + dst_usr + "'");
}

// "<position>:<arg_kind>:<literal>" rows owned by the symbol with this USR.
std::vector<std::string> args_probe(const std::string &db_path,
                                    const std::string &usr) {
  return query_col(db_path,
                   "SELECT ta.position || ':' || ta.arg_kind || ':' || "
                   "COALESCE(ta.literal, '') FROM template_arg ta "
                   "JOIN symbol os ON os.id = ta.owner_id "
                   "WHERE os.usr = '" +
                       usr + "'");
}

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

  TEST_CASE("review fix: named casts preserve the operand's provenance") {
    const IndexedTu tu(kProvenanceTu);
    // static_cast / const_cast / reinterpret_cast of a local still denote its
    // storage — like the implicit and C-style equivalents (functional casts
    // stay call_result).
    for (const char *callee : {"scast", "ccast", "rcast"}) {
      const ArgProbe p = probe_arg0(tu.db_path(), "named_casts", callee);
      CHECK_MESSAGE(p.src_kind == "local", callee);
      CHECK_MESSAGE(p.has_decl_usr, callee);
    }
  }

  TEST_CASE("review fix: value-producing casts do not claim the operand") {
    const IndexedTu tu(kProvenanceTu);
    // static_cast<int>(x) and (long)x construct a new value — like the
    // functional-cast equivalent they classify call_result, not local x.
    for (const char *callee : {"vcast", "cvalue"}) {
      const ArgProbe p = probe_arg0(tu.db_path(), "value_casts", callee);
      CHECK_MESSAGE(p.src_kind == "call_result", callee);
      CHECK_MESSAGE(!p.has_decl_usr, callee);
    }
    // Receiver path: static_cast<VBase>(d) slices — dispatch is exactly
    // VBase, so the receiver must not carry d's identity. It is a by-value
    // VBase (recv_type_is_value = 1), which devirtualization narrows on.
    CHECK(query_col(tu.db_path(),
                    "SELECT es.recv_src_kind || '/' || "
                    "COALESCE(es.recv_decl_usr, '') || '/' || "
                    "es.recv_type_is_value "
                    "FROM edge_site es "
                    "JOIN edge e ON e.id = es.edge_id "
                    "JOIN symbol ss ON ss.id = e.src_id "
                    "JOIN symbol ds ON ds.id = e.dst_id "
                    "WHERE ss.spelling = 'recv_value_cast' "
                    "AND ds.spelling = 'act'") ==
          std::vector<std::string>{"call_result//1"});
  }

  TEST_CASE("review fix: identity comes from the cast, not its syntax") {
    const IndexedTu tu(kProvenanceTu);
    // reinterpret_cast<VBase *>(raw) fabricates a pointer from an integer
    // (CK_IntegralToPointer) — the int parameter is NOT the receiver's
    // identity, and a pointer receiver is not held by value.
    CHECK(query_col(tu.db_path(),
                    "SELECT es.recv_src_kind || '/' || "
                    "COALESCE(es.recv_decl_usr, '') || '/' || "
                    "es.recv_type_is_value "
                    "FROM edge_site es "
                    "JOIN edge e ON e.id = es.edge_id "
                    "JOIN symbol ss ON ss.id = e.src_id "
                    "JOIN symbol ds ON ds.id = e.dst_id "
                    "WHERE ss.spelling = 'recv_intptr_cast' "
                    "AND ds.spelling = 'act'") ==
          std::vector<std::string>{"call_result//0"});
    // IntRef(x) — functional notation through a reference alias — is a
    // glvalue NoOp that still denotes x's storage.
    const ArgProbe p = probe_arg0(tu.db_path(), "alias_ref_cast", "sink_ref");
    CHECK(p.src_kind == "local");
    CHECK(p.has_decl_usr);
  }

  TEST_CASE("provenance pin: identity-preserving receiver casts keep d") {
    const IndexedTu tu(kProvenanceTu);
    // static_cast<VBase &>(d) and static_cast<VBase *>(d) still denote d's
    // storage — the receiver keeps the parameter's identity for
    // devirtualization.
    for (const char *src : {"recv_ref_cast", "recv_ptr_cast"}) {
      CHECK_MESSAGE(
          query_col(tu.db_path(),
                    std::string("SELECT es.recv_src_kind || '/' || "
                                "es.recv_param_pos "
                                "FROM edge_site es "
                                "JOIN edge e ON e.id = es.edge_id "
                                "JOIN symbol ss ON ss.id = e.src_id "
                                "JOIN symbol ds ON ds.id = e.dst_id "
                                "WHERE ss.spelling = '") +
                        src +
                        "' AND ds.spelling = 'act'") ==
              std::vector<std::string>{"local/0"},
          src);
    }
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

  // ---- template specialization handling (fix-spec acceptance matrix) -------

  TEST_CASE("template spec: partial specialization is a first-class symbol") {
    const IndexedTu tu(kSpecializationTu);
    // Indexed definition with its own USR and template-parameter row, even
    // though nothing selects it in another TU.
    CHECK(sym_probe(tu.db_path(), "c:@SP>1#T@Box>#*t0.0") ==
          std::vector<std::string>{"class-template/0"});
    CHECK(query_col(tu.db_path(),
                    "SELECT tp.position || ':' || tp.param_kind || ':' || "
                    "COALESCE(tp.name, '') FROM template_param tp "
                    "JOIN symbol os ON os.id = tp.owner_id "
                    "WHERE os.usr = 'c:@SP>1#T@Box>#*t0.0'") ==
          std::vector<std::string>{"0:1:T"});
    // Pattern argument keeps the authored spelling.
    CHECK(args_probe(tu.db_path(), "c:@SP>1#T@Box>#*t0.0") ==
          std::vector<std::string>{"0:1:T *"});
    CHECK(structural_edges(tu.db_path(), "c:@SP>1#T@Box>#*t0.0",
                           "c:@ST>1#T@Box") ==
          std::vector<std::string>{"specializes/1"});
  }

  TEST_CASE("template spec: class specialization and instantiation flags") {
    const IndexedTu tu(kSpecializationTu);
    // template<> struct Box<bool> — authored, never an instantiation, even
    // though flag_box uses it as a concrete type.
    CHECK(sym_probe(tu.db_path(), "c:@S@Box>#b") ==
          std::vector<std::string>{"struct/0"});
    CHECK(structural_edges(tu.db_path(), "c:@S@Box>#b", "c:@ST>1#T@Box") ==
          std::vector<std::string>{"specializes/1"});
    CHECK(args_probe(tu.db_path(), "c:@S@Box>#b") ==
          std::vector<std::string>{"0:1:bool"});
    // template struct Box<int> — explicit instantiation definition.
    CHECK(sym_probe(tu.db_path(), "c:@S@Box>#I") ==
          std::vector<std::string>{"struct/1"});
    CHECK(structural_edges(tu.db_path(), "c:@S@Box>#I", "c:@ST>1#T@Box") ==
          std::vector<std::string>{"instantiates/1"});
    CHECK(args_probe(tu.db_path(), "c:@S@Box>#I") ==
          std::vector<std::string>{"0:1:int"});
    // extern template struct Box<long> — same relationship and flag without
    // requiring a definition body.
    CHECK(sym_probe(tu.db_path(), "c:@S@Box>#L") ==
          std::vector<std::string>{"struct/1"});
    CHECK(structural_edges(tu.db_path(), "c:@S@Box>#L", "c:@ST>1#T@Box") ==
          std::vector<std::string>{"instantiates/1"});
    CHECK(args_probe(tu.db_path(), "c:@S@Box>#L") ==
          std::vector<std::string>{"0:1:long"});
  }

  TEST_CASE("template spec: concrete instance selects the partial") {
    const IndexedTu tu(kSpecializationTu);
    // Box<int *> value: the concrete instance instantiates the PARTIAL
    // specialization (not the primary) and records its own argument.
    CHECK(sym_probe(tu.db_path(), "c:@S@Box>#*I") ==
          std::vector<std::string>{"struct/1"});
    CHECK(structural_edges(tu.db_path(), "c:@S@Box>#*I",
                           "c:@SP>1#T@Box>#*t0.0") ==
          std::vector<std::string>{"instantiates/1"});
    CHECK(args_probe(tu.db_path(), "c:@S@Box>#*I") ==
          std::vector<std::string>{"0:1:int *"});
  }

  TEST_CASE("template spec: callable explicit spec/inst without call sites") {
    const IndexedTu tu(kSpecializationTu);
    // template<> int twice<int>(int) — indexed with no caller anywhere.
    CHECK(sym_probe(tu.db_path(), "c:@F@twice<#I>#I#") ==
          std::vector<std::string>{"function/0"});
    CHECK(structural_edges(tu.db_path(), "c:@F@twice<#I>#I#",
                           "c:@FT@>1#Ttwice#t0.0#S0_#") ==
          std::vector<std::string>{"specializes/1"});
    CHECK(args_probe(tu.db_path(), "c:@F@twice<#I>#I#") ==
          std::vector<std::string>{"0:1:int"});
    // template double twice<double>(double) — explicit instantiation, flag
    // true, instantiates, deduced argument stored.
    CHECK(sym_probe(tu.db_path(), "c:@F@twice<#d>#d#") ==
          std::vector<std::string>{"function/1"});
    CHECK(structural_edges(tu.db_path(), "c:@F@twice<#d>#d#",
                           "c:@FT@>1#Ttwice#t0.0#S0_#") ==
          std::vector<std::string>{"instantiates/1"});
    CHECK(args_probe(tu.db_path(), "c:@F@twice<#d>#d#") ==
          std::vector<std::string>{"0:1:double"});
  }

  TEST_CASE("template spec: method specializations keep method_of + args") {
    const IndexedTu tu(kSpecializationTu);
    const char *tmpl = "c:@S@Worker@FT@>1#Tconvert#t0.0#I#";
    // template<> int Worker::convert<int>(int) — declaration-driven, no call.
    CHECK(sym_probe(tu.db_path(), "c:@S@Worker@F@convert<#I>#I#") ==
          std::vector<std::string>{"method/0"});
    CHECK(structural_edges(tu.db_path(), "c:@S@Worker@F@convert<#I>#I#",
                           tmpl) == std::vector<std::string>{"specializes/1"});
    CHECK(structural_edges(tu.db_path(), "c:@S@Worker@F@convert<#I>#I#",
                           "c:@S@Worker") ==
          std::vector<std::string>{"method_of/1"});
    CHECK(args_probe(tu.db_path(), "c:@S@Worker@F@convert<#I>#I#") ==
          std::vector<std::string>{"0:1:int"});
    // Inferred w.convert(f): the deduced argument is recorded (the call-site
    // `<...>` fallback would have lost it).
    CHECK(sym_probe(tu.db_path(), "c:@S@Worker@F@convert<#f>#f#") ==
          std::vector<std::string>{"method/1"});
    CHECK(args_probe(tu.db_path(), "c:@S@Worker@F@convert<#f>#f#") ==
          std::vector<std::string>{"0:1:float"});
    CHECK(structural_edges(tu.db_path(), "c:@S@Worker@F@convert<#f>#f#",
                           tmpl) == std::vector<std::string>{"instantiates/1"});
    CHECK(structural_edges(tu.db_path(), "c:@S@Worker@F@convert<#f>#f#",
                           "c:@S@Worker") ==
          std::vector<std::string>{"method_of/1"});
    // Explicit w.convert<char>('a').
    CHECK(sym_probe(tu.db_path(), "c:@S@Worker@F@convert<#C>#C#") ==
          std::vector<std::string>{"method/1"});
    CHECK(args_probe(tu.db_path(), "c:@S@Worker@F@convert<#C>#C#") ==
          std::vector<std::string>{"0:1:char"});
  }

  TEST_CASE("template spec: non-type, template-template and pack args") {
    const IndexedTu tu(kSpecializationTu);
    // Non-type: explicit specialization and explicit instantiation both store
    // the VALUE with contract kind 2.
    CHECK(sym_probe(tu.db_path(), "c:@F@nth<#VI7>#") ==
          std::vector<std::string>{"function/0"});
    CHECK(args_probe(tu.db_path(), "c:@F@nth<#VI7>#") ==
          std::vector<std::string>{"0:2:7"});
    CHECK(sym_probe(tu.db_path(), "c:@F@nth<#VI9>#") ==
          std::vector<std::string>{"function/1"});
    CHECK(args_probe(tu.db_path(), "c:@F@nth<#VI9>#") ==
          std::vector<std::string>{"0:2:9"});
    // Pack -> contract kind 4; template-template -> contract kind 3.
    CHECK(arg_kinds_of(tu.db_path(), "cnt") == std::vector<std::string>{"4"});
    CHECK(arg_kinds_of(tu.db_path(), "pick") == std::vector<std::string>{"3"});
  }

  TEST_CASE("template spec: call sites do not duplicate structural edges") {
    const IndexedTu tu(kSpecializationTu);
    // Every specializes/instantiates/method_of asserted above came back as a
    // single row with count 1; the per-call caller -> primary instantiates
    // edge is the one that still counts sites (two convert calls in use()).
    CHECK(query_col(tu.db_path(),
                    "SELECT e.count FROM edge e "
                    "JOIN symbol ss ON ss.id = e.src_id "
                    "JOIN symbol ds ON ds.id = e.dst_id "
                    "WHERE e.kind = 5 AND ss.usr = 'c:@F@use#&$@S@Worker#f#' "
                    "AND ds.usr = 'c:@S@Worker@FT@>1#Tconvert#t0.0#I#'") ==
          std::vector<std::string>{"2"});
  }

  TEST_CASE("template spec: explicit instantiation is owned by its POI file") {
    // PR #16 review: the created FunctionDecl points at the template PATTERN
    // (templates.hpp); ownership must follow the point of instantiation.
    IndexedProject prj("#pragma once\n"
                       "template <class T> T twice(T x) { return x + x; }\n",
                       "#include \"templates.hpp\"\n"
                       "template double twice<double>(double);\n");
    // Owned by instantiate.cpp at the statement's line — so it IS a symbol of
    // that file — with declaration provenance anchored there too.
    CHECK(query_col(prj.db_path(),
                    "SELECT f.name || ':' || s.line || '/' || df.name FROM "
                    "symbol s JOIN file f ON f.id = s.file_id "
                    "JOIN file df ON df.id = s.decl_file_id "
                    "WHERE s.usr = 'c:@F@twice<#d>#d#'") ==
          std::vector<std::string>{"instantiate.cpp:2/instantiate.cpp"});
    CHECK(sym_probe(prj.db_path(), "c:@F@twice<#d>#d#") ==
          std::vector<std::string>{"function/1"});
    CHECK(structural_edges(prj.db_path(), "c:@F@twice<#d>#d#",
                           "c:@FT@>1#Ttwice#t0.0#S0_#") ==
          std::vector<std::string>{"instantiates/1"});
    // Deleting the statement and reindexing ONLY the TU (the header stays
    // md5-skipped) drops the RELATIONSHIP: per-file edge cleanup applies
    // because the instantiation symbol belongs to instantiate.cpp.
    prj.reindex_with_source("#include \"templates.hpp\"\n");
    CHECK(structural_edges(prj.db_path(), "c:@F@twice<#d>#d#",
                           "c:@FT@>1#Ttwice#t0.0#S0_#") ==
          std::vector<std::string>{});
    // Pinned repo-wide storage semantic, NOT a claim of this fix: reindexing
    // never garbage-collects the symbol row or its template_arg rows for a
    // removed declaration — a deleted plain function lingers identically.
    // Only the file's edges and definitions are dropped.
    CHECK(sym_probe(prj.db_path(), "c:@F@twice<#d>#d#") ==
          std::vector<std::string>{"function/1"});
    CHECK(args_probe(prj.db_path(), "c:@F@twice<#d>#d#") ==
          std::vector<std::string>{"0:1:double"});
  }

  TEST_CASE("template spec: POI anchor is the first materialization point") {
    // PR #16 review round 2. Clang gives a function explicit-instantiation
    // statement NO node of its own and the specialization ONE first-wins
    // PointOfInstantiation slot, so the statement's own line is not
    // recoverable when an earlier materialization exists. These cases pin
    // the achievable contract: ownership follows the FIRST point.
    // (a) A prior implicit use in the same file: ownership and cleanup stay
    // in tu.cpp; the recorded line is the use on line 3, not the statement
    // on line 4.
    const IndexedTu tu("template <class T> T twice(T x) { return x + x; }\n"
                       "\n"
                       "double use() { return twice(1.0); }\n"
                       "template double twice<double>(double);\n");
    CHECK(query_col(tu.db_path(),
                    "SELECT s.line || '/' || s.is_instantiation FROM symbol s "
                    "WHERE s.usr = 'c:@F@twice<#d>#d#'") ==
          std::vector<std::string>{"3/1"});
    CHECK(structural_edges(tu.db_path(), "c:@F@twice<#d>#d#",
                           "c:@FT@>1#Ttwice#t0.0#S0_#") ==
          std::vector<std::string>{"instantiates/1"});
  }

  TEST_CASE("template spec: extern declaration owns first, then re-anchors") {
    // PR #16 review round 2, two-file form: `extern template` in the header
    // followed by the explicit-instantiation DEFINITION in the TU. The
    // header's extern statement is the specialization's first (and only
    // recorded) point of instantiation, so it owns the symbol; the
    // definition-directive's own location is not modeled by Clang's AST.
    IndexedProject prj("#pragma once\n"
                       "template <class T> T twice(T x) { return x + x; }\n"
                       "extern template double twice<double>(double);\n",
                       "#include \"templates.hpp\"\n"
                       "template double twice<double>(double);\n");
    CHECK(owner_probe(prj.db_path(), "c:@F@twice<#d>#d#") ==
          std::vector<std::string>{"templates.hpp:3"});
    CHECK(sym_probe(prj.db_path(), "c:@F@twice<#d>#d#") ==
          std::vector<std::string>{"function/1"});
    CHECK(structural_edges(prj.db_path(), "c:@F@twice<#d>#d#",
                           "c:@FT@>1#Ttwice#t0.0#S0_#") ==
          std::vector<std::string>{"instantiates/1"});
    // Removing the extern statement re-anchors ownership to the next
    // remaining point — the definition statement in instantiate.cpp — on
    // reindex, with the relationship intact and still un-duplicated.
    prj.reindex_with("#pragma once\n"
                     "template <class T> T twice(T x) { return x + x; }\n",
                     "#include \"templates.hpp\"\n"
                     "template double twice<double>(double);\n"
                     "// reanchor\n");
    CHECK(owner_probe(prj.db_path(), "c:@F@twice<#d>#d#") ==
          std::vector<std::string>{"instantiate.cpp:2"});
    CHECK(structural_edges(prj.db_path(), "c:@F@twice<#d>#d#",
                           "c:@FT@>1#Ttwice#t0.0#S0_#") ==
          std::vector<std::string>{"instantiates/1"});
  }

  TEST_CASE("template spec: uncalled explicit member instantiation") {
    // PR #16 review: `template void PlainMember<int>::run();` lives only in
    // the class template's specialization list, not in any lexical position.
    const IndexedTu tu(kMemberInstantiationTu);
    // The ordinary member: symbol at the POI line, instantiation flag, its
    // instantiates edge to the member PATTERN, and method_of to the owner.
    CHECK(sym_probe(tu.db_path(), "c:@S@PlainMember>#I@F@run#") ==
          std::vector<std::string>{"method/1"});
    CHECK(query_col(tu.db_path(),
                    "SELECT s.line FROM symbol s "
                    "WHERE s.usr = 'c:@S@PlainMember>#I@F@run#'") ==
          std::vector<std::string>{"4"});
    CHECK(structural_edges(tu.db_path(), "c:@S@PlainMember>#I@F@run#",
                           "c:@ST>1#T@PlainMember@F@run#") ==
          std::vector<std::string>{"instantiates/1"});
    CHECK(structural_edges(tu.db_path(), "c:@S@PlainMember>#I@F@run#",
                           "c:@S@PlainMember>#I") ==
          std::vector<std::string>{"method_of/1"});
    // The promoted owner: PlainMember<int> instantiates PlainMember with its
    // argument recorded.
    CHECK(structural_edges(tu.db_path(), "c:@S@PlainMember>#I",
                           "c:@ST>1#T@PlainMember") ==
          std::vector<std::string>{"instantiates/1"});
    CHECK(args_probe(tu.db_path(), "c:@S@PlainMember>#I") ==
          std::vector<std::string>{"0:1:int"});
    // A member FUNCTION TEMPLATE explicit instantiation is found the same
    // way (Gadget<char>::conv<long>), with its deduced argument and owner.
    CHECK(sym_probe(tu.db_path(), "c:@S@Gadget>#C@F@conv<#L>#L#") ==
          std::vector<std::string>{"method/1"});
    CHECK(args_probe(tu.db_path(), "c:@S@Gadget>#C@F@conv<#L>#L#") ==
          std::vector<std::string>{"0:1:long"});
    CHECK(structural_edges(tu.db_path(), "c:@S@Gadget>#C@F@conv<#L>#L#",
                           "c:@S@Gadget>#C") ==
          std::vector<std::string>{"method_of/1"});
    CHECK(structural_edges(tu.db_path(), "c:@S@Gadget>#C",
                           "c:@ST>1#T@Gadget") ==
          std::vector<std::string>{"instantiates/1"});
    // PR #16 review round 3: a member of a NESTED record inside the
    // specialization (`template void Outer<int>::Inner::run();`) is reached
    // by recursing through instantiated contexts. Its owner Outer<int>::Inner
    // is never a lexical decl, so method_of relies on the minted owner.
    CHECK(sym_probe(tu.db_path(), "c:@S@Outer>#I@S@Inner@F@run#") ==
          std::vector<std::string>{"method/1"});
    CHECK(query_col(tu.db_path(),
                    "SELECT s.line FROM symbol s "
                    "WHERE s.usr = 'c:@S@Outer>#I@S@Inner@F@run#'") ==
          std::vector<std::string>{"13"});
    CHECK(structural_edges(tu.db_path(), "c:@S@Outer>#I@S@Inner@F@run#",
                           "c:@ST>1#T@Outer@S@Inner@F@run#") ==
          std::vector<std::string>{"instantiates/1"});
    CHECK(structural_edges(tu.db_path(), "c:@S@Outer>#I@S@Inner@F@run#",
                           "c:@S@Outer>#I@S@Inner") ==
          std::vector<std::string>{"method_of/1"});
    CHECK(sym_probe(tu.db_path(), "c:@S@Outer>#I@S@Inner") ==
          std::vector<std::string>{"struct/1"});
    // PR #16 review round 4: the minted owner must not be structurally
    // orphaned — it instantiates its member-class pattern Outer<T>::Inner
    // (CXXRecordDecl::getInstantiatedFromMemberClass).
    CHECK(structural_edges(tu.db_path(), "c:@S@Outer>#I@S@Inner",
                           "c:@ST>1#T@Outer@S@Inner") ==
          std::vector<std::string>{"instantiates/1"});
  }

  TEST_CASE("template spec: instantiation -> specialization downgrades") {
    // PR #16 review round 3: add_symbol previously merged is_instantiation
    // with MAX, so a `template double twice<double>(double);` replaced by an
    // authored `template<>` specialization (same USR) kept flag 1 forever.
    // A real decl row now states its TemplateSpecializationKind
    // authoritatively; stub promotion (mint_symbol_id) stays monotonic.
    IndexedProject prj("#pragma once\n"
                       "template <class T> T twice(T x) { return x + x; }\n",
                       "#include \"templates.hpp\"\n"
                       "template double twice<double>(double);\n");
    CHECK(sym_probe(prj.db_path(), "c:@F@twice<#d>#d#") ==
          std::vector<std::string>{"function/1"});
    CHECK(structural_edges(prj.db_path(), "c:@F@twice<#d>#d#",
                           "c:@FT@>1#Ttwice#t0.0#S0_#") ==
          std::vector<std::string>{"instantiates/1"});
    prj.reindex_with_source(
        "#include \"templates.hpp\"\n"
        "template <> double twice<double>(double v) { return v * 3; }\n");
    CHECK(sym_probe(prj.db_path(), "c:@F@twice<#d>#d#") ==
          std::vector<std::string>{"function/0"});
    CHECK(structural_edges(prj.db_path(), "c:@F@twice<#d>#d#",
                           "c:@FT@>1#Ttwice#t0.0#S0_#") ==
          std::vector<std::string>{"specializes/1"});
  }

} // TEST_SUITE("clang")

int main(int argc, char **argv) {
  doctest::Context ctx;
  ctx.applyCommandLine(argc, argv);
  return ctx.run();
}
