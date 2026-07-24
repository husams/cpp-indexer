// materialise_entity_edges: the pure-DB roll-up of all 11 entity relation kinds, plus resolve_pass.
// Split out of storage.cpp; Storage's interface is unchanged.
#include "storage/storage.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "compiledb/compiledb.hpp"
#include "storage/storage_detail.hpp"
#include "storage/storage_schema.hpp"
#include "util/errors.hpp"
#include "util/json_min.hpp"
#include "util/logger.hpp"
#include "util/pathutil.hpp"

namespace cidx {

using namespace detail;

// ---------------------------------------------------------------------------
// materialise_entity_edges: pure-DB roll-up of all 11 entity relation kinds.
// Mirrors indexer/entity_rollup.py:materialize_entity_edges() byte-identically.
// ---------------------------------------------------------------------------

// Per-pass precomputed lookups (perf: O(n^2) -> O(n)). The collapse +
// interface/abstractness helpers used to issue a fresh SQL query (often
// several) PER edge row across every phase -- on a large corpus that is the
// dominant cost of `resolve` and made the pass run for a very long time with no
// DB writes (read-heavy, so index.db mtime / journal never moved -- looking
// frozen). RollupState precomputes the same answers ONCE per materialise pass
// from tables that are READ-ONLY for the pass (edge, symbol), then the hot
// helpers become in-memory map/set lookups. Byte-identical to the old per-row
// queries (the parity gate + acceptance suite verify), so a pure speedup.
// Mirrors entity_rollup._RollupState (Python).
struct RollupState {
  std::unordered_map<int64_t, int64_t> next_hop;       // src -> first 4/5 dst
  std::unordered_map<int64_t, int64_t> collapse_cache; // memoised collapse
  std::unordered_set<std::string> non_pure_method_owners;
  std::unordered_set<std::string> field_owners;
  std::unordered_set<std::string> pure_method_owners;
  std::unordered_map<int64_t, std::string> usr_by_id; // entity-kind ids only

  explicit RollupState(cidx::SqliteDb &db) {
    // collapse next-hop: FIRST (kind, dst_id) per src among kind 4/5 edges ==
    // the old `WHERE src_id=? AND kind IN (4,5) ORDER BY kind, dst_id LIMIT 1`
    // for every src in one ordered scan (emplace keeps the first per key).
    {
      auto st = db.prepare("SELECT src_id, dst_id FROM edge WHERE kind IN (4, 5) "
                           "ORDER BY src_id, kind, dst_id");
      while (st.step()) {
        next_hop.emplace(st.col_int64(0), st.col_int64(1));
      }
    }
    // Interface / abstractness owner-sets keyed by parent_usr (the three
    // COUNT(*) probes the old is_interface ran PER call, hoisted to 3 scans).
    {
      auto st = db.prepare("SELECT DISTINCT parent_usr FROM symbol "
                           "WHERE kind = 21 AND is_pure = 0 AND parent_usr IS NOT NULL");
      while (st.step()) {
        non_pure_method_owners.insert(st.col_text(0));
      }
    }
    {
      auto st = db.prepare("SELECT DISTINCT parent_usr FROM symbol "
                           "WHERE kind = 6 AND parent_usr IS NOT NULL");
      while (st.step()) {
        field_owners.insert(st.col_text(0));
      }
    }
    {
      auto st = db.prepare("SELECT DISTINCT parent_usr FROM symbol "
                           "WHERE kind = 21 AND is_pure = 1 AND parent_usr IS NOT NULL");
      while (st.step()) {
        pure_method_owners.insert(st.col_text(0));
      }
    }
    {
      auto st = db.prepare("SELECT id, usr FROM symbol WHERE kind IN (2,3,4,5,31)");
      while (st.step()) {
        usr_by_id.emplace(st.col_int64(0), st.col_text(1));
      }
    }
  }

  int64_t collapse(int64_t sym_id) {
    auto hit = collapse_cache.find(sym_id);
    if (hit != collapse_cache.end()) {
      return hit->second;
    }
    std::set<int64_t> seen;
    int64_t cur = sym_id;
    while (!seen.contains(cur)) {
      seen.insert(cur);
      auto nit = next_hop.find(cur);
      if (nit == next_hop.end()) {
        break;
      }
      cur = nit->second;
    }
    collapse_cache.emplace(sym_id, cur);
    return cur;
  }

  [[nodiscard]] bool is_interface(int64_t sym_id) const {
    auto it = usr_by_id.find(sym_id);
    if (it == usr_by_id.end()) {
      return false;
    }
    const std::string &usr = it->second;
    if (non_pure_method_owners.contains(usr)) {
      return false;
    }
    if (field_owners.contains(usr)) {
      return false;
    }
    return pure_method_owners.contains(usr);
  }

  [[nodiscard]] bool has_pure(int64_t sym_id) const {
    auto it = usr_by_id.find(sym_id);
    return it != usr_by_id.end() && pure_method_owners.contains(it->second);
  }
};

// Module-global state for the in-progress pass. Set by materialise_entity_edges
// (and cpp_materialise_entity_nodes when called standalone for the v21->v22
// backfill); cleared by the matching CtxGuard destructor. resolve is
// single-threaded and the helpers only run synchronously within a pass.
RollupState *g_rollup_ctx = nullptr;

// RAII guard: builds + installs a RollupState only if none is active yet (the
// outermost caller "owns" it), and uninstalls on scope exit. A nested call
// (entity_nodes inside materialise_entity_edges) reuses the active state with
// no rebuild. Mirrors the owns_ctx logic in entity_rollup (Python).
struct CtxGuard {
  std::optional<RollupState> st;
  bool owned;
  explicit CtxGuard(cidx::SqliteDb &db) : owned(g_rollup_ctx == nullptr) {
    if (owned) {
      st.emplace(db);
      g_rollup_ctx = &*st;
    }
  }
  ~CtxGuard() {
    if (owned) {
      g_rollup_ctx = nullptr;
    }
  }
  CtxGuard(const CtxGuard &) = delete;
  CtxGuard &operator=(const CtxGuard &) = delete;
  CtxGuard(CtxGuard &&) = delete;
  CtxGuard &operator=(CtxGuard &&) = delete;
};

// Template-instance collapse (ADR-008 decision 6 / OQ-3): map a template
// instance/specialization symbol onto its primary template. Both the Layer-0
// instantiates(5) and specializes(4) edges point instance -> primary, so we
// follow an outgoing 4/5 edge until none remains. Returns sym_id unchanged
// when it is not an instance/specialization. Mirrors entity_rollup._collapse_to_primary.
// Delegates to the per-pass precomputed next-hop map; `db` is unused (kept for
// signature stability with the call sites).
static int64_t cpp_collapse_to_primary(cidx::SqliteDb &db, int64_t sym_id) {
  (void)db;
  return g_rollup_ctx->collapse(sym_id);
}

// Phase 1: generalizes(1) / implements(2) from inherits(2) edges.
static void cpp_materialise_inheritance(cidx::SqliteDb &db) {
  // Is sym_id a pure Interface? Delegates to the per-pass precomputed
  // owner-sets (RollupState), identical to the old per-row COUNT(*) probes.
  const auto is_interface = [](int64_t sym_id) -> bool {
    return g_rollup_ctx->is_interface(sym_id);
  };

  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id, e.base_access, e.is_virtual "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN symbol dst ON dst.id = e.dst_id "
      "WHERE e.kind = 2 "
      "  AND src.kind IN (2,3,4,5) "
      "  AND dst.kind IN (2,3,4,5)");

  struct InhRow { int64_t src; int64_t dst; int64_t acc; int64_t virt; };
  std::vector<InhRow> rows;
  while (st.step()) {
    InhRow r;
    r.src  = st.col_int64(0);
    r.dst  = st.col_int64(1);
    r.acc  = st.col_int64(2);
    r.virt = st.col_int64(3);
    rows.push_back(r);
  }

  for (const auto &r : rows) {
    // Collapse the DERIVED side (src) onto its primary template, but keep the
    // BASE (dst) un-collapsed: a template used as a base
    // (`class Cache : public Singleton<Cache>`) is its OWN design entity, so we
    // want `Cache generalizes Singleton<Cache>` and let the separate
    // instantiates(11) edge carry `Singleton<Cache> -> Singleton`.  (Pre-CRTP-
    // fix this was moot -- no base specifier had an instantiates(5) Layer-0
    // edge, so collapsing the dst was always a no-op.)
    int64_t src = cpp_collapse_to_primary(db, r.src);
    int64_t dst = r.dst;
    if (src == dst) {
      continue; // no self-edge
    }
    int64_t ek = is_interface(dst) ? 2 : 1;  // implements=2 or generalizes=1
    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, ?, 1, NULL, 1, ?, ?, NULL, 0) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  access     = excluded.access, "
        "  is_virtual = excluded.is_virtual");
    ins.bind(1, src);
    ins.bind(2, dst);
    ins.bind(3, ek);
    ins.bind(4, r.acc);
    ins.bind(5, r.virt);
    ins.step_done();
  }
}

// Phase 2: specializes(3) from Layer-0 specializes(4) edges between entity
// symbols. These come ONLY from EXPLICIT / PARTIAL specializations (the
// extractor emits kind 4 for those and kind 5 (instantiates) for plain
// instantiations, so the two are disjoint at Layer-0). Mirrors
// entity_rollup._materialise_specializes.
static void cpp_materialise_specializes(cidx::SqliteDb &db) {
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN symbol dst ON dst.id = e.dst_id "
      "WHERE e.kind = 4 "
      "  AND src.kind IN (2,3,4,5,31) "
      "  AND dst.kind IN (2,3,4,5,31)");
  std::vector<std::pair<int64_t,int64_t>> rows;
  while (st.step()) {
    rows.emplace_back(st.col_int64(0), st.col_int64(1));
  }
  for (const auto &[src0, dst0] : rows) {
    // The specialization is its OWN design entity -- do NOT collapse the SOURCE
    // onto the primary (that would self-suppress the edge). Collapse only the
    // destination (already the primary; this is a no-op there but keeps the
    // phase robust to chains).
    int64_t src = src0;
    int64_t dst = cpp_collapse_to_primary(db, dst0);
    if (src == dst) {
      continue;
    }
    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, 3, 1, NULL, 1, 0, 0, NULL, 0) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  count = entity_edge.count + 1");
    ins.bind(1, src);
    ins.bind(2, dst);
    ins.step_done();
  }
}

// Phase 2b: instantiates(11) from Layer-0 instantiates(5) edges between entity
// symbols. src = the concrete instance `X<B>`, dst = the primary template `X`.
// An implicit instantiation is a distinct design entity (UML <<bind>>), so --
// exactly like specializes -- the SOURCE is kept un-collapsed (collapsing it
// would follow its own kind-5 edge to the primary and self-suppress the row).
// Mirrors entity_rollup._materialise_instantiates.
static void cpp_materialise_instantiates(cidx::SqliteDb &db) {
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN symbol dst ON dst.id = e.dst_id "
      "WHERE e.kind = 5 "
      "  AND src.kind IN (2,3,4,5,31) "
      "  AND dst.kind IN (2,3,4,5,31)");
  std::vector<std::pair<int64_t,int64_t>> rows;
  while (st.step()) {
    rows.emplace_back(st.col_int64(0), st.col_int64(1));
  }
  for (const auto &[src0, dst0] : rows) {
    int64_t src = src0;
    int64_t dst = cpp_collapse_to_primary(db, dst0);
    if (src == dst) {
      continue;
    }
    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, 11, 1, NULL, 1, 0, 0, NULL, 0) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  count = entity_edge.count + 1");
    ins.bind(1, src);
    ins.bind(2, dst);
    ins.step_done();
  }
}

// Classify field type spelling → (entity_edge kind, multiplicity).
// Split the inside of a <...> on TOP-LEVEL commas (depth-aware). Mirrors
// entity_rollup._split_template_args.
static std::vector<std::string> cpp_split_template_args(const std::string &inner) {
  std::vector<std::string> args;
  int depth = 0;
  std::string cur;
  auto flush = [&] {
    size_t a = cur.find_first_not_of(' ');
    if (a != std::string::npos) {
      size_t b = cur.find_last_not_of(' ');
      args.push_back(cur.substr(a, b - a + 1));
    }
    cur.clear();
  };
  for (char ch : inner) {
    if (ch == '<') { ++depth; cur.push_back(ch); }
    else if (ch == '>') { --depth; cur.push_back(ch); }
    else if (ch == ',' && depth == 0) { flush();
    } else {
      {
        cur.push_back(ch);
      }
    }
  }
  flush();
  return args;
}

// For `s` starting with a `...<` wrapper `prefix`, return the VALUE type: the
// LAST top-level template arg (map<K,V> -> V). Mirrors _wrapper_value_type.
static std::string cpp_wrapper_value_type(const std::string &s,
                                          const char *prefix) {
  std::string inner = s.substr(strlen(prefix));
  while (!inner.empty() && inner.back() == ' ') {
    inner.pop_back();
  }
  if (!inner.empty() && inner.back() == '>') {
    inner.pop_back();
  }
  while (!inner.empty() && inner.back() == ' ') {
    inner.pop_back();
  }
  auto args = cpp_split_template_args(inner);
  return args.empty() ? inner : args.back();
}

static std::pair<int64_t,int64_t> cpp_classify_field_type(
    const std::string &type_info) {
  const std::string s = [&] {
    std::string r = type_info;
    // Strip const/volatile
    for (const auto *q : {"const ", "volatile "}) {
      std::string::size_type p;
      while ((p = r.find(q)) != std::string::npos) {
        r.erase(p, strlen(q));
      }
    }
    while (!r.empty() && r.front() == ' ') {
      r.erase(r.begin());
    }
    while (!r.empty() && r.back() == ' ') {
      r.pop_back();
    }
    return r;
  }();
  // Array
  if (!s.empty() && s.back() == ']') {
    return {4, 4};
  }
  // Containers
  static const char *containers[] = {
    "std::vector<", "vector<", "std::list<", "list<",
    "std::deque<", "deque<", "std::set<", "set<",
    "std::unordered_set<", "unordered_set<",
    "std::map<", "std::unordered_map<", nullptr
  };
  for (const char **c = containers; (*c) != nullptr; ++c) {
    if (s.substr(0, strlen(*c)) == *c) {
      // Classify the VALUE type (last template arg, so map<K,V> uses V).
      auto [ik, _] = cpp_classify_field_type(cpp_wrapper_value_type(s, *c));
      return {ik, 3};
    }
  }
  // unique_ptr / optional -> composes (EXCLUSIVE ownership: destroyed with the
  // owner, cannot outlive it -- same lifetime as a value member), 0..1.
  static const char *excl[] = {"std::unique_ptr<", "unique_ptr<",
                                "std::optional<", "optional<", nullptr};
  for (const char **u = excl; (*u) != nullptr; ++u) {
    if (s.substr(0, strlen(*u)) == *u) {
      return {4, 2}; // composes=4
    }
  }
  // shared_ptr -> aggregates (SHARED ownership: the pointee can outlive the
  // owner while other shared_ptrs keep it alive).
  static const char *shared[] = {"std::shared_ptr<", "shared_ptr<", nullptr};
  for (const char **u = shared; (*u) != nullptr; ++u) {
    if (s.substr(0, strlen(*u)) == *u) {
      return {5, 2}; // aggregates=5
    }
  }
  static const char *weak_raw[] = {"std::weak_ptr<", "weak_ptr<", nullptr};
  for (const char **w = weak_raw; (*w) != nullptr; ++w) {
    if (s.substr(0, strlen(*w)) == *w) {
      return {6, 2}; // associates=6
    }
  }
  if (!s.empty() && s.back() == '*') {
    return {6, 2};
  }
  if (!s.empty() && s.back() == '&') {
    return {6, 2};
  }
  return {4, 1};  // composes=4, multiplicity=1 (value)
}

// Resolve entity from type spelling (strips wrappers, looks up by qual_name/spelling).
static std::optional<int64_t> cpp_resolve_entity_from_type(
    cidx::SqliteDb &db, std::string type_info) {
  // Strip qualifiers
  for (const auto *q : {"const ", "volatile "}) {
    std::string::size_type p;
    while ((p = type_info.find(q)) != std::string::npos) {
      type_info.erase(p, strlen(q));
    }
  }
  while (!type_info.empty() && type_info.front() == ' ') {
    type_info.erase(type_info.begin());
  }
  while (!type_info.empty() && type_info.back() == ' ') {
    type_info.pop_back();
  }
  // Strip trailing * & []
  bool stripped = true;
  while (stripped) {
    stripped = false;
    if (!type_info.empty() && type_info.back() == '*') {
      type_info.pop_back(); stripped = true;
    } else if (!type_info.empty() && type_info.back() == '&') {
      type_info.pop_back(); stripped = true;
    } else if (!type_info.empty() && type_info.back() == ']') {
      auto p = type_info.rfind('[');
      if (p != std::string::npos) { type_info = type_info.substr(0,p); stripped = true; }
    }
    while (!type_info.empty() && type_info.back() == ' ') {
      type_info.pop_back();
    }
  }
  // Strip smart-ptr / container wrappers
  static const char *wrappers[] = {
    "std::unique_ptr<", "unique_ptr<", "std::shared_ptr<", "shared_ptr<",
    "std::weak_ptr<",   "weak_ptr<",   "std::optional<",   "optional<",
    "std::vector<", "vector<", "std::list<", "list<",
    "std::deque<", "deque<", "std::set<", "set<",
    "std::unordered_set<", "unordered_set<",
    "std::map<", "std::unordered_map<", nullptr
  };
  for (const char **w = wrappers; (*w) != nullptr; ++w) {
    if (type_info.substr(0, strlen(*w)) == *w) {
      // Recurse on the VALUE type (last template arg) so map<K,V> -> V and
      // nested generics peel one level at a time.
      return cpp_resolve_entity_from_type(db, cpp_wrapper_value_type(type_info, *w));
    }
  }
  // Lookup by qual_name
  auto st1 = db.prepare(
      "SELECT id FROM symbol WHERE qual_name = ? AND kind IN (2,3,4,5) LIMIT 1");
  st1.bind(1, std::string_view(type_info));
  if (st1.step()) {
    return st1.col_int64(0);
  }
  // Lookup by spelling
  auto st2 = db.prepare(
      "SELECT id FROM symbol WHERE spelling = ? AND kind IN (2,3,4,5) LIMIT 1");
  st2.bind(1, std::string_view(type_info));
  if (st2.step()) {
    return st2.col_int64(0);
  }
  return std::nullopt;
}

// Phase 3: composes/aggregates/associates from field_of(8) edges.
static void cpp_materialise_field_relations(cidx::SqliteDb &db) {
  struct FieldRow {
    int64_t field_id, owner_id, field_kind_int;
    std::string type_info, field_access;
  };
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id, s.type_info, s.kind AS field_kind, "
      "       s.access AS field_access "
      "FROM edge e "
      "JOIN symbol s ON s.id = e.src_id "
      "JOIN symbol owner ON owner.id = e.dst_id "
      "WHERE e.kind = 8 "
      "  AND owner.kind IN (2,3,4,5) "
      "  AND s.kind IN (6, 21)");
  std::vector<FieldRow> rows;
  while (st.step()) {
    FieldRow r;
    r.field_id       = st.col_int64(0);
    r.owner_id       = st.col_int64(1);
    r.type_info      = st.col_text(2);
    r.field_kind_int = st.col_int64(3);
    r.field_access   = st.col_text(4);
    rows.push_back(r);
  }

  static const std::map<std::string,int64_t> acc_map = {
    {"public",0}, {"protected",1}, {"private",2}
  };

  for (const auto &r : rows) {
    if (r.field_kind_int != 6) {
      continue; // only data members
    }
    if (r.type_info.empty()) {
      continue;
    }

    // Stage 4: prefer a structural member -> NAMED-INSTANCE of_type(20) edge
    // (v34: was uses(7)). A `X<B> m_;` member mints the `X<B>` instance
    // (is_named_instance=1) and the
    // extractor records an of_type edge member -> instance keyed on the spec USR
    // (unambiguous across namespaces -- unlike a display_name match). The named
    // instance is its OWN design entity, so it is NOT collapsed onto the primary
    // -> we emit `A composes/associates X<B>`, completing A -> X<B> -> B. Reached
    // ONLY for minted named instances (non-system specializations); `std::vector
    // <Foo>` is never minted, so its peel-to-Foo resolution below is unchanged.
    std::optional<int64_t> ref_entity_id;
    bool skip_ref_collapse = false;
    auto nist = db.prepare(
        "SELECT e.dst_id FROM edge e "
        "JOIN symbol s ON s.id = e.dst_id "
        "WHERE e.src_id = ? AND e.kind = 20 AND s.is_named_instance = 1 "
        "ORDER BY e.dst_id LIMIT 1");
    nist.bind(1, r.field_id);
    if (nist.step()) {
      ref_entity_id = nist.col_int64(0);
      skip_ref_collapse = true;
    }

    if (!ref_entity_id) {
      // Try template_arg.ref_id first.  Use the LAST type arg (highest position)
      // so map<K,V> picks the VALUE V, not the key K; single-arg containers /
      // smart-ptrs are unaffected.
      auto tst = db.prepare(
          "SELECT ref_id FROM template_arg WHERE owner_id = ? "
          "AND arg_kind = 1 AND ref_id IS NOT NULL ORDER BY position DESC LIMIT 1");
      tst.bind(1, r.field_id);
      if (tst.step()) {
        ref_entity_id = tst.col_int64(0);
      }

      if (!ref_entity_id) {
        ref_entity_id = cpp_resolve_entity_from_type(db, r.type_info);
      }
    }
    if (!ref_entity_id) {
      continue;
    }

    // Confirm referent is entity
    auto ck = db.prepare("SELECT kind FROM symbol WHERE id = ?");
    ck.bind(1, *ref_entity_id);
    if (!ck.step()) {
      continue;
    }
    const int64_t ref_kind = ck.col_int64(0);
    if (ref_kind != 2 && ref_kind != 3 && ref_kind != 4 && ref_kind != 5) {
      continue;
    }

    auto [ek, mult] = cpp_classify_field_type(r.type_info);
    int64_t access_int = 0;
    if (acc_map.contains(r.field_access)) {
      access_int = acc_map.at(r.field_access);
    }

    // Collapse the owner onto its primary template.  The referent is collapsed
    // too UNLESS it is a named instance (kept un-collapsed so the edge points at
    // `X<B>`, not the primary `X`).
    int64_t owner_pid = cpp_collapse_to_primary(db, r.owner_id);
    int64_t ref_pid = skip_ref_collapse
                          ? *ref_entity_id
                          : cpp_collapse_to_primary(db, *ref_entity_id);
    if (owner_pid == ref_pid) {
      continue;
    }

    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, ?, 1, ?, ?, ?, 0, NULL, 0) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  count = entity_edge.count + 1");
    ins.bind(1, owner_pid);
    ins.bind(2, ref_pid);
    ins.bind(3, ek);
    ins.bind(4, r.field_id);
    ins.bind(5, mult);
    ins.bind(6, access_int);
    ins.step_done();
  }
}

// Strip wrappers/qualifiers/ptr-ref-array off a field type, returning the bare
// innermost token (e.g. 'std::vector<T>' -> 'T', 'const T *' -> 'T'). Mirrors
// entity_rollup._strip_to_param_core. Used to discover which template parameter
// a primary-template member binds.
static std::string cpp_strip_to_param_core(const std::string &type_spelling) {
  auto strip_quals = [](std::string s) {
    for (const auto *q : {"const ", "volatile "}) {
      std::string::size_type p;
      while ((p = s.find(q)) != std::string::npos) {
        s.erase(p, strlen(q));
      }
    }
    while (!s.empty() && s.front() == ' ') {
      s.erase(s.begin());
    }
    while (!s.empty() && s.back() == ' ') {
      s.pop_back();
    }
    return s;
  };
  std::string s = strip_quals(type_spelling);
  while (!s.empty() && (s.back() == '&' || s.back() == '*' || s.back() == ']')) {
    if (s.back() == ']') {
      auto p = s.rfind('[');
      s = (p == std::string::npos) ? std::string() : s.substr(0, p);
    } else {
      s.pop_back();
    }
    while (!s.empty() && s.front() == ' ') {
      s.erase(s.begin());
    }
    while (!s.empty() && s.back() == ' ') {
      s.pop_back();
    }
  }
  s = strip_quals(s);
  static const char *wrappers[] = {
    "std::unique_ptr<", "unique_ptr<", "std::shared_ptr<", "shared_ptr<",
    "std::weak_ptr<",   "weak_ptr<",   "std::optional<",   "optional<",
    "std::vector<", "vector<", "std::list<", "list<",
    "std::deque<", "deque<", "std::set<", "set<",
    "std::unordered_set<", "unordered_set<",
    "std::map<", "std::unordered_map<", nullptr
  };
  for (const char **w = wrappers; (*w) != nullptr; ++w) {
    if (s.substr(0, strlen(*w)) == *w) {
      return cpp_strip_to_param_core(cpp_wrapper_value_type(s, *w));
    }
  }
  return s;
}

// Phase 3b: composes/aggregates/associates for NAMED template instances.
// A `using Y = X<B>;` mints the X<B> instance (is_named_instance=1) but libclang
// materialises NO members for it, so Phase 3 cannot classify them. Instead read
// the PRIMARY's members and SUBSTITUTE the instance's bound type: for a member
// binding template param i (bare T, vector<T>, unique_ptr<T>, T*, ...), look up
// the instance's template_arg at position i (-> B) and emit X<B> <ownership> B.
// The instance is NOT collapsed onto the primary. Mirrors
// entity_rollup._materialise_instance_composition.
static void cpp_materialise_instance_composition(cidx::SqliteDb &db) {
  struct InstRow { int64_t inst_id, prim_id; };
  std::vector<InstRow> instances;
  {
    auto st = db.prepare(
        "SELECT e.src_id, e.dst_id "
        "FROM edge e "
        "JOIN symbol inst ON inst.id = e.src_id "
        "JOIN symbol prim ON prim.id = e.dst_id "
        "WHERE e.kind = 5 AND inst.is_named_instance = 1 AND prim.kind = 31 "
        "ORDER BY e.src_id, e.dst_id");
    while (st.step()) {
      instances.push_back(
          {.inst_id = st.col_int64(0), .prim_id = st.col_int64(1)});
    }
  }

  static const std::map<std::string,int64_t> acc_map = {
    {"public",0}, {"protected",1}, {"private",2}
  };

  for (const auto &inst : instances) {
    // primary template parameter NAME -> position (type params only)
    std::map<std::string,int64_t> param_pos;
    {
      auto st = db.prepare(
          "SELECT position, name FROM template_param WHERE owner_id = ? "
          "AND param_kind = 1 ORDER BY position");
      st.bind(1, inst.prim_id);
      while (st.step()) {
        const std::string nm = st.col_text(1);
        if (!nm.empty()) {
          param_pos.emplace(nm, st.col_int64(0));
        }
      }
    }

    // instance bound TYPE args: position -> ref_id (the entity B). NULL ref_id
    // (builtin arg) recorded as nullopt so it is skipped below.
    std::map<int64_t, std::optional<int64_t>> bound;
    {
      auto st = db.prepare(
          "SELECT position, ref_id FROM template_arg WHERE owner_id = ? "
          "AND arg_kind = 1 ORDER BY position");
      st.bind(1, inst.inst_id);
      while (st.step()) {
        std::optional<int64_t> ref;
        if (!st.col_is_null(1)) {
          ref = st.col_int64(1);
        }
        bound[st.col_int64(0)] = ref;
      }
    }

    // primary template's data members
    struct FieldRow { int64_t field_id; std::string type_info, access; };
    std::vector<FieldRow> fields;
    {
      auto st = db.prepare(
          "SELECT e.src_id, s.type_info, s.access "
          "FROM edge e "
          "JOIN symbol s ON s.id = e.src_id "
          "WHERE e.kind = 8 AND e.dst_id = ? AND s.kind = 6 "
          "ORDER BY e.src_id");
      st.bind(1, inst.prim_id);
      while (st.step()) {
        fields.push_back({.field_id = st.col_int64(0),
                          .type_info = st.col_text(1),
                          .access = st.col_text(2)});
      }
    }

    for (const auto &f : fields) {
      if (f.type_info.empty()) {
        continue;
      }
      const std::string core = cpp_strip_to_param_core(f.type_info);
      auto pit = param_pos.find(core);
      int64_t ref_entity_id;
      if (pit != param_pos.end()) {
        // Parameterised member (binds T): substitute the instance's bound type
        // -> X<B> <ownership> B.
        auto bit = bound.find(pit->second);
        if (bit == bound.end() || !bit->second) {
          continue; // builtin/unindexed
        }
        ref_entity_id = *bit->second;
      } else {
        // Stage 3: CONCRETE (non-parameterised) member, e.g. `Widget w;` on the
        // primary -> carry `X<B> <ownership> Widget` onto the instance too.
        // System / unindexed concrete types resolve to nullopt and are skipped,
        // so no std:: explosion.
        auto re = cpp_resolve_entity_from_type(db, f.type_info);
        if (!re) {
          continue;
        }
        ref_entity_id = *re;
      }

      auto ck = db.prepare("SELECT kind FROM symbol WHERE id = ?");
      ck.bind(1, ref_entity_id);
      if (!ck.step()) {
        continue;
      }
      const int64_t ref_kind = ck.col_int64(0);
      if (ref_kind != 2 && ref_kind != 3 && ref_kind != 4 && ref_kind != 5) {
        continue;
      }
      if (inst.inst_id == ref_entity_id) {
        continue;
      }

      auto [ek, mult] = cpp_classify_field_type(f.type_info);
      int64_t access_int = 0;
      if (acc_map.contains(f.access)) {
        access_int = acc_map.at(f.access);
      }

      auto ins = db.prepare(
          "INSERT INTO entity_edge "
          "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
          " access, is_virtual, create_form, partial) "
          "VALUES (?, ?, ?, 1, ?, ?, ?, 0, NULL, 0) "
          "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
          "  count = entity_edge.count + 1");
      ins.bind(1, inst.inst_id);
      ins.bind(2, ref_entity_id);
      ins.bind(3, ek);
      ins.bind(4, f.field_id);
      ins.bind(5, mult);
      ins.bind(6, access_int);
      ins.step_done();
    }
  }
}

// Phase 4: creates(7) / destroys(9) from PR1 construction/destruction edges.
static void cpp_materialise_creates_destroys(cidx::SqliteDb &db) {
  // Layer-0 construct/destroy edge.kind -> create_form
  static const std::map<int64_t,int64_t> form_map = {
    {10,3},{11,4},{12,5},{13,7},{14,8},{15,6}
  };
  constexpr int64_t destroy_kind = 16;

  struct SiteRow { int64_t src_fn, dst_sym, l0_kind; };
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id, e.kind "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN symbol dst ON dst.id = e.dst_id "
      "WHERE e.kind IN (10,11,12,13,14,15,16)");
  std::vector<SiteRow> rows;
  while (st.step()) {
    rows.push_back({.src_fn = st.col_int64(0),
                    .dst_sym = st.col_int64(1),
                    .l0_kind = st.col_int64(2)});
  }

  for (const auto &r : rows) {
    // Enclosing entity (method_of=9, owner must be entity)
    auto own_st = db.prepare(
        "SELECT e.dst_id FROM edge e "
        "JOIN symbol owner ON owner.id = e.dst_id "
        "WHERE e.src_id = ? AND e.kind = 9 "
        "  AND owner.kind IN (2,3,4,5) LIMIT 1");
    own_st.bind(1, r.src_fn);
    if (!own_st.step()) {
      continue; // free fn: no entity src
    }
    const int64_t owner_entity = own_st.col_int64(0);

    // Target entity: ctor/dtor parent → record
    std::optional<int64_t> target;
    auto par_st = db.prepare(
        "SELECT id FROM symbol "
        "WHERE usr = (SELECT parent_usr FROM symbol WHERE id = ?) "
        "  AND kind IN (2,3,4,5) LIMIT 1");
    par_st.bind(1, r.dst_sym);
    if (par_st.step()) {
      target = par_st.col_int64(0);
    } else {
      // dst itself might be entity (rare)
      auto dk = db.prepare("SELECT kind FROM symbol WHERE id = ?");
      dk.bind(1, r.dst_sym);
      if (dk.step()) {
        int64_t k = dk.col_int64(0);
        if (k == 2 || k == 3 || k == 4 || k == 5) {
          target = r.dst_sym;
        }
      }
    }
    if (!target) {
      continue;
    }

    // Collapse both endpoints onto their primary template.
    int64_t owner_pid = cpp_collapse_to_primary(db, owner_entity);
    int64_t target_pid = cpp_collapse_to_primary(db, *target);
    if (owner_pid == target_pid) {
      continue;
    }

    if (r.l0_kind == destroy_kind) {
      auto ins = db.prepare(
          "INSERT INTO entity_edge "
          "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
          " access, is_virtual, create_form, partial) "
          "VALUES (?, ?, 9, 1, NULL, 1, 0, 0, NULL, 0) "
          "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
          "  count = entity_edge.count + 1");
      ins.bind(1, owner_pid);
      ins.bind(2, target_pid);
      ins.step_done();
    } else {
      int64_t create_form = form_map.at(r.l0_kind);
      int64_t partial = (r.l0_kind == 15) ? 1 : 0;
      auto ins = db.prepare(
          "INSERT INTO entity_edge "
          "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
          " access, is_virtual, create_form, partial) "
          "VALUES (?, ?, 7, 1, NULL, 1, 0, 0, ?, ?) "
          "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
          "  count = entity_edge.count + 1, "
          "  create_form = COALESCE(excluded.create_form, entity_edge.create_form), "
          "  partial = excluded.partial");
      ins.bind(1, owner_pid);
      ins.bind(2, target_pid);
      ins.bind(3, create_form);
      ins.bind(4, partial);
      ins.step_done();
    }
  }

  // By-value return (create_form=2): method return type → creates(7, partial=1)
  struct RetRow { int64_t method_id, owner_id; std::string type_info; };
  auto rst = db.prepare(
      "SELECT s.id, s.type_info, e.dst_id AS owner_id "
      "FROM symbol s "
      "JOIN edge e ON e.src_id = s.id AND e.kind = 9 "
      "JOIN symbol owner ON owner.id = e.dst_id AND owner.kind IN (2,3,4,5) "
      "WHERE s.kind IN (21, 24) AND s.type_info IS NOT NULL");
  std::vector<RetRow> ret_rows;
  while (rst.step()) {
    ret_rows.push_back({.method_id = rst.col_int64(0),
                        .owner_id = rst.col_int64(2),
                        .type_info = rst.col_text(1)});
  }
  for (const auto &r : ret_rows) {
    const std::string &ti = r.type_info;
    std::string ret_type;
    auto paren = ti.find('(');
    if (paren != std::string::npos && paren > 0) {
      ret_type = ti.substr(0, paren);
      while (!ret_type.empty() && ret_type.back() == ' ') {
        ret_type.pop_back();
      }
    } else {
      ret_type = ti;
    }
    if (ret_type.empty() || ret_type == "void" || ret_type == "auto") {
      continue;
    }
    auto ret_eid = cpp_resolve_entity_from_type(db, ret_type);
    if (!ret_eid) {
      continue;
    }

    // Collapse both endpoints onto their primary template.
    int64_t owner_pid = cpp_collapse_to_primary(db, r.owner_id);
    int64_t ret_pid = cpp_collapse_to_primary(db, *ret_eid);
    if (ret_pid == owner_pid) {
      continue; // constructors return own type
    }

    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, 7, 1, NULL, 1, 0, 0, 2, 1) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  count = entity_edge.count + 1");
    ins.bind(1, owner_pid);
    ins.bind(2, ret_pid);
    ins.step_done();
  }
}

// Phase 5: uses(8) from method→method calls across entity boundaries.
static void cpp_materialise_uses(cidx::SqliteDb &db) {
  struct UseRow { int64_t caller, callee, is_pure; };
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id, dst.is_pure "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN symbol dst ON dst.id = e.dst_id "
      "WHERE e.kind IN (1, 7) "
      "  AND src.kind IN (21, 8, 24, 25, 30) "
      "  AND dst.kind IN (21, 8, 24, 25, 30)");
  std::vector<UseRow> rows;
  while (st.step()) {
    rows.push_back({.caller = st.col_int64(0),
                    .callee = st.col_int64(1),
                    .is_pure = st.col_int64(2)});
  }
  for (const auto &r : rows) {
    // Caller owner entity
    auto co = db.prepare(
        "SELECT e.dst_id FROM edge e "
        "JOIN symbol owner ON owner.id = e.dst_id "
        "WHERE e.src_id = ? AND e.kind = 9 "
        "  AND owner.kind IN (2,3,4,5) LIMIT 1");
    co.bind(1, r.caller);
    if (!co.step()) {
      continue;
    }
    int64_t src_eid = co.col_int64(0);

    // Callee owner entity
    auto coe = db.prepare(
        "SELECT e.dst_id FROM edge e "
        "JOIN symbol owner ON owner.id = e.dst_id "
        "WHERE e.src_id = ? AND e.kind = 9 "
        "  AND owner.kind IN (2,3,4,5) LIMIT 1");
    coe.bind(1, r.callee);
    if (!coe.step()) {
      continue;
    }
    int64_t dst_eid = coe.col_int64(0);

    // Collapse both endpoints onto their primary template.
    src_eid = cpp_collapse_to_primary(db, src_eid);
    dst_eid = cpp_collapse_to_primary(db, dst_eid);
    if (src_eid == dst_eid) {
      continue;
    }
    int64_t partial = (r.is_pure != 0) ? 1 : 0;

    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, 8, 1, ?, 1, 0, 0, NULL, ?) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  count = entity_edge.count + 1, "
        "  partial = MAX(entity_edge.partial, excluded.partial)");
    ins.bind(1, src_eid);
    ins.bind(2, dst_eid);
    ins.bind(3, r.callee);
    ins.bind(4, partial);
    ins.step_done();
  }
}

// Phase 6: befriends(10) from friend(17) edges between entity symbols.
static void cpp_materialise_befriends(cidx::SqliteDb &db) {
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN symbol dst ON dst.id = e.dst_id "
      "WHERE e.kind = 17 "
      "  AND src.kind IN (2,3,4,5) "
      "  AND dst.kind IN (2,3,4,5)");
  std::vector<std::pair<int64_t,int64_t>> rows;
  while (st.step()) {
    rows.emplace_back(st.col_int64(0), st.col_int64(1));
  }
  for (const auto &[src0, dst0] : rows) {
    int64_t src = cpp_collapse_to_primary(db, src0);
    int64_t dst = cpp_collapse_to_primary(db, dst0);
    if (src == dst) {
      continue;
    }
    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, 10, 1, NULL, 1, 0, 0, NULL, 0) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), COALESCE(create_form, -1)) DO UPDATE SET "
        "  count = entity_edge.count + 1");
    ins.bind(1, src);
    ins.bind(2, dst);
    ins.step_done();
  }
}

// Phase 7: entity_node(id, kind) -- the materialized design type of every
// entity symbol. Mirrors entity_rollup._materialise_entity_nodes byte-identically.
// Abstractness (own pure-virtual methods + own data fields) decides
// class/abstract_class/interface (and the same split for class templates);
// union/enum keep their own type. The C++ keyword (class vs struct) is NOT
// distinguished here -- that lives at the low-level symbol layer.
void cpp_materialise_entity_nodes(cidx::SqliteDb &db) {
  // Usable standalone (the v21->v22 entity_node backfill in the Storage ctor
  // calls this directly), so it installs the per-pass RollupState itself when
  // one is not already active (i.e. when NOT called from materialise_entity_edges).
  CtxGuard guard(db);
  // entity_kind ids: class=1 abstract_class=2 interface=3 union=4 enum=5
  // class_template=6 abstract_class_template=7 interface_template=8.
  // is_interface / has_pure delegate to the precomputed owner-sets.
  const auto classify = [](int64_t sym_id, int64_t sym_kind) -> int64_t {
    if (sym_kind == 5) {
      return 5; // enum
    }
    if (sym_kind == 3) {
      return 4; // union
    }
    bool is_template = (sym_kind == 31);
    if (g_rollup_ctx->is_interface(sym_id)) {
      return is_template ? 8 : 3;
    }
    if (g_rollup_ctx->has_pure(sym_id)) {
      return is_template ? 7 : 2;
    }
    return is_template ? 6 : 1;
  };

  db.exec("DELETE FROM entity_node");
  std::vector<std::pair<int64_t, int64_t>> rows;  // (id, kind)
  {
    auto st = db.prepare("SELECT id, kind FROM symbol WHERE kind IN (2,3,4,5,31)");
    while (st.step()) {
      rows.emplace_back(st.col_int64(0), st.col_int64(1));
    }
  }
  for (const auto &[sym_id, sym_kind] : rows) {
    auto ins = db.prepare(
        "INSERT OR REPLACE INTO entity_node (id, kind) VALUES (?, ?)");
    ins.bind(1, sym_id);
    ins.bind(2, classify(sym_id, sym_kind));
    ins.step_done();
  }
  // v26: namespaces (kind 22) are first-class entity nodes too. A single
  // canonical node per namespace USR (already collapsed in `symbol`), so one
  // entity_node covers all its reopenings across files/components/repos.
  std::vector<int64_t> ns_ids;
  {
    auto st = db.prepare("SELECT id FROM symbol WHERE kind = 22");
    while (st.step()) {
      ns_ids.push_back(st.col_int64(0));
    }
  }
  for (const int64_t ns_id : ns_ids) {
    auto ins = db.prepare(
        "INSERT OR REPLACE INTO entity_node (id, kind) VALUES (?, ?)");
    ins.bind(1, ns_id);
    ins.bind(2, static_cast<int64_t>(9)); // entity_kind 9 = namespace
    ins.step_done();
  }
}

// v26: namespace --declares--> member entity node. A `declares` entity edge
// (kind 12) for every Layer-0 `contains`(3) edge whose SRC is a namespace
// (symbol kind 22) and whose DST is an entity node (record/enum/class-template/
// nested namespace -- present in entity_node). DIRECT only: `contains` is
// already the direct lexical link, so ABC never `declares` ABC::XXX's members
// -- ABC and ABC::XXX are distinct entities (content is not recursive). Members
// that are not entities (free functions, variables) have no entity_node and are
// intentionally skipped. Must run AFTER cpp_materialise_entity_nodes (reads
// entity_node). Mirrors entity_rollup._materialise_declares.
static void cpp_materialise_declares(cidx::SqliteDb &db) {
  auto st = db.prepare(
      "SELECT e.src_id, e.dst_id "
      "FROM edge e "
      "JOIN symbol src ON src.id = e.src_id "
      "JOIN entity_node en ON en.id = e.dst_id "
      "WHERE e.kind = 3 AND src.kind = 22");
  std::vector<std::pair<int64_t, int64_t>> rows;
  while (st.step()) {
    rows.emplace_back(st.col_int64(0), st.col_int64(1));
  }
  for (const auto &[src_id, dst_id] : rows) {
    if (src_id == dst_id) {
      continue;
    }
    auto ins = db.prepare(
        "INSERT INTO entity_edge "
        "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
        " access, is_virtual, create_form, partial) "
        "VALUES (?, ?, 12, 1, NULL, 1, 0, 0, NULL, 0) "
        "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, -1), "
        "COALESCE(create_form, -1)) DO NOTHING");
    ins.bind(1, src_id);
    ins.bind(2, dst_id);
    ins.step_done();
  }
}

void SqliteStorageService::materialise_entity_edges() {
  // Idempotent: full re-materialise each resolve. The DELETE runs INSIDE the
  // rebuild transaction so a failure in any phase rolls back to the previous
  // rows instead of leaving entity_edge empty (atomic resolve).
  //
  // RollupState precomputes the collapse next-hop map + interface owner-sets
  // ONCE for the whole pass (edge/symbol are read-only here), so every phase's
  // collapse / interface lookups are in-memory instead of per-row SQL.
  CtxGuard guard(db_);
  {
    auto txn = transaction();
    db_.exec("DELETE FROM entity_edge");
    const auto run = [&]([[maybe_unused]] const char *name,
                         void (*fn)(cidx::SqliteDb &)) { fn(db_); };
    run("inheritance", cpp_materialise_inheritance);
    run("specializes", cpp_materialise_specializes);
    run("instantiates", cpp_materialise_instantiates);
    run("field_relations", cpp_materialise_field_relations);
    run("instance_composition", cpp_materialise_instance_composition);
    run("creates_destroys", cpp_materialise_creates_destroys);
    run("uses", cpp_materialise_uses);
    run("befriends", cpp_materialise_befriends);
    run("entity_nodes", cpp_materialise_entity_nodes);
    run("declares", cpp_materialise_declares); // v26: needs entity_node populated
    txn.commit();
  }
}

int SqliteStorageService::resolve_pass() {
  // Roll up edge.count for calls/uses from edge_site counts.
  rollup_edge_counts();
  // v27: multi-definition. definition/def_edge are already written at index
  // time; here we count them into symbol.multi_def and fan per-body calls into
  // possible_call. Order: possible_call needs multi_def.
  set_multi_def();
  materialize_possible_calls();
  // Materialise virtual-dispatch caller edges (kind 18) from calls + overrides.
  materialize_dispatch_calls();
  // Materialise Layer-1 entity_edge from the Layer-0 graph.
  materialise_entity_edges();
  // Count remaining stub symbols: a minted placeholder never backfilled by a
  // real symbol -- resolved=0 with NO location (neither a definition nor a decl
  // site). NOT keyed on spelling -- stubs are now minted NAMED, so the absence
  // of any location is the robust signal (matches Sym::is_stub).
  auto st = db_.prepare(
      "SELECT COUNT(*) FROM symbol "
      "WHERE resolved = 0 AND file_id IS NULL AND decl_file_id IS NULL");
  if (!st.step()) {
    return 0;
  }
  return static_cast<int>(st.col_int64(0));
}

// -- fuzzy matching
// -----------------------------------------------------------------

} // namespace cidx
