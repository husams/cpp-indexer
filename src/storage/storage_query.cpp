// Read-side queries: stats, symbol search, graph rows, labels and aliases.
// Split out of storage.cpp; Storage's interface is unchanged.
#include "storage/storage.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <ctime>
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
#include "util/hashing.hpp"
#include "util/json_min.hpp"
#include "util/logger.hpp"
#include "util/pathutil.hpp"

namespace cidx {

using namespace detail;

namespace {

struct IdentityFile {
  int64_t id = -1;
  std::string key;
  std::string compile_options;
  std::string driver;
  bool compile_options_null = true;
  bool driver_null = true;
  bool indexed = false;
};

std::vector<IdentityFile> identity_files(SqliteStorageService &db) {
  std::vector<IdentityFile> files;
  auto st = db.raw_db().prepare(
      "SELECT f.id, c.name, c.path, c.kind, c.version, r.name, "
      "r.remote_url, d.path, f.name, f.compile_options, f.driver, f.indexed "
      "FROM file f JOIN directory d ON d.id = f.directory_id "
      "JOIN component c ON c.id = d.component_id "
      "LEFT JOIN repository r ON r.id = c.repository_id "
      "ORDER BY c.name, c.path, c.kind, COALESCE(c.version, '<null>'), "
      "COALESCE(r.name, '<null>'), COALESCE(r.remote_url, '<null>'), "
      "d.path, f.name");
  while (st.step()) {
    IdentityFile file;
    file.id = st.col_int64(0);
    const auto append_identity_field = [&file, &st](int column) {
      file.key += st.col_is_null(column) ? "<null>" : st.col_text(column);
      file.key.push_back('\0');
    };
    append_identity_field(1);   // component.name
    append_identity_field(2);   // component.path
    append_identity_field(3);   // component.kind
    append_identity_field(4);   // component.version
    append_identity_field(5);   // repository.name
    append_identity_field(6);   // repository.remote_url
    file.key += st.col_text(7); // directory.path
    file.key.push_back('\0');
    file.key += st.col_text(8); // file.name
    file.compile_options_null = st.col_is_null(9);
    file.compile_options = st.col_text(9);
    file.driver_null = st.col_is_null(10);
    file.driver = st.col_text(10);
    file.indexed = st.col_int64(11) != 0;
    files.push_back(std::move(file));
  }
  return files;
}

std::string source_manifest(SqliteStorageService &db,
                            const std::vector<IdentityFile> &files,
                            bool &complete) {
  std::string manifest;
  complete = true;
  for (const IdentityFile &file : files) {
    const auto md5 = md5_of(db.file_abs_path(file.id).value_or(""));
    if (!md5) {
      complete = false;
    }
    manifest += file.key;
    manifest.push_back('\0');
    manifest += md5.value_or("<unreadable>");
    manifest.push_back('\0');
    manifest += file.indexed ? "1\n" : "0\n";
  }
  return manifest;
}

std::string config_manifest(const std::vector<IdentityFile> &files) {
  std::string manifest;
  for (const IdentityFile &file : files) {
    manifest += file.key;
    manifest.push_back('\0');
    manifest += file.compile_options_null ? "<null>" : file.compile_options;
    manifest.push_back('\0');
    manifest += file.driver_null ? "<null>" : file.driver;
    manifest += "\n";
  }
  return manifest;
}

std::optional<std::string> meta_value(SqliteStorageService &db,
                                      const char *key) {
  auto st = db.raw_db().prepare("SELECT value FROM meta WHERE key = ?");
  st.bind(1, std::string_view(key));
  if (!st.step() || st.col_is_null(0) || st.col_text(0).empty()) {
    return std::nullopt;
  }
  return st.col_text(0);
}

void set_meta_value(SqliteStorageService &db, const char *key,
                    std::string_view value) {
  auto st = db.raw_db().prepare(
      "INSERT INTO meta (key, value) VALUES (?, ?) "
      "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  st.bind(1, std::string_view(key));
  st.bind(2, std::string_view(value));
  st.step_done();
}

} // namespace

IndexIdentity SqliteStorageService::index_identity() {
  const auto files = identity_files(*this);
  bool complete = true;
  const std::string source = source_manifest(*this, files, complete);
  const std::string source_fingerprint = sha1_hex(source);
  const std::string config_fingerprint = sha1_hex(config_manifest(files));
  const auto stored_source = meta_value(*this, "source_fingerprint");
  const auto stored_revision = meta_value(*this, "source_revision");
  const auto stored_config = meta_value(*this, "index_config");
  const auto stored_config_fingerprint =
      meta_value(*this, "index_config_fingerprint");
  const auto identity_version = meta_value(*this, "index_identity_version");

  IndexIdentity identity;
  identity.schema_version = kSchemaVersion;
  std::vector<std::string> owners;
  auto owner_stmt = db_.prepare(
      "SELECT DISTINCT COALESCE(r.remote_url, ''), COALESCE(r.name, ''), "
      "c.path FROM component c LEFT JOIN repository r ON r.id = "
      "c.repository_id "
      "ORDER BY 1, 2, 3");
  while (owner_stmt.step()) {
    const std::string remote = owner_stmt.col_text(0);
    const std::string repository = owner_stmt.col_text(1);
    const std::string component = owner_stmt.col_text(2);
    if (!remote.empty()) {
      owners.push_back("remote:" + remote);
    } else if (!repository.empty()) {
      owners.push_back("repo:" + repository);
    } else {
      owners.push_back("component:" + component);
    }
  }
  std::ranges::sort(owners);
  owners.erase(std::ranges::unique(owners).begin(), owners.end());
  std::string owner_material;
  for (std::size_t index = 0; index < owners.size(); ++index) {
    if (index != 0) {
      owner_material.push_back('\0');
    }
    owner_material += owners[index];
  }
  if (owner_material.empty()) {
    owner_material = "memory";
  }
  identity.workspace = "workspace:" + sha1_hex(owner_material);
  identity.source_revision = stored_revision;
  identity.source_fingerprint = stored_source;
  identity.index_config = stored_config;
  identity.index_config_fingerprint = stored_config_fingerprint;
  if (!identity_version || *identity_version != "1" || !stored_source ||
      !stored_revision || !stored_config || !stored_config_fingerprint) {
    return identity;
  }
  if (!complete) {
    return identity;
  }
  if (*stored_source != source_fingerprint ||
      *stored_revision != "content-sha1:" + source_fingerprint ||
      *stored_config_fingerprint != config_fingerprint ||
      std::ranges::any_of(
          files, [](const IdentityFile &file) { return !file.indexed; })) {
    identity.freshness = "stale";
    return identity;
  }
  identity.freshness = "current";
  return identity;
}

void SqliteStorageService::stamp_index_identity() {
  const auto files = identity_files(*this);
  bool complete = true;
  const std::string source = source_manifest(*this, files, complete);
  const std::string config = config_manifest(files);
  const std::string source_fingerprint = sha1_hex(source);
  const std::string config_fingerprint = sha1_hex(config);
  set_meta_value(*this, "index_identity_version", "1");
  set_meta_value(*this, "index_config", "manifest-sha1-v1");
  set_meta_value(*this, "index_config_fingerprint", config_fingerprint);
  set_meta_value(*this, "source_fingerprint",
                 complete ? source_fingerprint : "");
  set_meta_value(*this, "source_revision",
                 complete ? "content-sha1:" + source_fingerprint : "");
}

std::string SqliteStorageService::fuzzy_like(std::string_view text) {
  // '%c%c%' from the non-space chars, escaping '\ % _' (G18); used with
  // LIKE ... ESCAPE '\' — ASCII case-insensitive.
  std::vector<std::string> chars;
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      continue;
    }
    if (c == '\\') {
      chars.emplace_back("\\\\");
    } else if (c == '%') {
      chars.emplace_back("\\%");
    } else if (c == '_') {
      chars.emplace_back("\\_");
    } else {
      chars.emplace_back(1, c);
    }
  }
  return "%" + join_strings(chars, "%") + "%";
}

// -- stats
// ----------------------------------------------------------------------------

Stats SqliteStorageService::stats() {
  const auto one = [this](const char *sql) {
    auto st = db_.prepare(sql);
    if (!st.step()) {
      throw StorageError("stats query returned no row");
    }
    return st.col_int64(0);
  };
  Stats s;
  s.components = one("SELECT COUNT(*) FROM component");
  s.directories = one("SELECT COUNT(*) FROM directory");
  s.files = one("SELECT COUNT(*) FROM file");
  s.files_indexed = one("SELECT COUNT(*) FROM file WHERE indexed = 1");
  s.symbols = one("SELECT COUNT(*) FROM symbol");
  s.symbols_unresolved = one("SELECT COUNT(*) FROM symbol WHERE resolved = 0");
  {
    auto st = db_.prepare(
        "SELECT kind, COUNT(*) AS n FROM symbol GROUP BY kind ORDER BY kind");
    while (st.step()) {
      s.symbols_by_kind[symbol_kind_name(st.col_int64(0))] = st.col_int64(1);
    }
  }
  s.edges = one("SELECT COUNT(*) FROM edge");
  {
    auto st = db_.prepare(
        "SELECT k.name, COUNT(*) AS n FROM edge e "
        "JOIN edge_kind k ON k.id = e.kind GROUP BY k.name ORDER BY k.name");
    while (st.step()) {
      s.edges_by_kind[st.col_text(0)] = st.col_int64(1);
    }
  }
  return s;
}

auto SqliteStorageService::integrity_ok() -> bool {
  auto st = db_.prepare("PRAGMA integrity_check");
  return st.step() && st.col_text(0) == "ok";
}

auto SqliteStorageService::foreign_keys_ok() -> bool {
  auto st = db_.prepare("PRAGMA foreign_key_check");
  return !st.step();
}

// ============================================================================
// M6 graph read-only accessors (A1–A8)
// ============================================================================

// A1 — total edge count (query.py:558)
int64_t SqliteStorageService::edge_count() {
  auto st = db_.prepare("SELECT COUNT(*) FROM edge");
  if (!st.step()) {
    return 0;
  }
  return st.col_int64(0);
}

// A2 — true once graph_resolved_at is set (query.py:579-583)
bool SqliteStorageService::graph_resolved() {
  auto st =
      db_.prepare("SELECT value FROM meta WHERE key = 'graph_resolved_at'");
  if (!st.step()) {
    return false;
  }
  const std::string val = st.col_text(0);
  return !val.empty();
}

void SqliteStorageService::stamp_graph_resolved() {
  const std::time_t now = std::time(nullptr);
  std::array<char, 32> buffer{};
  std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ",
                std::gmtime(&now));
  auto statement = db_.prepare("INSERT OR REPLACE INTO meta (key, value) "
                               "VALUES ('graph_resolved_at', ?)");
  statement.bind(1, std::string_view(buffer.data()));
  statement.step_done();
}

// A3 — fetch one symbol by USR (query.py:666-668)
std::optional<Symbol>
SqliteStorageService::graph_symbol_by_usr(const std::string &usr) {
  return lookup_symbol(usr);
}

// A4 — fetch one symbol by numeric id (query.py:666-668)
std::optional<Symbol> SqliteStorageService::graph_symbol_by_id(int64_t id) {
  auto st = db_.prepare(std::string("SELECT ") + kSymbolColsS +
                        " FROM symbol s WHERE s.id = ?");
  st.bind(1, id);
  if (!st.step()) {
    return std::nullopt;
  }
  return symbol_from_offset(st, 0);
}

// A5 — fuzzy COALESCE(qual_name,spelling) lookup (query.py:707-738, R1)
// Escapes ONLY % and _ (NOT backslash — matching query.py:719).
std::vector<Symbol>
SqliteStorageService::find_symbols(const std::string &pattern,
                                   const std::optional<std::string> &kind,
                                   int limit) {
  // Build like: "%" + join("%", escaped_segs) + "%"
  // where escaped_segs = each "::" segment with % and _ escaped.
  std::vector<std::string> segs;
  std::size_t start = 0;
  while (start <= pattern.size()) {
    const std::size_t pos = pattern.find("::", start);
    const std::string seg = pattern.substr(
        start, pos == std::string::npos ? std::string::npos : pos - start);
    if (!seg.empty()) {
      std::string esc;
      esc.reserve(seg.size());
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
  std::string like = "%";
  for (std::size_t i = 0; i < segs.size(); ++i) {
    if (i != 0) {
      like += "%";
    }
    like += segs[i];
  }
  like += "%";

  std::string sql = std::string("SELECT ") + kSymbolColsS +
                    " FROM symbol s WHERE COALESCE(s.qual_name, s.spelling) "
                    "LIKE ? ESCAPE '\\'";
  std::vector<SqlValue> args;
  args.emplace_back(like);
  if (kind) {
    sql += " AND s.kind = ?";
    args.emplace_back(symbol_kind_id(*kind)); // stored as int (v16)
  }
  sql += " ORDER BY LENGTH(COALESCE(s.qual_name, s.spelling)), "
         "COALESCE(s.qual_name, s.spelling) LIMIT ?";
  args.emplace_back(static_cast<int64_t>(limit));

  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < args.size(); ++i) {
    st.bind(static_cast<int>(i + 1), args[i]);
  }
  std::vector<Symbol> out;
  while (st.step()) {
    out.push_back(symbol_from_offset(st, 0));
  }
  return out;
}

// v27 — symbols defined in >1 backend (query.py:GraphQuery.redefined)
std::vector<Symbol> SqliteStorageService::redefined_symbols(int limit) {
  auto st = db_.prepare(std::string("SELECT ") + kSymbolColsS +
                        " FROM symbol s WHERE s.multi_def > 1 "
                        "ORDER BY s.multi_def DESC, s.qual_name, s.spelling "
                        "LIMIT ?");
  st.bind(1, static_cast<int64_t>(limit));
  std::vector<Symbol> out;
  while (st.step()) {
    out.push_back(symbol_from_offset(st, 0));
  }
  return out;
}

// v27 — the backend bodies of a symbol (query.py:GraphQuery.definitions).
std::vector<SqliteStorageService::DefinitionRow>
SqliteStorageService::definitions_of(int64_t symbol_id) {
  auto st = db_.prepare(
      "SELECT symbol_id, file_id, line, col, end_line, end_col, init_text "
      "FROM definition WHERE symbol_id = ? ORDER BY file_id, line");
  st.bind(1, symbol_id);
  std::vector<DefinitionRow> out;
  while (st.step()) {
    DefinitionRow d;
    d.symbol_id = st.col_int64(0);
    d.file_id = opt_int64(st, 1);
    d.line = opt_int64(st, 2);
    d.col = opt_int64(st, 3);
    d.end_line = opt_int64(st, 4);
    d.end_col = opt_int64(st, 5);
    d.init_text = opt_text(st, 6);
    out.push_back(d);
  }
  return out;
}

// v27 — possible-call fan-out: candidate target bodies for calls made by any of
// this symbol's bodies (query.py:GraphQuery.possible_callees).
std::vector<SqliteStorageService::DefinitionRow>
SqliteStorageService::possible_callees_of(int64_t symbol_id) {
  auto st = db_.prepare(
      "SELECT td.symbol_id, td.file_id, td.line, td.col, td.end_line, "
      "       td.end_col, td.init_text "
      "FROM possible_call pc "
      "JOIN definition sd ON sd.id = pc.src_def_id "
      "JOIN definition td ON td.id = pc.dst_def_id "
      "WHERE sd.symbol_id = ? ORDER BY td.symbol_id, td.file_id");
  st.bind(1, symbol_id);
  std::vector<DefinitionRow> out;
  while (st.step()) {
    DefinitionRow d;
    d.symbol_id = st.col_int64(0);
    d.file_id = opt_int64(st, 1);
    d.line = opt_int64(st, 2);
    d.col = opt_int64(st, 3);
    d.end_line = opt_int64(st, 4);
    d.end_col = opt_int64(st, 5);
    d.init_text = opt_text(st, 6);
    out.push_back(d);
  }
  return out;
}

// A6 — typed-edge query (query.py:782-813)
std::vector<SqliteStorageService::GraphEdgeRow>
SqliteStorageService::graph_edges(int64_t mine_id, const std::string &direction,
                                  const std::vector<int64_t> &kind_ids,
                                  bool count_resolved, int limit) {
  // direction "in": mine=dst_id, peer=src_id
  // direction "out": mine=src_id, peer=dst_id
  std::string mine;
  std::string peer;
  if (direction == "in") {
    mine = "dst_id";
    peer = "src_id";
  } else {
    mine = "src_id";
    peer = "dst_id";
  }

  const std::string count_expr =
      count_resolved
          ? "e.count"
          : "(SELECT COUNT(*) FROM edge_site es WHERE es.edge_id = e.id)";

  std::string sql =
      "SELECT e.id AS eid, e.src_id, e.dst_id, e.kind AS ekind, " + count_expr +
      " AS ecount, e.count AS rawcount, "
      "e.base_access, e.is_virtual, " +
      std::string(kSymbolColsS) + " FROM edge e JOIN symbol s ON s.id = e." +
      peer + " WHERE e." + mine + " = ?";

  std::vector<SqlValue> args;
  args.emplace_back(mine_id);

  if (!kind_ids.empty()) {
    sql += " AND e.kind IN (";
    for (std::size_t i = 0; i < kind_ids.size(); ++i) {
      if (i != 0) {
        sql += ",";
      }
      sql += "?";
    }
    sql += ")";
    for (int64_t kid : kind_ids) {
      args.emplace_back(kid);
    }
  }
  sql += " ORDER BY ecount DESC, e.kind LIMIT ?";
  args.emplace_back(static_cast<int64_t>(limit));

  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < args.size(); ++i) {
    st.bind(static_cast<int>(i + 1), args[i]);
  }

  std::vector<GraphEdgeRow> out;
  while (st.step()) {
    GraphEdgeRow row;
    row.eid = st.col_int64(0);
    row.src_id = st.col_int64(1);
    row.dst_id = st.col_int64(2);
    row.ekind = st.col_int64(3);
    row.ecount = st.col_int64(4);
    row.rawcount = st.col_int64(5);
    row.base_access = opt_int64(st, 6);
    row.is_virtual = opt_int64(st, 7);
    // Sym columns start at col 8 (R: column-order mismatch, plan §CRITICAL)
    row.sym = symbol_from_offset(st, 8);
    out.push_back(std::move(row));
  }
  return out;
}

// A7 — batch-load edge_site rows for many edge_ids (query.py:839-870)
std::map<int64_t, std::vector<SqliteStorageService::EdgeSiteRow>>
SqliteStorageService::edge_sites_for(const std::vector<int64_t> &edge_ids) {
  if (edge_ids.empty()) {
    return {};
  }
  std::string sql =
      "SELECT edge_id, file_id, line, col, conditional, args_sig, "
      "       recv_src_kind, recv_type_usr, recv_decl_usr, recv_param_pos, "
      "       recv_type_is_value "
      "FROM edge_site_read WHERE edge_id IN (";
  for (std::size_t i = 0; i < edge_ids.size(); ++i) {
    if (i != 0) {
      sql += ",";
    }
    sql += "?";
  }
  sql += ") ORDER BY edge_id, file_id, line, col";

  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < edge_ids.size(); ++i) {
    st.bind(static_cast<int>(i + 1), edge_ids[i]);
  }

  std::map<int64_t, std::vector<EdgeSiteRow>> out;
  while (st.step()) {
    EdgeSiteRow row;
    row.edge_id = st.col_int64(0);
    row.file_id = opt_int64(st, 1);
    row.line = opt_int64(st, 2);
    row.col = opt_int64(st, 3);
    row.conditional = st.col_int64(4) != 0;
    row.args_sig = opt_text(st, 5);
    row.recv_src_kind = opt_text(st, 6);
    row.recv_type_usr = opt_text(st, 7);
    row.recv_decl_usr = opt_text(st, 8);
    row.recv_param_pos = opt_int64(st, 9);
    row.recv_type_is_value = opt_int64(st, 10);
    out[row.edge_id].push_back(std::move(row));
  }
  return out;
}

// A8 — single-edge sites with LIMIT (query.py:884-906)
std::vector<SqliteStorageService::EdgeSiteRow>
SqliteStorageService::edge_sites_one(int64_t edge_id, int limit) {
  auto st = db_.prepare(
      "SELECT file_id, line, col, conditional, args_sig, "
      "       recv_src_kind, recv_type_usr, recv_decl_usr, recv_param_pos, "
      "       recv_type_is_value "
      "FROM edge_site_read WHERE edge_id = ? ORDER BY file_id, line, col "
      "LIMIT ?");
  st.bind(1, edge_id);
  st.bind(2, static_cast<int64_t>(limit));

  std::vector<EdgeSiteRow> out;
  while (st.step()) {
    EdgeSiteRow row;
    row.edge_id = edge_id;
    row.file_id = opt_int64(st, 0);
    row.line = opt_int64(st, 1);
    row.col = opt_int64(st, 2);
    row.conditional = st.col_int64(3) != 0;
    row.args_sig = opt_text(st, 4);
    row.recv_src_kind = opt_text(st, 5);
    row.recv_type_usr = opt_text(st, 6);
    row.recv_decl_usr = opt_text(st, 7);
    row.recv_param_pos = opt_int64(st, 8);
    row.recv_type_is_value = opt_int64(st, 9);
    out.push_back(std::move(row));
  }
  return out;
}

bool SqliteStorageService::edge_has_conditional_site(int64_t edge_id) {
  auto st = db_.prepare(
      "SELECT EXISTS(SELECT 1 FROM edge_site WHERE edge_id = ? AND "
      "conditional != 0)");
  st.bind(1, edge_id);
  if (!st.step()) {
    return false;
  }
  return st.col_int64(0) != 0;
}

// -- labels (v14) ------------------------------------------------------------

int64_t SqliteStorageService::add_label(const std::string &name,
                                        const std::string &path) {
  auto st = db_.prepare("INSERT INTO label (name, path) VALUES (?, ?) "
                        "ON CONFLICT(name) DO UPDATE SET path = excluded.path "
                        "RETURNING id");
  st.bind(1, std::string_view(name));
  st.bind(2, std::string_view(path));
  if (!st.step()) {
    throw StorageError("label upsert returned no id");
  }
  const int64_t lid = st.col_int64(0);
  st.step_done();
  return lid;
}

bool SqliteStorageService::remove_label(const std::string &name) {
  auto st = db_.prepare("DELETE FROM label WHERE name = ?");
  st.bind(1, std::string_view(name));
  st.step_done();
  return db_.changes() > 0;
}

std::optional<std::string>
SqliteStorageService::get_label(const std::string &name) {
  auto st = db_.prepare("SELECT path FROM label WHERE name = ?");
  st.bind(1, std::string_view(name));
  if (!st.step()) {
    return std::nullopt;
  }
  return st.col_text(0);
}

std::vector<std::pair<std::string, std::string>>
SqliteStorageService::list_labels() {
  auto st = db_.prepare("SELECT name, path FROM label ORDER BY name");
  std::vector<std::pair<std::string, std::string>> out;
  while (st.step()) {
    out.emplace_back(st.col_text(0), st.col_text(1));
  }
  return out;
}

std::map<std::string, std::tuple<std::string, std::string, bool>>
SqliteStorageService::component_alias_index() {
  // Group components by name; split the resolved effective root into
  // (base, version) so matching is version-agnostic. Mirrors
  // Python Storage.component_alias_index.
  struct Row {
    std::string base;
    std::string ver;       // "" = none
    bool path_unversioned; // stored path carries no embedded version
  };
  std::map<std::string, std::vector<Row>> by_name;
  for (const auto &c : list_components()) {
    const std::string eff = component_abs_base(c);
    const auto [base, ver] = CompileDb::split_base_version(eff);
    Component cbase = c;
    cbase.version = std::nullopt;
    const auto [pbase, pver] =
        CompileDb::split_base_version(component_abs_base(cbase));
    (void)pbase;
    by_name[c.name].push_back(
        {.base = base, .ver = ver, .path_unversioned = pver.empty()});
  }
  std::map<std::string, std::tuple<std::string, std::string, bool>> out;
  for (const auto &[name, rows] : by_name) {
    std::string base = rows.front().base;
    bool one_base = true;
    for (const auto &r : rows) {
      if (r.base != base) {
        one_base = false;
        break;
      }
    }
    if (!one_base) {
      continue; // ambiguous: same name, different base dirs
    }
    std::string maxver;
    for (const auto &r : rows) {
      if (r.ver.empty()) {
        continue;
      }
      if (maxver.empty() ||
          CompileDb::version_key(r.ver) > CompileDb::version_key(maxver)) {
        maxver = r.ver;
      }
    }
    const bool bumpable = rows.size() == 1 && rows.front().path_unversioned;
    out.emplace(name, std::make_tuple(base, maxver, bumpable));
  }
  return out;
}

std::vector<std::tuple<std::string, std::string, bool>>
SqliteStorageService::list_alias_pairs() {
  // Explicit labels (exact) PLUS components (version-stripped base,
  // version-agnostic). Labels win on a name collision. std::map keeps the
  // result sorted by name (== Python sorted). Mirrors Python list_alias_pairs.
  std::map<std::string, std::tuple<std::string, bool>>
      pairs; // name->(path,ver)
  for (const auto &nv : list_labels()) {
    pairs[nv.first] = {nv.second, false}; // labels first / win
  }
  for (const auto &[name, entry] : component_alias_index()) {
    if (!pairs.contains(name)) {
      pairs[name] = {std::get<0>(entry), true}; // version-stripped base
    }
  }
  std::vector<std::tuple<std::string, std::string, bool>> out;
  out.reserve(pairs.size());
  for (const auto &[name, pv] : pairs) {
    out.emplace_back(name, std::get<0>(pv), std::get<1>(pv));
  }
  return out;
}

std::optional<std::string>
SqliteStorageService::get_alias(const std::string &name) {
  std::optional<std::string> lab = get_label(name);
  if (lab.has_value()) {
    return lab;
  }
  const auto idx = component_alias_index();
  const auto it = idx.find(name);
  if (it == idx.end()) {
    return std::nullopt;
  }
  const auto &[base, maxver, bump] = it->second;
  (void)bump;
  return maxver.empty() ? base : pathutil::join(base, maxver);
}

} // namespace cidx
