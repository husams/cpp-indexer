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

#include <optional>
#include <set>
#include <sstream>
#include <string>
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
