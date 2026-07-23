// Row decoders, column lists and small SQL/string helpers shared by the
// storage_*.cpp translation units. These were file-local helpers inside
// storage.cpp; splitting Storage across TUs made them shared, so they live
// here in cidx::detail rather than an anonymous namespace.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "storage/records.hpp"
#include "storage/sqlite.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"
#include "util/json_min.hpp"
#include "util/pathutil.hpp"

namespace cidx::detail {

constexpr std::array<std::string_view, 17> kSymbolKinds = {
    "class",          "struct",
    "union",          "function",
    "method",         "member",
    "constructor",    "destructor",
    "enum",           "enum-constant",
    "typedef",        "type-alias",
    "class-template", "function-template",
    "variable",       "namespace",
    "macro",
};

// symbol.kind name <-> stored integer (== libclang CXCursorKind). Mirrors
// storage.py SYMBOL_KIND_IDS / SYMBOL_KIND_NAMES. The metadata symbol_kind
// table is seeded with the same pairs (display only).
inline const std::map<std::string_view, int64_t> &symbol_kind_ids_map() {
  static const std::map<std::string_view, int64_t> m = {
      {"struct", 2},          {"union", 3},
      {"class", 4},           {"enum", 5},
      {"member", 6},          {"enum-constant", 7},
      {"function", 8},        {"variable", 9},
      {"typedef", 20},        {"method", 21},
      {"namespace", 22},      {"constructor", 24},
      {"destructor", 25},     {"function-template", 30},
      {"class-template", 31}, {"type-alias", 36},
      {"macro", 501},
  };
  return m;
}

inline const std::map<int64_t, std::string> &symbol_kind_names_map() {
  static const std::map<int64_t, std::string> m = [] {
    std::map<int64_t, std::string> r;
    for (const auto &kv : symbol_kind_ids_map()) {
      r.emplace(kv.second, std::string(kv.first));
    }
    return r;
  }();
  return m;
}

// Python Storage._SYMBOL_COLS — insert/update order is load-bearing for the
// upsert statement and for update_symbol validation.
constexpr std::array<std::string_view, 24> kSymbolInsertCols = {
    "usr",
    "spelling",
    "qual_name",
    "display_name",
    "kind",
    "type_info",
    "file_id",
    "line",
    "col",
    "decl_file_id",
    "decl_line",
    "decl_col",
    "decl_path",
    "is_definition",
    "is_pure",
    "is_static",
    "is_instantiation",
    "linkage",
    "access",
    "parent_usr",
    "resolved",
    "end_line",
    "end_col",
    "const_value",
};

// Explicit SELECT lists (stable column positions even on migrated DBs).
// Column order mirrors _SYMBOL_COLS in storage.py; is_instantiation comes
// right after is_static (col index 16), then linkage/access/parent_usr/resolved
// (17-20), then decl_path appended last (21) -- append-at-end pattern for
// migrated DBs (ALTER TABLE appends; positional decode in symbol_from must
// match). v14: version appended at end (append-at-end discipline so migrated
// DBs whose ALTER added the column last decode positionally).
constexpr const char *kComponentCols =
    "id, name, path, kind, version, repository_id";
constexpr const char *kRepositoryCols =
    "id, name, kind, remote_url, active_clone_id";
constexpr const char *kCloneCols = "id, repository_id, path, label";
constexpr const char *kDirectoryCols = "id, component_id, path";
constexpr const char *kFileCols =
    "id, directory_id, name, mtime, md5, compile_options, driver, indexed, "
    "indexed_at, args_overridden";
constexpr const char *kSymbolCols =
    "id, usr, spelling, qual_name, display_name, kind, type_info, file_id, "
    "line, col, decl_file_id, decl_line, decl_col, is_definition, is_pure, "
    "is_static, linkage, access, parent_usr, resolved, decl_path, "
    "is_instantiation, end_line, end_col, multi_def, const_value";
constexpr const char *kSymbolColsS =
    "s.id, s.usr, s.spelling, s.qual_name, s.display_name, s.kind, "
    "s.type_info, s.file_id, s.line, s.col, s.decl_file_id, s.decl_line, "
    "s.decl_col, s.is_definition, s.is_pure, s.is_static, s.linkage, s.access, "
    "s.parent_usr, s.resolved, s.decl_path, s.is_instantiation, "
    "s.end_line, s.end_col, s.multi_def, s.const_value";

inline std::optional<int64_t> opt_int64(const SqliteStmt &st, int idx) {
  if (st.col_is_null(idx)) {
    return std::nullopt;
  }
  return st.col_int64(idx);
}

inline std::optional<double> opt_double(const SqliteStmt &st, int idx) {
  if (st.col_is_null(idx)) {
    return std::nullopt;
  }
  return st.col_double(idx);
}

inline std::optional<std::string> opt_text(const SqliteStmt &st, int idx) {
  if (st.col_is_null(idx)) {
    return std::nullopt;
  }
  return st.col_text(idx);
}

inline void bind_opt(SqliteStmt &st, int idx, const std::optional<int64_t> &v) {
  if (v) {
    st.bind(idx, *v);
  } else {
    st.bind_null(idx);
  }
}

inline void bind_opt(SqliteStmt &st, int idx, const std::optional<double> &v) {
  if (v) {
    st.bind(idx, *v);
  } else {
    st.bind_null(idx);
  }
}

inline void bind_opt(SqliteStmt &st, int idx,
                     const std::optional<std::string> &v) {
  if (v) {
    st.bind(idx, std::string_view(*v));
  } else {
    st.bind_null(idx);
  }
}

inline Component component_from(const SqliteStmt &st) {
  Component c;
  c.id = st.col_int64(0);
  c.name = st.col_text(1);
  c.path = st.col_text(2);
  c.kind = st.col_text(3);
  // v14: version at column 4 (appended last; nullopt when NULL)
  c.version = opt_text(st, 4);
  // v23: repository_id at column 5 (SELECT * order; nullopt when
  // NULL/ungrouped)
  c.repository_id = opt_int64(st, 5);
  return c;
}

inline Repository repository_from(const SqliteStmt &st) {
  Repository r;
  r.id = st.col_int64(0);
  r.name = st.col_text(1);
  r.kind = st.col_text(2);
  r.remote_url = opt_text(st, 3);
  r.active_clone_id = opt_int64(st, 4);
  return r;
}

inline Clone clone_from(const SqliteStmt &st) {
  Clone c;
  c.id = st.col_int64(0);
  c.repository_id = st.col_int64(1);
  c.path = st.col_text(2);
  c.label = opt_text(st, 3);
  return c;
}

inline Directory directory_from(const SqliteStmt &st) {
  Directory d;
  d.id = st.col_int64(0);
  d.component_id = st.col_int64(1);
  d.path = st.col_text(2);
  return d;
}

inline File file_from(const SqliteStmt &st) {
  File f;
  f.id = st.col_int64(0);
  f.directory_id = st.col_int64(1);
  f.name = st.col_text(2);
  f.mtime = opt_double(st, 3);
  f.md5 = opt_text(st, 4);
  if (!st.col_is_null(5)) {
    f.compile_options = json_min::decode_string_array(st.col_text(5));
  }
  f.driver = opt_text(st, 6);
  f.indexed = st.col_int64(7) != 0;
  f.indexed_at = opt_text(st, 8);
  f.args_overridden = st.col_int64(9) != 0;
  return f;
}

// symbol_from_offset: decode kSymbolColsS starting at column `off`.
// Called by symbol_from (off=0) and by A6 graph_edges (off=8, where 8 edge
// columns precede the symbol columns).
inline Symbol symbol_from_offset(const SqliteStmt &st, int off) {
  Symbol s;
  s.id = st.col_int64(off + 0);
  s.usr = st.col_text(off + 1);
  s.spelling = st.col_text(off + 2);
  s.qual_name = opt_text(st, off + 3);
  s.display_name = opt_text(st, off + 4);
  s.kind =
      symbol_kind_name(st.col_int64(off + 5)); // stored as CXCursorKind int
  s.type_info = opt_text(st, off + 6);
  s.file_id = opt_int64(st, off + 7);
  s.line = opt_int64(st, off + 8);
  s.col = opt_int64(st, off + 9);
  s.decl_file_id = opt_int64(st, off + 10);
  s.decl_line = opt_int64(st, off + 11);
  s.decl_col = opt_int64(st, off + 12);
  s.is_definition = st.col_int64(off + 13) != 0;
  s.is_pure = st.col_int64(off + 14) != 0;
  s.is_static = st.col_int64(off + 15) != 0;
  s.linkage = opt_text(st, off + 16);
  s.access = opt_text(st, off + 17);
  s.parent_usr = opt_text(st, off + 18);
  s.resolved = st.col_int64(off + 19) != 0;
  s.decl_path = opt_text(st, off + 20);
  s.is_instantiation = st.col_int64(off + 21) != 0;
  s.end_line = opt_int64(st, off + 22);
  s.end_col = opt_int64(st, off + 23);
  s.multi_def = st.col_int64(off + 24);   // v27
  s.const_value = opt_text(st, off + 25); // v33
  return s;
}

inline Symbol symbol_from(const SqliteStmt &st) {
  return symbol_from_offset(st, 0);
}

// Escape the LIKE metacharacters for use with ESCAPE '\' — order matters:
// backslash first.
inline std::string escape_like(const std::string &text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '%') {
      out += "\\%";
    } else if (c == '_') {
      out += "\\_";
    } else {
      out += c;
    }
  }
  return out;
}

inline std::string join_strings(const std::vector<std::string> &parts,
                                const std::string &sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out += sep;
    }
    out += parts[i];
  }
  return out;
}

// abs path = component.path / directory.path / file.name (rel may be '').
inline std::string reconstruct_path(const std::string &root,
                                    const std::string &rel,
                                    const std::string &name) {
  if (rel.empty()) {
    return pathutil::join(root, name);
  }
  return pathutil::join(root, rel, name);
}

// mkdir -p the DB directory before opening (storage.py:202-203); :memory:
// passes through untouched.
inline std::string prepare_db_path(const std::string &path) {
  if (path != ":memory:") {
    const std::string dir = pathutil::dirname(pathutil::abspath(path));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      throw StorageError("cannot create database directory " + dir + ": " +
                         ec.message());
    }
  }
  return path;
}

// Decode one include_config row starting at column `base`:
//   id, tu_file_id, digest, driver, working_dir, arguments, lang_mode,
//   resource_dir
inline IncludeConfig include_config_from(const SqliteStmt &st) {
  IncludeConfig c;
  c.id = st.col_int64(0);
  c.tu_file_id = st.col_int64(1);
  c.digest = st.col_text(2);
  c.driver = opt_text(st, 3);
  c.working_dir = opt_text(st, 4);
  if (!st.col_is_null(5)) {
    c.arguments = json_min::decode_string_array(st.col_text(5));
  }
  c.lang_mode = opt_text(st, 6);
  c.resource_dir = opt_text(st, 7);
  return c;
}

constexpr const char *kIncludeConfigCols =
    "id, tu_file_id, digest, driver, working_dir, arguments, lang_mode, "
    "resource_dir";

// Decode one include_edge row: id, src_file_id, dst_file_id, dst_path,
// config_id, is_system, is_generated, count
inline IncludeEdge include_edge_from(const SqliteStmt &st) {
  IncludeEdge e;
  e.id = st.col_int64(0);
  e.src_file_id = st.col_int64(1);
  e.dst_file_id = opt_int64(st, 2);
  e.dst_path = st.col_text(3);
  e.config_id = st.col_int64(4);
  e.is_system = st.col_int64(5) != 0;
  e.is_generated = st.col_int64(6) != 0;
  e.count = st.col_int64(7);
  return e;
}

constexpr const char *kIncludeEdgeCols =
    "e.id, e.src_file_id, e.dst_file_id, e.dst_path, e.config_id, e.is_system, "
    "e.is_generated, e.count";

// "?, ?, ?" for an IN clause of n binds.
inline std::string in_placeholders(std::size_t n) {
  std::string s;
  for (std::size_t i = 0; i < n; ++i) {
    s += i == 0 ? "?" : ", ?";
  }
  return s;
}

} // namespace cidx::detail

namespace cidx {

// Defined in storage_entity_rollup.cpp. The v21 -> v22 upgrade path in
// storage.cpp calls it standalone, so it crosses a TU boundary now that the
// roll-up lives in its own translation unit.
void cpp_materialise_entity_nodes(SqliteDb &db);

} // namespace cidx
