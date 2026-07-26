// query_plan_test — hermetic tests for the CXQ QueryPlan tier (src/query/).
//
// Category: hermetic (label "default") — no libclang, in-memory SQLite.
// Contract: docs/query-plan.md (v1).
//
// Covers:
//   Q1  canonical JSON vs the shared cross-language golden
//       (tests/golden/cxq_plans.txt — the Python suite pins the SAME bytes,
//        regenerate with CIDX_UPDATE_GOLDEN=1)
//   Q2  normalization: relation qualification, all_of/any_of flattening,
//       not(not(p)) reduction
//   Q3  validation errors: stable E_* codes
//   Q4  execution: source resolution, out/in, depth windows, where,
//       select/order_by/limit/distinct, union/intersect/except, count,
//       entity view traversal, default result cap + truncated
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "graph/query.hpp"
#include "query/cxq.hpp"
#include "query/exec.hpp"
#include "query/plan.hpp"
#include "storage/records.hpp"
#include "storage/storage.hpp"
#include "util/hashing.hpp"

using namespace cidx::query;
using cidx::Storage;
using cidx::Symbol;

template <typename T> concept NarrowQueryReadPort = requires(T & port) {
  {port.read_db()}->std::same_as<cidx::storage::SqliteReadDb &>;
  {port.graph_read()}->std::same_as<cidx::storage::GraphReadPort &>;
};

template <typename T>
concept HasMutableQueryEscapeHatches = requires(T & port) {
  port.raw_db();
  port.graph_service();
};

static_assert(NarrowQueryReadPort<QueryReadPort>);
static_assert(!HasMutableQueryEscapeHatches<QueryReadPort>);

namespace {

class QueryExecutor {
public:
  explicit QueryExecutor(Storage &db) : read_(db), executor_(read_) {}

  Result run(const Plan &plan) { return executor_.run(plan); }
  cidx::json_out::Value explain(const Plan &plan) {
    return executor_.explain(plan);
  }

private:
  SqliteQueryReadAdapter read_;
  cidx::query::Executor executor_;
};

std::string make_temp_dir() {
  char tmpl[] = "/tmp/cidx_query_identity_XXXXXX";
  char *dir = ::mkdtemp(tmpl);
  REQUIRE(dir != nullptr);
  return dir;
}

Symbol make_sym(const std::string &usr, const std::string &spelling,
                const std::string &kind = "function",
                const std::string &qual_name = "") {
  Symbol s;
  s.usr = usr;
  s.spelling = spelling;
  s.kind = kind;
  if (!qual_name.empty()) {
    s.qual_name = qual_name;
  }
  s.is_definition = true;
  s.resolved = true;
  return s;
}

cidx::Edge make_edge(int64_t src, int64_t dst, int64_t kind) {
  cidx::Edge e;
  e.src_id = src;
  e.dst_id = dst;
  e.kind = kind;
  e.count = 1;
  return e;
}

// The named plans shared with python/tests/test_queryplan.py. Adding a plan
// here requires adding the SAME plan to the Python suite.
std::map<std::string, Plan> golden_plans() {
  std::map<std::string, Plan> plans;
  plans["entity_uses"] =
      (start(entity("PaymentService")) | out("uses") |
       where(in_list("entity_type", {"class", "interface"})) |
       select({"name", "usr"}) | limit(100))
          .plan();
  plans["codebase_abstract"] =
      (start(codebase()) | view(View::Entity) |
       nodes(eq("entity_type", "abstract_class")) |
       where(all_of(
           {eq("is_definition", true), not_(glob("name", "*Legacy*"))})) |
       select({"name", "kind"}) | order_by({"name"}) | limit(50))
          .plan();
  plans["symbol_callers"] =
      (start(symbol("c:@F@normalize#")) | in_("calls") |
       select({"name", "file", "line"}) | order_by({"name"}))
          .plan();
  plans["union_count"] =
      (start(symbol("A")) | out("calls", 1, 3) |
       union_(start(symbol("A")) | out("uses")) | distinct() | count())
          .plan();
  plans["qualified_relation"] = (start(symbol("Widget")) | out("entity.uses") |
                                 view(View::Entity) | in_("generalizes", 1, 4))
                                    .plan();
  plans["boolean_normalization"] =
      (start(codebase()) |
       nodes(all_of({all_of({eq("kind", "class"), eq("is_static", false)}),
                     not_(not_(ne("spelling", "x")))})) |
       count())
          .plan();
  plans["path_calls"] =
      (start(symbol("USR::A")) |
       path(start(symbol("USR::C")), "calls", 1, 8, 1) | rank(5) | limit(1))
          .plan();
  return plans;
}

std::string render_golden() {
  std::ostringstream out;
  for (const auto &[name, plan] : golden_plans()) {
    out << "== " << name << " ==\n" << canonical_json(plan) << "\n";
  }
  return out.str();
}

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Seed graph:
//   symbols: funcA(fn) -> funcB(fn) -> funcC(fn); ClassD/ClassE(class);
//            AbsS(struct)
//   calls:   A->B, B->C          uses: A->C
//   inherits: E->D (symbol layer)
//   entity:  D,E are entity_node kind=1(class); AbsS is kind=2
//            (abstract_class) -- declaration kind `struct`, classification
//            `abstract_class` (the two-fields split, PR #20 review);
//            entity uses: D->E; generalizes: E->D
struct Seeded {
  Storage db;
  int64_t A = -1, B = -1, C = -1, D = -1, E = -1, S = -1, T = -1, I = -1;

  Seeded() : db(":memory:") {
    A = db.add_symbol(make_sym("USR::A", "funcA", "function", "ns::funcA"));
    B = db.add_symbol(make_sym("USR::B", "funcB", "function"));
    C = db.add_symbol(make_sym("USR::C", "funcC", "function"));
    D = db.add_symbol(make_sym("USR::D", "ClassD", "class"));
    E = db.add_symbol(make_sym("USR::E", "ClassE", "class"));
    S = db.add_symbol(make_sym("USR::S", "AbsS", "struct"));
    T = db.add_symbol(make_sym("USR::T", "Thing", "class-template"));
    I = db.add_symbol(make_sym("USR::I", "Thing<int>", "class"));
    db.add_edge(make_edge(A, B, 1)); // calls
    db.add_edge(make_edge(B, C, 1)); // calls
    db.add_edge(make_edge(A, C, 7)); // uses
    db.add_edge(make_edge(E, D, 2)); // inherits
    db.add_edge(make_edge(E, C, 2)); // inherits to a non-entity target
    db.add_edge(make_edge(I, T, 5)); // instantiates
    // Layer-1: D and E are class entities; AbsS is an abstract struct;
    // D entity-uses E; E generalizes D.
    auto ins = db.raw_db().prepare(
        "INSERT INTO entity_node (id, kind) VALUES (?, 1), (?, 1), (?, 2)");
    ins.bind(1, D);
    ins.bind(2, E);
    ins.bind(3, S);
    ins.step_done();
    db.add_entity_edge(D, E, 8); // entity uses
    db.add_entity_edge(E, D, 1); // generalizes
  }
};

std::string error_code(const std::function<void()> &fn) {
  try {
    fn();
  } catch (const PlanError &e) {
    const std::string msg = e.what();
    return msg.substr(0, msg.find(':'));
  }
  return "<no-error>";
}

} // namespace

// ---------------------------------------------------------------------------
// Q1: canonical JSON golden (cross-language parity anchor)
// ---------------------------------------------------------------------------
TEST_CASE("query_plan: canonical JSON matches the shared golden") {
  const std::string rendered = render_golden();
  const char *update = std::getenv("CIDX_UPDATE_GOLDEN");
  if (update != nullptr && std::string(update) == "1") {
    std::ofstream out(CIDX_CXQ_GOLDEN, std::ios::binary);
    out << rendered;
  }
  CHECK(rendered == read_file(CIDX_CXQ_GOLDEN));
}

TEST_CASE("query_plan: CXQ text lowers to the immutable plan") {
  const Plan parsed =
      parse_cxq("codebase() | nodes(kind = class) | "
                "where(is_definition = true and name ~= 'Widget*') | "
                "select(name, usr) | order_by(name) | limit(10)");
  const Plan expected =
      (start(codebase()) | nodes(eq("kind", "class")) |
       where(all_of({eq("is_definition", true), glob("name", "Widget*")})) |
       select({"name", "usr"}) | order_by({"name"}) | limit(10))
          .plan();
  CHECK(canonical_json(parsed) == canonical_json(expected));

  const Plan symbol_plan = parse_cxq("symbol('USR::f') | out(calls, 1, 2)");
  CHECK(canonical_json(symbol_plan).find("USR::f") != std::string::npos);
}

TEST_CASE("query_plan: CXQ text reports stable parse errors") {
  CHECK_THROWS_WITH(
      parse_cxq("nodes()"),
      "E_PARSE: query must start with codebase(), symbol(), or entity()");
  CHECK_THROWS_WITH(parse_cxq("codebase() | limit(nope)"),
                    "E_PARSE: limit() requires one integer");
  CHECK_THROWS_WITH(
      parse_cxq("codebase() | nodes(kind = class, name = Widget)"),
      "E_PARSE: nodes() takes zero or one predicate");
  CHECK_THROWS_WITH(parse_cxq("codebase() | out(calls, mode=static)"),
                    "E_PARSE: depth must be an integer or depth=min..max");
  CHECK_THROWS_WITH(parse_cxq("codebase() | nodes() | limit(+1)"),
                    "E_PARSE: limit() requires one integer");
  CHECK_THROWS_WITH(parse_cxq("codebase() | nodes() | limit(1_0)"),
                    "E_PARSE: limit() requires one integer");
  CHECK_THROWS_WITH(
      parse_cxq("codebase() | nodes() | limit(9223372036854775808)"),
      "E_PARSE: limit() requires one integer");
  CHECK_THROWS_WITH(
      parse_cxq("codebase() | nodes() | limit(-9223372036854775809)"),
      "E_PARSE: limit() requires one integer");
  CHECK_THROWS_WITH(parse_cxq("codebase() | out(calls, +1)"),
                    "E_PARSE: depth must be an integer or depth=min..max");
  CHECK_THROWS_WITH(parse_cxq("codebase() | out(calls, 1_0)"),
                    "E_PARSE: depth must be an integer or depth=min..max");
  CHECK_THROWS_WITH(parse_cxq("codebase() | out(calls, 9223372036854775808)"),
                    "E_PARSE: depth must be an integer or depth=min..max");
  CHECK_THROWS_WITH(parse_cxq("codebase() | out(calls, depth=+1..2)"),
                    "E_PARSE: depth must be written as depth=min..max");
  CHECK_THROWS_WITH(parse_cxq("codebase() | count(extra)"),
                    "E_PARSE: count() takes no arguments");
  CHECK_THROWS_WITH(parse_cxq("codebase() | rank(name)"),
                    "E_PARSE: rank() is not available in v1");
}

TEST_CASE("query_plan: CXQ text rejects oversized integer tokens") {
  const std::string digits(5000, '9');
  CHECK_THROWS_WITH(parse_cxq("codebase() | nodes() | limit(" + digits + ")"),
                    "E_PARSE: limit() requires one integer");
  CHECK_THROWS_WITH(parse_cxq("codebase() | out(calls, " + digits + ")"),
                    "E_PARSE: depth must be an integer or depth=min..max");
}

// ---------------------------------------------------------------------------
// Q2: normalization
// ---------------------------------------------------------------------------
TEST_CASE(
    "query_plan: normalization qualifies relations and flattens booleans") {
  // Bare relation in the active view qualifies to the layer name.
  Plan p = (start(symbol("A")) | out("calls")).plan();
  Plan n = validate(p);
  CHECK(n.stages[0].relation == "symbol.calls");

  // entity() source starts in the entity view.
  Plan pe = (start(entity("X")) | out("uses")).plan();
  CHECK(validate(pe).stages[0].relation == "entity.uses");

  // The stream view follows a traversal's layer: after a qualified entity
  // hop, a bare relation resolves in the entity namespace.
  Plan pq = (start(symbol("Widget")) | out("entity.uses") | in_("generalizes"))
                .plan();
  CHECK(validate(pq).stages[1].relation == "entity.generalizes");

  // all_of(all_of(a,b), not(not(c))) -> all_of(a,b,c)
  Plan pb = (start(symbol("A")) |
             where(all_of({all_of({eq("spelling", "a"), eq("spelling", "b")}),
                           not_(not_(eq("spelling", "c")))})))
                .plan();
  const Plan nb = validate(pb);
  const Pred &np = *nb.stages[0].pred;
  CHECK(np.op == PredOp::AllOf);
  CHECK(np.kids.size() == 3);
  CHECK(np.kids[2].op == PredOp::Eq);
}

// ---------------------------------------------------------------------------
// Q3: validation errors -- stable E_* codes
// ---------------------------------------------------------------------------
TEST_CASE("query_plan: validation error codes") {
  auto code = [](const Plan &p) {
    return error_code([&] { (void)validate(p); });
  };

  CHECK(code((start(symbol(""))).plan()) == "E_SOURCE");
  CHECK(code((start(symbol("A")) | out("bogus")).plan()) == "E_RELATION");
  CHECK(code((start(symbol("A")) | out("generalizes")).plan()) ==
        "E_RELATION"); // entity relation in symbol view
  CHECK(code((start(symbol("A")) | out("calls", 1, 33)).plan()) == "E_DEPTH");
  CHECK(code((start(symbol("A")) | out("calls", 0, 2)).plan()) == "E_DEPTH");
  CHECK(code((start(symbol("A")) | where(eq("bogus", "x"))).plan()) ==
        "E_FIELD");
  CHECK(code((start(symbol("A")) | where(eq("file", "x"))).plan()) ==
        "E_FIELD"); // select-only field
  CHECK(code((start(symbol("A")) | where(eq("kind", "bogus_kind"))).plan()) ==
        "E_KIND");
  CHECK(
      code((start(symbol("A")) | where(eq("kind", "abstract_class"))).plan()) ==
      "E_KIND"); // classification is not a decl kind
  CHECK(code((start(codebase()) | view(View::Entity) |
              nodes(eq("entity_type", "struct")))
                 .plan()) == "E_KIND"); // struct is not a classification
  CHECK(code((start(codebase()) | view(View::Entity) |
              nodes(eq("kind", "struct")))
                 .plan()) == "<no-error>"); // decl kind is view-independent
  CHECK(code((start(symbol("A")) | limit(0)).plan()) == "E_LIMIT");
  CHECK(code((start(symbol("A")) | union_(start(entity("B")) | out("uses")))
                 .plan()) == "E_SETOP"); // view mismatch
  CHECK(code((start(symbol("A")) | select({"name"}) | out("calls")).plan()) ==
        "E_STAGE"); // traversal after select
  CHECK(code((start(symbol("A")) | count() | limit(1)).plan()) ==
        "E_STAGE"); // stage after count
  CHECK(code((start(symbol("A")) | nodes()).plan()) ==
        "E_STAGE"); // nodes on non-codebase source
  CHECK(code((start(codebase()) | count()).plan()) ==
        "E_STAGE"); // codebase not enumerated
  CHECK(code((start(codebase()) | nodes()).plan()) == "<no-error>");
  CHECK(code((start(symbol("A")) | select({"name"}) | order_by({"usr"}))
                 .plan()) == "E_FIELD"); // order_by outside selection
}

// ---------------------------------------------------------------------------
// Q4: execution
// ---------------------------------------------------------------------------
TEST_CASE("query_plan: source resolution by usr / qual_name / spelling") {
  Seeded s;
  QueryExecutor ex(s.db);

  auto by_usr = ex.run((start(symbol("USR::A")) | out("calls")).plan());
  REQUIRE(by_usr.rows.size() == 1);
  CHECK(std::get<int64_t>(by_usr.rows[0][0]) == s.B);

  auto by_qual = ex.run((start(symbol("ns::funcA")) | out("calls")).plan());
  CHECK(by_qual.rows.size() == 1);

  auto by_spelling = ex.run((start(symbol("funcA")) | out("calls")).plan());
  CHECK(by_spelling.rows.size() == 1);

  auto missing = ex.run((start(symbol("nope")) | out("calls")).plan());
  CHECK(missing.rows.empty());
}

TEST_CASE("query_plan: traversal depth windows and direction") {
  Seeded s;
  QueryExecutor ex(s.db);

  // depth 1..1: A -> B only
  auto d1 = ex.run((start(symbol("USR::A")) | out("calls")).plan());
  REQUIRE(d1.rows.size() == 1);
  CHECK(std::get<int64_t>(d1.rows[0][0]) == s.B);

  // depth 1..2: B and C
  auto d2 = ex.run((start(symbol("USR::A")) | out("calls", 1, 2)).plan());
  CHECK(d2.rows.size() == 2);

  // depth 2..2: C only
  auto d22 = ex.run((start(symbol("USR::A")) | out("calls", 2, 2)).plan());
  REQUIRE(d22.rows.size() == 1);
  CHECK(std::get<int64_t>(d22.rows[0][0]) == s.C);

  // inbound: callers of B = A
  auto in1 = ex.run((start(symbol("USR::B")) | in_("calls")).plan());
  REQUIRE(in1.rows.size() == 1);
  CHECK(std::get<int64_t>(in1.rows[0][0]) == s.A);
}

TEST_CASE("query_plan: where filter and select fields") {
  Seeded s;
  QueryExecutor ex(s.db);

  auto r = ex.run((start(codebase()) | nodes(eq("kind", "class")) |
                   select({"name", "kind", "usr"}))
                      .plan());
  REQUIRE(r.rows.size() == 3);
  CHECK(r.fields == std::vector<std::string>{"name", "kind", "usr"});
  CHECK(std::get<std::string>(r.rows[0][0]) == "ClassD");
  CHECK(std::get<std::string>(r.rows[0][1]) == "class");

  // where() on a node stream: keep only funcB among A's calls+uses closure.
  auto w = ex.run((start(symbol("USR::A")) | out("calls", 1, 2) |
                   where(eq("spelling", "funcB")))
                      .plan());
  REQUIRE(w.rows.size() == 1);
  CHECK(std::get<int64_t>(w.rows[0][0]) == s.B);
}

TEST_CASE("query_plan: set operations, distinct, count") {
  Seeded s;
  QueryExecutor ex(s.db);

  // calls(1..2) = {B, C}; uses = {C}; union is a SET union -- the shared C
  // must NOT be double-counted (PR #20 review).
  auto u = ex.run((start(symbol("USR::A")) | out("calls", 1, 2) |
                   union_(start(symbol("USR::A")) | out("uses")))
                      .plan());
  CHECK(u.rows.size() == 2);

  auto uc = ex.run((start(symbol("USR::A")) | out("calls", 1, 2) |
                    union_(start(symbol("USR::A")) | out("uses")) | count())
                       .plan());
  CHECK(uc.scalar == 2);

  // distinct on an already-deduped stream is a no-op.
  auto ud = ex.run((start(symbol("USR::A")) | out("calls", 1, 2) |
                    union_(start(symbol("USR::A")) | out("uses")) | distinct())
                       .plan());
  CHECK(ud.rows.size() == 2);

  // intersect = {C}; except = {B}.
  auto ix = ex.run((start(symbol("USR::A")) | out("calls", 1, 2) |
                    intersect(start(symbol("USR::A")) | out("uses")))
                       .plan());
  REQUIRE(ix.rows.size() == 1);
  CHECK(std::get<int64_t>(ix.rows[0][0]) == s.C);

  auto ec = ex.run((start(symbol("USR::A")) | out("calls", 1, 2) |
                    except_(start(symbol("USR::A")) | out("uses")))
                       .plan());
  REQUIRE(ec.rows.size() == 1);
  CHECK(std::get<int64_t>(ec.rows[0][0]) == s.B);

  // count() is a scalar.
  auto c =
      ex.run((start(symbol("USR::A")) | out("calls", 1, 2) | count()).plan());
  CHECK(c.shape == Shape::Scalar);
  CHECK(c.scalar == 2);
}

TEST_CASE("query_plan: entity view traversal") {
  Seeded s;
  QueryExecutor ex(s.db);

  // D entity-uses E.
  auto uses = ex.run((start(entity("ClassD")) | out("uses")).plan());
  REQUIRE(uses.rows.size() == 1);
  CHECK(std::get<int64_t>(uses.rows[0][0]) == s.E);
  CHECK(uses.view == View::Entity);

  // Subclasses of D at the entity level: in(generalizes) = E.
  auto subs = ex.run((start(entity("ClassD")) | in_("generalizes")).plan());
  REQUIRE(subs.rows.size() == 1);
  CHECK(std::get<int64_t>(subs.rows[0][0]) == s.E);

  // Cross-altitude: symbol source, qualified entity relation.
  auto q = ex.run((start(symbol("ClassD")) | out("entity.uses")).plan());
  CHECK(q.rows.size() == 1);

  // entity() source only resolves entity nodes.
  auto notent = ex.run((start(entity("funcA")) | out("uses")).plan());
  CHECK(notent.rows.empty());
}

TEST_CASE("query_plan: order_by, limit, default fields, result JSON") {
  Seeded s;
  QueryExecutor ex(s.db);

  auto r = ex.run((start(codebase()) | nodes(eq("kind", "function")) |
                   select({"spelling"}) | order_by({"spelling"}) | limit(2))
                      .plan());
  REQUIRE(r.rows.size() == 2);
  CHECK(std::get<std::string>(r.rows[0][0]) == "funcA");
  CHECK(std::get<std::string>(r.rows[1][0]) == "funcB");
  CHECK(
      !r.truncated); // explicit limit is requested cardinality, not truncation

  // Default fields for a node stream include portable scope identity.
  auto d = ex.run((start(symbol("USR::A")) | out("calls")).plan());
  CHECK(d.fields == std::vector<std::string>{"id", "usr", "semantic_universe",
                                             "identity_key", "name", "kind"});

  // Result JSON shape.
  auto j = cidx::json_out::dumps_indent2(d.to_json());
  CHECK(j.find("\"shape\": \"nodes\"") != std::string::npos);
  CHECK(j.find("\"view\": \"symbol\"") != std::string::npos);
  CHECK(j.find("\"count\": 1") != std::string::npos);
  CHECK(j.find("\"funcB\"") != std::string::npos);

  const auto envelope = d.to_envelope();
  CHECK(envelope.status == cidx::protocol::Status::Unknown);
  CHECK(envelope.identity.fact_sets == std::vector<std::string>{"symbols"});
  const auto envelope_json = cidx::json_out::dumps_indent2(envelope.to_json());
  CHECK(envelope_json.find("\"protocol\": \"cidx.result/v1\"") !=
        std::string::npos);
  CHECK(envelope_json.find("\"evidence\"") != std::string::npos);
}

TEST_CASE("query_plan: default result cap reports truncation") {
  Storage db(":memory:");
  {
    auto txn = db.transaction();
    for (int i = 0; i < 1200; ++i) {
      db.add_symbol(
          make_sym("USR::f" + std::to_string(i), "f" + std::to_string(i)));
    }
    txn.commit(); // the destructor is ROLLBACK-only (R2)
  }
  QueryExecutor ex(db);
  auto r = ex.run((start(codebase()) | nodes()).plan());
  CHECK(r.rows.size() == 1000);
  CHECK(r.truncated);

  // count() ignores the default cap.
  auto c = ex.run((start(codebase()) | nodes() | count()).plan());
  CHECK(c.scalar == 1200);

  // An explicit limit is honored without the truncated flag.
  auto lim = ex.run((start(codebase()) | nodes() | limit(1100)).plan());
  CHECK(lim.rows.size() == 1100);
  CHECK(!lim.truncated);
}

TEST_CASE(
    "query_plan: legacy identity and result key order are deterministic") {
  const std::string dir = make_temp_dir();
  const std::string source = dir + "/source.cpp";
  {
    std::ofstream out(source);
    out << "int answer = 1;\n";
  }

  Storage db(":memory:");
  db.add_component("fixture", dir);
  const auto file_id =
      db.add_file_path(source, std::nullopt, cidx::md5_of(source));
  db.mark_file_indexed(file_id);

  CHECK(db.index_identity().freshness == "unverifiable");

  QueryExecutor executor(db);
  const auto result = executor.run((start(codebase()) | nodes()).plan());
  CHECK(result.index.freshness == "unverifiable");
  const std::string row_json = cidx::json_out::dumps_indent2(result.to_json());
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
  CHECK(scalar_json.find("\"index\"") < scalar_json.size());

  const std::string explained = cidx::json_out::dumps_indent2(
      executor.explain((start(codebase()) | nodes()).plan()));
  CHECK(explained.find("\"freshness\": \"unverifiable\"") != std::string::npos);
  CHECK(explained.find("\"plan\"") != std::string::npos);
}

TEST_CASE("query_plan: identity is stable across component insertion order") {
  const std::string dir = make_temp_dir();
  const std::string alpha_dir = dir + "/alpha";
  const std::string beta_dir = dir + "/beta";
  std::filesystem::create_directories(alpha_dir);
  std::filesystem::create_directories(beta_dir);
  const std::string alpha_source = alpha_dir + "/alpha.cpp";
  const std::string beta_source = beta_dir + "/beta.cpp";
  {
    std::ofstream out(alpha_source);
    out << "int alpha() { return 1; }\n";
  }
  {
    std::ofstream out(beta_source);
    out << "int beta() { return 2; }\n";
  }

  const auto make_identity = [&](const std::vector<std::string> &order) {
    Storage db(":memory:");
    for (const std::string &name : order) {
      db.add_component(name, dir + "/" + name);
    }
    for (const std::string &source : {alpha_source, beta_source}) {
      const auto file_id =
          db.add_file_path(source, std::nullopt, cidx::md5_of(source));
      db.mark_file_indexed(file_id, std::nullopt, cidx::md5_of(source));
    }
    db.stamp_index_identity();
    return db.index_identity();
  };

  const auto forward = make_identity({"alpha", "beta"});
  const auto reverse = make_identity({"beta", "alpha"});
  CHECK(forward.freshness == "current");
  CHECK(reverse.freshness == "current");
  CHECK(forward.source_revision == reverse.source_revision);
  CHECK(forward.source_fingerprint == reverse.source_fingerprint);
  CHECK(forward.index_config_fingerprint == reverse.index_config_fingerprint);
}

// ---------------------------------------------------------------------------
// PR #20 review regressions
// ---------------------------------------------------------------------------
TEST_CASE("query_plan: view(entity) drops ids without an entity_node row") {
  Seeded s;
  QueryExecutor ex(s.db);

  // A function is NOT an entity: relabelling must not surface it as one.
  auto fn = ex.run((start(symbol("funcA")) | view(View::Entity)).plan());
  CHECK(fn.rows.empty());
  CHECK(fn.view == View::Entity);

  // A real entity survives the switch; view(symbol) is a pure relabel.
  auto cls = ex.run((start(symbol("ClassD")) | view(View::Entity)).plan());
  REQUIRE(cls.rows.size() == 1);
  auto back = ex.run((start(entity("ClassD")) | view(View::Symbol)).plan());
  CHECK(back.rows.size() == 1);
}

TEST_CASE(
    "query_plan: min_depth window uses path length, not first discovery") {
  // Diamond: P -> Q, P -> R -> Q. A length-2 path reaches Q, so
  // out(calls, 2, 2) must emit it even though Q is first seen at depth 1.
  Storage db(":memory:");
  const int64_t P = db.add_symbol(make_sym("USR::P", "p"));
  const int64_t Q = db.add_symbol(make_sym("USR::Q", "q"));
  const int64_t R = db.add_symbol(make_sym("USR::R", "r"));
  db.add_edge(make_edge(P, Q, 1));
  db.add_edge(make_edge(P, R, 1));
  db.add_edge(make_edge(R, Q, 1));

  QueryExecutor ex(db);
  auto d2 = ex.run((start(symbol("USR::P")) | out("calls", 2, 2)).plan());
  REQUIRE(d2.rows.size() == 1);
  CHECK(std::get<int64_t>(d2.rows[0][0]) == Q);

  // The window 1..2 emits both targets exactly once.
  auto d12 = ex.run((start(symbol("USR::P")) | out("calls", 1, 2)).plan());
  CHECK(d12.rows.size() == 2);
}

TEST_CASE("query_plan: default cap re-applies after an expanding stage") {
  // hub calls 1200 targets: limit(1) before the traversal must not disable
  // the final safety cap on the expanded result.
  Storage db(":memory:");
  {
    auto txn = db.transaction();
    const int64_t hub = db.add_symbol(make_sym("USR::hub", "hub"));
    for (int i = 0; i < 1200; ++i) {
      const int64_t t = db.add_symbol(
          make_sym("USR::t" + std::to_string(i), "t" + std::to_string(i)));
      db.add_edge(make_edge(hub, t, 1));
    }
    txn.commit(); // the destructor is ROLLBACK-only (R2)
  }
  QueryExecutor ex(db);
  auto r = ex.run((start(symbol("USR::hub")) | limit(1) | out("calls")).plan());
  CHECK(r.rows.size() == 1000);
  CHECK(r.truncated);
}

TEST_CASE(
    "query_plan: kind is the decl kind; entity_type is the classification") {
  Seeded s;
  QueryExecutor ex(s.db);

  // The abstract struct matches the declaration-kind predicate...
  auto decl = ex.run(
      (start(codebase()) | nodes(in_list("kind", {"class", "struct"})) |
       select({"spelling", "kind", "entity_type"}) | order_by({"spelling"}))
          .plan());
  REQUIRE(decl.rows.size() == 4); // AbsS, ClassD, ClassE, Thing<int>
  CHECK(std::get<std::string>(decl.rows[0][0]) == "AbsS");
  CHECK(std::get<std::string>(decl.rows[0][1]) == "struct");
  CHECK(std::get<std::string>(decl.rows[0][2]) == "abstract_class");

  // ...and the classification predicate selects it without collapsing the
  // declaration kind.
  auto cls = ex.run((start(codebase()) | view(View::Entity) |
                     nodes(eq("entity_type", "abstract_class")) |
                     select({"spelling", "kind"}))
                        .plan());
  REQUIRE(cls.rows.size() == 1);
  CHECK(std::get<std::string>(cls.rows[0][0]) == "AbsS");
  CHECK(std::get<std::string>(cls.rows[0][1]) == "struct");

  // entity_type is null for a non-entity symbol.
  auto fn = ex.run((start(symbol("funcA")) | select({"entity_type"})).plan());
  REQUIRE(fn.rows.size() == 1);
  CHECK(std::holds_alternative<std::nullptr_t>(fn.rows[0][0]));
}

TEST_CASE("query_plan: scoped identity fields are selectable") {
  Seeded s;
  QueryExecutor ex(s.db);
  const auto result =
      ex.run((start(symbol("USR::A")) |
              select({"usr", "semantic_universe", "identity_key"}))
                 .plan());
  REQUIRE(result.rows.size() == 1);
  CHECK(std::get<std::string>(result.rows[0][0]) == "USR::A");
  CHECK(std::get<std::string>(result.rows[0][1]) == "legacy");
  CHECK(std::get<std::string>(result.rows[0][2]) == "legacy\x1fUSR::A");
}

TEST_CASE("query_plan: typed parameter views preserve natural slot identity") {
  Storage db(":memory:");
  const int64_t owner =
      db.add_symbol(make_sym("USR::typed", "typed", "function"));
  db.raw_db().exec(
      "INSERT INTO parameter(owner_id,position,pack_index,name,default_text,"
      "reference_semantics) VALUES (1,0,-1,'value','0','lvalue')");

  QueryExecutor ex(db);
  const Result result =
      ex.run((start(symbol("USR::typed")) | out("has_parameter") |
              select({"owner_id", "position", "pack_index", "name",
                      "default_text", "identity_key"}))
                 .plan());
  REQUIRE(result.view == View::Parameter);
  REQUIRE(result.rows.size() == 1);
  CHECK(std::get<int64_t>(result.rows[0][0]) == owner);
  CHECK(std::get<int64_t>(result.rows[0][1]) == 0);
  CHECK(std::get<int64_t>(result.rows[0][2]) == -1);
  CHECK(std::get<std::string>(result.rows[0][3]) == "value");
  CHECK(std::get<std::string>(result.rows[0][4]) == "0");
  CHECK(std::get<std::string>(result.rows[0][5])
            .starts_with("parameter:legacy\x1fUSR::typed:0:-1"));

  db.raw_db().exec("INSERT INTO parameter(owner_id,position,pack_index,name) "
                   "VALUES (1,1,-1,'other')");
  const Result filtered =
      ex.run((start(symbol("USR::typed")) | out("has_parameter") |
              where(eq("position", int64_t{1})) | select({"name"}))
                 .plan());
  REQUIRE(filtered.rows.size() == 1);
  CHECK(std::get<std::string>(filtered.rows[0][0]) == "other");

  const Result reverse =
      ex.run((start(codebase()) | view(View::Parameter) | nodes() |
              in_("has_parameter") | select({"name"}))
                 .plan());
  REQUIRE(reverse.view == View::Symbol);
  REQUIRE(reverse.rows.size() == 1);
  CHECK(std::get<std::string>(reverse.rows[0][0]) == "typed");
}

TEST_CASE(
    "query_plan: named signature slots and recursive type layers are typed") {
  Storage db(":memory:");
  Symbol callable = make_sym("USR::typed_views", "typed_views", "function");
  callable.callable_kind = "free-function";
  callable.template_origin = "typed_views<T>";
  callable.template_form = "pattern";
  const int64_t owner = db.add_symbol(callable);
  db.raw_db().exec(
      "INSERT INTO type_node(type_key,spelling,kind,extent) VALUES "
      "('A4(b:int)','int[4]',8,'4'),('b:int','int',1,NULL),"
      "('b:float','float',1,NULL),('b:char','char',1,NULL)");
  auto type_ids = db.raw_db().prepare("SELECT id FROM type_node ORDER BY id");
  REQUIRE(type_ids.step());
  const int64_t array_id = type_ids.col_int64(0);
  REQUIRE(type_ids.step());
  const int64_t int_id = type_ids.col_int64(0);
  REQUIRE(type_ids.step());
  REQUIRE(type_ids.step());
  db.add_type_edge(array_id, 2, 0, int_id);
  db.add_symbol_type(owner, 1, array_id);
  db.raw_db().exec(
      "INSERT INTO parameter(owner_id,position,pack_index,name,type_id,"
      "declared_type_id,adjusted_type_id) VALUES (1,0,-1,'value',2,2,2)");
  db.raw_db().exec(
      "INSERT INTO template_param(owner_id,position,param_kind,name,type_id) "
      "VALUES (1,0,1,'T',3)");
  db.raw_db().exec(
      "INSERT INTO template_arg(owner_id,position,pack_index,arg_kind,type_id) "
      "VALUES (1,1,0,1,4),(1,1,1,1,4)");
  const auto structure = [&] {
    auto symbols = db.raw_db().prepare(
        "SELECT count(*),COALESCE(group_concat(id,','),'') FROM symbol");
    REQUIRE(symbols.step());
    auto edges = db.raw_db().prepare(
        "SELECT count(*),COALESCE(group_concat(id,','),'') FROM edge");
    REQUIRE(edges.step());
    return std::tuple{symbols.col_int64(0), symbols.col_text(1),
                      edges.col_int64(0), edges.col_text(1)};
  };
  const auto before = structure();
  QueryExecutor ex(db);
  const auto symbols =
      ex.run((start(symbol("USR::typed_views")) |
              where(all_of({eq("callable_kind", "free-function"),
                            eq("template_origin", "typed_views<T>"),
                            eq("template_form", "pattern")})) |
              select({"callable_kind", "template_origin", "template_form"}))
                 .plan());
  REQUIRE(symbols.rows.size() == 1);
  CHECK(std::get<std::string>(symbols.rows[0][0]) == "free-function");
  const auto slots =
      ex.run((start(symbol("USR::typed_views")) | out("has_signature_slot") |
              where(eq("slot_kind", "parameter")) |
              select({"slot_kind", "position", "name", "type_id"}))
                 .plan());
  REQUIRE(slots.rows.size() == 1);
  CHECK(std::get<std::string>(slots.rows[0][0]) == "parameter");
  CHECK(std::get<int64_t>(slots.rows[0][1]) == 0);
  CHECK(std::get<std::string>(slots.rows[0][2]) == "value");
  CHECK(std::get<int64_t>(slots.rows[0][3]) == int_id);
  const auto callable_roundtrip =
      ex.run((start(symbol("USR::typed_views")) | out("has_signature_slot") |
              out("of_callable") | select({"usr"}))
                 .plan());
  REQUIRE(callable_roundtrip.rows.size() == 1);
  CHECK(std::get<std::string>(callable_roundtrip.rows[0][0]) ==
        "USR::typed_views");
  const auto type_roundtrip =
      ex.run((start(symbol("USR::typed_views")) | out("has_signature_slot") |
              out("of_type") | select({"type_key"}))
                 .plan());
  REQUIRE(type_roundtrip.rows.size() == 4);
  CHECK(std::get<std::string>(type_roundtrip.rows[0][0]) == "A4(b:int)");
  CHECK(std::get<std::string>(type_roundtrip.rows[1][0]) == "b:int");
  CHECK(std::get<std::string>(type_roundtrip.rows[2][0]) == "b:float");
  CHECK(std::get<std::string>(type_roundtrip.rows[3][0]) == "b:char");
  const auto callable_inverse =
      ex.run((start(symbol("USR::typed_views")) | out("has_signature_slot") |
              in_("has_signature_slot") | select({"usr"}))
                 .plan());
  REQUIRE(callable_inverse.rows.size() == 1);
  CHECK(std::get<std::string>(callable_inverse.rows[0][0]) ==
        "USR::typed_views");
  const auto type_inverse =
      ex.run((start(codebase()) | view(View::Type) | nodes() |
              where(eq("type_key", "b:int")) | in_("signature_slot.of_type") |
              select({"slot_kind"}))
                 .plan());
  REQUIRE(type_inverse.rows.size() == 1);
  CHECK(std::get<std::string>(type_inverse.rows[0][0]) == "parameter");
  const auto layers = ex.run(
      (start(codebase()) | view(View::Type) | nodes() | out("has_layer") |
       where(eq("root_id", array_id)) |
       select({"root_id", "path", "relation", "depth", "status", "extent"}))
          .plan());
  REQUIRE(layers.rows.size() == 2);
  CHECK(std::get<std::string>(layers.rows[0][1]) == "root");
  CHECK(std::get<std::string>(layers.rows[1][1]) == "root.element");
  CHECK(std::get<std::string>(layers.rows[1][2]) == "element_type");
  CHECK(std::get<std::string>(layers.rows[0][4]) == "complete");
  CHECK(std::get<std::string>(layers.rows[0][5]) == "4");
  const auto parent = ex.run(
      (start(codebase()) | view(View::Type) | nodes() | out("has_layer") |
       where(eq("root_id", array_id)) | where(eq("path", "root.element")) |
       in_("child") | select({"path"}))
          .plan());
  REQUIRE(parent.rows.size() == 1);
  CHECK(std::get<std::string>(parent.rows[0][0]) == "root");
  const auto root_type = ex.run(
      (start(codebase()) | view(View::Type) | nodes() | out("has_layer") |
       where(eq("root_id", array_id)) | where(eq("path", "root")) |
       in_("has_layer") | select({"type_key"}))
          .plan());
  REQUIRE(root_type.rows.size() == 1);
  CHECK(std::get<std::string>(root_type.rows[0][0]) == "A4(b:int)");
  CHECK(structure() == before);
}

TEST_CASE(
    "query_plan: exact recursive and pointer type acceptance is read-only") {
  Storage db(":memory:");
  Symbol owner = make_sym("cidx::version_re", "version_re", "function");
  const int64_t owner_id = db.add_symbol(owner);
  db.add_symbol(make_sym("USR::std::regex", "regex", "class", "std::regex"));
  Symbol record = make_sym("USR::Owner", "Owner", "struct");
  db.add_symbol(record);
  db.raw_db().exec(
      "INSERT INTO type_node(type_key,spelling,kind,decl_usr) VALUES "
      "('alias:A','A',4,NULL),('alias:B','B',4,NULL),"
      "('fn:ret-param','int(float)',9,NULL),('b:int','int',1,NULL),"
      "('b:float','float',1,NULL),('record:Owner','Owner',2,NULL),"
      "('mfp:Owner','int (Owner::*)(float)',13,NULL),"
      "('pack:int','int...',14,NULL),"
      "('ref:regex','const std::regex &',6,'USR::std::regex'),"
      "('record:regex','std::regex',2,'USR::std::regex')");
  const auto type_id = [&db](const char *key) {
    auto st = db.raw_db().prepare("SELECT id FROM type_node WHERE type_key=?");
    st.bind(1, std::string_view(key));
    REQUIRE(st.step());
    return st.col_int64(0);
  };
  const int64_t alias_a = type_id("alias:A");
  const int64_t alias_b = type_id("alias:B");
  const int64_t function = type_id("fn:ret-param");
  const int64_t integer = type_id("b:int");
  const int64_t floating = type_id("b:float");
  const int64_t member_owner = type_id("record:Owner");
  const int64_t member_function = type_id("mfp:Owner");
  const int64_t regex_reference = type_id("ref:regex");
  const int64_t regex_record = type_id("record:regex");
  db.add_type_edge(alias_a, 3, 0, alias_b);
  db.add_type_edge(alias_b, 3, 0, alias_a);
  db.add_type_edge(function, 4, 0, integer);
  db.add_type_edge(function, 5, 0, floating);
  db.add_type_edge(member_function, 7, 0, member_owner);
  db.add_type_edge(member_function, 8, 0, function);
  db.add_type_edge(regex_reference, 1, 0, regex_record);
  db.add_symbol_type(owner_id, 1, regex_reference);
  auto unknown_parameter = db.raw_db().prepare(
      "INSERT INTO parameter(owner_id,position,pack_index,name) "
      "VALUES (?,?,?,?)");
  unknown_parameter.bind(1, owner_id);
  unknown_parameter.bind(2, int64_t{9});
  unknown_parameter.bind(3, int64_t{-1});
  unknown_parameter.bind(4, std::string_view{"unknown"});
  unknown_parameter.step_done();
  auto type_only_parameter = db.raw_db().prepare(
      "INSERT INTO parameter(owner_id,position,pack_index,name,type_id) "
      "VALUES (?,?,?,?,?)");
  type_only_parameter.bind(1, owner_id);
  type_only_parameter.bind(2, int64_t{10});
  type_only_parameter.bind(3, int64_t{-1});
  type_only_parameter.bind(4, std::string_view{"type-only"});
  type_only_parameter.bind(5, regex_reference);
  type_only_parameter.step_done();

  cidx::query::SqliteQueryReadAdapter read(db);
  cidx::graph::GraphQuery graph(read);
  const auto alias_layers = graph.type_layers(alias_a);
  REQUIRE(alias_layers.size() == 3);
  CHECK(alias_layers[0].path == "root");
  CHECK(alias_layers[0].type.kind == "alias");
  CHECK(alias_layers[1].path == "root.alias_of");
  CHECK(alias_layers[1].status == "complete");
  CHECK(alias_layers[2].path == "root.alias_of.alias_of");
  CHECK(alias_layers[2].status == "cycle");
  const auto function_layers = graph.type_layers(function);
  REQUIRE(function_layers.size() == 3);
  CHECK(function_layers[1].path == "root.return_type");
  CHECK(function_layers[1].type.spelling == "int");
  CHECK(function_layers[2].path == "root.param_type[0]");
  CHECK(function_layers[2].type.spelling == "float");
  const auto member_layers = graph.type_layers(member_function);
  REQUIRE(member_layers.size() == 5);
  CHECK(member_layers[1].path == "root.member_owner");
  CHECK(member_layers[1].type.spelling == "Owner");
  CHECK(member_layers[2].path == "root.member_component");
  CHECK(member_layers[2].type.kind == "function");
  const auto unknown_layers = graph.type_layers(999999);
  REQUIRE(unknown_layers.size() == 1);
  CHECK(unknown_layers[0].status == "unknown");

  QueryExecutor ex(db);
  const auto version_return = ex.run(
      (start(symbol("cidx::version_re")) | out("has_signature_slot") |
       where(all_of({eq("slot_kind", "return"), eq("mode", "lvalue-reference"),
                     eq("value_kind", "record"),
                     eq("named_decl", "std::regex")})) |
       select({"mode", "value_kind", "named_decl"}))
          .plan());
  REQUIRE(version_return.rows.size() == 1);
  CHECK(std::get<std::string>(version_return.rows[0][0]) == "lvalue-reference");
  CHECK(std::get<std::string>(version_return.rows[0][1]) == "record");
  CHECK(std::get<std::string>(version_return.rows[0][2]) == "std::regex");
  const auto signature = graph.signature(owner_id);
  const auto type_only_graph =
      [&]() -> const cidx::graph::GraphQuery::ParamInfo * {
    for (const auto &param : signature.params) {
      if (param.position == 10) {
        return &param;
      }
    }
    return nullptr;
  }();
  REQUIRE(type_only_graph != nullptr);
  REQUIRE(type_only_graph->declared_type.has_value());
  REQUIRE(type_only_graph->adjusted_type.has_value());
  CHECK(type_only_graph->declared_type->id == regex_reference);
  CHECK(type_only_graph->adjusted_type->id == regex_reference);
  CHECK(type_only_graph->mode == "lvalue-reference");
  CHECK(type_only_graph->value_kind == "record");
  REQUIRE(type_only_graph->named_decl.has_value());
  CHECK(*type_only_graph->named_decl == "std::regex");
  const auto type_only_plan =
      ex.run((start(symbol("cidx::version_re")) | out("has_signature_slot") |
              where(all_of({eq("slot_kind", "parameter"),
                            eq("position", int64_t{10})})) |
              select({"type_id", "declared_type_id", "adjusted_type_id", "mode",
                      "value_kind", "named_decl"}))
                 .plan());
  REQUIRE(type_only_plan.rows.size() == 1);
  CHECK(std::get<int64_t>(type_only_plan.rows[0][0]) == regex_reference);
  CHECK(std::holds_alternative<std::nullptr_t>(type_only_plan.rows[0][1]));
  CHECK(std::holds_alternative<std::nullptr_t>(type_only_plan.rows[0][2]));
  CHECK(std::get<std::string>(type_only_plan.rows[0][3]) == "lvalue-reference");
  CHECK(std::get<std::string>(type_only_plan.rows[0][4]) == "record");
  CHECK(std::get<std::string>(type_only_plan.rows[0][5]) == "std::regex");
  const auto type_only_filtered = ex.run(
      (start(symbol("cidx::version_re")) | out("has_signature_slot") |
       where(all_of({eq("slot_kind", "parameter"), eq("position", int64_t{10}),
                     eq("mode", "lvalue-reference"), eq("value_kind", "record"),
                     eq("named_decl", "std::regex")})) |
       select({"position"}))
          .plan());
  REQUIRE(type_only_filtered.rows.size() == 1);
  CHECK(std::get<int64_t>(type_only_filtered.rows[0][0]) == 10);
  const auto null_slot =
      ex.run((start(symbol("cidx::version_re")) | out("has_parameter") |
              where(eq("position", int64_t{9})) | select({"type_id"}))
                 .plan());
  REQUIRE(null_slot.rows.size() == 1);
  CHECK(std::holds_alternative<std::nullptr_t>(null_slot.rows[0][0]));
  const auto counts = [&db] {
    auto st = db.raw_db().prepare("SELECT (SELECT count(*) FROM symbol), "
                                  "(SELECT count(*) FROM edge)");
    REQUIRE(st.step());
    return std::tuple{st.col_int64(0), st.col_int64(1)};
  };
  const auto before = counts();
  const auto pack_layers = graph.type_layers(type_id("pack:int"));
  REQUIRE(pack_layers.size() == 1);
  CHECK(pack_layers[0].type.kind == "pack-expansion");
  CHECK(counts() == before);
}

TEST_CASE("query_plan: template defaults expose logical evidence") {
  Storage db(":memory:");
  const int64_t owner =
      db.add_symbol(make_sym("USR::template", "template", "class"));
  auto insert = db.raw_db().prepare(
      "INSERT INTO template_param(owner_id,position,param_kind,name,"
      "default_txt) VALUES (?,?,?,?,?)");
  insert.bind(1, owner);
  insert.bind(2, int64_t{0});
  insert.bind(3, int64_t{1});
  insert.bind(4, std::string_view{"T"});
  insert.bind(5, std::string_view{"int"});
  insert.step_done();

  QueryExecutor ex(db);
  const Result result =
      ex.run((start(symbol("USR::template")) | out("has_template_parameter") |
              out("has_default") |
              select({"owner_id", "position", "default_txt", "identity_key"}))
                 .plan());
  REQUIRE(result.view == View::Evidence);
  REQUIRE(result.rows.size() == 1);
  CHECK(std::get<int64_t>(result.rows[0][0]) == owner);
  CHECK(std::get<int64_t>(result.rows[0][1]) == 0);
  CHECK(std::get<std::string>(result.rows[0][2]) == "int");
  CHECK(std::get<std::string>(result.rows[0][3]) ==
        "evidence:template_default:legacy\x1fUSR::template:0");
}

TEST_CASE(
    "query_plan: reverse typed relations and file identities are portable") {
  const auto seed = [](Storage &db, std::string_view component_path,
                       std::string_view repository_name,
                       std::string_view symbol_suffix, bool grouped = true) {
    int64_t repository_id = 0;
    if (grouped) {
      repository_id =
          db.add_repository(std::string(repository_name), "repo",
                            std::string("https://example.test/") +
                                std::string(repository_name) + ".git");
    }
    const int64_t component_id =
        db.add_component("project", std::string(component_path));
    if (grouped) {
      db.set_component_repository(component_id, repository_id);
      auto update =
          db.raw_db().prepare("UPDATE component SET path = ? WHERE id = ?");
      update.bind(1, std::string_view{"src"});
      update.bind(2, component_id);
      update.step_done();
    }
    const int64_t directory_id =
        db.add_directory(component_id, grouped ? "include" : "src");
    const int64_t file_id =
        db.add_file(directory_id, grouped ? "unit.cpp" : "same.cpp");
    const std::string caller_usr =
        "USR::" + std::string(symbol_suffix) + "::caller";
    const std::string callee_usr =
        "USR::" + std::string(symbol_suffix) + "::callee";
    const int64_t caller = db.add_symbol(make_sym(caller_usr, "caller"));
    const int64_t callee = db.add_symbol(make_sym(callee_usr, "callee"));
    const int64_t edge_id = db.add_edge(make_edge(caller, callee, 1));

    cidx::EdgeSite site;
    site.edge_id = edge_id;
    site.file_id = file_id;
    site.line = 10;
    site.col = 2;
    db.add_edge_site(site);

    auto arg = db.raw_db().prepare(
        "INSERT INTO call_arg(edge_id,file_id,line,col,position) "
        "VALUES (?,?,?,?,?)");
    arg.bind(1, edge_id);
    arg.bind(2, file_id);
    arg.bind(3, int64_t{10});
    arg.bind(4, int64_t{2});
    arg.bind(5, int64_t{0});
    arg.step_done();
  };

  Storage first(":memory:");
  seed(first, "/tmp/a/cpp-indexer", "", "shared", false);
  QueryExecutor first_executor(first);
  const auto reverse_evidence =
      first_executor.run((start(codebase()) | view(View::Evidence) | nodes() |
                          in_("edge.has_evidence") | count())
                             .plan());
  const auto reverse_argument =
      first_executor.run((start(codebase()) | view(View::CallArgument) |
                          nodes() | in_("edge.has_argument") | count())
                             .plan());
  const auto reverse_occurrence =
      first_executor.run((start(codebase()) | view(View::CallArgument) |
                          nodes() | in_("evidence.of_occurrence") | count())
                             .plan());
  CHECK(reverse_evidence.scalar == 1);
  CHECK(reverse_argument.scalar == 1);
  CHECK(reverse_occurrence.scalar == 1);

  Storage second(":memory:");
  seed(second, "/tmp/b/cpp-indexer", "", "shared", false);
  QueryExecutor second_executor(second);
  const auto first_evidence =
      first_executor.run((start(codebase()) | view(View::Evidence) | nodes() |
                          select({"identity_key"}))
                             .plan());
  const auto second_evidence =
      second_executor.run((start(codebase()) | view(View::Evidence) | nodes() |
                           select({"identity_key"}))
                              .plan());
  const auto first_argument =
      first_executor.run((start(codebase()) | view(View::CallArgument) |
                          nodes() | select({"identity_key"}))
                             .plan());
  const auto second_argument =
      second_executor.run((start(codebase()) | view(View::CallArgument) |
                           nodes() | select({"identity_key"}))
                              .plan());
  REQUIRE(first_evidence.rows.size() == 1);
  REQUIRE(second_evidence.rows.size() == 1);
  REQUIRE(first_argument.rows.size() == 1);
  REQUIRE(second_argument.rows.size() == 1);
  CHECK(first_evidence.rows[0] == second_evidence.rows[0]);
  CHECK(first_argument.rows[0] == second_argument.rows[0]);

  Storage collision(":memory:");
  seed(collision, "/repo-a", "repo-a", "repo-a");
  seed(collision, "/repo-b", "repo-b", "repo-b");
  QueryExecutor collision_executor(collision);
  const auto collision_evidence =
      collision_executor.run((start(codebase()) | view(View::Evidence) |
                              nodes() | select({"id", "identity_key"}))
                                 .plan());
  const auto collision_arguments =
      collision_executor.run((start(codebase()) | view(View::CallArgument) |
                              nodes() | select({"id", "identity_key"}))
                                 .plan());
  REQUIRE(collision_evidence.rows.size() == 2);
  CHECK(std::get<int64_t>(collision_evidence.rows[0][0]) !=
        std::get<int64_t>(collision_evidence.rows[1][0]));
  CHECK(std::get<std::string>(collision_evidence.rows[0][1]) !=
        std::get<std::string>(collision_evidence.rows[1][1]));
  REQUIRE(collision_arguments.rows.size() == 2);
  CHECK(std::get<int64_t>(collision_arguments.rows[0][0]) !=
        std::get<int64_t>(collision_arguments.rows[1][0]));
  CHECK(std::get<std::string>(collision_arguments.rows[0][1]) !=
        std::get<std::string>(collision_arguments.rows[1][1]));

  Storage ungrouped(":memory:");
  seed(ungrouped, "/repo/A/project", "", "ungrouped-a", false);
  seed(ungrouped, "/repo/B/project", "", "ungrouped-b", false);
  QueryExecutor ungrouped_executor(ungrouped);
  CHECK_THROWS_WITH(
      ungrouped_executor.run((start(codebase()) | view(View::Evidence) |
                              nodes() | select({"id", "identity_key"}))
                                 .plan()),
      "E_IDENTITY: ambiguous ungrouped component identity");
  CHECK_THROWS_WITH(
      ungrouped_executor.run(
          (start(codebase()) | view(View::Evidence) | nodes() | count())
              .plan()),
      "E_IDENTITY: ambiguous ungrouped component identity");
  CHECK_THROWS_WITH(
      ungrouped_executor.run((start(codebase()) | view(View::CallArgument) |
                              nodes() | select({"id", "identity_key"}))
                                 .plan()),
      "E_IDENTITY: ambiguous ungrouped component identity");
  CHECK_THROWS_WITH(
      ungrouped_executor.run(
          (start(codebase()) | view(View::CallArgument) | nodes() | count())
              .plan()),
      "E_IDENTITY: ambiguous ungrouped component identity");
  const auto evidence_base = start(codebase()) | view(View::Evidence) | nodes();
  CHECK_THROWS_WITH(
      ungrouped_executor.run((evidence_base | except_(evidence_base)).plan()),
      "E_IDENTITY: ambiguous ungrouped component identity");
  CHECK_THROWS_WITH(
      ungrouped_executor.run(
          (evidence_base | except_(evidence_base) | count()).plan()),
      "E_IDENTITY: ambiguous ungrouped component identity");
  const auto argument_base =
      start(codebase()) | view(View::CallArgument) | nodes();
  CHECK_THROWS_WITH(
      ungrouped_executor.run((argument_base | except_(argument_base)).plan()),
      "E_IDENTITY: ambiguous ungrouped component identity");
  CHECK_THROWS_WITH(
      ungrouped_executor.run(
          (argument_base | except_(argument_base) | count()).plan()),
      "E_IDENTITY: ambiguous ungrouped component identity");

  Storage mirrored_first(":memory:");
  seed(mirrored_first, "/Users/husam/.codex/worktrees/a/cpp-indexer", "",
       "mirrored", false);
  Storage mirrored_second(":memory:");
  seed(mirrored_second, "/Users/husam/.codex/worktrees/b/cpp-indexer", "",
       "mirrored", false);
  QueryExecutor mirrored_first_executor(mirrored_first);
  QueryExecutor mirrored_second_executor(mirrored_second);
  const auto mirrored_first_evidence =
      mirrored_first_executor.run((start(codebase()) | view(View::Evidence) |
                                   nodes() | select({"identity_key"}))
                                      .plan());
  const auto mirrored_second_evidence =
      mirrored_second_executor.run((start(codebase()) | view(View::Evidence) |
                                    nodes() | select({"identity_key"}))
                                       .plan());
  const auto mirrored_first_arguments = mirrored_first_executor.run(
      (start(codebase()) | view(View::CallArgument) | nodes() |
       select({"identity_key"}))
          .plan());
  const auto mirrored_second_arguments = mirrored_second_executor.run(
      (start(codebase()) | view(View::CallArgument) | nodes() |
       select({"identity_key"}))
          .plan());
  CHECK(mirrored_first_evidence.rows == mirrored_second_evidence.rows);
  CHECK(mirrored_first_arguments.rows == mirrored_second_arguments.rows);

  Storage non_catalogued_first(":memory:");
  seed(non_catalogued_first, "/tmp/a/cpp-indexer", "",
       "non-catalogued-mirrored", false);
  Storage non_catalogued_second(":memory:");
  seed(non_catalogued_second, "/tmp/b/cpp-indexer", "",
       "non-catalogued-mirrored", false);
  QueryExecutor non_catalogued_first_executor(non_catalogued_first);
  QueryExecutor non_catalogued_second_executor(non_catalogued_second);
  const auto non_catalogued_first_evidence = non_catalogued_first_executor.run(
      (start(codebase()) | view(View::Evidence) | nodes() |
       select({"identity_key"}))
          .plan());
  const auto non_catalogued_second_evidence =
      non_catalogued_second_executor.run((start(codebase()) |
                                          view(View::Evidence) | nodes() |
                                          select({"identity_key"}))
                                             .plan());
  const auto non_catalogued_first_arguments = non_catalogued_first_executor.run(
      (start(codebase()) | view(View::CallArgument) | nodes() |
       select({"identity_key"}))
          .plan());
  const auto non_catalogued_second_arguments =
      non_catalogued_second_executor.run((start(codebase()) |
                                          view(View::CallArgument) | nodes() |
                                          select({"identity_key"}))
                                             .plan());
  CHECK(non_catalogued_first_evidence.rows ==
        non_catalogued_second_evidence.rows);
  CHECK(non_catalogued_first_arguments.rows ==
        non_catalogued_second_arguments.rows);
}

TEST_CASE("query_plan: sites expand edge provenance deterministically") {
  Storage db(":memory:");
  const int64_t component = db.add_component("project", "/tmp/site-view");
  const int64_t directory = db.add_directory(component, "src");
  const int64_t file = db.add_file(directory, "same.cpp");
  const int64_t caller = db.add_symbol(make_sym("USR::caller", "caller"));
  const int64_t callee = db.add_symbol(make_sym("USR::callee", "callee"));
  const int64_t edge = db.add_edge(make_edge(caller, callee, 1));
  const int64_t reverse_edge = db.add_edge(make_edge(callee, caller, 1));
  cidx::EdgeSite site;
  site.edge_id = edge;
  site.file_id = file;
  site.line = 10;
  site.col = 2;
  db.add_edge_site(site);
  site.edge_id = reverse_edge;
  site.line = 5;
  db.add_edge_site(site);

  QueryExecutor ex(db);
  const Result result =
      ex.run((start(codebase()) | view(View::Edge) | nodes() | sites() |
              select({"edge_id", "file", "line", "col", "relation", "evidence",
                      "status", "partial"}))
                 .plan());
  REQUIRE(result.view == View::Site);
  REQUIRE(result.rows.size() == 2);
  CHECK(std::get<int64_t>(result.rows[0][0]) == edge);
  CHECK(std::get<std::string>(result.rows[0][1]).ends_with("/same.cpp"));
  CHECK(std::get<int64_t>(result.rows[0][2]) == 10);
  CHECK(std::get<int64_t>(result.rows[0][3]) == 2);
  CHECK(std::get<std::string>(result.rows[0][4]) == "calls");
  CHECK(std::get<std::string>(result.rows[0][5]) == "call_site");
  CHECK(std::get<std::string>(result.rows[0][6]) == "partial");
  CHECK(std::get<int64_t>(result.rows[0][7]) == 1);
  CHECK(std::get<int64_t>(result.rows[1][0]) == reverse_edge);
  CHECK(std::get<int64_t>(result.rows[1][2]) == 5);
}

TEST_CASE("query_plan: typed provenance preserves status through select") {
  const auto run = [](int64_t kind, bool unresolved_endpoint,
                      bool unresolved_site) {
    Storage db(":memory:");
    const int64_t component = db.add_component("project", "/tmp/status-view");
    const int64_t directory = db.add_directory(component, "src");
    const int64_t file = db.add_file(directory, "status.cpp");
    const int64_t caller =
        db.add_symbol(make_sym("USR::status-caller", "caller"));
    auto callee_sym = make_sym("USR::status-callee", "callee");
    callee_sym.resolved = !unresolved_endpoint;
    const int64_t callee = db.add_symbol(callee_sym);
    const int64_t edge = db.add_edge(make_edge(caller, callee, kind));
    cidx::EdgeSite site;
    site.edge_id = edge;
    site.file_id = file;
    site.line = 1;
    site.col = 1;
    if (unresolved_site) {
      site.recv_decl_usr = "USR::missing-declaration";
    }
    db.add_edge_site(site);

    QueryExecutor ex(db);
    return ex.run((start(codebase()) | view(View::Edge) | nodes() |
                   select({"status", "partial", "unknown"}))
                      .plan());
  };

  const auto check = [](Result result, std::string_view status, int64_t partial,
                        int64_t unknown, bool result_partial,
                        bool result_unknown,
                        cidx::protocol::Status envelope_status) {
    REQUIRE(result.rows.size() == 1);
    CHECK(std::get<std::string>(result.rows[0][0]) == status);
    CHECK(std::get<int64_t>(result.rows[0][1]) == partial);
    CHECK(std::get<int64_t>(result.rows[0][2]) == unknown);
    CHECK(result.partial == result_partial);
    CHECK(result.unknown == result_unknown);
    result.index.freshness = "current";
    CHECK(result.to_envelope().status == envelope_status);
  };

  check(run(2, false, false), "complete", 0, 0, false, false,
        cidx::protocol::Status::Complete);
  check(run(1, false, false), "partial", 1, 0, true, false,
        cidx::protocol::Status::Partial);
  check(run(2, true, false), "unknown", 0, 1, false, true,
        cidx::protocol::Status::Unknown);
  check(run(2, false, true), "unknown", 0, 1, false, true,
        cidx::protocol::Status::Unknown);
}

TEST_CASE("query_plan: site status follows the full logical site key") {
  Storage db(":memory:");
  const int64_t component = db.add_component("project", "/tmp/mixed-site-view");
  const int64_t directory = db.add_directory(component, "src");
  const int64_t file = db.add_file(directory, "mixed.cpp");
  const int64_t caller = db.add_symbol(make_sym("USR::mixed-caller", "caller"));
  const int64_t callee = db.add_symbol(make_sym("USR::mixed-callee", "callee"));
  const int64_t edge = db.add_edge(make_edge(caller, callee, 1));
  cidx::EdgeSite resolved{
      .edge_id = edge, .file_id = file, .line = 1, .col = 1};
  db.add_edge_site(resolved);
  resolved.line = 2;
  resolved.recv_decl_usr = "USR::missing-declaration";
  db.add_edge_site(resolved);

  QueryExecutor ex(db);
  const auto check_rows = [](Result result) {
    REQUIRE(result.rows.size() == 2);
    CHECK(std::get<int64_t>(result.rows[0][0]) == 1);
    CHECK(std::get<std::string>(result.rows[0][1]) == "partial");
    CHECK(std::get<int64_t>(result.rows[0][2]) == 1);
    CHECK(std::get<int64_t>(result.rows[0][3]) == 0);
    CHECK(std::get<int64_t>(result.rows[1][0]) == 2);
    CHECK(std::get<std::string>(result.rows[1][1]) == "unknown");
    CHECK(std::get<int64_t>(result.rows[1][2]) == 0);
    CHECK(std::get<int64_t>(result.rows[1][3]) == 1);
    CHECK(result.partial);
    CHECK(result.unknown);
    result.index.freshness = "current";
    CHECK(result.to_envelope().status == cidx::protocol::Status::Unknown);
  };
  const auto site_plan = start(codebase()) | view(View::Site) | nodes() |
                         select({"line", "status", "partial", "unknown"});
  const auto evidence_plan = start(codebase()) | view(View::Evidence) |
                             nodes() |
                             select({"line", "status", "partial", "unknown"});
  check_rows(ex.run(site_plan.plan()));
  check_rows(ex.run(evidence_plan.plan()));

  Result ordered = ex.run((site_plan | order_by({"line"}) | limit(1)).plan());
  REQUIRE(ordered.rows.size() == 1);
  CHECK(std::get<std::string>(ordered.rows[0][1]) == "partial");
  CHECK(ordered.partial);
  CHECK_FALSE(ordered.unknown);
  ordered.index.freshness = "current";
  CHECK(ordered.to_envelope().status == cidx::protocol::Status::Partial);

  Result counted = ex.run((site_plan | count()).plan());
  CHECK(counted.scalar == 2);
  CHECK(counted.partial);
  CHECK(counted.unknown);
  counted.index.freshness = "current";
  CHECK(counted.to_envelope().status == cidx::protocol::Status::Unknown);

  Result distinct_result = ex.run((start(codebase()) | view(View::Site) |
                                   nodes() | select({"relation"}) | distinct())
                                      .plan());
  REQUIRE(distinct_result.rows.size() == 1);
  CHECK(std::get<std::string>(distinct_result.rows[0][0]) == "calls");
  CHECK(distinct_result.partial);
  CHECK_FALSE(distinct_result.unknown);
  distinct_result.index.freshness = "current";
  CHECK(distinct_result.to_envelope().status ==
        cidx::protocol::Status::Partial);
}

TEST_CASE("query_plan: default cap recomputes discarded site status") {
  Storage db(":memory:");
  const int64_t component =
      db.add_component("project", "/tmp/capped-site-view");
  const int64_t directory = db.add_directory(component, "src");
  const int64_t file = db.add_file(directory, "capped.cpp");
  const int64_t caller =
      db.add_symbol(make_sym("USR::capped-caller", "caller"));
  const int64_t callee =
      db.add_symbol(make_sym("USR::capped-callee", "callee"));
  const int64_t edge = db.add_edge(make_edge(caller, callee, 1));
  db.raw_db().exec(
      "WITH RECURSIVE lines(line) AS (SELECT 0 UNION ALL SELECT line + 1 "
      "FROM lines WHERE line < 1000) INSERT INTO edge_site "
      "(edge_id,file_id,line,col) SELECT " +
      std::to_string(edge) + "," + std::to_string(file) + ",line,0 FROM lines");
  cidx::EdgeSite unresolved{.edge_id = edge,
                            .file_id = file,
                            .line = 1000,
                            .col = 0,
                            .recv_decl_usr = "USR::missing-declaration"};
  db.add_edge_site(unresolved);

  QueryExecutor ex(db);
  Result result = ex.run((start(codebase()) | view(View::Site) | nodes() |
                          select({"line", "status", "unknown"}))
                             .plan());
  CHECK(result.rows.size() == kDefaultResultCap);
  CHECK(result.truncated);
  CHECK(result.partial);
  CHECK_FALSE(result.unknown);
  result.index.freshness = "current";
  CHECK(result.to_envelope().status == cidx::protocol::Status::Partial);
}

TEST_CASE("query_plan: sites budget boundaries are exact and ordered") {
  for (const int64_t site_count : {kTraverseNodeBudget - 1, kTraverseNodeBudget,
                                   kTraverseNodeBudget + 1}) {
    Storage db(":memory:");
    auto txn = db.transaction();
    const int64_t component = db.add_component("project", "/tmp/budget-view");
    const int64_t directory = db.add_directory(component, "src");
    const int64_t file = db.add_file(directory, "budget.cpp");
    const int64_t caller =
        db.add_symbol(make_sym("USR::budget-caller", "caller"));
    const int64_t callee =
        db.add_symbol(make_sym("USR::budget-callee", "callee"));
    const int64_t edge = db.add_edge(make_edge(caller, callee, 1));
    db.raw_db().exec(
        "WITH RECURSIVE lines(line) AS (SELECT 0 UNION ALL SELECT line + 1 "
        "FROM lines WHERE line < " +
        std::to_string(site_count - 1) +
        ") "
        "INSERT INTO edge_site(edge_id,file_id,line,col) "
        "SELECT " +
        std::to_string(edge) + "," + std::to_string(file) +
        ",line,0 FROM lines");
    txn.commit();

    QueryExecutor ex(db);
    const Result result =
        ex.run((start(codebase()) | view(View::Edge) | nodes() | sites() |
                limit(kTraverseNodeBudget) | count())
                   .plan());
    const auto expected = std::min(site_count, kTraverseNodeBudget);
    CHECK(result.scalar == expected);
    CHECK(result.truncated == (site_count > kTraverseNodeBudget));
  }
}

TEST_CASE("query_plan: devirtualized calls preserve the inherited receiver") {
  Storage db(":memory:");
  {
    auto txn = db.transaction();
    const int64_t component_id = db.add_component("test", "/repo");
    const int64_t directory_id = db.add_directory(component_id, "");
    const int64_t file_id = db.add_file(directory_id, "dispatch.cpp");
    const int64_t base_id =
        db.add_symbol(make_sym("USR::Base", "Base", "struct", "Base"));
    const int64_t x_id = db.add_symbol(make_sym("USR::X", "X", "struct", "X"));
    const int64_t y_id = db.add_symbol(make_sym("USR::Y", "Y", "struct", "Y"));

    Symbol do_something = make_sym("USR::Base::doSomething", "doSomething",
                                   "method", "Base::doSomething(int)");
    do_something.parent_usr = "USR::Base";
    const int64_t do_id = db.add_symbol(do_something);

    Symbol base_print =
        make_sym("USR::Base::print", "print", "method", "Base::print(int)");
    base_print.parent_usr = "USR::Base";
    base_print.is_pure = true;
    const int64_t base_print_id = db.add_symbol(base_print);

    Symbol x_print =
        make_sym("USR::X::print", "print", "method", "X::print(int)");
    x_print.parent_usr = "USR::X";
    const int64_t x_print_id = db.add_symbol(x_print);

    Symbol y_print =
        make_sym("USR::Y::print", "print", "method", "Y::print(int)");
    y_print.parent_usr = "USR::Y";
    const int64_t y_print_id = db.add_symbol(y_print);
    const int64_t main_id =
        db.add_symbol(make_sym("USR::main", "main", "function", "main()"));

    db.add_edge(make_edge(x_id, base_id, 2));
    db.add_edge(make_edge(y_id, base_id, 2));
    db.add_edge(make_edge(x_print_id, base_print_id, 6));
    db.add_edge(make_edge(y_print_id, base_print_id, 6));

    const int64_t outer_edge = db.add_edge(make_edge(main_id, do_id, 1));
    cidx::EdgeSite outer_site;
    outer_site.edge_id = outer_edge;
    outer_site.file_id = file_id;
    outer_site.line = 20;
    outer_site.col = 3;
    outer_site.recv_src_kind = "local";
    outer_site.recv_type_usr = "USR::X";
    outer_site.recv_decl_usr = "USR::main::x";
    outer_site.recv_type_is_value = 1;
    db.add_edge_site(outer_site);

    const int64_t inner_edge = db.add_edge(make_edge(do_id, base_print_id, 1));
    cidx::EdgeSite inner_site;
    inner_site.edge_id = inner_edge;
    inner_site.file_id = file_id;
    inner_site.line = 8;
    inner_site.col = 5;
    inner_site.recv_src_kind = "this";
    inner_site.recv_type_usr = "USR::Base";
    inner_site.recv_decl_usr = "USR::Base";
    db.add_edge_site(inner_site);
    txn.commit();
  }

  const Query plan = start(symbol("USR::main")) |
                     out("calls", 1, 2, TraversalMode::Devirtualized) |
                     select({"usr"});
  CHECK(canonical_json(plan.plan()).contains("\"mode\": \"devirtualized\""));

  QueryExecutor ex(db);
  const Result result = ex.run(plan.plan());
  std::set<std::string> usrs;
  for (const auto &row : result.rows) {
    usrs.insert(std::get<std::string>(row[0]));
  }
  CHECK(usrs.contains("USR::Base::doSomething"));
  CHECK(usrs.contains("USR::X::print"));
  CHECK_FALSE(usrs.contains("USR::Y::print"));
}

TEST_CASE("query_plan: semantic macros lower to quantifier primitives") {
  Seeded s;
  QueryExecutor ex(s.db);

  const auto abstract = ex.run(
      (start(codebase()) | view(View::Entity) | nodes(is_abstract())).plan());
  REQUIRE(abstract.rows.size() == 1);
  CHECK(std::get<int64_t>(abstract.rows[0][0]) == s.S);

  const auto target_sets =
      ex.run((start(symbol("USR::E")) |
              where(inherits_from(any_target({"ClassD", "Missing"}))) |
              select({"usr"}))
                 .plan());
  REQUIRE(target_sets.rows.size() == 1);
  CHECK(std::get<std::string>(target_sets.rows[0][0]) == "USR::E");

  const auto all_target_result =
      ex.run((start(symbol("USR::E")) |
              where(inherits_from(all_targets({"ClassD", "Missing"}))) |
              select({"usr"}))
                 .plan());
  CHECK(all_target_result.rows.empty());

  const std::string explained = cidx::json_out::dumps_indent2(ex.explain(
      (start(symbol("USR::E")) | where(inherits_from("ClassD"))).plan()));
  CHECK(explained.find("\"op\": \"exists\"") != std::string::npos);
  CHECK(explained.find("\"relation\": \"symbol.inherits\"") !=
        std::string::npos);

  const auto instances = ex.run(
      (start(codebase()) | nodes(is_instance()) | select({"usr"})).plan());
  REQUIRE(instances.rows.size() == 1);
  CHECK(std::get<std::string>(instances.rows[0][0]) == "USR::I");
}

TEST_CASE("query_plan: partial relation quantifiers preserve unknown") {
  Seeded s;
  QueryExecutor ex(s.db);

  const auto excluded = ex.run((start(symbol("USR::A")) |
                                where(none("calls", eq("spelling", "missing"))))
                                   .plan());
  CHECK(excluded.rows.empty());

  const auto included = ex.run(
      (start(symbol("USR::A")) |
       where(none("calls", eq("spelling", "missing")), UnknownPolicy::Include))
          .plan());
  REQUIRE(included.rows.size() == 1);
  CHECK(std::get<int64_t>(included.rows[0][0]) == s.A);

  CHECK_THROWS_WITH(ex.run((start(symbol("USR::A")) |
                            where(none("calls", eq("spelling", "missing")),
                                  UnknownPolicy::Error))
                               .plan()),
                    "E_UNKNOWN: predicate evaluation is unknown");

  const auto exact = ex.run((start(symbol("USR::A")) |
                             where(exactly(2, "calls"), UnknownPolicy::Include))
                                .plan());
  REQUIRE(exact.rows.size() == 1);
  CHECK(std::get<int64_t>(exact.rows[0][0]) == s.A);
}

TEST_CASE("query_plan: every relationship quantifier binds and aggregates") {
  Seeded s;
  QueryExecutor ex(s.db);

  const auto exists_plain =
      ex.run((start(symbol("USR::A")) | where(exists("calls"))).plan());
  REQUIRE(exists_plain.rows.size() == 1);
  CHECK(std::get<int64_t>(exists_plain.rows[0][0]) == s.A);

  const auto none_plain =
      ex.run((start(symbol("USR::A")) | where(none("calls"))).plan());
  CHECK(none_plain.rows.empty());

  const auto all_plain = ex.run(
      (start(symbol("USR::A")) | where(all("calls"), UnknownPolicy::Include))
          .plan());
  REQUIRE(all_plain.rows.size() == 1);

  const auto at_least_plain =
      ex.run((start(symbol("USR::A")) | where(at_least(1, "calls"))).plan());
  REQUIRE(at_least_plain.rows.size() == 1);

  const auto exactly_plain =
      ex.run((start(symbol("USR::A")) |
              where(exactly(1, "calls"), UnknownPolicy::Include))
                 .plan());
  REQUIRE(exactly_plain.rows.size() == 1);

  const auto target = eq("spelling", "funcB");
  const auto exists_target =
      ex.run((start(symbol("USR::A")) | where(exists("calls", target))).plan());
  REQUIRE(exists_target.rows.size() == 1);

  const auto none_target =
      ex.run((start(symbol("USR::A")) | where(none("calls", target))).plan());
  CHECK(none_target.rows.empty());

  const auto all_target =
      ex.run((start(symbol("USR::A")) |
              where(all("calls", target), UnknownPolicy::Include))
                 .plan());
  REQUIRE(all_target.rows.size() == 1);

  const auto at_least_target = ex.run(
      (start(symbol("USR::A")) | where(at_least(1, "calls", target))).plan());
  REQUIRE(at_least_target.rows.size() == 1);

  const auto exactly_target =
      ex.run((start(symbol("USR::A")) |
              where(exactly(1, "calls", target), UnknownPolicy::Include))
                 .plan());
  REQUIRE(exactly_target.rows.size() == 1);

  const auto nested =
      ex.run((start(symbol("USR::A")) |
              where(exists("calls", exists("calls", eq("spelling", "funcC")))))
                 .plan());
  REQUIRE(nested.rows.size() == 1);

  const auto recursive_target =
      ex.run((start(symbol("USR::A")) |
              where(exists("calls", eq("spelling", "funcC"), 2, 2)))
                 .plan());
  REQUIRE(recursive_target.rows.size() == 1);

  const auto nested_unknown =
      ex.run((start(symbol("USR::A")) |
              where(exists("calls", exists("calls", eq("spelling", "missing"))),
                    UnknownPolicy::Include))
                 .plan());
  REQUIRE(nested_unknown.rows.size() == 1);

  const auto target_unknown =
      ex.run((start(symbol("USR::E")) |
              where(all("inherits", eq("entity_type", "class")),
                    UnknownPolicy::Include))
                 .plan());
  REQUIRE(target_unknown.rows.size() == 1);
}

// ---------------------------------------------------------------------------
// HSE-31 (CXQ-004): bounded witness paths, ranking, explain
// ---------------------------------------------------------------------------

TEST_CASE("query_plan: path() validation errors") {
  auto code = [](const Plan &p) {
    return error_code([&] { (void)validate(p); });
  };

  CHECK(code((start(symbol("A")) | where(eq("kind", "function")) |
              path(start(symbol("C")), "calls"))
                 .plan()) == "<no-error>"); // path() after where() is valid
  CHECK(code((start(symbol("A")) | path(start(symbol("C")), "bogus")).plan()) ==
        "E_RELATION");
  CHECK(code((start(symbol("A")) | path(start(symbol("C")), "has_parameter"))
                 .plan()) == "E_RELATION"); // typed/virtual relation
  CHECK(code((start(symbol("A")) | path(start(symbol("C")), "calls", 1, 40))
                 .plan()) == "E_DEPTH");
  CHECK(code((start(symbol("A")) | path(start(symbol("C")), "calls", 1, 8, -1))
                 .plan()) == "E_LIMIT"); // negative shortest cap
  CHECK(code((start(symbol("A")) | path(start(entity("C")), "calls")).plan()) ==
        "E_SETOP"); // operand view mismatch
  CHECK(code((start(codebase()) | view(View::Type) | nodes() |
              path(start(symbol("C")), "calls"))
                 .plan()) == "E_VIEW"); // path() requires symbol/entity view
  CHECK(code((start(symbol("A")) | path(start(symbol("C")), "calls") | rank(-1))
                 .plan()) == "E_LIMIT"); // negative top_n
  CHECK(code((start(symbol("A")) | rank()).plan()) ==
        "E_STAGE"); // rank() without a preceding path()
  CHECK(code((start(symbol("A")) | path(start(symbol("C")), "calls") |
              out("calls"))
                 .plan()) == "E_STAGE"); // traversal after path()
  CHECK(code((start(symbol("A")) | path(start(symbol("C")), "calls") |
              order_by({"name"}))
                 .plan()) == "E_STAGE"); // order_by() does not apply to path()
}

TEST_CASE("query_plan: reverse_type_use() validation errors") {
  auto code = [](const Plan &p) {
    return error_code([&] { (void)validate(p); });
  };

  CHECK(code((start(symbol("A")) | reverse_type_use()).plan()) ==
        "E_VIEW"); // requires a type/type_layer node stream
  CHECK(code((start(codebase()) | view(View::Type) | nodes() |
              reverse_type_use(0))
                 .plan()) == "E_DEPTH");
  CHECK(code((start(codebase()) | view(View::Type) | nodes() |
              reverse_type_use(33))
                 .plan()) == "E_DEPTH");
}

TEST_CASE("query_plan: path() finds the shortest witness with sites") {
  Seeded s;
  QueryExecutor ex(s.db);
  const int64_t component = s.db.add_component("project", "/tmp/path-view");
  const int64_t directory = s.db.add_directory(component, "src");
  const int64_t file = s.db.add_file(directory, "path.cpp");
  auto edge_id_query = s.db.raw_db().prepare(
      "SELECT id FROM edge WHERE src_id=? AND dst_id=? AND kind=1");
  edge_id_query.bind(1, s.A);
  edge_id_query.bind(2, s.B);
  REQUIRE(edge_id_query.step());
  cidx::EdgeSite site;
  site.edge_id = edge_id_query.col_int64(0);
  site.file_id = file;
  site.line = 1;
  site.col = 1;
  s.db.add_edge_site(site);

  const auto result = ex.run((start(symbol("USR::A")) |
                              path(start(symbol("USR::C")), "calls", 1, 8, 1))
                                 .plan());
  REQUIRE(result.shape == Shape::Path);
  REQUIRE(result.paths.size() == 1);
  const auto &witness = result.paths[0];
  CHECK(witness.length == 2);
  REQUIRE(witness.steps.size() == 3);
  CHECK(witness.steps[0].node_id == s.A);
  CHECK(witness.steps[0].through.empty());
  CHECK(witness.steps[1].node_id == s.B);
  CHECK(witness.steps[1].through == "calls");
  CHECK(witness.steps[2].node_id == s.C);
  CHECK(witness.steps[2].through == "calls");
  REQUIRE(witness.steps[1].sites.size() == 1);
  CHECK(witness.steps[1].sites[0].line == 1);
  CHECK_FALSE(result.truncated);
}

TEST_CASE("query_plan: path() reports no witness within a narrow window") {
  // A -[calls]-> B -[calls]-> C, but max_depth=1 only reaches B, which is
  // itself expandable (B -[calls]-> C exists one hop further). This is a
  // finite-depth exhaustion (docs/query-plan.md), not a proven negative, so
  // it must be reported truncated even though no witness is returned.
  Seeded s;
  QueryExecutor ex(s.db);
  const auto result = ex.run(
      (start(symbol("USR::A")) | path(start(symbol("USR::C")), "calls", 1, 1))
          .plan());
  CHECK(result.paths.empty());
  CHECK(result.truncated);
}

TEST_CASE("query_plan: path() ties are broken by ascending node-id order") {
  Storage db(":memory:");
  const int64_t start_id = db.add_symbol(make_sym("USR::S", "s"));
  const int64_t left = db.add_symbol(make_sym("USR::L", "l"));
  const int64_t right = db.add_symbol(make_sym("USR::R", "r"));
  const int64_t target = db.add_symbol(make_sym("USR::T", "t"));
  db.add_edge(make_edge(start_id, left, 1));
  db.add_edge(make_edge(start_id, right, 1));
  db.add_edge(make_edge(left, target, 1));
  db.add_edge(make_edge(right, target, 1));

  QueryExecutor ex(db);
  const auto result = ex.run(
      (start(symbol("USR::S")) | path(start(symbol("USR::T")), "calls", 1, 8))
          .plan());
  REQUIRE(result.paths.size() == 2);
  CHECK(result.paths[0].steps[1].node_id == std::min(left, right));
  CHECK(result.paths[1].steps[1].node_id == std::max(left, right));

  const auto ranked =
      ex.run((start(symbol("USR::S")) |
              path(start(symbol("USR::T")), "calls", 1, 8) | rank(1))
                 .plan());
  REQUIRE(ranked.paths.size() == 1);
  CHECK(ranked.paths[0].steps[1].node_id == std::min(left, right));
}

TEST_CASE("query_plan: path() count/distinct/limit apply to witnesses") {
  Seeded s;
  QueryExecutor ex(s.db);
  const auto counted =
      ex.run((start(symbol("USR::A")) |
              path(start(symbol("USR::C")), "calls", 1, 8) | count())
                 .plan());
  CHECK(counted.shape == Shape::Scalar);
  CHECK(counted.scalar == 1);

  const auto limited =
      ex.run((start(symbol("USR::A")) |
              path(start(symbol("USR::C")), "calls", 1, 8) | limit(1))
                 .plan());
  CHECK(limited.paths.size() == 1);

  const auto deduped =
      ex.run((start(symbol("USR::A")) |
              path(start(symbol("USR::C")), "calls", 1, 8) | distinct())
                 .plan());
  CHECK(deduped.paths.size() == 1);
}

TEST_CASE("query_plan: path() level budget is exact at the boundary and "
          "truncates without a witness") {
  // A single start node fans out to `fanout` children in one BFS level: the
  // level-discovery query for that one level returns `fanout` rows from a
  // single chunk. Before the fix this only checked kPathNodeBudget AFTER the
  // whole level's rows were read into parent_of; now the row loop itself
  // breaks the moment the budget is exceeded, so the level (and hence any
  // witness reconstruction from it) is abandoned without waiting to finish
  // reading a level far larger than the budget.
  for (const int64_t fanout : {kPathNodeBudget, kPathNodeBudget + 1}) {
    Storage db(":memory:");
    auto txn = db.transaction();
    const int64_t start_id = db.add_symbol(make_sym("USR::fan-start", "start"));
    int64_t first_child = -1;
    for (int64_t i = 0; i < fanout; ++i) {
      const int64_t child = db.add_symbol(make_sym(
          "USR::fan-child-" + std::to_string(i), "child" + std::to_string(i)));
      db.add_edge(make_edge(start_id, child, 1));
      if (i == 0) {
        first_child = child;
      }
    }
    txn.commit();

    QueryExecutor ex(db);
    const auto result =
        ex.run((start(symbol("USR::fan-start")) |
                path(start(symbol("USR::fan-child-0")), "calls", 1, 1))
                   .plan());
    if (fanout > kPathNodeBudget) {
      CHECK(result.paths.empty());
      CHECK(result.truncated);
    } else {
      REQUIRE(result.paths.size() == 1);
      CHECK(result.paths[0].steps.back().node_id == first_child);
      CHECK_FALSE(result.truncated);
    }
  }
}

TEST_CASE("query_plan: reverse_type_use() retains every typed layer") {
  Storage db(":memory:");
  const int64_t owner = db.add_symbol(make_sym("USR::owner", "owner"));
  db.raw_db().exec(
      "INSERT INTO type_node(type_key,spelling,kind,extent) VALUES "
      "('A4(b:int)','int[4]',8,'4'),('b:int','int',1,NULL)");
  auto ids = db.raw_db().prepare("SELECT id FROM type_node ORDER BY id");
  REQUIRE(ids.step());
  const int64_t array_id = ids.col_int64(0);
  REQUIRE(ids.step());
  const int64_t int_id = ids.col_int64(0);
  db.add_type_edge(array_id, 2, 0, int_id); // element_type
  db.add_symbol_type(owner, 1, array_id);   // returns
  db.raw_db().exec(
      "INSERT INTO parameter(owner_id,position,pack_index,name,type_id) "
      "VALUES (1,0,-1,'value',2)"); // direct parameter use of int_id

  QueryExecutor ex(db);
  const auto result =
      ex.run((start(codebase()) | view(View::Type) | nodes() |
              where(eq("type_key", "b:int")) | reverse_type_use())
                 .plan());
  REQUIRE(result.shape == Shape::Path);
  REQUIRE(result.paths.size() == 2);

  const auto direct = std::ranges::find_if(
      result.paths, [](const PathWitness &w) { return w.length == 1; });
  REQUIRE(direct != result.paths.end());
  CHECK(direct->steps.back().domain == "parameter");
  CHECK(direct->steps.back().node_id == owner);

  const auto nested = std::ranges::find_if(
      result.paths, [](const PathWitness &w) { return w.length == 2; });
  REQUIRE(nested != result.paths.end());
  REQUIRE(nested->steps.size() == 3);
  CHECK(nested->steps[0].node_id == int_id);
  CHECK(nested->steps[1].node_id == array_id);
  CHECK(nested->steps[1].through == "element_type");
  CHECK(nested->steps[2].domain == "symbol");
  CHECK(nested->steps[2].node_id == owner);
}

TEST_CASE("query_plan: explain() reports budgets, shape, and input relations") {
  Seeded s;
  SqliteQueryReadAdapter read(s.db);
  Executor executor(read);
  const auto plan = (start(symbol("USR::A")) |
                     path(start(symbol("USR::C")), "calls", 1, 8, 1))
                        .plan();
  const auto explained = executor.explain(plan);
  const std::string rendered = cidx::json_out::dumps_indent2(explained);
  CHECK(rendered.find("\"execution_shape\": \"path\"") != std::string::npos);
  CHECK(rendered.find("\"traverse_node_budget\": 10000") != std::string::npos);
  CHECK(rendered.find("\"path_node_budget\": 10000") != std::string::npos);
  CHECK(rendered.find("\"default_result_cap\": 1000") != std::string::npos);
  CHECK(rendered.find("\"relation\": \"symbol.calls\"") != std::string::npos);
  CHECK(rendered.find("\"completeness\": \"partial\"") != std::string::npos);
  CHECK(rendered.find("\"partial_inputs\": true") != std::string::npos);
}

// ---------------------------------------------------------------------------
// PR #69 review round: path-window BFS, evidence caps, owner identity,
// explain() completeness/freshness regressions
// ---------------------------------------------------------------------------

TEST_CASE(
    "query_plan: path() finds an in-window witness reached only at a later "
    "depth") {
  // S->T is depth 1 (out of the [2,2] window); S->M->T is the only depth-2
  // route. A permanent cross-level visited set would mark T visited at
  // depth 1 and discard the depth-2 rediscovery via M.
  Storage db(":memory:");
  const int64_t s = db.add_symbol(make_sym("USR::S", "s"));
  const int64_t t = db.add_symbol(make_sym("USR::T", "t"));
  const int64_t m = db.add_symbol(make_sym("USR::M", "m"));
  db.add_edge(make_edge(s, t, 1));
  db.add_edge(make_edge(s, m, 1));
  db.add_edge(make_edge(m, t, 1));

  QueryExecutor ex(db);
  const auto result = ex.run(
      (start(symbol("USR::S")) | path(start(symbol("USR::T")), "calls", 2, 2))
          .plan());
  REQUIRE(result.paths.size() == 1);
  const auto &witness = result.paths[0];
  CHECK(witness.length == 2);
  REQUIRE(witness.steps.size() == 3);
  CHECK(witness.steps[0].node_id == s);
  CHECK(witness.steps[1].node_id == m);
  CHECK(witness.steps[2].node_id == t);
  CHECK_FALSE(result.truncated);
}

TEST_CASE(
    "query_plan: path() reports truncation when a hop has more sites than "
    "the cap") {
  Storage db(":memory:");
  const int64_t component =
      db.add_component("project", "/tmp/path-evidence-cap");
  const int64_t directory = db.add_directory(component, "src");
  const int64_t file = db.add_file(directory, "cap.cpp");
  const int64_t a = db.add_symbol(make_sym("USR::CapA", "a"));
  const int64_t b = db.add_symbol(make_sym("USR::CapB", "b"));
  const int64_t edge = db.add_edge(make_edge(a, b, 1));
  db.raw_db().exec(
      "WITH RECURSIVE lines(line) AS (SELECT 0 UNION ALL SELECT line + 1 "
      "FROM lines WHERE line < 1000) INSERT INTO edge_site "
      "(edge_id,file_id,line,col) SELECT " +
      std::to_string(edge) + "," + std::to_string(file) + ",line,0 FROM lines");

  QueryExecutor ex(db);
  const auto result = ex.run((start(symbol("USR::CapA")) |
                              path(start(symbol("USR::CapB")), "calls", 1, 1))
                                 .plan());
  REQUIRE(result.paths.size() == 1);
  const auto &witness = result.paths[0];
  REQUIRE(witness.steps.size() == 2);
  CHECK(witness.steps[1].sites.size() == kDefaultResultCap);
  CHECK(witness.steps[1].status == "partial");
  CHECK(witness.status == "partial");
  CHECK(result.truncated); // incomplete evidence must never look complete
}

TEST_CASE(
    "query_plan: reverse_type_use() preserves distinct parameter slots on "
    "one owner") {
  Storage db(":memory:");
  const int64_t owner =
      db.add_symbol(make_sym("USR::two_params", "two_params"));
  db.raw_db().exec("INSERT INTO type_node(type_key,spelling,kind,extent) "
                   "VALUES ('b:int','int',1,NULL)");
  auto ids = db.raw_db().prepare("SELECT id FROM type_node ORDER BY id");
  REQUIRE(ids.step());
  const int64_t int_id = ids.col_int64(0);
  for (const int64_t position : {0, 1}) {
    auto stmt = db.raw_db().prepare(
        "INSERT INTO parameter(owner_id,position,pack_index,name,type_id) "
        "VALUES (?,?,-1,'p',?)");
    stmt.bind(1, owner);
    stmt.bind(2, position);
    stmt.bind(3, int_id);
    stmt.step_done();
  }

  QueryExecutor ex(db);
  const auto result =
      ex.run((start(codebase()) | view(View::Type) | nodes() |
              where(eq("type_key", "b:int")) | reverse_type_use())
                 .plan());
  REQUIRE(result.paths.size() == 2);
  std::vector<int64_t> positions;
  for (const auto &witness : result.paths) {
    REQUIRE(witness.steps.size() == 2);
    CHECK(witness.steps[1].node_id == owner);
    CHECK(witness.steps[1].domain == "parameter");
    CHECK(witness.steps[1].through == "parameter");
    positions.push_back(witness.steps[1].position);
  }
  std::ranges::sort(positions);
  CHECK(positions == std::vector<int64_t>{0, 1});
}

TEST_CASE("query_plan: explain() reports reverse_type_use()'s real inputs") {
  Storage db(":memory:");
  db.raw_db().exec("INSERT INTO type_node(type_key,spelling,kind,extent) "
                   "VALUES ('b:int','int',1,NULL)");

  SqliteQueryReadAdapter read(db);
  Executor executor(read);
  const auto plan =
      (start(codebase()) | view(View::Type) | nodes() | reverse_type_use())
          .plan();
  const auto explained = executor.explain(plan);
  const std::string rendered = cidx::json_out::dumps_indent2(explained);
  CHECK(rendered.find("\"execution_shape\": \"path\"") != std::string::npos);
  CHECK(rendered.find("\"relation\": \"type.has_type_edge\"") !=
        std::string::npos);
  CHECK(rendered.find("\"relation\": \"type.canonical_id\"") !=
        std::string::npos);
  CHECK(rendered.find("\"relation\": \"symbol.of_type\"") != std::string::npos);
  CHECK(rendered.find("\"relation\": \"parameter.of_type\"") !=
        std::string::npos);
  CHECK(rendered.find("\"relation\": \"template_parameter.of_type\"") !=
        std::string::npos);
  CHECK(rendered.find("\"relation\": \"template_argument.of_type\"") !=
        std::string::npos);
  CHECK(rendered.find("\"partial_inputs\": true") != std::string::npos);
  CHECK(rendered.find("\"unknown_capable_inputs\": true") != std::string::npos);
}

TEST_CASE(
    "query_plan: explain() exposes expected vs indexed source revision on a "
    "stale index") {
  const std::string dir = make_temp_dir();
  const std::string source = dir + "/answer.cpp";
  {
    std::ofstream out(source);
    out << "int answer = 1;\n";
  }
  Storage db(":memory:");
  db.add_component("fixture", dir);
  const auto file_id =
      db.add_file_path(source, std::nullopt, cidx::md5_of(source));
  db.mark_file_indexed(file_id, std::nullopt, cidx::md5_of(source));
  db.stamp_index_identity();

  const auto current = db.index_identity();
  REQUIRE(current.freshness == "current");
  REQUIRE(current.source_revision.has_value());
  REQUIRE(current.expected_source_revision.has_value());
  REQUIRE(current.expected_index_config_fingerprint.has_value());
  if (current.source_revision && current.expected_source_revision) {
    CHECK(*current.source_revision == *current.expected_source_revision);
  }

  // Change the checkout after stamping: the persisted identity now
  // disagrees with what the current source hashes to.
  {
    std::ofstream out(source);
    out << "int answer = 2;\n";
  }
  const auto stale = db.index_identity();
  CHECK(stale.freshness == "stale");
  REQUIRE(stale.source_revision.has_value());
  REQUIRE(stale.expected_source_revision.has_value());
  if (stale.source_revision && stale.expected_source_revision) {
    CHECK(*stale.source_revision != *stale.expected_source_revision);
  }
  if (stale.source_revision && current.source_revision) {
    CHECK(*stale.source_revision == *current.source_revision); // unchanged
  }

  SqliteQueryReadAdapter read(db);
  Executor executor(read);
  const auto explained = executor.explain((start(codebase()) | nodes()).plan());
  const std::string rendered = cidx::json_out::dumps_indent2(explained);
  CHECK(rendered.find("\"freshness\": \"stale\"") != std::string::npos);
  CHECK(rendered.find("\"expected_source_revision\"") != std::string::npos);
  CHECK(rendered.find("\"expected_source_fingerprint\"") != std::string::npos);
  CHECK(rendered.find("\"expected_index_config_fingerprint\"") !=
        std::string::npos);
}

// ---------------------------------------------------------------------------
// PR #69 review round 2: intermediate type_edge identity and path
// distinctness
// ---------------------------------------------------------------------------

TEST_CASE(
    "query_plan: reverse_type_use() retains each intermediate type_edge's "
    "position") {
  // One function type F has two `param_type` edges to the same pointee
  // (int) at positions 0 and 1. Both climbs share (parent=F,
  // through=param_type); without `position` they would serialize as
  // byte-identical witnesses.
  Storage db(":memory:");
  const int64_t owner =
      db.add_symbol(make_sym("USR::two_int_params_fn", "two_int_params_fn"));
  db.raw_db().exec(
      "INSERT INTO type_node(type_key,spelling,kind,extent) VALUES "
      "('fn(int,int)','void(int,int)',9,NULL),('b:int','int',1,NULL)");
  auto ids = db.raw_db().prepare("SELECT id FROM type_node ORDER BY id");
  REQUIRE(ids.step());
  const int64_t fn_id = ids.col_int64(0);
  REQUIRE(ids.step());
  const int64_t int_id = ids.col_int64(0);
  db.add_type_edge(fn_id, 5, 0, int_id); // param_type position 0
  db.add_type_edge(fn_id, 5, 1, int_id); // param_type position 1
  db.add_symbol_type(owner, 1, fn_id);   // returns

  QueryExecutor ex(db);
  const auto result =
      ex.run((start(codebase()) | view(View::Type) | nodes() |
              where(eq("type_key", "b:int")) | reverse_type_use())
                 .plan());
  REQUIRE(result.paths.size() == 2);
  std::vector<int64_t> positions;
  for (const auto &witness : result.paths) {
    CHECK(witness.length == 2);
    REQUIRE(witness.steps.size() == 3);
    CHECK(witness.steps[0].node_id == int_id);
    CHECK(witness.steps[1].node_id == fn_id);
    CHECK(witness.steps[1].through == "param_type");
    positions.push_back(witness.steps[1].position);
    CHECK(witness.steps[2].node_id == owner);
  }
  std::ranges::sort(positions);
  CHECK(positions == std::vector<int64_t>{0, 1});

  // distinct() must not collapse these two structurally-different witnesses
  // just because the F-hop's `through` label is the same for both.
  const auto deduped =
      ex.run((start(codebase()) | view(View::Type) | nodes() |
              where(eq("type_key", "b:int")) | reverse_type_use() | distinct())
                 .plan());
  CHECK(deduped.paths.size() == 2);
}

TEST_CASE(
    "query_plan: path() distinct() compares full typed slot identity, not "
    "just node id") {
  // One owner has two `int` parameters at positions 0 and 1. Both witnesses
  // share (node_id, through, inbound) on the final step -- only position
  // distinguishes them -- so a distinctness key that ignores position would
  // wrongly collapse them to one.
  Storage db(":memory:");
  const int64_t owner =
      db.add_symbol(make_sym("USR::two_params_distinct", "two_params"));
  db.raw_db().exec("INSERT INTO type_node(type_key,spelling,kind,extent) "
                   "VALUES ('b:int','int',1,NULL)");
  auto ids = db.raw_db().prepare("SELECT id FROM type_node ORDER BY id");
  REQUIRE(ids.step());
  const int64_t int_id = ids.col_int64(0);
  for (const int64_t position : {0, 1}) {
    auto stmt = db.raw_db().prepare(
        "INSERT INTO parameter(owner_id,position,pack_index,name,type_id) "
        "VALUES (?,?,-1,'p',?)");
    stmt.bind(1, owner);
    stmt.bind(2, position);
    stmt.bind(3, int_id);
    stmt.step_done();
  }

  QueryExecutor ex(db);
  const auto before_distinct =
      ex.run((start(codebase()) | view(View::Type) | nodes() |
              where(eq("type_key", "b:int")) | reverse_type_use())
                 .plan());
  REQUIRE(before_distinct.paths.size() == 2);

  const auto after_distinct =
      ex.run((start(codebase()) | view(View::Type) | nodes() |
              where(eq("type_key", "b:int")) | reverse_type_use() | distinct())
                 .plan());
  REQUIRE(after_distinct.paths.size() == 2);
  std::vector<int64_t> positions;
  for (const auto &witness : after_distinct.paths) {
    REQUIRE(witness.steps.size() == 2);
    positions.push_back(witness.steps[1].position);
  }
  std::ranges::sort(positions);
  CHECK(positions == std::vector<int64_t>{0, 1});
}

// ---------------------------------------------------------------------------
// PR #69 review round 3: finite-depth exhaustion, incomplete chain
// reconstruction, symbol-owner provenance, and a total rank key
// ---------------------------------------------------------------------------

TEST_CASE(
    "query_plan: path() reports truncation when the depth window cuts off "
    "an expandable frontier") {
  // S -> M -> T, but max_depth=1 only lets the BFS reach M. M is not a
  // target, and M is itself expandable (M -> T exists one hop further), so
  // this "no witness" is a finite-depth exhaustion, not proof no path
  // exists.
  Storage db(":memory:");
  const int64_t s = db.add_symbol(make_sym("USR::FDE_S", "s"));
  const int64_t m = db.add_symbol(make_sym("USR::FDE_M", "m"));
  const int64_t t = db.add_symbol(make_sym("USR::FDE_T", "t"));
  db.add_edge(make_edge(s, m, 1));
  db.add_edge(make_edge(m, t, 1));

  QueryExecutor ex(db);
  const auto result = ex.run((start(symbol("USR::FDE_S")) |
                              path(start(symbol("USR::FDE_T")), "calls", 1, 1))
                                 .plan());
  CHECK(result.paths.empty());
  CHECK(result.truncated);
}

TEST_CASE("query_plan: path() does not report truncation when a start's window "
          "is genuinely exhausted") {
  // S has no outgoing edges at all: the BFS dies at depth 1 with an empty
  // frontier, a real dead end, not a depth-limit cutoff.
  Storage db(":memory:");
  db.add_symbol(make_sym("USR::FDE_Dead_S", "s"));
  db.add_symbol(make_sym("USR::FDE_Dead_T", "t"));

  QueryExecutor ex(db);
  const auto result =
      ex.run((start(symbol("USR::FDE_Dead_S")) |
              path(start(symbol("USR::FDE_Dead_T")), "calls", 1, 4))
                 .plan());
  CHECK(result.paths.empty());
  CHECK_FALSE(result.truncated);
}

TEST_CASE("query_plan: path() never serializes a chain reconstruction that was "
          "cut short by the witness cap") {
  // A depth-3 layered DAG where the middle layer alone has far more than
  // kDefaultResultCap predecessor combinations: reconstruction from the
  // single target must either complete every emitted witness back to the
  // real source, or emit none -- never a short chain that silently starts
  // mid-path.
  Storage db(":memory:");
  const int64_t source = db.add_symbol(make_sym("USR::Chain_Source", "src"));
  const int64_t target = db.add_symbol(make_sym("USR::Chain_Target", "tgt"));
  constexpr int kFanout = 1200; // > kDefaultResultCap
  std::vector<int64_t> middle;
  middle.reserve(kFanout);
  for (int i = 0; i < kFanout; ++i) {
    const int64_t node = db.add_symbol(make_sym(
        "USR::Chain_Mid_" + std::to_string(i), "mid" + std::to_string(i)));
    db.add_edge(make_edge(source, node, 1));
    db.add_edge(make_edge(node, target, 1));
    middle.push_back(node);
  }

  QueryExecutor ex(db);
  const auto result =
      ex.run((start(symbol("USR::Chain_Source")) |
              path(start(symbol("USR::Chain_Target")), "calls", 2, 2))
                 .plan());
  CHECK(result.truncated);
  for (const auto &witness : result.paths) {
    REQUIRE(witness.length == 2);
    REQUIRE(witness.steps.size() == 3); // source, middle, target -- complete
    CHECK(witness.steps[0].node_id == source);
    CHECK(witness.steps[2].node_id == target);
  }
}

TEST_CASE("query_plan: reverse_type_use() reports a direct symbol owner's "
          "declaration site") {
  Storage db(":memory:");
  const int64_t component =
      db.add_component("project", "/tmp/owner-site-provenance");
  const int64_t directory = db.add_directory(component, "src");
  const int64_t file = db.add_file(directory, "owner_site.cpp");
  Symbol owner_sym = make_sym("USR::OwnerWithSite", "owner_with_site");
  owner_sym.file_id = file;
  owner_sym.line = 41;
  owner_sym.col = 7;
  const int64_t owner = db.add_symbol(owner_sym);
  db.raw_db().exec("INSERT INTO type_node(type_key,spelling,kind,extent) "
                   "VALUES ('b:owner_site','int',1,NULL)");
  auto ids = db.raw_db().prepare(
      "SELECT id FROM type_node WHERE type_key='b:owner_site'");
  REQUIRE(ids.step());
  const int64_t type_id = ids.col_int64(0);
  db.add_symbol_type(owner, 2, type_id); // of_type

  QueryExecutor ex(db);
  const auto result =
      ex.run((start(codebase()) | view(View::Type) | nodes() |
              where(eq("type_key", "b:owner_site")) | reverse_type_use())
                 .plan());
  REQUIRE(result.paths.size() == 1);
  const auto &witness = result.paths[0];
  REQUIRE(witness.steps.size() == 2);
  const auto &owner_step = witness.steps.back();
  CHECK(owner_step.node_id == owner);
  CHECK(owner_step.domain == "symbol");
  CHECK(owner_step.through == "of_type");
  REQUIRE(owner_step.sites.size() == 1);
  const auto &site = owner_step.sites[0];
  REQUIRE(site.file_id.has_value());
  REQUIRE(site.line.has_value());
  REQUIRE(site.col.has_value());
  if (site.file_id && site.line && site.col) {
    CHECK(*site.file_id == file);
    CHECK(*site.line == 41);
    CHECK(*site.col == 7);
  }
}

TEST_CASE(
    "query_plan: reverse_type_use() | rank() orders witnesses by the full "
    "typed-step identity, not just (node_id, position, pack_index)") {
  // Two type_edge hops (member_component, then member_owner) both land on
  // M at position 0 -- an identical (node_id, position, pack_index) key --
  // but they are structurally distinct witnesses. Only `through` tells
  // them apart, so the rank key must include it to be total.
  Storage db(":memory:");
  const int64_t owner_sym = db.add_symbol(make_sym("USR::RankOwner", "owner"));
  db.raw_db().exec(
      "INSERT INTO type_node(type_key,spelling,kind,extent) VALUES "
      "('b:rank_a','A',1,NULL),('b:rank_m','M',1,NULL)");
  auto ids = db.raw_db().prepare(
      "SELECT id FROM type_node WHERE type_key IN ('b:rank_a','b:rank_m') "
      "ORDER BY type_key");
  REQUIRE(ids.step());
  const int64_t a_id = ids.col_int64(0); // 'b:rank_a' sorts first
  REQUIRE(ids.step());
  const int64_t m_id = ids.col_int64(0);

  db.add_type_edge(m_id, 8, 0, a_id);     // member_component, inserted first
  db.add_type_edge(m_id, 7, 0, a_id);     // member_owner, inserted second
  db.add_symbol_type(owner_sym, 2, m_id); // of_type

  QueryExecutor ex(db);
  const auto result = ex.run((start(codebase()) | view(View::Type) | nodes() |
                              where(eq("type_key", "b:rank_a")) |
                              reverse_type_use() | distinct() | rank())
                                 .plan());
  REQUIRE(result.paths.size() == 2);
  REQUIRE(result.paths[0].steps.size() == 3);
  REQUIRE(result.paths[1].steps.size() == 3);
  // Both witnesses share the same node-id sequence (a_id, m_id, owner_sym)
  // and the same (position, pack_index); only the M-hop's `through` label
  // differs, and it must sort ascending: "member_component" < "member_owner".
  CHECK(result.paths[0].steps[1].node_id == m_id);
  CHECK(result.paths[1].steps[1].node_id == m_id);
  CHECK(result.paths[0].steps[1].through == "member_component");
  CHECK(result.paths[1].steps[1].through == "member_owner");
}
