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
