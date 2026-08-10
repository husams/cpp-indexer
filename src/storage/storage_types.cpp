// The type tier: interned type nodes, type edges, parameters, symbol types.
// Split out of storage.cpp; Storage's interface is unchanged.
#include "storage/storage.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <set>
#include <string>

#include "storage/storage_detail.hpp"
#include "util/errors.hpp"

namespace cidx {

using namespace detail;

int64_t SqliteStorageService::intern_type_node(const TypeNode &n) {
  std::optional<int64_t> decl_id;
  if (n.decl_usr && !n.decl_usr->empty()) {
    auto decl = db_.prepare("SELECT id FROM symbol WHERE usr = ? LIMIT 1");
    decl.bind(1, std::string_view(*n.decl_usr));
    if (decl.step()) {
      decl_id = decl.col_int64(0);
    }
  }
  // Interned dictionary row keyed by type_key. Every attribute EXCEPT
  // canonical_id is derived from the key (kind, qualifier flags and decl_usr
  // are structural; spelling keeps the FIRST writer's form, since a key
  // deliberately collapses canonically equivalent written forms -- Box<Foo>
  // vs Box<Alias> -- and refreshing it would let an unrelated partial
  // reindex rewrite existing signature output). canonical_id alone is
  // authoritative on conflict: an ALIAS node is keyed by its declaration USR
  // while its target is mutable (`using Alias = Foo;` can become `= Bar;`),
  // so a re-intern must retarget it in place.
  {
    auto ins = db_.prepare(
        "INSERT INTO type_node "
        "(type_key, spelling, kind, is_const, is_volatile, is_restrict, "
        " decl_usr, decl_id, canonical_id, extent) VALUES (?, ?, ?, ?, ?, ?, "
        "?, ?, ?, ?) "
        "ON CONFLICT(type_key) DO UPDATE SET "
        "canonical_id = excluded.canonical_id, "
        "extent = COALESCE(excluded.extent, type_node.extent), "
        "decl_id = COALESCE(excluded.decl_id, type_node.decl_id)");
    ins.bind(1, std::string_view(n.type_key));
    ins.bind(2, std::string_view(n.spelling));
    ins.bind(3, n.kind);
    ins.bind(4, static_cast<int64_t>(n.is_const ? 1 : 0));
    ins.bind(5, static_cast<int64_t>(n.is_volatile ? 1 : 0));
    ins.bind(6, static_cast<int64_t>(n.is_restrict ? 1 : 0));
    bind_opt(ins, 7, n.decl_usr);
    bind_opt(ins, 8, decl_id);
    bind_opt(ins, 9, n.canonical_id);
    bind_opt(ins, 10, n.extent);
    ins.step_done();
  }
  auto sel = db_.prepare("SELECT id FROM type_node WHERE type_key = ?");
  sel.bind(1, std::string_view(n.type_key));
  if (!sel.step()) {
    throw StorageError("type_node intern failed for key " + n.type_key);
  }
  const int64_t type_id = sel.col_int64(0);
  if (n.decl_usr.has_value()) {
    reconcile_type_identity(type_id, *n.decl_usr);
  }
  return type_id;
}

std::optional<TypeNode> SqliteStorageService::type_node_by_id(int64_t type_id) {
  auto st =
      db_.prepare("SELECT id, type_key, spelling, kind, is_const, is_volatile, "
                  "is_restrict, decl_usr, canonical_id, extent FROM type_node "
                  "WHERE id = ?");
  st.bind(1, type_id);
  if (!st.step()) {
    return std::nullopt;
  }
  TypeNode n;
  n.id = st.col_int64(0);
  n.type_key = st.col_text(1);
  n.spelling = st.col_text(2);
  n.kind = st.col_int64(3);
  n.is_const = st.col_int64(4) != 0;
  n.is_volatile = st.col_int64(5) != 0;
  n.is_restrict = st.col_int64(6) != 0;
  n.decl_usr = opt_text(st, 7);
  n.canonical_id = opt_int64(st, 8);
  n.extent = opt_text(st, 9);
  return n;
}

std::vector<TypeEdge> SqliteStorageService::type_edges_from(int64_t type_id) {
  auto st = db_.prepare("SELECT src_id, kind, position, dst_id FROM type_edge "
                        "WHERE src_id = ? ORDER BY kind, position, dst_id");
  st.bind(1, type_id);
  std::vector<TypeEdge> out;
  while (st.step()) {
    out.push_back(TypeEdge{.src_id = st.col_int64(0),
                           .kind = st.col_int64(1),
                           .position = st.col_int64(2),
                           .dst_id = st.col_int64(3)});
  }
  return out;
}

void SqliteStorageService::add_type_edge(int64_t src_id, int64_t kind,
                                         int64_t position, int64_t dst_id) {
  // OR REPLACE on the (src, kind, position) key: for structural nodes the
  // re-derived dst is identical, and for a retargeted alias (see
  // intern_type_node) the alias_of edge must follow the new target.
  auto st = db_.prepare(
      "INSERT OR REPLACE INTO type_edge (src_id, kind, position, dst_id) "
      "VALUES (?, ?, ?, ?)");
  st.bind(1, src_id);
  st.bind(2, kind);
  st.bind(3, position);
  st.bind(4, dst_id);
  st.step_done();
}

void SqliteStorageService::replace_parameters(
    int64_t owner_id, const std::vector<Parameter> &params) {
  // Wholesale per-owner refresh: an arity change on re-index must drop the
  // stale higher positions, which a positional upsert alone cannot do.
  {
    auto del = db_.prepare("DELETE FROM parameter WHERE owner_id = ?");
    del.bind(1, owner_id);
    del.step_done();
  }
  for (const Parameter &incoming : params) {
    const Parameter &p = incoming;
    auto ins = db_.prepare(
        "INSERT OR REPLACE INTO parameter "
        "(owner_id, position, pack_index, name, type_id, declared_type_id, "
        "adjusted_type_id, default_text, default_origin, reference_semantics, "
        "file_id, line, col) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    ins.bind(1, owner_id);
    ins.bind(2, p.position);
    ins.bind(3, p.pack_index);
    bind_opt(ins, 4, p.name);
    bind_opt(ins, 5, p.type_id);
    bind_opt(ins, 6, p.declared_type_id);
    bind_opt(ins, 7, p.adjusted_type_id);
    bind_opt(ins, 8, p.default_text);
    bind_opt(ins, 9, p.default_origin);
    bind_opt(ins, 10, p.reference_semantics);
    bind_opt(ins, 11, p.file_id);
    bind_opt(ins, 12, p.line);
    bind_opt(ins, 13, p.col);
    ins.step_done();
  }
}

std::vector<Parameter> SqliteStorageService::parameters_of(int64_t symbol_id) {
  auto st = db_.prepare(
      "SELECT owner_id, position, pack_index, name, type_id, declared_type_id, "
      "adjusted_type_id, default_text, default_origin, reference_semantics, "
      "file_id, line, col "
      "FROM parameter WHERE owner_id = ? ORDER BY position, pack_index");
  st.bind(1, symbol_id);
  std::vector<Parameter> out;
  while (st.step()) {
    Parameter p;
    p.owner_id = st.col_int64(0);
    p.position = st.col_int64(1);
    p.pack_index = st.col_int64(2);
    p.name = opt_text(st, 3);
    p.type_id = opt_int64(st, 4);
    p.declared_type_id = opt_int64(st, 5);
    p.adjusted_type_id = opt_int64(st, 6);
    p.default_text = opt_text(st, 7);
    p.default_origin = opt_text(st, 8);
    p.reference_semantics = opt_text(st, 9);
    p.file_id = opt_int64(st, 10);
    p.line = opt_int64(st, 11);
    p.col = opt_int64(st, 12);
    out.push_back(std::move(p));
  }
  return out;
}

void SqliteStorageService::add_symbol_type(int64_t symbol_id, int64_t kind,
                                           int64_t type_id) {
  auto st = db_.prepare(
      "INSERT OR REPLACE INTO symbol_type (symbol_id, kind, type_id) "
      "VALUES (?, ?, ?)");
  st.bind(1, symbol_id);
  st.bind(2, kind);
  st.bind(3, type_id);
  st.step_done();
}

std::optional<int64_t> SqliteStorageService::symbol_type_of(int64_t symbol_id,
                                                            int64_t kind) {
  auto st = db_.prepare(
      "SELECT type_id FROM symbol_type WHERE symbol_id = ? AND kind = ?");
  st.bind(1, symbol_id);
  st.bind(2, kind);
  if (!st.step()) {
    return std::nullopt;
  }
  return st.col_int64(0);
}

std::vector<int64_t>
SqliteStorageService::type_ids_reaching(const std::string &decl_usr) {
  // Closure of type nodes from which a node NAMING decl_usr is reachable:
  // seed = every node whose decl_usr matches (the bare type plus qualified/
  // sugared variants carry it too), then walk type_edge backwards (src wraps
  // dst) and canonical_id backwards (sugared node -> canonical). Deterministic:
  // ordered by id.
  auto st = db_.prepare(
      "WITH RECURSIVE reach(id) AS ("
      "  SELECT id FROM type_node WHERE decl_usr = ?"
      "  UNION"
      "  SELECT te.src_id FROM type_edge te JOIN reach r ON te.dst_id = r.id"
      "  UNION"
      "  SELECT tn.id FROM type_node tn JOIN reach r ON tn.canonical_id = r.id"
      ") SELECT id FROM reach ORDER BY id");
  st.bind(1, std::string_view(decl_usr));
  std::vector<int64_t> out;
  while (st.step()) {
    out.push_back(st.col_int64(0));
  }
  return out;
}

std::vector<std::pair<int64_t, int64_t>>
SqliteStorageService::param_owners_of_types(
    const std::vector<int64_t> &type_ids) {
  std::vector<std::pair<int64_t, int64_t>> out;
  if (type_ids.empty()) {
    return out;
  }
  std::string sql =
      "SELECT owner_id, position FROM parameter WHERE type_id IN (";
  for (std::size_t i = 0; i < type_ids.size(); ++i) {
    sql += i == 0 ? "?" : ", ?";
  }
  sql += ") ORDER BY owner_id, position";
  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < type_ids.size(); ++i) {
    st.bind(static_cast<int>(i + 1), type_ids[i]);
  }
  while (st.step()) {
    out.emplace_back(st.col_int64(0), st.col_int64(1));
  }
  return out;
}

std::vector<std::pair<int64_t, int64_t>>
SqliteStorageService::symbol_type_owners_of_types(
    const std::vector<int64_t> &type_ids) {
  std::vector<std::pair<int64_t, int64_t>> out;
  if (type_ids.empty()) {
    return out;
  }
  std::string sql =
      "SELECT symbol_id, kind FROM symbol_type WHERE type_id IN (";
  for (std::size_t i = 0; i < type_ids.size(); ++i) {
    sql += i == 0 ? "?" : ", ?";
  }
  sql += ") ORDER BY symbol_id, kind";
  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < type_ids.size(); ++i) {
    st.bind(static_cast<int>(i + 1), type_ids[i]);
  }
  while (st.step()) {
    out.emplace_back(st.col_int64(0), st.col_int64(1));
  }
  return out;
}

// -- v31 include tier ---------------------------------------------------------

} // namespace cidx
