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

#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "query/exec.hpp"
#include "query/plan.hpp"
#include "storage/records.hpp"
#include "storage/storage.hpp"

using namespace cidx::query;
using cidx::Storage;
using cidx::Symbol;

namespace {

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
       where(in_list("kind", {"class", "interface"})) |
       select({"name", "usr"}) | limit(100))
          .plan();
  plans["codebase_abstract"] =
      (start(codebase()) | view(View::Entity) |
       nodes(eq("kind", "abstract_class")) |
       where(all_of({eq("is_definition", true),
                     not_(glob("name", "*Legacy*"))})) |
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
  plans["qualified_relation"] =
      (start(symbol("Widget")) | out("entity.uses") | view(View::Entity) |
       in_("generalizes", 1, 4))
          .plan();
  plans["boolean_normalization"] =
      (start(codebase()) |
       nodes(all_of({all_of({eq("kind", "class"), eq("is_static", false)}),
                     not_(not_(ne("spelling", "x")))})) |
       count())
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
//   symbols: funcA(fn) -> funcB(fn) -> funcC(fn); ClassD/ClassE(class)
//   calls:   A->B, B->C          uses: A->C
//   inherits: E->D (symbol layer)
//   entity:  D,E are entity_node kind=1(class); entity uses: D->E;
//            generalizes: E->D
struct Seeded {
  Storage db;
  int64_t A = -1, B = -1, C = -1, D = -1, E = -1;

  Seeded() : db(":memory:") {
    A = db.add_symbol(make_sym("USR::A", "funcA", "function", "ns::funcA"));
    B = db.add_symbol(make_sym("USR::B", "funcB", "function"));
    C = db.add_symbol(make_sym("USR::C", "funcC", "function"));
    D = db.add_symbol(make_sym("USR::D", "ClassD", "class"));
    E = db.add_symbol(make_sym("USR::E", "ClassE", "class"));
    db.add_edge(make_edge(A, B, 1)); // calls
    db.add_edge(make_edge(B, C, 1)); // calls
    db.add_edge(make_edge(A, C, 7)); // uses
    db.add_edge(make_edge(E, D, 2)); // inherits
    // Layer-1: D and E are class entities; D entity-uses E; E generalizes D.
    auto ins = db.raw_db().prepare(
        "INSERT INTO entity_node (id, kind) VALUES (?, 1), (?, 1)");
    ins.bind(1, D);
    ins.bind(2, E);
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

// ---------------------------------------------------------------------------
// Q2: normalization
// ---------------------------------------------------------------------------
TEST_CASE("query_plan: normalization qualifies relations and flattens booleans") {
  // Bare relation in the active view qualifies to the layer name.
  Plan p = (start(symbol("A")) | out("calls")).plan();
  Plan n = validate(p);
  CHECK(n.stages[0].relation == "symbol.calls");

  // entity() source starts in the entity view.
  Plan pe = (start(entity("X")) | out("uses")).plan();
  CHECK(validate(pe).stages[0].relation == "entity.uses");

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
  CHECK(code((start(codebase()) | view(View::Entity) |
              nodes(eq("kind", "struct")))
                 .plan()) == "E_KIND"); // struct is not an entity kind
  CHECK(code((start(symbol("A")) | limit(0)).plan()) == "E_LIMIT");
  CHECK(code((start(symbol("A")) |
              union_(start(entity("B")) | out("uses")))
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
  CHECK(code((start(symbol("A")) | select({"name"}) |
              order_by({"usr"}))
                 .plan()) == "E_FIELD"); // order_by outside selection
}

// ---------------------------------------------------------------------------
// Q4: execution
// ---------------------------------------------------------------------------
TEST_CASE("query_plan: source resolution by usr / qual_name / spelling") {
  Seeded s;
  Executor ex(s.db);

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
  Executor ex(s.db);

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
  Executor ex(s.db);

  auto r = ex.run((start(codebase()) | nodes(eq("kind", "class")) |
                   select({"name", "kind", "usr"}))
                      .plan());
  REQUIRE(r.rows.size() == 2);
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
  Executor ex(s.db);

  // calls(1..2) = {B, C}; uses = {C}; union keeps multiplicity (3 rows).
  auto u = ex.run((start(symbol("USR::A")) | out("calls", 1, 2) |
                   union_(start(symbol("USR::A")) | out("uses")))
                      .plan());
  CHECK(u.rows.size() == 3);

  // distinct dedups to {B, C}.
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
  auto c = ex.run((start(symbol("USR::A")) | out("calls", 1, 2) | count())
                      .plan());
  CHECK(c.shape == Shape::Scalar);
  CHECK(c.scalar == 2);
}

TEST_CASE("query_plan: entity view traversal") {
  Seeded s;
  Executor ex(s.db);

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
  Executor ex(s.db);

  auto r = ex.run((start(codebase()) | nodes(eq("kind", "function")) |
                   select({"spelling"}) | order_by({"spelling"}) | limit(2))
                      .plan());
  REQUIRE(r.rows.size() == 2);
  CHECK(std::get<std::string>(r.rows[0][0]) == "funcA");
  CHECK(std::get<std::string>(r.rows[1][0]) == "funcB");
  CHECK(!r.truncated); // explicit limit is requested cardinality, not truncation

  // Default fields for a node stream: id, usr, name, kind.
  auto d = ex.run((start(symbol("USR::A")) | out("calls")).plan());
  CHECK(d.fields == std::vector<std::string>{"id", "usr", "name", "kind"});

  // Result JSON shape.
  auto j = cidx::json_out::dumps_indent2(d.to_json());
  CHECK(j.find("\"shape\": \"nodes\"") != std::string::npos);
  CHECK(j.find("\"view\": \"symbol\"") != std::string::npos);
  CHECK(j.find("\"count\": 1") != std::string::npos);
  CHECK(j.find("\"funcB\"") != std::string::npos);
}

TEST_CASE("query_plan: default result cap reports truncation") {
  Storage db(":memory:");
  {
    auto txn = db.transaction();
    for (int i = 0; i < 1200; ++i) {
      db.add_symbol(make_sym("USR::f" + std::to_string(i),
                             "f" + std::to_string(i)));
    }
    txn.commit(); // the destructor is ROLLBACK-only (R2)
  }
  Executor ex(db);
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
