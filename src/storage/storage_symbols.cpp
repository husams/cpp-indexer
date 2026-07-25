// Symbol, edge, edge-site, call-arg and template-param/arg rows.
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

int64_t SqliteStorageService::add_symbol(const Symbol &sym) {
  if (!is_symbol_kind(sym.kind)) {
    throw StorageError("unknown symbol kind '" + sym.kind + "'");
  }
  const std::optional<int64_t> identity_file =
      sym.file_id ? sym.file_id : sym.decl_file_id;
  const int64_t universe_id = sym.semantic_universe_id > 0
                                  ? sym.semantic_universe_id
                                  : semantic_universe_for_file(identity_file);
  const std::string identity_key =
      symbol_identity_key(sym, universe_id, identity_file, sym.identity_source,
                          sym.identity_translation_unit);
  auto st = db_.prepare(
      "INSERT INTO symbol (usr, spelling, qual_name, display_name, kind, "
      "type_info, file_id, line, col, decl_file_id, decl_line, decl_col, "
      "is_definition, is_pure, is_static, is_instantiation, linkage, access, "
      "parent_usr, resolved, decl_path, end_line, end_col, const_value, "
      "semantic_universe_id, identity_key, callable_kind, template_origin, "
      "template_form) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
      "?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(semantic_universe_id, identity_key) WHERE identity_key <> "
      "'' "
      "DO UPDATE SET "
      "  spelling         = excluded.spelling, "
      "  qual_name        = COALESCE(excluded.qual_name, symbol.qual_name), "
      "  display_name     = COALESCE(excluded.display_name, "
      "symbol.display_name), "
      "  kind             = excluded.kind, "
      "  type_info        = COALESCE(excluded.type_info, symbol.type_info), "
      "  file_id          = CASE WHEN excluded.is_definition >= "
      "symbol.is_definition "
      "                          THEN excluded.file_id ELSE symbol.file_id "
      "END, "
      "  line             = CASE WHEN excluded.is_definition >= "
      "symbol.is_definition "
      "                          THEN excluded.line ELSE symbol.line END, "
      "  col              = CASE WHEN excluded.is_definition >= "
      "symbol.is_definition "
      "                          THEN excluded.col ELSE symbol.col END, "
      "  end_line         = CASE WHEN excluded.is_definition >= "
      "symbol.is_definition "
      "                          THEN excluded.end_line ELSE symbol.end_line "
      "END, "
      "  end_col          = CASE WHEN excluded.is_definition >= "
      "symbol.is_definition "
      "                          THEN excluded.end_col ELSE symbol.end_col "
      "END, "
      "  decl_file_id     = COALESCE(excluded.decl_file_id, "
      "symbol.decl_file_id), "
      "  decl_line        = COALESCE(excluded.decl_line, symbol.decl_line), "
      "  decl_col         = COALESCE(excluded.decl_col, symbol.decl_col), "
      "  is_definition    = MAX(excluded.is_definition, symbol.is_definition), "
      "  is_pure          = MAX(excluded.is_pure, symbol.is_pure), "
      "  is_static        = MAX(excluded.is_static, symbol.is_static), "
      // A real decl row states its TemplateSpecializationKind authoritatively,
      // so reindexing an instantiation-turned-explicit-specialization (same
      // USR) must DOWNGRADE the flag. Stub promotion stays monotonic in
      // mint_symbol_id, which keeps its MAX.
      "  is_instantiation = excluded.is_instantiation, "
      "  linkage          = COALESCE(excluded.linkage, symbol.linkage), "
      "  access           = COALESCE(excluded.access, symbol.access), "
      "  parent_usr       = COALESCE(excluded.parent_usr, symbol.parent_usr), "
      "  resolved         = MAX(excluded.resolved, symbol.resolved), "
      // v33: only the initializer-bearing decl evaluates to a value, so a
      // plain declaration must not erase the definition's stored constant.
      "  const_value      = COALESCE(excluded.const_value, "
      "symbol.const_value), "
      "  callable_kind   = COALESCE(excluded.callable_kind, "
      "symbol.callable_kind), "
      "  template_origin = COALESCE(excluded.template_origin, "
      "symbol.template_origin), "
      "  template_form = COALESCE(excluded.template_form, "
      "symbol.template_form) "
      "RETURNING id");
  st.bind(1, std::string_view(sym.usr));
  st.bind(2, std::string_view(sym.spelling));
  bind_opt(st, 3, sym.qual_name);
  bind_opt(st, 4, sym.display_name);
  st.bind(5, symbol_kind_id(sym.kind)); // stored as CXCursorKind int (v16)
  bind_opt(st, 6, sym.type_info);
  bind_opt(st, 7, sym.file_id);
  bind_opt(st, 8, sym.line);
  bind_opt(st, 9, sym.col);
  bind_opt(st, 10, sym.decl_file_id);
  bind_opt(st, 11, sym.decl_line);
  bind_opt(st, 12, sym.decl_col);
  st.bind(13, static_cast<int64_t>(sym.is_definition ? 1 : 0));
  st.bind(14, static_cast<int64_t>(sym.is_pure ? 1 : 0));
  st.bind(15, static_cast<int64_t>(sym.is_static ? 1 : 0));
  st.bind(16, static_cast<int64_t>(sym.is_instantiation ? 1 : 0));
  bind_opt(st, 17, sym.linkage);
  bind_opt(st, 18, sym.access);
  bind_opt(st, 19, sym.parent_usr);
  st.bind(20, static_cast<int64_t>(sym.resolved ? 1 : 0));
  // decl_path is INSERTed but intentionally NOT in the ON CONFLICT SET: a real
  // add_symbol never clobbers a stub's recorded external path (mirrors Python).
  bind_opt(st, 21, sym.decl_path);
  // end_line/end_col move in lockstep with line/col (same CASE in the SET
  // above).
  bind_opt(st, 22, sym.end_line);
  bind_opt(st, 23, sym.end_col);
  bind_opt(st, 24, sym.const_value); // v33
  st.bind(25, universe_id);
  st.bind(26, std::string_view(identity_key));
  bind_opt(st, 27, sym.callable_kind);
  bind_opt(st, 28, sym.template_origin);
  bind_opt(st, 29, sym.template_form);
  if (!st.step()) {
    throw StorageError("symbol upsert returned no id");
  }
  const int64_t sid = st.col_int64(0);
  st.step_done();
  if (sym.parent_usr) {
    auto parent = db_.prepare("UPDATE symbol SET parent_id = "
                              "(SELECT id FROM symbol p WHERE p.usr = ?) "
                              "WHERE id = ?");
    parent.bind(1, std::string_view(*sym.parent_usr));
    parent.bind(2, sid);
    parent.step_done();
  }
  // A child may be indexed before its semantic parent. When the parent arrives,
  // reconcile every waiting child deterministically and idempotently.
  {
    auto children =
        db_.prepare("UPDATE symbol SET parent_id = ? WHERE parent_usr = ? "
                    "AND (parent_id IS NULL OR parent_id <> ?)");
    children.bind(1, sid);
    children.bind(2, std::string_view(sym.usr));
    children.bind(3, sid);
    children.step_done();
  }
  // v26: record THIS cursor's own site (mirrors Python add_symbol). The symbol
  // row keeps only the winning definition + one declaration; decl_site keeps
  // every physical site so references() can list all reopenings of an open
  // symbol (a namespace above all). Guard on a real (file, line): locationless
  // stubs would otherwise fan out on the NULL-file UNIQUE (NULL != NULL).
  // INSERT OR IGNORE is idempotent -- reindexing the same TU re-adds nothing.
  if (sym.file_id.has_value() && sym.line.has_value()) {
    auto ds = db_.prepare(
        "INSERT OR IGNORE INTO decl_site "
        "(symbol_id, file_id, line, col, end_line, end_col, is_definition) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)");
    ds.bind(1, sid);
    bind_opt(ds, 2, sym.file_id);
    bind_opt(ds, 3, sym.line);
    bind_opt(ds, 4, sym.col);
    bind_opt(ds, 5, sym.end_line);
    bind_opt(ds, 6, sym.end_col);
    ds.bind(7, static_cast<int64_t>(sym.is_definition ? 1 : 0));
    ds.step_done();
  }
  reconcile_symbol_identity(sid, sym.usr);
  return sid;
}

void SqliteStorageService::add_decl_site(int64_t symbol_id,
                                          const Symbol &sym) {
  if (!sym.file_id.has_value() || !sym.line.has_value()) {
    return;
  }
  auto symbol = db_.prepare(
      "UPDATE symbol SET decl_file_id = COALESCE(?, decl_file_id), "
      "decl_line = COALESCE(?, decl_line), "
      "decl_col = COALESCE(?, decl_col) WHERE id = ?");
  bind_opt(symbol, 1, sym.decl_file_id);
  bind_opt(symbol, 2, sym.decl_line);
  bind_opt(symbol, 3, sym.decl_col);
  symbol.bind(4, symbol_id);
  symbol.step_done();
  auto ds = db_.prepare(
      "INSERT OR IGNORE INTO decl_site "
      "(symbol_id, file_id, line, col, end_line, end_col, is_definition) "
      "VALUES (?, ?, ?, ?, ?, ?, ?)");
  ds.bind(1, symbol_id);
  bind_opt(ds, 2, sym.file_id);
  bind_opt(ds, 3, sym.line);
  bind_opt(ds, 4, sym.col);
  bind_opt(ds, 5, sym.end_line);
  bind_opt(ds, 6, sym.end_col);
  ds.bind(7, static_cast<int64_t>(sym.is_definition ? 1 : 0));
  ds.step_done();
}

bool SqliteStorageService::update_symbol(
    const std::string &usr,
    const std::vector<std::pair<std::string, SqlValue>> &values,
    const std::optional<int64_t> &semantic_universe_id,
    const std::optional<std::string> &identity_source,
    const std::optional<std::string> &identity_translation_unit) {
  const auto target = lookup_symbol(usr, semantic_universe_id, identity_source,
                                    identity_translation_unit);
  if (!target) {
    return false;
  }
  return update_symbol_by_id(target->id, values);
}

bool SqliteStorageService::update_symbol_by_id(
    int64_t symbol_id,
    const std::vector<std::pair<std::string, SqlValue>> &values) {
  std::vector<std::string> bad;
  for (const auto &kv : values) {
    if (std::ranges::find(kSymbolInsertCols, kv.first) ==
        kSymbolInsertCols.end()) {
      bad.push_back(kv.first);
    }
  }
  if (!bad.empty()) {
    // Dedupe then sort, then format as Python's list repr: ['col1', 'col2']
    // (Python: raises f"unknown symbol column(s): {sorted(set(bad))}")
    std::ranges::sort(bad);
    bad.erase(std::ranges::unique(bad).begin(), bad.end());
    std::string repr = "[";
    for (std::size_t i = 0; i < bad.size(); ++i) {
      if (i > 0) {
        repr += ", ";
      }
      repr += '\'' + bad[i] + '\'';
    }
    repr += ']';
    throw StorageError("unknown symbol column(s): " + repr);
  }
  for (const auto &kv : values) {
    if (kv.first == "kind") {
      const auto *k = std::get_if<std::string>(&kv.second);
      if (k == nullptr || !is_symbol_kind(*k)) {
        throw StorageError("unknown symbol kind in update_symbol");
      }
    }
  }
  if (values.empty()) {
    return lookup_symbol_by_id(symbol_id).has_value();
  }
  std::vector<std::string> sets;
  sets.reserve(values.size());
  for (const auto &kv : values) {
    sets.push_back(kv.first + " = ?");
  }
  auto st = db_.prepare("UPDATE symbol SET " + join_strings(sets, ", ") +
                        " WHERE id = ?");
  int idx = 1;
  for (const auto &kv : values) {
    st.bind(idx++, kv.second);
  }
  st.bind(idx, symbol_id);
  st.step_done();
  return db_.changes() > 0;
}

void SqliteStorageService::delete_symbols_for_file(int64_t file_id) {
  auto del = db_.prepare("DELETE FROM symbol WHERE file_id = ?");
  del.bind(1, file_id);
  del.step_done();
}

std::optional<Symbol> SqliteStorageService::lookup_symbol(
    const std::string &usr, const std::optional<int64_t> &semantic_universe_id,
    const std::optional<std::string> &identity_source,
    const std::optional<std::string> &identity_translation_unit) {
  if (semantic_universe_id && identity_source && !identity_source->empty() &&
      identity_translation_unit && !identity_translation_unit->empty()) {
    const auto universe = get_semantic_universe_by_id(*semantic_universe_id);
    const std::string universe_key = universe ? universe->key : "legacy";
    const auto find_by_identity_key =
        [&](const std::string &identity_key) -> std::optional<Symbol> {
      auto scoped = db_.prepare(std::string("SELECT ") + kSymbolCols +
                                " FROM symbol WHERE semantic_universe_id = ?"
                                " AND identity_key = ?");
      scoped.bind(1, *semantic_universe_id);
      scoped.bind(2, std::string_view(identity_key));
      if (scoped.step()) {
        return symbol_from(scoped);
      }
      return std::nullopt;
    };
    const std::string source_key =
        portable_source_identity_for_path(*identity_source);
    if (const auto local = find_by_identity_key(
            universe_key + '\x1f' + "local:" + *identity_translation_unit +
            '\x1f' + source_key + '\x1f' + usr)) {
      return local;
    }
    if (const auto external =
            find_by_identity_key(universe_key + '\x1f' + usr)) {
      return external;
    }
    return std::nullopt;
  }
  const auto matches = lookup_symbols_by_usr(usr, semantic_universe_id);
  if (matches.empty()) {
    return std::nullopt;
  }
  if (identity_source && !identity_source->empty()) {
    for (const Symbol &candidate : matches) {
      if (candidate.identity_key ==
          symbol_identity_key(candidate, candidate.semantic_universe_id,
                              candidate.file_id, identity_source,
                              identity_translation_unit)) {
        return candidate;
      }
    }
    const auto universe =
        get_semantic_universe_by_id(matches.front().semantic_universe_id);
    const std::string universe_key = universe ? universe->key : "legacy";
    std::vector<Symbol> portable_matches;
    for (const Symbol &candidate : matches) {
      std::string portable_key = universe_key;
      portable_key += "\x1f";
      portable_key += usr;
      if (candidate.identity_key == portable_key) {
        portable_matches.push_back(candidate);
      }
    }
    if (portable_matches.size() == 1) {
      return portable_matches.front();
    }
    return std::nullopt;
  }
  if (identity_translation_unit && !identity_translation_unit->empty()) {
    const auto universe =
        get_semantic_universe_by_id(matches.front().semantic_universe_id);
    const std::string prefix = (universe ? universe->key : "legacy") +
                               "\x1flocal:" + *identity_translation_unit +
                               "\x1f";
    std::vector<Symbol> tu_matches;
    for (const Symbol &candidate : matches) {
      if (candidate.identity_key.starts_with(prefix)) {
        tu_matches.push_back(candidate);
      }
    }
    if (tu_matches.size() == 1) {
      return tu_matches.front();
    }
    if (tu_matches.size() > 1) {
      throw StorageError("ambiguous symbol USR within translation unit: " +
                         usr);
    }
  }
  if (matches.size() > 1) {
    throw StorageError("ambiguous symbol USR; pass semantic universe scope: " +
                       usr);
  }
  return matches.front();
}

std::vector<Symbol> SqliteStorageService::lookup_symbols_by_usr(
    const std::string &usr,
    const std::optional<int64_t> &semantic_universe_id) {
  std::string sql =
      std::string("SELECT ") + kSymbolCols + " FROM symbol WHERE usr = ?";
  if (semantic_universe_id) {
    sql += " AND semantic_universe_id = ?";
  }
  sql += " ORDER BY semantic_universe_id, identity_key";
  auto st = db_.prepare(sql);
  st.bind(1, std::string_view(usr));
  if (semantic_universe_id) {
    st.bind(2, *semantic_universe_id);
  }
  std::vector<Symbol> out;
  while (st.step()) {
    out.push_back(symbol_from(st));
  }
  return out;
}

std::optional<Symbol>
SqliteStorageService::lookup_symbol_by_id(int64_t symbol_id) {
  auto st = db_.prepare(std::string("SELECT ") + kSymbolCols +
                        " FROM symbol WHERE id = ?");
  st.bind(1, symbol_id);
  if (!st.step()) {
    return std::nullopt;
  }
  return symbol_from(st);
}

std::vector<Symbol> SqliteStorageService::lookup_symbols_by_name(
    const std::string &spelling, const std::optional<std::string> &kind,
    const std::optional<int64_t> &semantic_universe_id) {
  std::string sql =
      std::string("SELECT ") + kSymbolCols + " FROM symbol WHERE spelling = ?";
  std::vector<SqlValue> args;
  args.emplace_back(spelling);
  if (kind) {
    sql += " AND kind = ?";
    args.emplace_back(symbol_kind_id(*kind)); // stored as int (v16)
  }
  if (semantic_universe_id) {
    sql += " AND semantic_universe_id = ?";
    args.emplace_back(*semantic_universe_id);
  }
  sql += " ORDER BY usr";
  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < args.size(); ++i) {
    st.bind(static_cast<int>(i + 1), args[i]);
  }
  std::vector<Symbol> out;
  while (st.step()) {
    out.push_back(symbol_from(st));
  }
  return out;
}

std::vector<Symbol> SqliteStorageService::lookup_symbols_by_qual_name(
    const std::string &qual_name, const std::optional<std::string> &kind,
    const std::optional<int64_t> &semantic_universe_id) {
  std::string sql =
      std::string("SELECT ") + kSymbolCols + " FROM symbol WHERE qual_name = ?";
  std::vector<SqlValue> args;
  args.emplace_back(qual_name);
  if (kind) {
    sql += " AND kind = ?";
    args.emplace_back(symbol_kind_id(*kind)); // stored as int (v16)
  }
  if (semantic_universe_id) {
    sql += " AND semantic_universe_id = ?";
    args.emplace_back(*semantic_universe_id);
  }
  sql += " ORDER BY usr";
  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < args.size(); ++i) {
    st.bind(static_cast<int>(i + 1), args[i]);
  }
  std::vector<Symbol> out;
  while (st.step()) {
    out.push_back(symbol_from(st));
  }
  return out;
}

std::vector<Symbol>
SqliteStorageService::search_symbols(const std::string &pattern,
                                     const std::optional<std::string> &kind,
                                     const std::optional<int64_t> &config_id) {
  // '%seg%seg%' on qual_name: each '::'-separated segment must appear, in
  // order, as a substring. Only % and _ are escaped (storage.py parity).
  std::vector<std::string> segs;
  std::size_t start = 0;
  while (start <= pattern.size()) {
    const std::size_t pos = pattern.find("::", start);
    const std::string seg = pattern.substr(
        start, pos == std::string::npos ? std::string::npos : pos - start);
    if (!seg.empty()) {
      std::string esc;
      for (const char c : seg) {
        if (c == '%') {
          esc += "\\%";
        } else if (c == '_') {
          esc += "\\_";
        } else {
          esc += c;
        }
      }
      segs.push_back(esc);
    }
    if (pos == std::string::npos) {
      break;
    }
    start = pos + 2;
  }
  const std::string like = "%" + join_strings(segs, "%") + "%";
  std::string sql = std::string("SELECT ") + kSymbolCols +
                    " FROM symbol WHERE qual_name LIKE ? ESCAPE '\\'";
  std::vector<SqlValue> args;
  args.emplace_back(like);
  if (kind) {
    sql += " AND kind = ?";
    args.emplace_back(symbol_kind_id(*kind)); // stored as int (v16)
  }
  if (config_id) {
    sql += " AND EXISTS (SELECT 1 FROM fact_applicability fa WHERE "
           "fa.fact_kind = 'symbol' AND fa.fact_id = symbol.id AND "
           "fa.config_id = ?)";
    args.emplace_back(*config_id);
  }
  sql += " ORDER BY LENGTH(qual_name), qual_name";
  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < args.size(); ++i) {
    st.bind(static_cast<int>(i + 1), args[i]);
  }
  std::vector<Symbol> out;
  while (st.step()) {
    out.push_back(symbol_from(st));
  }
  return out;
}

std::vector<Symbol>
SqliteStorageService::list_symbols(const std::optional<int64_t> &component_id,
                                   const std::optional<std::string> &dir_path,
                                   const std::optional<int64_t> &file_id,
                                   const std::optional<std::string> &name,
                                   const std::optional<std::string> &kind) {
  std::string sql = std::string("SELECT ") + kSymbolColsS + " FROM symbol s";
  std::vector<std::string> where;
  std::vector<SqlValue> args;
  if (component_id || dir_path) {
    // Location scope matches if EITHER the definition site or the declaration
    // site falls inside (storage.py:701-714).
    std::vector<std::string> scope;
    std::vector<SqlValue> scope_args;
    if (component_id) {
      scope.emplace_back("d.component_id = ?");
      scope_args.emplace_back(*component_id);
    }
    if (dir_path) {
      scope.push_back(dir_scope_sql(*dir_path, scope_args));
    }
    where.push_back("EXISTS (SELECT 1 FROM file f "
                    "JOIN directory d ON d.id = f.directory_id "
                    "WHERE f.id IN (s.file_id, s.decl_file_id) AND " +
                    join_strings(scope, " AND ") + ")");
    for (auto &a : scope_args) {
      args.push_back(std::move(a));
    }
  }
  if (file_id) {
    where.emplace_back("(s.file_id = ? OR s.decl_file_id = ?)");
    args.emplace_back(*file_id);
    args.emplace_back(*file_id);
  }
  if (name && !name->empty()) {
    where.emplace_back("COALESCE(s.qual_name, s.spelling) LIKE ? ESCAPE '\\'");
    args.emplace_back(fuzzy_like(*name));
  }
  if (kind) {
    where.emplace_back("s.kind = ?");
    args.emplace_back(symbol_kind_id(*kind)); // stored as int (v16)
  }
  if (!where.empty()) {
    sql += " WHERE " + join_strings(where, " AND ");
  }
  sql += " ORDER BY LENGTH(COALESCE(s.qual_name, s.spelling)),"
         " COALESCE(s.qual_name, s.spelling)";
  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < args.size(); ++i) {
    st.bind(static_cast<int>(i + 1), args[i]);
  }
  std::vector<Symbol> out;
  while (st.step()) {
    out.push_back(symbol_from(st));
  }
  return out;
}

std::vector<Symbol> SqliteStorageService::symbols_in_file(int64_t file_id) {
  auto st = db_.prepare(std::string("SELECT ") + kSymbolCols +
                        " FROM symbol WHERE file_id = ? ORDER BY line, col");
  st.bind(1, file_id);
  std::vector<Symbol> out;
  while (st.step()) {
    out.push_back(symbol_from(st));
  }
  return out;
}

std::vector<Symbol> SqliteStorageService::unresolved_symbols() {
  auto st = db_.prepare(std::string("SELECT ") + kSymbolCols +
                        " FROM symbol WHERE resolved = 0 ORDER BY usr");
  std::vector<Symbol> out;
  while (st.step()) {
    out.push_back(symbol_from(st));
  }
  return out;
}

// -- graph layer (v7)
// -----------------------------------------------------------------

int64_t SqliteStorageService::mint_symbol_id(
    const std::string &usr, const std::string &spelling,
    const std::string &qual_name, const std::string &display_name,
    const std::string &kind, const std::optional<int64_t> &decl_file_id,
    const std::optional<int64_t> &decl_line,
    const std::optional<int64_t> &decl_col,
    const std::optional<std::string> &decl_path, bool is_instantiation,
    bool is_named_instance, const std::optional<std::string> &type_info,
    const std::optional<int64_t> &semantic_universe_id,
    const std::optional<std::string> &identity_source,
    const std::optional<std::string> &linkage,
    const std::optional<std::string> &identity_translation_unit) {
  Symbol identity;
  identity.usr = usr;
  identity.linkage = linkage;
  const int64_t universe_id = semantic_universe_id
                                  ? *semantic_universe_id
                                  : semantic_universe_for_file(decl_file_id);
  const std::string identity_key =
      symbol_identity_key(identity, universe_id, decl_file_id, identity_source,
                          identity_translation_unit);
  // The follow-up SELECT returns the stable id whether the row was minted or
  // already present. 'function' is the fallback kind when the cursor kind is
  // unknown; the real def's add_symbol upsert overwrites kind/location/resolved
  // later. On a repeat mint we only UPGRADE an unnamed stub (empty spelling) --
  // name and kind together -- never clobber a real symbol's; the decl location
  // (registered id or raw path) is filled in only when still absent (COALESCE).
  // decl_path carries the location of a target in an UNREGISTERED (system/
  // stdlib) file so the stub stays located instead of @<no-location>.
  // is_instantiation marks implicit template-instantiation nodes (v13); MAX()
  // ensures a stub->instantiation promotion always upgrades but never
  // downgrades.
  auto ins = db_.prepare(
      "INSERT INTO symbol (usr, spelling, qual_name, display_name, kind, "
      "                    decl_file_id, decl_line, decl_col, decl_path, "
      "                    is_instantiation, is_named_instance, type_info, "
      "                    resolved, semantic_universe_id, identity_key) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?) "
      "ON CONFLICT(semantic_universe_id, identity_key) WHERE identity_key <> "
      "'' "
      "DO UPDATE SET "
      "  kind             = CASE WHEN symbol.spelling = '' "
      "                          THEN excluded.kind ELSE symbol.kind END, "
      "  spelling         = CASE WHEN symbol.spelling = '' "
      "                          THEN excluded.spelling ELSE symbol.spelling "
      "END, "
      "  qual_name        = COALESCE(symbol.qual_name, excluded.qual_name), "
      "  display_name     = COALESCE(symbol.display_name, "
      "excluded.display_name), "
      "  type_info        = COALESCE(symbol.type_info, excluded.type_info), "
      "  decl_file_id     = COALESCE(symbol.decl_file_id, "
      "excluded.decl_file_id), "
      "  decl_line        = COALESCE(symbol.decl_line, excluded.decl_line), "
      "  decl_col         = COALESCE(symbol.decl_col, excluded.decl_col), "
      "  decl_path        = COALESCE(symbol.decl_path, excluded.decl_path), "
      "  is_instantiation = MAX(symbol.is_instantiation, "
      "excluded.is_instantiation), "
      "  is_named_instance = MAX(symbol.is_named_instance, "
      "excluded.is_named_instance)");
  ins.bind(1, std::string_view(usr));
  ins.bind(2, std::string_view(spelling));
  if (qual_name.empty()) {
    ins.bind_null(3);
  } else {
    ins.bind(3, std::string_view(qual_name));
  }
  if (display_name.empty()) {
    ins.bind_null(4);
  } else {
    ins.bind(4, std::string_view(display_name));
  }
  ins.bind(5, symbol_kind_id(kind)); // kind stored as CXCursorKind int (v16)
  bind_opt(ins, 6, decl_file_id);
  bind_opt(ins, 7, decl_line);
  bind_opt(ins, 8, decl_col);
  bind_opt(ins, 9, decl_path);
  ins.bind(10, static_cast<int64_t>(is_instantiation ? 1 : 0));
  ins.bind(11, static_cast<int64_t>(is_named_instance ? 1 : 0));
  bind_opt(ins, 12, type_info);
  ins.bind(13, universe_id);
  ins.bind(14, std::string_view(identity_key));
  ins.step_done();
  auto sel = db_.prepare("SELECT id FROM symbol WHERE semantic_universe_id = ? "
                         "AND identity_key = ?");
  sel.bind(1, universe_id);
  sel.bind(2, std::string_view(identity_key));
  if (!sel.step()) {
    throw StorageError("mint_symbol_id: SELECT returned no row for usr=" + usr);
  }
  return sel.col_int64(0);
}

int64_t SqliteStorageService::add_edge(const Edge &e) {
  auto st = db_.prepare(
      "INSERT INTO edge (src_id, dst_id, kind, count, base_access, is_virtual, "
      "                  vtable_slot) "
      "VALUES (?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(src_id, dst_id, kind) DO UPDATE SET "
      "  count       = edge.count + excluded.count, "
      "  base_access = COALESCE(excluded.base_access, edge.base_access), "
      "  is_virtual  = COALESCE(excluded.is_virtual,  edge.is_virtual), "
      "  vtable_slot = COALESCE(excluded.vtable_slot, edge.vtable_slot) "
      "RETURNING id");
  st.bind(1, e.src_id);
  st.bind(2, e.dst_id);
  st.bind(3, e.kind);
  st.bind(4, e.count);
  bind_opt(st, 5, e.base_access);
  bind_opt(st, 6, e.is_virtual);
  bind_opt(st, 7, e.vtable_slot);
  if (!st.step()) {
    throw StorageError("add_edge: upsert returned no id");
  }
  const int64_t eid = st.col_int64(0);
  st.step_done();
  return eid;
}

int64_t SqliteStorageService::ensure_edge(const Edge &e) {
  // Structural upsert: presence matters, count does not accumulate. The
  // DO UPDATE keeps count as-is (attributes still COALESCE-fill) so the
  // statement can RETURN the stable id on both paths.
  auto st = db_.prepare(
      "INSERT INTO edge (src_id, dst_id, kind, count, base_access, is_virtual, "
      "                  vtable_slot) "
      "VALUES (?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(src_id, dst_id, kind) DO UPDATE SET "
      "  count       = edge.count, "
      "  base_access = COALESCE(excluded.base_access, edge.base_access), "
      "  is_virtual  = COALESCE(excluded.is_virtual,  edge.is_virtual), "
      "  vtable_slot = COALESCE(excluded.vtable_slot, edge.vtable_slot) "
      "RETURNING id");
  st.bind(1, e.src_id);
  st.bind(2, e.dst_id);
  st.bind(3, e.kind);
  st.bind(4, e.count);
  bind_opt(st, 5, e.base_access);
  bind_opt(st, 6, e.is_virtual);
  bind_opt(st, 7, e.vtable_slot);
  if (!st.step()) {
    throw StorageError("ensure_edge: upsert returned no id");
  }
  const int64_t eid = st.col_int64(0);
  st.step_done();
  return eid;
}

void SqliteStorageService::add_edge_site(const EdgeSite &s) {
  const auto source_id =
      s.recv_src_kind ? std::optional<int64_t>(source_kind_id(*s.recv_src_kind))
                      : std::nullopt;
  if (s.recv_src_kind && source_id < 0) {
    throw StorageError("unknown source kind '" + *s.recv_src_kind + "'");
  }
  const auto symbol_id =
      [this](const std::optional<std::string> &usr) -> std::optional<int64_t> {
    if (!usr || usr->empty()) {
      return std::nullopt;
    }
    auto st = db_.prepare("SELECT id FROM symbol WHERE usr = ? LIMIT 1");
    st.bind(1, std::string_view(*usr));
    return st.step() ? std::optional<int64_t>(st.col_int64(0)) : std::nullopt;
  };
  const auto type_id =
      [this](const std::optional<std::string> &usr) -> std::optional<int64_t> {
    if (!usr || usr->empty()) {
      return std::nullopt;
    }
    auto st = db_.prepare(
        "SELECT id FROM type_node WHERE decl_usr = ? ORDER BY id LIMIT 1");
    st.bind(1, std::string_view(*usr));
    return st.step() ? std::optional<int64_t>(st.col_int64(0)) : std::nullopt;
  };
  const auto unresolved_id =
      [this](int64_t kind, const std::optional<std::string> &text,
             const std::optional<int64_t> &sid,
             const std::optional<int64_t> &tid) -> std::optional<int64_t> {
    if (!text || text->empty() || sid || tid) {
      return std::nullopt;
    }
    auto st = db_.prepare(
        "INSERT INTO external_identity(identity_kind, identity_text, "
        "resolution_status, symbol_id, type_id) VALUES (?, ?, 0, NULL, NULL) "
        "ON CONFLICT(identity_kind, identity_text) DO UPDATE SET "
        "resolution_status = 0 RETURNING id");
    st.bind(1, kind);
    st.bind(2, std::string_view(*text));
    if (!st.step()) {
      throw StorageError("external identity insert returned no id");
    }
    const int64_t id = st.col_int64(0);
    st.step_done();
    return id;
  };
  const auto recv_type = type_id(s.recv_type_usr);
  const auto recv_decl = symbol_id(s.recv_decl_usr);
  const auto recv_type_external =
      unresolved_id(1, s.recv_type_usr, std::nullopt, recv_type);
  const auto recv_decl_external =
      unresolved_id(2, s.recv_decl_usr, recv_decl, std::nullopt);
  auto st = db_.prepare(
      "INSERT OR IGNORE INTO edge_site "
      "(edge_id, file_id, line, col, conditional, args_sig, "
      " recv_src_kind, recv_type_usr, recv_decl_usr, recv_src_kind_id, "
      " recv_type_id, recv_decl_id, recv_type_identity_id, "
      " recv_decl_identity_id, recv_param_pos, recv_type_is_value) "
      "VALUES (?, ?, ?, ?, ?, ?, NULL, NULL, NULL, ?, ?, ?, ?, ?, ?, ?)");
  st.bind(1, s.edge_id);
  bind_opt(st, 2, s.file_id);
  bind_opt(st, 3, s.line);
  bind_opt(st, 4, s.col);
  st.bind(5, s.conditional);
  bind_opt(st, 6, s.args_sig);
  bind_opt(st, 7, source_id);
  bind_opt(st, 8, recv_type);
  bind_opt(st, 9, recv_decl);
  bind_opt(st, 10, recv_type_external);
  bind_opt(st, 11, recv_decl_external);
  bind_opt(st, 12, s.recv_param_pos);
  bind_opt(st, 13, s.recv_type_is_value);
  st.step_done();
}

void SqliteStorageService::add_call_arg(const CallArg &a) {
  const int64_t source_id = source_kind_id(a.src_kind);
  if (source_id < 0) {
    throw StorageError("unknown source kind '" + a.src_kind + "'");
  }
  const auto symbol_id =
      [this](const std::optional<std::string> &usr) -> std::optional<int64_t> {
    if (!usr || usr->empty()) {
      return std::nullopt;
    }
    auto st = db_.prepare("SELECT id FROM symbol WHERE usr = ? LIMIT 1");
    st.bind(1, std::string_view(*usr));
    return st.step() ? std::optional<int64_t>(st.col_int64(0)) : std::nullopt;
  };
  const auto type_id =
      [this](const std::optional<std::string> &usr) -> std::optional<int64_t> {
    if (!usr || usr->empty()) {
      return std::nullopt;
    }
    auto st = db_.prepare(
        "SELECT id FROM type_node WHERE decl_usr = ? ORDER BY id LIMIT 1");
    st.bind(1, std::string_view(*usr));
    return st.step() ? std::optional<int64_t>(st.col_int64(0)) : std::nullopt;
  };
  const auto unresolved_id =
      [this](const std::optional<std::string> &text,
             const std::optional<int64_t> &sid,
             const std::optional<int64_t> &tid) -> std::optional<int64_t> {
    if (!text || text->empty() || sid || tid) {
      return std::nullopt;
    }
    auto st = db_.prepare(
        "INSERT INTO external_identity(identity_kind, identity_text, "
        "resolution_status, symbol_id, type_id) VALUES (?, ?, 0, NULL, NULL) "
        "ON CONFLICT(identity_kind, identity_text) DO UPDATE SET "
        "resolution_status = 0 RETURNING id");
    st.bind(1, int64_t{2});
    st.bind(2, std::string_view(*text));
    if (!st.step()) {
      throw StorageError("external identity insert returned no id");
    }
    const int64_t id = st.col_int64(0);
    st.step_done();
    return id;
  };
  const auto arg_type = type_id(a.type_usr);
  const auto arg_decl = symbol_id(a.decl_usr);
  const auto arg_callee = symbol_id(a.callee_usr);
  const auto arg_type_external = [&]() -> std::optional<int64_t> {
    if (!a.type_usr || a.type_usr->empty() || arg_type) {
      return std::nullopt;
    }
    auto st = db_.prepare(
        "INSERT INTO external_identity(identity_kind, identity_text, "
        "resolution_status, symbol_id, type_id) VALUES (1, ?, 0, NULL, NULL) "
        "ON CONFLICT(identity_kind, identity_text) DO UPDATE SET "
        "resolution_status = 0 RETURNING id");
    st.bind(1, std::string_view(*a.type_usr));
    if (!st.step()) {
      throw StorageError("external identity insert returned no id");
    }
    const int64_t id = st.col_int64(0);
    st.step_done();
    return id;
  }();
  const auto arg_decl_external =
      unresolved_id(a.decl_usr, arg_decl, std::nullopt);
  const auto arg_callee_external =
      unresolved_id(a.callee_usr, arg_callee, std::nullopt);
  auto st = db_.prepare(
      "INSERT OR IGNORE INTO call_arg "
      "(edge_id, file_id, line, col, position, src_kind, "
      " type_usr, decl_usr, callee_usr, src_kind_id, type_id, "
      " decl_id, callee_id, type_identity_id, decl_identity_id, "
      " callee_identity_id, type_is_value) "
      "VALUES (?, ?, ?, ?, ?, NULL, NULL, NULL, NULL, ?, ?, ?, ?, ?, ?, ?, ?)");
  st.bind(1, a.edge_id);
  st.bind(2, a.file_id);
  st.bind(3, a.line);
  st.bind(4, a.col);
  st.bind(5, a.position);
  st.bind(6, source_id);
  bind_opt(st, 7, arg_type);
  bind_opt(st, 8, arg_decl);
  bind_opt(st, 9, arg_callee);
  bind_opt(st, 10, arg_type_external);
  bind_opt(st, 11, arg_decl_external);
  bind_opt(st, 12, arg_callee_external);
  bind_opt(st, 13, a.type_is_value);
  st.step_done();
}

void SqliteStorageService::add_template_param(const TemplateParam &p) {
  auto st = db_.prepare("INSERT OR REPLACE INTO template_param "
                        "(owner_id, position, param_kind, name, default_txt, "
                        "type_id, default_type_id, default_ref_id) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
  st.bind(1, p.owner_id);
  st.bind(2, p.position);
  st.bind(3, p.param_kind);
  bind_opt(st, 4, p.name);
  bind_opt(st, 5, p.default_txt);
  bind_opt(st, 6, p.type_id);
  bind_opt(st, 7, p.default_type_id);
  bind_opt(st, 8, p.default_ref_id);
  st.step_done();
}

void SqliteStorageService::add_template_arg(const TemplateArg &a) {
  auto st = db_.prepare("INSERT OR REPLACE INTO template_arg "
                        "(owner_id, position, pack_index, arg_kind, ref_id, "
                        "literal, type_id) VALUES (?, ?, ?, ?, ?, ?, ?)");
  st.bind(1, a.owner_id);
  st.bind(2, a.position);
  st.bind(3, a.pack_index);
  st.bind(4, a.arg_kind);
  bind_opt(st, 5, a.ref_id);
  bind_opt(st, 6, a.literal);
  bind_opt(st, 7, a.type_id);
  st.step_done();
}

// -- v30 signature/type tier
// ---------------------------------------------------

} // namespace cidx
