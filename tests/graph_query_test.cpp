// graph_query_test — Hermetic unit tests for the M6 graph query layer.
//
// Category: hermetic (label "default") — no libclang, no filesystem.
// Uses in-memory SQLite DBs seeded via Storage write methods.
//
// Covers:
//   G1  get_by_id / get_by_usr / find on empty DB
//   G2  get_by_id / get_by_usr on a seeded DB
//   G3  edges_in / edges_out direction
//   G4  count fallback (R3): ecount=0 -> rawcount, else 1
//   G5  references() = calls + uses in
//   G6  walk() BFS depth and max_nodes
//   G7  reaches() shortest path and null when unreachable
//   G8  bases() / subclasses() / members()
//   G9  dispatch_targets() insertion order
//   G10 kind_ids() valid and invalid kind names
//   G11 Sym.is_stub(), Sym.loc(), Sym.to_dict() key order
//   G12 emit_edges text header + count suffix + trailer
//   G13 emit_syms text header + depth suffix + trailer
//   G14 aliased_by() inverse alias traversal

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include <sqlite3.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "graph/emit.hpp"
#include "graph/query.hpp"
#include "graph/records.hpp"
#include "query/exec.hpp"
#include "storage/records.hpp"
#include "storage/storage.hpp"

using cidx::Storage;
using cidx::Symbol;
using cidx::graph::emit_edges;
using cidx::graph::emit_syms;
using cidx::graph::GraphQuery;
using cidx::graph::Site;
using cidx::graph::Sym;
using cidx::graph::Traversal;

namespace {
int count_sqlite_progress(void *counter) {
  ++*static_cast<int *>(counter);
  return 0;
}
} // namespace

namespace {

// Minimal symbol builder for in-memory test seeding.
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

// Helper: build a storage Edge record for add_edge.
cidx::Edge make_edge(int64_t src, int64_t dst, int64_t kind,
                     int64_t count = 1) {
  cidx::Edge e;
  e.src_id = src;
  e.dst_id = dst;
  e.kind = kind;
  e.count = count;
  return e;
}

template <typename Operation>
void require_production_plan(const char *name, Operation &&operation) {
  std::vector<cidx::query::Plan> captured;
  {
    cidx::query::ScopedPlanObserver observer(
        [&](const cidx::query::Plan &plan) { captured.push_back(plan); });
    std::forward<Operation>(operation)();
  }
  REQUIRE_MESSAGE(!captured.empty(), name);
  for (const auto &plan : captured) {
    const auto normalized = cidx::query::validate(plan);
    const bool owns_semantics = std::ranges::any_of(
        normalized.stages, [](const cidx::query::Stage &stage) {
          using Op = cidx::query::StageOp;
          return stage.op == Op::Nodes || stage.op == Op::Where ||
                 stage.op == Op::Out || stage.op == Op::In ||
                 stage.op == Op::ChangeView || stage.op == Op::Sites ||
                 stage.op == Op::Select || stage.op == Op::OrderBy ||
                 stage.op == Op::Limit || stage.op == Op::Count;
        });
    CHECK_MESSAGE(owns_semantics, name);
  }
}

// Helper: build a storage EdgeSite record for add_edge_site.
cidx::EdgeSite make_edge_site(int64_t eid,
                              std::optional<int64_t> line = std::nullopt,
                              std::optional<int64_t> col = std::nullopt) {
  cidx::EdgeSite s;
  s.edge_id = eid;
  s.line = line;
  s.col = col;
  return s;
}

// Seed a small graph in an in-memory Storage.
//   A --calls--> B, B --calls--> C, A --uses--> C
//   D --inherits--> A (subclass), A --contains--> E (member)
//   B is a pure virtual method (is_pure=1)
struct Seeded {
  Storage db;
  cidx::query::SqliteQueryReadAdapter read;
  int64_t id_A = -1, id_B = -1, id_C = -1, id_D = -1, id_E = -1;
  int64_t eid_AB = -1, eid_BC = -1, eid_AC = -1;
  int64_t eid_DA = -1, eid_AE = -1;

  Seeded() : db(":memory:"), read(db) {
    auto sym_A = make_sym("USR::A", "funcA", "function", "ns::funcA");
    auto sym_B = make_sym("USR::B", "funcB", "function");
    sym_B.is_pure = true;
    auto sym_C = make_sym("USR::C", "funcC", "function");
    auto sym_D = make_sym("USR::D", "ClassD", "class");
    auto sym_E = make_sym("USR::E", "field_e", "member");

    id_A = db.add_symbol(sym_A);
    id_B = db.add_symbol(sym_B);
    id_C = db.add_symbol(sym_C);
    id_D = db.add_symbol(sym_D);
    id_E = db.add_symbol(sym_E);

    // A --calls--> B (count 3: add 3 separate edges, each count=1)
    eid_AB = db.add_edge(make_edge(id_A, id_B, 1));
    db.add_edge(make_edge(id_A, id_B, 1));
    db.add_edge(make_edge(id_A, id_B, 1));

    // B --calls--> C (count 1)
    eid_BC = db.add_edge(make_edge(id_B, id_C, 1));

    // A --uses--> C (count 2)
    eid_AC = db.add_edge(make_edge(id_A, id_C, 7));
    db.add_edge(make_edge(id_A, id_C, 7));

    // D --inherits--> A
    eid_DA = db.add_edge(make_edge(id_D, id_A, 2));

    // A --contains--> E
    eid_AE = db.add_edge(make_edge(id_A, id_E, 3));

    // B --overrides--> A
    db.add_edge(make_edge(id_B, id_A, 6));

    // Add one call site for AB
    db.add_edge_site(make_edge_site(eid_AB, 10, 5));
  }
};

} // namespace

// ---------------------------------------------------------------------------
// G1: empty DB — lookups return nullopt/empty
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: empty DB — get_by_id returns nullopt") {
  Storage db(":memory:");
  cidx::query::SqliteQueryReadAdapter read(db);
  GraphQuery g(read, ":memory:");
  CHECK(!g.get_by_id(1));
  CHECK(!g.get_by_usr("USR::X"));
  CHECK(g.find("anything").empty());
  CHECK(g.edge_count() == 0);
}

// ---------------------------------------------------------------------------
// G2: seeded DB — symbol lookup
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: get_by_id / get_by_usr on seeded DB") {
  Seeded s;
  GraphQuery g(s.read, ":memory:");

  auto sym_A = g.get_by_id(s.id_A);
  REQUIRE(sym_A);
  CHECK(sym_A->spelling == "funcA");
  CHECK(sym_A->kind == "function");
  // qual_name set -> name = qual_name
  CHECK(sym_A->name == "ns::funcA");

  auto sym_B = g.get_by_usr("USR::B");
  REQUIRE(sym_B);
  CHECK(sym_B->is_pure);
  CHECK(sym_B->name == "funcB"); // no qual_name -> name = spelling

  CHECK(!g.get_by_id(9999));
  CHECK(!g.get_by_usr("USR::NONE"));
}

TEST_CASE("graph_query: find preserves exact order above executor cap") {
  Storage db(":memory:");
  std::vector<std::tuple<int, std::string, int64_t>> expected;
  for (int index = 0; index < 1205; ++index) {
    const std::string spelling =
        "symbol" + std::to_string((index * 37) % 10000);
    const auto id = db.add_symbol(
        make_sym("USR::ordered-" + std::to_string(index), spelling));
    expected.emplace_back(static_cast<int>(spelling.size()), spelling, id);
  }
  std::ranges::sort(expected, [](const auto &lhs, const auto &rhs) {
    if (std::get<0>(lhs) != std::get<0>(rhs)) {
      return std::get<0>(lhs) < std::get<0>(rhs);
    }
    if (std::get<1>(lhs) != std::get<1>(rhs)) {
      return std::get<1>(lhs) < std::get<1>(rhs);
    }
    return std::get<2>(lhs) < std::get<2>(rhs);
  });

  cidx::query::SqliteQueryReadAdapter read(db);
  GraphQuery graph(read, ":memory:");
  const auto actual = graph.find("", std::nullopt, 1205);
  REQUIRE(actual.size() == expected.size());
  std::set<int64_t> unique_ids;
  for (std::size_t index = 0; index < actual.size(); ++index) {
    unique_ids.insert(actual[index].id);
    CHECK(actual[index].id == std::get<2>(expected[index]));
  }
  CHECK(unique_ids.size() == expected.size());
}

TEST_CASE("graph_query: executable differential operation inventory") {
  Seeded s;
  cidx::query::SqliteQueryReadAdapter read(s.db);
  GraphQuery g(read, ":memory:");

  std::vector<cidx::graph::Edge> actual;
  require_production_plan("edges_out", [&] {
    actual = g.edges_out(s.id_A, std::vector<std::string>{"calls"}, 10);
  });

  auto legacy = s.db.raw_db().prepare(
      "SELECT id, dst_id FROM edge WHERE src_id = ? AND kind = ? "
      "ORDER BY count DESC, kind, id LIMIT ?");
  legacy.bind(1, s.id_A);
  legacy.bind(2, static_cast<int64_t>(1));
  legacy.bind(3, static_cast<int64_t>(10));
  std::vector<std::pair<int64_t, int64_t>> legacy_rows;
  while (legacy.step()) {
    legacy_rows.emplace_back(legacy.col_int64(0), legacy.col_int64(1));
  }
  REQUIRE(legacy_rows.size() == actual.size());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    CHECK(actual[i].edge_id == legacy_rows[i].first);
    CHECK(actual[i].peer.id == legacy_rows[i].second);
  }

  std::vector<Site> page;
  require_production_plan("sites_page",
                          [&] { page = g.sites_page(s.eid_AB, 0, 1); });
  if (!page.empty()) {
    CHECK(page.front().line.has_value());
  }
}

TEST_CASE("graph_query: production adapters use executable plans and legacy "
          "oracles") {
  Seeded s;
  Symbol redefined = make_sym("USR::redefined", "redefined", "function");
  const auto redefined_id = s.db.add_symbol(redefined);
  auto multi_def =
      s.db.raw_db().prepare("UPDATE symbol SET multi_def = 2 WHERE id = ?");
  multi_def.bind(1, redefined_id);
  multi_def.step_done();
  const auto component = s.db.add_component("test", "/tmp/hse27-inventory");
  const auto directory = s.db.add_directory(component, "");
  const auto file = s.db.add_file(directory, "fixture.cpp");
  cidx::EdgeSite site;
  site.edge_id = s.eid_AB;
  site.file_id = file;
  site.line = 10;
  site.col = 5;
  site.conditional = true;
  s.db.add_edge_site(site);
  cidx::query::SqliteQueryReadAdapter read(s.db);
  GraphQuery g(read, ":memory:");

  require_production_plan("get_by_id", [&] -> void {
    const auto result = g.get_by_id(s.id_A);
    REQUIRE(result);
    CHECK(result->id == s.id_A);
  });
  require_production_plan("get_by_usr", [&] -> void {
    const auto result = g.get_by_usr("USR::A");
    REQUIRE(result);
    CHECK(result->id == s.id_A);
  });
  require_production_plan("find", [&] -> void {
    const auto result = g.find("func", std::nullopt, 50);
    REQUIRE(result.size() == 2);
    CHECK(result[0].id == s.id_B);
    CHECK(result[1].id == s.id_C);
  });
  require_production_plan("edge_count", [&] -> void {
    auto expected = s.db.raw_db().prepare("SELECT COUNT(*) FROM edge");
    REQUIRE(expected.step());
    CHECK(g.edge_count() == expected.col_int64(0));
  });
  require_production_plan("require_edges", [&] -> void { g.require_edges(); });
  require_production_plan("edges", [&] -> void {
    const auto result =
        g.edges(s.id_A, "out", std::vector<int64_t>{1}, 50, true);
    REQUIRE(result.size() == 1);
    CHECK(result.front().edge_id == s.eid_AB);
    CHECK(result.front().peer.id == s.id_B);
  });
  require_production_plan("edges_in", [&] -> void {
    const auto result = g.edges_in(s.id_B, std::vector<std::string>{"calls"});
    REQUIRE(result.size() == 1);
    CHECK(result.front().edge_id == s.eid_AB);
    CHECK(result.front().peer.id == s.id_A);
  });
  require_production_plan("edges_out", [&] -> void {
    const auto result = g.edges_out(s.id_A, std::vector<std::string>{"calls"});
    REQUIRE(result.size() == 1);
    CHECK(result.front().edge_id == s.eid_AB);
    CHECK(result.front().peer.id == s.id_B);
  });
  require_production_plan("references", [&] -> void {
    const auto result = g.references(s.id_C);
    REQUIRE(result.size() == 2);
    CHECK(result[0].edge_id == s.eid_BC);
    CHECK(result[1].edge_id == s.eid_AC);
  });
  require_production_plan("aliased_by",
                          [&] -> void { CHECK(g.aliased_by(s.id_A).empty()); });
  require_production_plan("sites", [&] -> void {
    const auto result = g.sites(s.eid_AB);
    REQUIRE(result.size() == 1);
    CHECK(result[0].line == 10);
    CHECK(result[0].col == 5);
  });
  require_production_plan("sites_page", [&] -> void {
    const auto result = g.sites_page(s.eid_AB, 0, 1);
    REQUIRE(result.size() == 1);
    CHECK(result.front().line == 10);
    CHECK(result.front().col == 5);
  });
  require_production_plan("edge_conditional",
                          [&] -> void { CHECK(g.edge_conditional(s.eid_AB)); });
  require_production_plan("edge_id_for", [&] -> void {
    CHECK(g.edge_id_for(s.id_A, s.id_B, 1) == s.eid_AB);
  });
  require_production_plan("peers", [&] -> void {
    const auto result = g.peers(s.id_A, std::vector<std::string>{"calls"});
    REQUIRE(result.size() == 1);
    CHECK(result.front().id == s.id_B);
  });
  require_production_plan("walk", [&] -> void {
    const auto result =
        g.walk(s.id_A, std::vector<std::string>{"calls"}, "out", 3, 50);
    CHECK(result.nodes_by_id.size() == 3);
    CHECK(result.nodes_by_id.contains(s.id_A));
    CHECK(result.nodes_by_id.contains(s.id_B));
    CHECK(result.nodes_by_id.contains(s.id_C));
  });
  require_production_plan("reaches", [&] -> void {
    const auto result =
        g.reaches(s.id_A, s.id_C, std::vector<std::string>{"calls"});
    REQUIRE(result);
    REQUIRE(result->size() == 3);
    CHECK((*result)[0].id == s.id_A);
    CHECK((*result)[1].id == s.id_B);
    CHECK((*result)[2].id == s.id_C);
  });
  require_production_plan("bases", [&] -> void {
    const auto result = g.bases(s.id_D);
    REQUIRE(result.size() == 1);
    CHECK(result.front().id == s.id_A);
  });
  require_production_plan("subclasses", [&] -> void {
    const auto result = g.subclasses(s.id_A);
    REQUIRE(result.size() == 1);
    CHECK(result.front().id == s.id_D);
  });
  require_production_plan("members", [&] -> void {
    const auto result = g.members(s.id_A);
    REQUIRE(result.size() == 1);
    CHECK(result.front().id == s.id_E);
  });
  require_production_plan("overrides_of", [&] -> void {
    const auto result = g.overrides_of(s.id_B);
    REQUIRE(result.size() == 1);
    CHECK(result.front().id == s.id_A);
  });
  require_production_plan("overridden_by", [&] -> void {
    const auto result = g.overridden_by(s.id_A);
    REQUIRE(result.size() == 1);
    CHECK(result.front().id == s.id_B);
  });
  require_production_plan("is_virtual_method",
                          [&] -> void { CHECK(g.is_virtual_method(s.id_B)); });
  require_production_plan("dispatch_targets", [&] -> void {
    CHECK(g.dispatch_targets(s.id_B).empty());
  });
  require_production_plan("redefined", [&] -> void {
    const auto result = g.redefined(10);
    REQUIRE(result.size() == 1);
    CHECK(result.front().id == redefined_id);
  });
  require_production_plan(
      "definitions", [&] -> void { CHECK(g.definitions(s.id_A).empty()); });
  require_production_plan("possible_callees", [&] -> void {
    CHECK(g.possible_callees(s.id_A).empty());
  });
  require_production_plan("signature",
                          [&] -> void { CHECK(g.signature(s.id_A).empty()); });
  require_production_plan("type_layers", [&] -> void {
    const auto result = g.type_layers(-1);
    REQUIRE(result.size() == 1);
    CHECK(result.front().status == "unknown");
  });
  require_production_plan("type_child",
                          [&] -> void { CHECK(!g.type_child(-1, 1)); });
  require_production_plan("type_users", [&] -> void {
    CHECK(g.type_users("missing", 50).empty());
  });
  auto legacy_conditional = s.db.raw_db().prepare(
      "SELECT EXISTS(SELECT 1 FROM edge_site WHERE edge_id = ? "
      "AND conditional = 1)");
  legacy_conditional.bind(1, s.eid_AB);
  REQUIRE(legacy_conditional.step());
  CHECK(g.edge_conditional(s.eid_AB) == (legacy_conditional.col_int64(0) != 0));
  CHECK(g.edge_conditional(999999) == false);
  auto legacy_edge = s.db.raw_db().prepare(
      "SELECT id FROM edge WHERE src_id = ? AND dst_id = ? AND kind = ?");
  legacy_edge.bind(1, s.id_A);
  legacy_edge.bind(2, s.id_B);
  legacy_edge.bind(3, int64_t{1});
  REQUIRE(legacy_edge.step());
  CHECK(g.edge_id_for(s.id_A, s.id_B, 1) == legacy_edge.col_int64(0));
  CHECK(!g.edge_id_for(s.id_A, s.id_B, 7));
  CHECK(!g.edge_id_for(999999, s.id_B, 1));
  const auto red = g.redefined(10);
  REQUIRE(red.size() == 1);
  CHECK(red.front().id == redefined_id);

  // The adapter's site page must expose exactly the rows selected by its
  // QueryPlan-owned key/order/limit stages; GraphReadPort only hydrates them.
  auto raw_site = s.db.raw_db().prepare(
      "SELECT edge_id,file_id,line,col FROM edge_site WHERE edge_id = ?");
  raw_site.bind(1, s.eid_AB);
  REQUIRE(raw_site.step());
  CHECK(raw_site.col_int64(0) == s.eid_AB);
  CHECK(raw_site.col_int64(1) > 0);
  const auto page = g.sites_page(s.eid_AB, 0, 1);
  REQUIRE(page.size() == 1);
  CHECK(page.front().line == raw_site.col_int64(2));
  CHECK(page.front().col == raw_site.col_int64(3));
}

TEST_CASE("graph_query: effective edge count is plan-owned across states") {
  Storage db(":memory:");
  const auto src = db.add_symbol(make_sym("USR::count-src", "count_src"));
  const auto with_sites_dst =
      db.add_symbol(make_sym("USR::count-with-sites", "count_with_sites"));
  const auto without_sites_dst = db.add_symbol(
      make_sym("USR::count-without-sites", "count_without_sites"));
  const auto zero_dst =
      db.add_symbol(make_sym("USR::count-zero", "count_zero"));
  const auto resolved_dst =
      db.add_symbol(make_sym("USR::count-resolved", "count_resolved"));
  const auto component = db.add_component("count-test", "/tmp/hse27-count");
  const auto directory = db.add_directory(component, "");
  const auto file = db.add_file(directory, "counts.cpp");

  const auto with_sites = db.add_edge(make_edge(src, with_sites_dst, 1, 7));
  const auto without_sites =
      db.add_edge(make_edge(src, without_sites_dst, 1, 7));
  const auto zero_count = db.add_edge(make_edge(src, zero_dst, 1, 0));
  const auto resolved = db.add_edge(make_edge(src, resolved_dst, 1, 7));
  cidx::EdgeSite with_sites_site;
  with_sites_site.edge_id = with_sites;
  with_sites_site.file_id = file;
  with_sites_site.line = 10;
  with_sites_site.col = 1;
  db.add_edge_site(with_sites_site);
  cidx::EdgeSite zero_count_site;
  zero_count_site.edge_id = zero_count;
  zero_count_site.file_id = file;
  zero_count_site.line = 20;
  zero_count_site.col = 1;
  db.add_edge_site(zero_count_site);

  cidx::query::SqliteQueryReadAdapter read(db);
  GraphQuery graph(read, ":memory:");
  const auto expected_count = [&](int64_t edge_id) {
    auto stmt = db.raw_db().prepare(
        "SELECT CASE WHEN COALESCE((SELECT value FROM meta WHERE "
        "key = 'graph_resolved_at'), '') <> '' THEN "
        "CASE WHEN e.count <> 0 THEN e.count ELSE 1 END "
        "WHEN (SELECT COUNT(*) FROM edge_site WHERE edge_id = e.id) > 0 "
        "THEN (SELECT COUNT(*) FROM edge_site WHERE edge_id = e.id) "
        "WHEN e.count <> 0 THEN e.count ELSE 1 END FROM edge e WHERE e.id = ?");
    stmt.bind(1, edge_id);
    REQUIRE(stmt.step());
    return stmt.col_int64(0);
  };
  const auto actual_count = [&](int64_t dst) {
    const auto rows =
        graph.edges_out(src, std::vector<std::string>{"calls"}, 100);
    const auto it = std::ranges::find_if(
        rows, [dst](const auto &edge) { return edge.dst_id == dst; });
    REQUIRE(it != rows.end());
    return it->count;
  };

  CHECK(actual_count(with_sites_dst) == expected_count(with_sites));
  CHECK(actual_count(without_sites_dst) == expected_count(without_sites));
  CHECK(actual_count(zero_dst) == expected_count(zero_count));
  CHECK(actual_count(resolved_dst) == expected_count(resolved));

  db.stamp_graph_resolved();
  CHECK(actual_count(with_sites_dst) == expected_count(with_sites));
  CHECK(actual_count(without_sites_dst) == expected_count(without_sites));
  CHECK(actual_count(zero_dst) == expected_count(zero_count));
  CHECK(actual_count(resolved_dst) == expected_count(resolved));
}

// ---------------------------------------------------------------------------
// G2b: files() resolves a grouped component's CLONE-RELATIVE path (v24/v25
// regression). A component grouped under a repository stores its `path`
// RELATIVE to the repository's active clone root (relativize_component);
// files() must route through Storage::component_abs_base like the writable
// side's file_abs_path does, else Sym.file comes back clone-relative
// ("./region.c") instead of the real absolute path.
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: files() resolves grouped relative component path") {
  Storage db(":memory:");
  int64_t rid = db.add_repository("r");
  int64_t clone_id = db.add_clone(rid, "/w/a");
  db.set_active_clone(rid, clone_id);
  int64_t comp = db.add_component("r", "/w/a", "repo");
  db.set_component_repository(comp, rid);
  db.relativize_component(comp, "/w/a"); // stored path becomes "."
  int64_t dir = db.add_directory(comp, "");
  int64_t fid = db.add_file(dir, "region.c");

  Symbol sym = make_sym("c:@F@main", "main", "function", "main");
  sym.file_id = fid;
  sym.line = 1;
  sym.col = 1;
  db.add_symbol(sym);

  cidx::query::SqliteQueryReadAdapter read(db);
  GraphQuery g(read, ":memory:");
  auto s = g.get_by_usr("c:@F@main");
  REQUIRE(s);
  REQUIRE(s->file);
  CHECK(*s->file == "/w/a/region.c");
}

// ---------------------------------------------------------------------------
// G3: edges_in / edges_out direction
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: edges_in / edges_out direction") {
  Seeded s;
  GraphQuery g(s.read, ":memory:");

  // A calls B and uses C (out)
  auto out_A = g.edges_out(s.id_A, std::vector<std::string>{"calls"}, 50);
  REQUIRE(out_A.size() == 1);
  CHECK(out_A[0].peer.id == s.id_B);
  CHECK(out_A[0].kind == "calls");

  // B called by A (in)
  auto in_B = g.edges_in(s.id_B, std::vector<std::string>{"calls"}, 50);
  REQUIRE(in_B.size() == 1);
  CHECK(in_B[0].peer.id == s.id_A);

  // A is inherited by D (in, inherits)
  auto in_A_inh = g.edges_in(s.id_A, std::vector<std::string>{"inherits"}, 50);
  REQUIRE(in_A_inh.size() == 1);
  CHECK(in_A_inh[0].peer.id == s.id_D);
}

// ---------------------------------------------------------------------------
// G4: count fallback R3
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: count fallback (R3) from accumulated ecount") {
  Seeded s;
  GraphQuery g(s.read, ":memory:");

  // A --calls--> B was added 3 times; ecount should reflect that
  auto out_A = g.edges_out(s.id_A, std::vector<std::string>{"calls"}, 50);
  REQUIRE(!out_A.empty());
  // ecount accumulates
  CHECK(out_A[0].count >=
        1); // at least 1 (may be 3 depending on count_resolved)

  // A --uses--> C was added 2 times
  auto out_AC = g.edges_out(s.id_A, std::vector<std::string>{"uses"}, 50);
  REQUIRE(!out_AC.empty());
  CHECK(out_AC[0].count >= 1);
}

// ---------------------------------------------------------------------------
// G5: references() = calls + uses in
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: references() = calls + uses inbound") {
  Seeded s;
  GraphQuery g(s.read, ":memory:");

  // C is called by B (calls) and used by A (uses)
  auto refs_C = g.references(s.id_C, 50);
  CHECK(refs_C.size() == 2);
  // Verify both peers exist
  bool found_B = false, found_A = false;
  for (const auto &e : refs_C) {
    if (e.peer.id == s.id_B)
      found_B = true;
    if (e.peer.id == s.id_A)
      found_A = true;
  }
  CHECK(found_B);
  CHECK(found_A);
}

TEST_CASE("graph_query: mixed references retain legacy typed rows") {
  Storage db(":memory:");
  const int64_t target =
      db.add_symbol(make_sym("USR::mixed-target", "target", "function"));
  const int64_t caller =
      db.add_symbol(make_sym("USR::mixed-caller", "caller", "function"));
  const int64_t typed =
      db.add_symbol(make_sym("USR::mixed-typed", "typed", "member"));
  db.add_edge(make_edge(caller, target, 1)); // calls: QueryPlan-supported
  db.add_edge(make_edge(typed, target, 20)); // of_type: legacy fallback

  cidx::query::SqliteQueryReadAdapter read(db);
  GraphQuery g(read, ":memory:");
  const auto refs = g.references(target);
  REQUIRE(refs.size() == 2);
  std::set<std::pair<int64_t, std::string>> found;
  for (const auto &edge : refs) {
    found.emplace(edge.peer.id, edge.kind);
  }
  CHECK(found == std::set<std::pair<int64_t, std::string>>{{caller, "calls"},
                                                           {typed, "of_type"}});
}

TEST_CASE("graph_query: truncated plan candidates preserve legacy edge order") {
  Storage db(":memory:");
  const int64_t target =
      db.add_symbol(make_sym("USR::degree-target", "target", "function"));
  for (int index = 0; index < 1005; ++index) {
    const auto caller =
        db.add_symbol(make_sym("USR::degree-caller-" + std::to_string(index),
                               "caller" + std::to_string(index), "function"));
    db.add_edge(make_edge(caller, target, 1, index == 1004 ? 999 : 1));
  }
  db.stamp_graph_resolved();

  cidx::query::SqliteQueryReadAdapter read(db);
  GraphQuery g(read, ":memory:");
  const auto edges = g.edges_in(target, std::vector<std::string>{"calls"}, 1);
  REQUIRE(edges.size() == 1);
  CHECK(edges.front().peer.spelling == "caller1004");
  CHECK(edges.front().count == 999);
}

TEST_CASE("graph_query: edge_id_for exact edge survives high degree") {
  Storage db(":memory:");
  const auto src = db.add_symbol(make_sym("USR::edge-id-src", "edge_id_src"));
  int64_t last_dst = -1;
  int64_t last_edge = -1;
  for (int index = 0; index < 1005; ++index) {
    last_dst =
        db.add_symbol(make_sym("USR::edge-id-dst-" + std::to_string(index),
                               "edge_id_dst_" + std::to_string(index)));
    last_edge = db.add_edge(make_edge(src, last_dst, 1));
  }
  cidx::query::SqliteQueryReadAdapter read(db);
  GraphQuery graph(read, ":memory:");
  CHECK(graph.edge_id_for(src, last_dst, 1) == last_edge);
  CHECK(!graph.edge_id_for(src, last_dst, 7));
}

TEST_CASE("graph_query: find preserves legacy case-insensitive matches") {
  Storage db(":memory:");
  db.add_symbol(make_sym("USR::upper-find", "ALPHAThing", "function"));
  db.add_symbol(make_sym("USR::lower-find", "alphaThing", "function"));

  cidx::query::SqliteQueryReadAdapter read(db);
  GraphQuery g(read, ":memory:");
  const auto matches = g.find("ALPHA", std::nullopt, 50);
  REQUIRE(matches.size() == 2);
  CHECK(std::set<std::string>{matches[0].spelling, matches[1].spelling} ==
        std::set<std::string>{"ALPHAThing", "alphaThing"});
}

TEST_CASE("graph_query: find preserves segmented fuzzy matches") {
  Storage db(":memory:");
  db.add_symbol(make_sym("USR::literal-segment", "xFoo::Barx", "function"));
  db.add_symbol(make_sym("USR::wildcard-segment", "xFooXBarx", "function"));

  cidx::query::SqliteQueryReadAdapter read(db);
  GraphQuery g(read, ":memory:");
  const auto matches = g.find("Foo::Bar", std::nullopt, 50);
  REQUIRE(matches.size() == 2);
  CHECK(std::set<std::string>{matches[0].spelling, matches[1].spelling} ==
        std::set<std::string>{"xFoo::Barx", "xFooXBarx"});
}

TEST_CASE(
    "graph_query: aliased_by() returns typedef and type-alias users only") {
  Storage db(":memory:");

  const int64_t target_id =
      db.add_symbol(make_sym("USR::Target", "Target", "struct"));
  const int64_t alias_a_id =
      db.add_symbol(make_sym("USR::AliasA", "AliasA", "type-alias"));
  const int64_t alias_b_id =
      db.add_symbol(make_sym("USR::AliasB", "AliasB", "typedef"));
  const int64_t ordinary_user_id =
      db.add_symbol(make_sym("USR::use_target", "use_target", "function"));

  // v34: the alias -> target relation is alias_of(19); an ordinary reference
  // stays uses(7) and must not be reported as an alias.
  db.add_edge(make_edge(alias_b_id, target_id, 19));
  db.add_edge(make_edge(ordinary_user_id, target_id, 7));
  db.add_edge(make_edge(alias_a_id, target_id, 19));

  cidx::query::SqliteQueryReadAdapter read(db);
  GraphQuery g(read, ":memory:");
  auto aliases = g.aliased_by(target_id, 50);

  REQUIRE(aliases.size() == 2);
  CHECK(aliases[0].spelling == "AliasA");
  CHECK(aliases[0].kind == "type-alias");
  CHECK(aliases[1].spelling == "AliasB");
  CHECK(aliases[1].kind == "typedef");
}

// ---------------------------------------------------------------------------
// G6: walk() BFS depth and max_nodes
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: walk() BFS depth") {
  Seeded s;
  GraphQuery g(s.read, ":memory:");

  // walk from A out over "calls", depth=1 -> only B
  auto tr1 = g.walk(s.id_A, std::vector<std::string>{"calls"}, "out", 1, 100);
  auto nodes1 = tr1.nodes();
  CHECK(nodes1.size() == 2); // A(d0) + B(d1)

  // walk depth=2 -> A, B, C
  auto tr2 = g.walk(s.id_A, std::vector<std::string>{"calls"}, "out", 2, 100);
  auto nodes2 = tr2.nodes();
  CHECK(nodes2.size() == 3); // A(d0) + B(d1) + C(d2)

  // max_nodes=2 terminates after adding first neighbor (start + 1 neighbor)
  auto tr_lim = g.walk(s.id_A, std::vector<std::string>{"calls"}, "out", 5, 2);
  CHECK(tr_lim.nodes().size() <= 2);
}

// ---------------------------------------------------------------------------
// G7: reaches() shortest path and null when unreachable
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: reaches() shortest path") {
  Seeded s;
  GraphQuery g(s.read, ":memory:");

  // A -> B -> C via calls
  auto path =
      g.reaches(s.id_A, s.id_C, std::vector<std::string>{"calls"}, "out", 5);
  REQUIRE(path);
  CHECK(path->size() == 3); // A, B, C
  CHECK((*path)[0].id == s.id_A);
  CHECK((*path)[2].id == s.id_C);

  // C -> A: unreachable via calls out
  auto no_path =
      g.reaches(s.id_C, s.id_A, std::vector<std::string>{"calls"}, "out", 5);
  CHECK(!no_path);

  // A -> A: same node
  auto self_path =
      g.reaches(s.id_A, s.id_A, std::vector<std::string>{"calls"}, "out", 5);
  REQUIRE(self_path);
  CHECK(self_path->size() == 1);
}

// ---------------------------------------------------------------------------
// G8: bases() / subclasses() / members()
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: hierarchy — bases, subclasses, members") {
  Seeded s;
  GraphQuery g(s.read, ":memory:");

  // D --inherits--> A; so A has subclass D
  auto subs = g.subclasses(s.id_A, true);
  REQUIRE(subs.size() == 1);
  CHECK(subs[0].id == s.id_D);

  // A has no bases
  auto bases_A = g.bases(s.id_A, true);
  CHECK(bases_A.empty());

  // D has base A
  auto bases_D = g.bases(s.id_D, true);
  REQUIRE(bases_D.size() == 1);
  CHECK(bases_D[0].id == s.id_A);

  // A --contains--> E
  auto mems = g.members(s.id_A);
  CHECK(!mems.empty());
  bool has_E = false;
  for (const auto &m : mems) {
    if (m.id == s.id_E)
      has_E = true;
  }
  CHECK(has_E);
}

// ---------------------------------------------------------------------------
// G9: dispatch_targets() insertion order
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: dispatch_targets() BFS from virtual root") {
  Seeded s;
  GraphQuery g(s.read, ":memory:");

  // A is a non-pure function; B --overrides--> A and B is pure.
  // dispatch_targets(A): root A is not pure -> A first, then overriders.
  // B is pure -> NOT added to targets.
  auto targets = g.dispatch_targets(s.id_A);
  // A is not pure -> in targets; B is pure -> NOT in targets
  bool has_A = false, has_B = false;
  for (const auto &t : targets) {
    if (t.id == s.id_A)
      has_A = true;
    if (t.id == s.id_B)
      has_B = true;
  }
  CHECK(has_A);
  CHECK(!has_B);
}

// ---------------------------------------------------------------------------
// G10: kind_ids() valid and invalid
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: kind_ids() valid kinds and invalid kind throws") {
  Storage db(":memory:");
  cidx::query::SqliteQueryReadAdapter read(db);
  GraphQuery g(read, ":memory:");

  auto kv = g.kind_ids(std::vector<std::string>{"calls", "uses"});
  REQUIRE(kv);
  CHECK(kv->size() == 2);

  // nullopt input -> nullopt output
  CHECK(!g.kind_ids(std::nullopt));

  // invalid kind
  CHECK_THROWS_AS(g.kind_ids(std::vector<std::string>{"bogus_kind"}),
                  std::invalid_argument);
}

// ---------------------------------------------------------------------------
// G11: Sym.is_stub(), Sym.loc(), Sym.to_dict() key order
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: Sym value type — is_stub, loc, to_dict key order") {
  // Build a stub Sym manually
  Sym stub;
  stub.id = 5;
  stub.usr = "USR::stub";
  stub.spelling = "stb";
  stub.name = "stb";
  stub.kind = "function";
  stub.resolved = false;
  stub.file = std::nullopt;
  stub.external = false;
  CHECK(stub.is_stub());
  CHECK(stub.loc() == "<no-location>");

  // Build a non-stub with file
  Sym real;
  real.id = 6;
  real.usr = "USR::real";
  real.spelling = "fn";
  real.name = "ns::fn";
  real.kind = "function";
  real.resolved = true;
  real.file = "/some/path/foo.cpp";
  real.line = 42;
  real.col = 3;
  CHECK(!real.is_stub());
  CHECK(real.loc() == "foo.cpp:42");

  // to_dict key order (R7): id,usr,semantic_universe,identity_key,spelling,
  //                         qual_name,kind,type_info,
  //                         const_value,file,line,col,end_line,end_col,
  //                         is_definition,is_pure,is_static,is_instantiation,
  //                         is_stub
  auto dict = real.to_dict();
  REQUIRE(dict.o.size() == 19);
  CHECK(dict.o[0].first == "id");
  CHECK(dict.o[1].first == "usr");
  CHECK(dict.o[2].first == "semantic_universe");
  CHECK(dict.o[3].first == "identity_key");
  CHECK(dict.o[4].first == "spelling");
  CHECK(dict.o[5].first == "qual_name");
  CHECK(dict.o[6].first == "kind");
  CHECK(dict.o[7].first == "type_info");
  CHECK(dict.o[8].first == "const_value");
  CHECK(dict.o[9].first == "file");
  CHECK(dict.o[10].first == "line");
  CHECK(dict.o[11].first == "col");
  CHECK(dict.o[12].first == "end_line");
  CHECK(dict.o[13].first == "end_col");
  CHECK(dict.o[14].first == "is_definition");
  CHECK(dict.o[15].first == "is_pure");
  CHECK(dict.o[16].first == "is_static");
  CHECK(dict.o[17].first == "is_instantiation");
  CHECK(dict.o[18].first == "is_stub");
}

// ---------------------------------------------------------------------------
// G12: emit_edges text output
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: emit_edges text — header, count suffix, trailer") {
  Seeded s;
  GraphQuery g(s.read, ":memory:");

  auto edges = g.edges_out(s.id_A, std::vector<std::string>{"calls"}, 50);
  REQUIRE(!edges.empty());

  std::ostringstream out;
  emit_edges(g, edges, false, out, "header:");
  const std::string txt = out.str();

  // Header on first line
  CHECK(txt.substr(0, 7) == "header:");

  // Trailing N result(s)
  std::string last_line;
  {
    auto pos = txt.rfind('\n', txt.size() - 2);
    last_line = txt.substr(pos + 1);
  }
  CHECK(last_line.find("result(s)") != std::string::npos);
}

// ---------------------------------------------------------------------------
// G13: emit_syms text output with depth
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: emit_syms text — depth suffix, trailer") {
  std::vector<Sym> syms;
  Sym a;
  a.id = 1;
  a.usr = "u1";
  a.spelling = "fnA";
  a.name = "fnA";
  a.kind = "function";
  a.resolved = true;
  syms.push_back(a);

  std::unordered_map<int64_t, int> depths = {{1, 2}};

  std::ostringstream out;
  emit_syms(syms, false, out, "reach:", &depths);
  const std::string txt = out.str();

  // Contains "d2"
  CHECK(txt.find("d2") != std::string::npos);

  // Trailer
  CHECK(txt.find("1 result(s)") != std::string::npos);
}

// ---------------------------------------------------------------------------
// HSE-92 round 3: sites_page() -- bounded, delivery-order-correct edge site
// pagination (no more "fetch up to 1,000,000 rows, then re-sort" probe).
// ---------------------------------------------------------------------------
TEST_CASE("graph_query: sites_page() orders by resolved path, not raw "
          "insertion/file_id order, and pages correctly across a file "
          "boundary") {
  Storage db(":memory:");
  cidx::query::SqliteQueryReadAdapter read(db);
  GraphQuery g(read, ":memory:");
  const int64_t component = db.add_component("test", "/tmp/cidx-sites-page");
  const int64_t directory = db.add_directory(component, "");
  // Inserted in the OPPOSITE of path order (c.cpp, b.cpp, a.cpp), same
  // adversarial shape as the UI-level evidence-ordering regression: file_id
  // insertion order must not leak into delivery order.
  const int64_t file_c = db.add_file(directory, "c.cpp");
  const int64_t file_b = db.add_file(directory, "b.cpp");
  const int64_t file_a = db.add_file(directory, "a.cpp");
  auto sym_src = make_sym("USR::sp_src", "sp_src");
  auto sym_dst = make_sym("USR::sp_dst", "sp_dst");
  const int64_t src = db.add_symbol(sym_src);
  const int64_t dst = db.add_symbol(sym_dst);
  const int64_t edge_id = db.add_edge(make_edge(src, dst, 1));
  // Two sites in c.cpp, one each in b.cpp and a.cpp: four sites total,
  // three distinct files, so a window can legitimately span a file
  // boundary.
  const auto place_site = [&](int64_t file_id, int64_t line) {
    cidx::EdgeSite s;
    s.edge_id = edge_id;
    s.file_id = file_id;
    s.line = line;
    s.col = 1;
    db.add_edge_site(s);
  };
  place_site(file_c, 1);
  place_site(file_c, 2);
  place_site(file_b, 1);
  place_site(file_a, 1);

  auto all = g.sites_page(edge_id, 0, 100);
  REQUIRE(all.size() == 4);
  // Delivery order must be alphabetical by path: a.cpp, b.cpp, c.cpp(x2) --
  // NOT (file_c, file_b, file_a)'s insertion/file_id order.
  CHECK(all[0].file->ends_with("a.cpp"));
  CHECK(all[1].file->ends_with("b.cpp"));
  CHECK(all[2].file->ends_with("c.cpp"));
  CHECK(all[3].file->ends_with("c.cpp"));
  CHECK(all[2].line == 1);
  CHECK(all[3].line == 2);

  // A window that spans the b.cpp/c.cpp boundary (offset=1, limit=2) must
  // return exactly [b.cpp, c.cpp:line1] -- no duplication, no gap -- proving
  // per-file bounded fetches are stitched together correctly across a
  // boundary, not just within a single file.
  auto spanning = g.sites_page(edge_id, 1, 2);
  REQUIRE(spanning.size() == 2);
  CHECK(spanning[0].file->ends_with("b.cpp"));
  CHECK(spanning[1].file->ends_with("c.cpp"));
  CHECK(spanning[1].line == 1);

  // The page is the exact offset slice of the adapter's ordered QueryPlan
  // keys. The legacy page's next row (c.cpp:2) is outside that key slice and
  // must not leak into the hydrated result.
  const auto selected = cidx::query::Executor(read).run(
      (cidx::query::start(cidx::query::codebase()) |
       cidx::query::view(cidx::query::View::Site) |
       cidx::query::nodes(cidx::query::eq("edge_id", edge_id)) |
       cidx::query::select({"edge_id", "file_id", "file", "line", "col"}) |
       cidx::query::order_by({"file", "line", "col"}) | cidx::query::limit(3))
          .plan());
  REQUIRE(selected.rows.size() == 3);
  CHECK(spanning[0].line == std::get<int64_t>(selected.rows[1][3]));
  CHECK(spanning[1].line == std::get<int64_t>(selected.rows[2][3]));
  CHECK(spanning[1].line != 2);

  const auto negative_offset = g.sites_page(edge_id, -1, 1);
  REQUIRE(negative_offset.size() == 1);
  CHECK(negative_offset.front().line == all.front().line);
  CHECK(g.sites_page(edge_id, std::numeric_limits<int>::max(),
                     std::numeric_limits<int>::max())
            .empty());

  // Paging one-at-a-time across all 4 sites must reach every one exactly
  // once -- the delivery-order contract the UI layer's pagination depends
  // on.
  std::set<std::string> seen_locations;
  for (int offset = 0; offset < 4; ++offset) {
    auto page = g.sites_page(edge_id, offset, 1);
    REQUIRE(page.size() == 1);
    seen_locations.insert(page.front().loc());
  }
  CHECK(seen_locations.size() == 4);
}

TEST_CASE("graph_query: first evidence page does not count the whole edge") {
  Storage db(":memory:");
  cidx::query::SqliteQueryReadAdapter read(db);
  GraphQuery g(read, ":memory:");
  const int64_t component = db.add_component("test", "/tmp/cidx-sites-bound");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "many.cpp");
  const int64_t src = db.add_symbol(make_sym("USR::bound_src", "bound_src"));
  const int64_t dst = db.add_symbol(make_sym("USR::bound_dst", "bound_dst"));
  const int64_t edge_id = db.add_edge(make_edge(src, dst, 1));
  db.raw_db().exec("BEGIN");
  for (int line = 1; line <= 100'000; ++line) {
    cidx::EdgeSite site;
    site.edge_id = edge_id;
    site.file_id = file;
    site.line = line;
    site.col = 1;
    db.add_edge_site(site);
  }
  db.raw_db().exec("COMMIT");

  int progress_callbacks = 0;
  sqlite3_progress_handler(db.raw_db().raw(), 1000, count_sqlite_progress,
                           &progress_callbacks);
  const auto page = g.sites_page(edge_id, 0, 1);
  sqlite3_progress_handler(db.raw_db().raw(), 0, nullptr, nullptr);
  REQUIRE(page.size() == 1);
  CHECK(page.front().line == 1);
  CHECK(progress_callbacks < 50);
}
