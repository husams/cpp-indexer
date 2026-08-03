// Per-file symbol/edge sweeps, the definition tier and dispatch roll-up.
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

namespace {

void stage_transform_changes(SqliteDb &db, const TransformChangeSet &changes) {
  db.exec("CREATE TEMP TABLE IF NOT EXISTS cidx_transform_changes ("
          "kind TEXT NOT NULL, id INTEGER NOT NULL, PRIMARY KEY(kind, id));"
          "DELETE FROM temp.cidx_transform_changes");
  auto insert =
      db.prepare("INSERT OR IGNORE INTO temp.cidx_transform_changes(kind, id) "
                 "VALUES (?, ?)");
  const auto stage = [&](std::string_view kind,
                         const std::vector<std::int64_t> &ids) {
    for (const std::int64_t id : ids) {
      insert.bind(1, kind);
      insert.bind(2, id);
      insert.step_done();
      insert.reset();
    }
  };
  stage("file", changes.file_ids);
  stage("symbol", changes.symbol_ids);
  stage("edge", changes.edge_ids);
  stage("definition", changes.definition_ids);
}

} // namespace

std::vector<int64_t>
SqliteStorageService::symbol_ids_for_file(int64_t file_id) {
  // Definition site OR declaration site: a class declared in a header and
  // defined in a source file is owned by both, which is what the unused rule
  // needs (either site makes the header the provider).
  //
  // The `definition` UNION is not redundant. symbol.usr is UNIQUE, so symbols
  // that share a USR across translation units collapse into ONE row whose
  // file_id is a single arbitrary winner -- `int main(int, char**)` is
  // `c:@F@main#I#**C#` in every TU that defines it, so seven of cidx's own
  // eight mains would otherwise have no owner at all. A file with no owners
  // makes every one of its includes look unreferenced, which is exactly the
  // silent false positive this rule must not produce. The v27 `definition`
  // table keeps the per-file bodies the collapse loses, so it is the
  // authoritative record of what a file actually defines.
  auto st = db_.prepare("SELECT id FROM symbol "
                        "WHERE file_id = ? OR decl_file_id = ? "
                        "UNION "
                        "SELECT symbol_id FROM definition WHERE file_id = ? "
                        "ORDER BY id");
  st.bind(1, file_id);
  st.bind(2, file_id);
  st.bind(3, file_id);
  std::vector<int64_t> out;
  while (st.step()) {
    out.push_back(st.col_int64(0));
  }
  return out;
}

std::vector<int64_t>
SqliteStorageService::edge_targets_from(const std::vector<int64_t> &src_ids) {
  std::vector<int64_t> out;
  if (src_ids.empty()) {
    return out;
  }
  // No kind filter: EVERY persisted relation counts as a reference. A new
  // edge_kind is therefore covered automatically, which is the point -- the
  // rule must not silently miss a relation the indexer learns later.
  const std::string sql = "SELECT DISTINCT dst_id FROM edge WHERE src_id IN (" +
                          in_placeholders(src_ids.size()) + ") ORDER BY dst_id";
  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < src_ids.size(); ++i) {
    st.bind(static_cast<int>(i + 1), src_ids[i]);
  }
  while (st.step()) {
    out.push_back(st.col_int64(0));
  }
  return out;
}

std::vector<int64_t>
SqliteStorageService::def_edge_targets_for_file(int64_t file_id) {
  auto st = db_.prepare("SELECT DISTINCT de.dst_id FROM def_edge de "
                        "JOIN definition d ON d.id = de.src_def_id "
                        "WHERE d.file_id = ? ORDER BY de.dst_id");
  st.bind(1, file_id);
  std::vector<int64_t> out;
  while (st.step()) {
    out.push_back(st.col_int64(0));
  }
  return out;
}

std::vector<int64_t>
SqliteStorageService::type_ids_used_by(const std::vector<int64_t> &symbol_ids) {
  std::vector<int64_t> out;
  if (symbol_ids.empty()) {
    return out;
  }
  const std::string ph = in_placeholders(symbol_ids.size());
  const std::string sql = "SELECT DISTINCT type_id FROM symbol_type "
                          "WHERE symbol_id IN (" +
                          ph +
                          ") "
                          "UNION "
                          "SELECT DISTINCT type_id FROM parameter "
                          "WHERE owner_id IN (" +
                          ph +
                          ") AND type_id IS NOT NULL "
                          "ORDER BY type_id";
  auto st = db_.prepare(sql);
  int idx = 1;
  for (int pass = 0; pass < 2; ++pass) {
    for (const int64_t id : symbol_ids) {
      st.bind(idx++, id);
    }
  }
  while (st.step()) {
    out.push_back(st.col_int64(0));
  }
  return out;
}

std::vector<int64_t> SqliteStorageService::symbols_named_by_types(
    const std::vector<int64_t> &type_ids) {
  std::vector<int64_t> out;
  if (type_ids.empty()) {
    return out;
  }
  // Forward structural closure: from each seed node follow type_edge (pointee,
  // element_type, alias_of, return_type, param_type, template_argument_type)
  // and canonical_id. `const Foo&` is a reference node whose pointee is a
  // const-qualified Foo; vector<Foo> names Foo as a template argument; an alias
  // reaches Foo through alias_of. All three must count as naming Foo.
  const std::string sql =
      "WITH RECURSIVE reach(id) AS ("
      "  SELECT id FROM type_node WHERE id IN (" +
      in_placeholders(type_ids.size()) +
      ") "
      "  UNION "
      "  SELECT te.dst_id FROM type_edge te JOIN reach r ON te.src_id = r.id "
      "  UNION "
      "  SELECT tn.canonical_id FROM type_node tn JOIN reach r ON tn.id = r.id "
      "   WHERE tn.canonical_id IS NOT NULL"
      ") "
      "SELECT DISTINCT s.id FROM reach "
      "JOIN type_node t ON t.id = reach.id "
      "JOIN symbol s ON s.usr = t.decl_usr "
      "WHERE t.decl_usr IS NOT NULL "
      "ORDER BY s.id";
  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < type_ids.size(); ++i) {
    st.bind(static_cast<int>(i + 1), type_ids[i]);
  }
  while (st.step()) {
    out.push_back(st.col_int64(0));
  }
  return out;
}

void SqliteStorageService::delete_edges_for_file(int64_t file_id) {
  // Exclude contains (kind=3): declaration-level structural edges emitted
  // during header indexing. Namespaces reopen in every .cpp TU, so deleting
  // contains on each re-index would permanently erase the header-phase edges.
  // Contains edges are idempotent (UPSERT); excluding them here is safe.
  auto st = db_.prepare("DELETE FROM edge WHERE kind != 3 AND src_id IN "
                        "(SELECT id FROM symbol WHERE file_id = ?)");
  st.bind(1, file_id);
  st.step_done();
}

TransformWork SqliteStorageService::rollup_edge_counts() {
  // For calls (1) and uses (7): set count = COUNT(edge_site) so the edge
  // reflects true site count after multi-TU indexing.
  db_.exec("UPDATE edge SET count = ("
           "  SELECT COUNT(*) FROM edge_site WHERE edge_site.edge_id = edge.id"
           ") "
           "WHERE kind IN (1, 7)"
           "  AND EXISTS (SELECT 1 FROM edge_site WHERE edge_site.edge_id = "
           "edge.id)");
  return TransformWork{.rows_updated = db_.changes()};
}

TransformWork
SqliteStorageService::rollup_edge_counts(const TransformChangeSet &changes) {
  stage_transform_changes(db_, changes);
  auto update = db_.prepare(
      "UPDATE edge SET count = (SELECT COUNT(*) FROM edge_site "
      "WHERE edge_site.edge_id = edge.id) WHERE kind IN (1, 7) AND id IN "
      "(SELECT id FROM temp.cidx_transform_changes WHERE kind = 'edge') "
      "AND EXISTS (SELECT 1 FROM edge_site WHERE edge_site.edge_id = edge.id)");
  update.step_done();
  return TransformWork{
      .rows_scanned = static_cast<std::int64_t>(changes.edge_ids.size()),
      .rows_updated = db_.changes(),
      .affected_keys = static_cast<std::int64_t>(changes.edge_ids.size())};
}

TransformWork SqliteStorageService::materialize_dispatch_calls() {
  // Materialise virtual-dispatch caller edges (kind 18, 'dispatch_calls').
  // A static 'calls' edge (1) into a virtual method B understates reality: at
  // run time the call can land on any method that overrides B, transitively
  // down the class hierarchy. libclang records the site against the declared
  // target (e.g. execute() -> base::doSomething for a pure-virtual base), so
  // callers(child::doSomething) -- the concrete override -- comes back empty
  // even though execute reaches it by dynamic dispatch. For each caller -> B
  // calls edge and each transitive override M of B, store a caller -> M edge so
  // callers(M, include_overrides) recovers the virtual caller in one hop. The
  // bridge is the existing 'overrides' edge (6). Idempotent: kind-18 edges are
  // deleted and rebuilt each pass. Mirrors
  // indexer/storage.py:materialize_dispatch_calls() byte-identically.
  db_.exec("DELETE FROM edge WHERE kind = 18");
  const std::int64_t deleted = db_.changes();
  db_.exec("WITH RECURSIVE dispatch(base_id, target_id) AS ("
           "    SELECT dst_id AS base_id, src_id AS target_id"
           "    FROM edge WHERE kind = 6"
           "  UNION"
           "    SELECT d.base_id, o.src_id"
           "    FROM dispatch d"
           "    JOIN edge o ON o.dst_id = d.target_id AND o.kind = 6"
           ") "
           "INSERT OR IGNORE INTO edge (src_id, dst_id, kind, count) "
           "SELECT c.src_id, d.target_id, 18, c.count "
           "FROM edge c "
           "JOIN dispatch d ON d.base_id = c.dst_id "
           "WHERE c.kind = 1 "
           "  AND c.src_id != d.target_id");
  return TransformWork{.rows_inserted = db_.changes(), .rows_deleted = deleted};
}

TransformWork SqliteStorageService::materialize_dispatch_calls(
    const TransformChangeSet &changes) {
  stage_transform_changes(db_, changes);
  db_.exec(
      "CREATE TEMP TABLE IF NOT EXISTS cidx_dispatch_callers ("
      "id INTEGER PRIMARY KEY); DELETE FROM temp.cidx_dispatch_callers; "
      "INSERT OR IGNORE INTO temp.cidx_dispatch_callers(id) "
      "WITH RECURSIVE family(id) AS ("
      "  SELECT id FROM temp.cidx_transform_changes WHERE kind = 'symbol' "
      "  UNION SELECT e.dst_id FROM edge e JOIN family f ON e.src_id = f.id "
      "        WHERE e.kind = 6 "
      "  UNION SELECT e.src_id FROM edge e JOIN family f ON e.dst_id = f.id "
      "        WHERE e.kind = 6"
      ") SELECT id FROM temp.cidx_transform_changes WHERE kind = 'symbol' "
      "UNION SELECT c.src_id FROM edge c JOIN family f ON c.dst_id = f.id "
      "WHERE c.kind = 1");
  db_.exec("DELETE FROM edge WHERE kind = 18 AND src_id IN "
           "(SELECT id FROM temp.cidx_dispatch_callers)");
  const std::int64_t deleted = db_.changes();
  db_.exec("WITH RECURSIVE dispatch(base_id, target_id) AS ("
           "  SELECT dst_id, src_id FROM edge WHERE kind = 6 "
           "  UNION SELECT d.base_id, o.src_id FROM dispatch d "
           "        JOIN edge o ON o.dst_id = d.target_id AND o.kind = 6"
           ") INSERT OR IGNORE INTO edge (src_id, dst_id, kind, count) "
           "SELECT c.src_id, d.target_id, 18, c.count FROM edge c "
           "JOIN dispatch d ON d.base_id = c.dst_id "
           "JOIN temp.cidx_dispatch_callers a ON a.id = c.src_id "
           "WHERE c.kind = 1 AND c.src_id != d.target_id");
  auto count = db_.prepare("SELECT COUNT(*) FROM temp.cidx_dispatch_callers");
  const std::int64_t affected = count.step() ? count.col_int64(0) : 0;
  return TransformWork{.rows_scanned = affected,
                       .rows_inserted = db_.changes(),
                       .rows_deleted = deleted,
                       .affected_keys = affected};
}

// -- v27: multi-definition (per-backend redefinition). Mirrors
// indexer/storage.py byte-identically. `definition`/`def_edge` are written at
// INDEX time (deriving them in resolve is impossible -- delete_edges_for_file
// wipes a losing backend's edges when the shared symbol's file_id flips to the
// last-indexed TU). resolve only counts (set_multi_def) + fans out
// (materialize_possible_calls).

std::optional<int64_t>
SqliteStorageService::component_id_for_file(std::optional<int64_t> file_id) {
  if (!file_id) {
    return std::nullopt;
  }
  auto st = db_.prepare("SELECT d.component_id AS cid FROM file f "
                        "JOIN directory d ON d.id = f.directory_id "
                        "WHERE f.id = ?");
  st.bind(1, *file_id);
  if (!st.step()) {
    return std::nullopt;
  }
  const std::optional<int64_t> cid = opt_int64(st, 0);
  st.step_done();
  return cid;
}

int64_t SqliteStorageService::get_or_create_definition(
    int64_t symbol_id, std::optional<int64_t> file_id,
    std::optional<int64_t> line, std::optional<int64_t> col,
    std::optional<int64_t> end_line, std::optional<int64_t> end_col,
    const std::optional<std::string> &init_text) {
  const std::optional<int64_t> component_id = component_id_for_file(file_id);
  auto st = db_.prepare(
      "INSERT INTO definition "
      "(symbol_id, component_id, file_id, line, col, end_line, end_col, "
      " init_text) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(component_id, file_id, symbol_id) DO UPDATE SET "
      "  line = excluded.line, col = excluded.col, "
      "  end_line = excluded.end_line, end_col = excluded.end_col, "
      "  init_text = excluded.init_text "
      "RETURNING id");
  st.bind(1, symbol_id);
  bind_opt(st, 2, component_id);
  bind_opt(st, 3, file_id);
  bind_opt(st, 4, line);
  bind_opt(st, 5, col);
  bind_opt(st, 6, end_line);
  bind_opt(st, 7, end_col);
  bind_opt(st, 8, init_text);
  if (!st.step()) {
    throw StorageError("get_or_create_definition: upsert returned no id");
  }
  const int64_t id = st.col_int64(0);
  st.step_done();
  return id;
}

int64_t SqliteStorageService::add_def_edge(int64_t src_def_id, int64_t dst_id,
                                           int64_t kind, int64_t count) {
  auto st =
      db_.prepare("INSERT INTO def_edge (src_def_id, dst_id, kind, count) "
                  "VALUES (?, ?, ?, ?) "
                  "ON CONFLICT(src_def_id, dst_id, kind) DO UPDATE SET "
                  "  count = def_edge.count + excluded.count "
                  "RETURNING id");
  st.bind(1, src_def_id);
  st.bind(2, dst_id);
  st.bind(3, kind);
  st.bind(4, count);
  if (!st.step()) {
    throw StorageError("add_def_edge: upsert returned no id");
  }
  const int64_t id = st.col_int64(0);
  st.step_done();
  return id;
}

void SqliteStorageService::copy_body_edges_to_def_edge(int64_t def_id,
                                                       int64_t symbol_id) {
  // At the instant this runs (right after body_descent for one function in one
  // TU) `edge` holds exactly THIS TU's kind-1/7 edges for the symbol
  // (delete_edges_for_file cleared the prior TU's). Copying them keyed by this
  // backend's def_id preserves them after a later TU re-index wipes `edge`.
  auto st =
      db_.prepare("INSERT INTO def_edge (src_def_id, dst_id, kind, count) "
                  "SELECT ?, dst_id, kind, count FROM edge "
                  "WHERE src_id = ? AND kind IN (1, 7) "
                  "ON CONFLICT(src_def_id, dst_id, kind) DO UPDATE SET "
                  "  count = excluded.count");
  st.bind(1, def_id);
  st.bind(2, symbol_id);
  st.step_done();
}

auto SqliteStorageService::body_edge_count(int64_t symbol_id) -> std::size_t {
  auto st = db_.prepare(
      "SELECT COUNT(*) FROM edge WHERE src_id = ? AND kind IN (1, 7)");
  st.bind(1, symbol_id);
  return st.step() ? static_cast<std::size_t>(st.col_int64(0)) : 0;
}

void SqliteStorageService::delete_definitions_for_file(int64_t file_id) {
  // Keyed on definition.file_id (the actual body file), so re-indexing one
  // backend never disturbs another backend's rows. Cascades def_edge.
  auto st = db_.prepare("DELETE FROM definition WHERE file_id = ?");
  st.bind(1, file_id);
  st.step_done();
}

TransformWork SqliteStorageService::set_multi_def() {
  db_.exec("UPDATE symbol SET multi_def = 0");
  std::int64_t updated = db_.changes();
  db_.exec(
      "UPDATE symbol SET multi_def = "
      "  (SELECT COUNT(*) FROM definition d WHERE d.symbol_id = symbol.id) "
      "WHERE id IN (SELECT DISTINCT symbol_id FROM definition)");
  updated += db_.changes();
  return TransformWork{.rows_updated = updated};
}

TransformWork
SqliteStorageService::set_multi_def(const TransformChangeSet &changes) {
  stage_transform_changes(db_, changes);
  db_.exec(
      "UPDATE symbol SET multi_def = (SELECT COUNT(*) FROM definition d "
      "WHERE d.symbol_id = symbol.id) WHERE id IN "
      "(SELECT id FROM temp.cidx_transform_changes WHERE kind = 'symbol')");
  return TransformWork{
      .rows_scanned = static_cast<std::int64_t>(changes.symbol_ids.size()),
      .rows_updated = db_.changes(),
      .affected_keys = static_cast<std::int64_t>(changes.symbol_ids.size())};
}

TransformWork SqliteStorageService::materialize_possible_calls() {
  db_.exec("DELETE FROM possible_call");
  const std::int64_t deleted = db_.changes();
  db_.exec(
      "INSERT OR IGNORE INTO possible_call (src_def_id, dst_def_id, count) "
      "SELECT de.src_def_id, td.id, SUM(de.count) "
      "FROM def_edge de "
      "JOIN symbol s     ON s.id = de.dst_id "
      "JOIN definition td ON td.symbol_id = de.dst_id "
      "WHERE de.kind = 1 AND s.multi_def > 1 "
      "GROUP BY de.src_def_id, td.id");
  return TransformWork{.rows_inserted = db_.changes(), .rows_deleted = deleted};
}

TransformWork SqliteStorageService::materialize_possible_calls(
    const TransformChangeSet &changes) {
  stage_transform_changes(db_, changes);
  db_.exec(
      "CREATE TEMP TABLE IF NOT EXISTS cidx_possible_call_sources ("
      "id INTEGER PRIMARY KEY); DELETE FROM temp.cidx_possible_call_sources; "
      "INSERT OR IGNORE INTO temp.cidx_possible_call_sources(id) "
      "SELECT id FROM temp.cidx_transform_changes WHERE kind = 'definition' "
      "UNION SELECT DISTINCT de.src_def_id FROM def_edge de JOIN "
      "temp.cidx_transform_changes c ON c.kind = 'symbol' AND c.id = de.dst_id "
      "WHERE de.kind = 1");
  db_.exec("DELETE FROM possible_call WHERE src_def_id IN "
           "(SELECT id FROM temp.cidx_possible_call_sources) OR dst_def_id IN "
           "(SELECT d.id FROM definition d JOIN temp.cidx_transform_changes c "
           "ON c.kind = 'symbol' AND c.id = d.symbol_id)");
  const std::int64_t deleted = db_.changes();
  db_.exec(
      "INSERT OR IGNORE INTO possible_call (src_def_id, dst_def_id, count) "
      "SELECT de.src_def_id, td.id, SUM(de.count) FROM def_edge de "
      "JOIN symbol s ON s.id = de.dst_id "
      "JOIN definition td ON td.symbol_id = de.dst_id "
      "WHERE de.kind = 1 AND s.multi_def > 1 AND (de.src_def_id IN "
      "(SELECT id FROM temp.cidx_possible_call_sources) OR de.dst_id IN "
      "(SELECT id FROM temp.cidx_transform_changes WHERE kind = 'symbol')) "
      "GROUP BY de.src_def_id, td.id");
  auto count =
      db_.prepare("SELECT COUNT(*) FROM temp.cidx_possible_call_sources");
  const std::int64_t affected = count.step() ? count.col_int64(0) : 0;
  return TransformWork{.rows_scanned = affected,
                       .rows_inserted = db_.changes(),
                       .rows_deleted = deleted,
                       .affected_keys = affected};
}

std::vector<Edge> SqliteStorageService::cross_repo_edges() {
  auto st = db_.prepare("SELECT e.id, e.src_id, e.dst_id, e.kind, e.count, "
                        "       e.base_access, e.is_virtual, e.vtable_slot "
                        "FROM edge e "
                        "  JOIN symbol s1 ON s1.id = e.src_id "
                        "  JOIN symbol s2 ON s2.id = e.dst_id "
                        "  JOIN file f1 ON f1.id = s1.file_id "
                        "  JOIN directory d1 ON d1.id = f1.directory_id "
                        "  JOIN file f2 ON f2.id = s2.file_id "
                        "  JOIN directory d2 ON d2.id = f2.directory_id "
                        "WHERE d1.component_id <> d2.component_id");
  std::vector<Edge> out;
  while (st.step()) {
    Edge e;
    e.id = st.col_int64(0);
    e.src_id = st.col_int64(1);
    e.dst_id = st.col_int64(2);
    e.kind = st.col_int64(3);
    e.count = st.col_int64(4);
    e.base_access = opt_int64(st, 5);
    e.is_virtual = opt_int64(st, 6);
    e.vtable_slot = opt_int64(st, 7);
    out.push_back(e);
  }
  return out;
}

// -- entity_edge (v17) --------------------------------------------------------

void SqliteStorageService::add_entity_edge(
    int64_t src_id, int64_t dst_id, int64_t kind, int64_t count,
    std::optional<int64_t> via_member_id, int64_t multiplicity, int64_t access,
    int64_t is_virtual, std::optional<int64_t> create_form, int64_t partial) {
  auto st =
      db_.prepare("INSERT INTO entity_edge "
                  "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
                  " access, is_virtual, create_form, partial) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                  "ON CONFLICT(src_id, dst_id, kind, COALESCE(via_member_id, "
                  "-1), COALESCE(create_form, -1)) DO UPDATE SET "
                  "  count       = excluded.count, "
                  "  multiplicity = excluded.multiplicity, "
                  "  access      = excluded.access, "
                  "  is_virtual  = excluded.is_virtual, "
                  "  create_form = COALESCE(excluded.create_form, "
                  "entity_edge.create_form), "
                  "  partial     = excluded.partial");
  st.bind(1, src_id);
  st.bind(2, dst_id);
  st.bind(3, kind);
  st.bind(4, count);
  if (via_member_id) {
    st.bind(5, *via_member_id);
  } else {
    st.bind_null(5);
  }
  st.bind(6, multiplicity);
  st.bind(7, access);
  st.bind(8, is_virtual);
  if (create_form) {
    st.bind(9, *create_form);
  } else {
    st.bind_null(9);
  }
  st.bind(10, partial);
  st.step_done();
}

void SqliteStorageService::clear_entity_edges() {
  db_.exec("DELETE FROM entity_edge");
}

std::optional<EntityNode> SqliteStorageService::entity_node_by_id(int64_t id) {
  auto st =
      db_.prepare("SELECT en.id, en.kind, ek.name FROM entity_node en "
                  "JOIN entity_kind ek ON ek.id = en.kind WHERE en.id = ?");
  st.bind(1, id);
  if (!st.step()) {
    return std::nullopt;
  }
  return EntityNode{.id = st.col_int64(0),
                    .kind = st.col_int64(1),
                    .kind_name = st.col_text(2)};
}

std::vector<EntityEdge> SqliteStorageService::entity_edges_from(int64_t id) {
  auto st = db_.prepare(
      "SELECT ee.src_id, ee.dst_id, ee.kind, ek.name, ee.count, "
      "ee.via_member_id, ee.multiplicity, ee.access, ee.is_virtual, "
      "ee.create_form, ee.partial FROM entity_edge ee "
      "JOIN entity_edge_kind ek ON ek.id = ee.kind "
      "WHERE ee.src_id = ? ORDER BY ek.name, ee.dst_id, ee.via_member_id, "
      "ee.create_form");
  st.bind(1, id);
  std::vector<EntityEdge> out;
  while (st.step()) {
    out.push_back(EntityEdge{.src_id = st.col_int64(0),
                             .dst_id = st.col_int64(1),
                             .kind = st.col_int64(2),
                             .kind_name = st.col_text(3),
                             .count = st.col_int64(4),
                             .via_member_id = opt_int64(st, 5),
                             .multiplicity = st.col_int64(6),
                             .access = st.col_int64(7),
                             .is_virtual = st.col_int64(8),
                             .create_form = opt_int64(st, 9),
                             .partial = st.col_int64(10)});
  }
  return out;
}

} // namespace cidx
