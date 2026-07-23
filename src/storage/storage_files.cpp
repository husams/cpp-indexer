// Directory, file and diagnostic rows: the per-file tier.
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
#include "util/hashing.hpp"
#include "util/json_min.hpp"
#include "util/logger.hpp"
#include "util/pathutil.hpp"

namespace cidx {

using namespace detail;

int64_t Storage::add_directory(int64_t component_id, const std::string &path) {
  std::string p = path.empty() ? std::string(".") : pathutil::normpath(path);
  if (p == ".") {
    p = "";
  }
  auto st = db_.prepare(
      "INSERT INTO directory (component_id, path) VALUES (?, ?) "
      "ON CONFLICT(component_id, path) DO UPDATE SET path = excluded.path "
      "RETURNING id");
  st.bind(1, component_id);
  st.bind(2, std::string_view(p));
  if (!st.step()) {
    throw StorageError("directory upsert returned no id");
  }
  const int64_t did = st.col_int64(0);
  st.step_done();
  return did;
}

std::optional<Directory> Storage::get_directory(int64_t component_id,
                                                const std::string &path) {
  const std::string p =
      (path.empty() || path == ".") ? std::string() : pathutil::normpath(path);
  auto st = db_.prepare(std::string("SELECT ") + kDirectoryCols +
                        " FROM directory WHERE component_id = ? AND path = ?");
  st.bind(1, component_id);
  st.bind(2, std::string_view(p));
  if (!st.step()) {
    return std::nullopt;
  }
  return directory_from(st);
}

std::optional<Directory> Storage::get_directory_by_id(int64_t directory_id) {
  auto st = db_.prepare(std::string("SELECT ") + kDirectoryCols +
                        " FROM directory WHERE id = ?");
  st.bind(1, directory_id);
  if (!st.step()) {
    return std::nullopt;
  }
  return directory_from(st);
}

std::vector<std::pair<Directory, std::string>>
Storage::list_directories(const std::optional<int64_t> &component_id,
                          const std::optional<std::string> &name) {
  std::string sql =
      "SELECT d.id, d.component_id, d.path, c.name AS comp_name "
      "FROM directory d JOIN component c ON c.id = d.component_id";
  std::vector<std::string> where;
  std::vector<SqlValue> args;
  if (component_id) {
    where.emplace_back("d.component_id = ?");
    args.emplace_back(*component_id);
  }
  if (name && !name->empty()) {
    where.emplace_back("d.path LIKE ? ESCAPE '\\'");
    args.emplace_back(fuzzy_like(*name));
  }
  if (!where.empty()) {
    sql += " WHERE " + join_strings(where, " AND ");
  }
  sql += " ORDER BY c.name, d.path";
  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < args.size(); ++i) {
    st.bind(static_cast<int>(i + 1), args[i]);
  }
  std::vector<std::pair<Directory, std::string>> out;
  while (st.step()) {
    out.emplace_back(directory_from(st), st.col_text(3));
  }
  return out;
}

std::string Storage::dir_scope_sql(const std::string &dir_path,
                                   std::vector<SqlValue> &args) {
  std::string rel = pathutil::normpath(dir_path);
  if (rel == "." || rel.empty()) {
    rel = "";
  }
  const std::string esc = escape_like(rel);
  args.emplace_back(rel);
  // '' is the component root: its subtree is every directory.
  args.emplace_back(rel.empty() ? std::string("%") : esc + "/%");
  return "(d.path = ? OR d.path LIKE ? ESCAPE '\\')";
}

// -- files
// -------------------------------------------------------------------------

int64_t Storage::add_file(
    int64_t directory_id, const std::string &name,
    const std::optional<double> &mtime, const std::optional<std::string> &md5,
    const std::optional<std::vector<std::string>> &compile_options,
    const std::optional<std::string> &driver) {
  std::optional<std::string> opts;
  if (compile_options) {
    opts = json_min::encode_string_array(*compile_options);
  }
  bool config_changed = false;
  auto previous =
      db_.prepare("SELECT compile_options, driver, args_overridden FROM file "
                  "WHERE directory_id = ? AND name = ?");
  previous.bind(1, directory_id);
  previous.bind(2, std::string_view(name));
  if (previous.step() && previous.col_int64(2) == 0) {
    config_changed =
        (opts && (previous.col_is_null(0) || previous.col_text(0) != *opts)) ||
        (driver &&
         (previous.col_is_null(1) || previous.col_text(1) != *driver));
  }
  auto st = db_.prepare(
      "INSERT INTO file (directory_id, name, mtime, md5, "
      "compile_options, driver) "
      "VALUES (?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(directory_id, name) DO UPDATE SET "
      "  mtime           = COALESCE(excluded.mtime, file.mtime), "
      "  compile_options = CASE WHEN file.args_overridden = 1 "
      "                         THEN file.compile_options "
      "                         ELSE COALESCE(excluded.compile_options, "
      "file.compile_options) END, "
      "  driver          = CASE WHEN file.args_overridden = 1 "
      "                         THEN file.driver "
      "                         ELSE COALESCE(excluded.driver, "
      "file.driver) END, "
      "  indexed         = CASE WHEN (excluded.md5 IS NOT NULL "
      "                          AND excluded.md5 IS NOT file.md5) "
      "                         OR (file.args_overridden = 0 AND "
      "                             ((excluded.compile_options IS NOT NULL "
      "                               AND excluded.compile_options IS NOT "
      "file.compile_options) "
      "                              OR (excluded.driver IS NOT NULL "
      "                               AND excluded.driver IS NOT "
      "file.driver))) "
      "                         THEN 0 ELSE file.indexed END, "
      "  md5             = COALESCE(excluded.md5, file.md5) "
      "RETURNING id");
  st.bind(1, directory_id);
  st.bind(2, std::string_view(name));
  bind_opt(st, 3, mtime);
  bind_opt(st, 4, md5);
  bind_opt(st, 5, opts);
  bind_opt(st, 6, driver);
  if (!st.step()) {
    throw StorageError("file upsert returned no id");
  }
  const int64_t fid = st.col_int64(0);
  st.step_done();
  if (config_changed) {
    auto stale =
        db_.prepare("UPDATE file_config SET state = 'stale', "
                    "reason = 'translation-unit configuration changed' "
                    "WHERE file_id = ? AND role = 'translation_unit'");
    stale.bind(1, fid);
    stale.step_done();
    auto tu = db_.prepare(
        "UPDATE translation_unit SET state = 'stale' WHERE file_id = ?");
    tu.bind(1, fid);
    tu.step_done();
  }
  return fid;
}

std::optional<std::tuple<int64_t, std::string, std::string>>
Storage::split_path(const std::string &abs_path) {
  const std::string abs = pathutil::abspath(abs_path);
  const auto comp = component_for_path(abs);
  if (!comp) {
    return std::nullopt;
  }
  // Use the resolved effective root (base+version, clone-anchored when grouped)
  // for relpath computation (contract §4.4 item 2).
  const std::string root = component_abs_base(*comp);
  const std::string rel = pathutil::relpath(abs, root);
  auto [rel_dir, name] = pathutil::split(rel);
  if (rel_dir == ".") {
    rel_dir = "";
  }
  return std::make_tuple(comp->id, rel_dir, name);
}

int64_t Storage::add_file_path(
    const std::string &abs_path, const std::optional<double> &mtime,
    const std::optional<std::string> &md5,
    const std::optional<std::vector<std::string>> &compile_options,
    const std::optional<std::string> &driver) {
  const auto sp = split_path(abs_path);
  if (!sp) {
    throw StorageError("no component owns " + pathutil::abspath(abs_path) +
                       " (add_component first)");
  }
  const auto &[comp_id, rel_dir, name] = *sp;
  const int64_t dir_id = add_directory(comp_id, rel_dir);
  return add_file(dir_id, name, mtime, md5, compile_options, driver);
}

std::optional<File> Storage::get_file(const std::string &abs_path) {
  const auto sp = split_path(abs_path);
  if (!sp) {
    return std::nullopt;
  }
  const auto &[comp_id, rel_dir, name] = *sp;
  auto st = db_.prepare(
      "SELECT f.id, f.directory_id, f.name, f.mtime, f.md5, f.compile_options, "
      "f.driver, f.indexed, f.indexed_at, f.args_overridden "
      "FROM file f JOIN directory d ON d.id = f.directory_id "
      "WHERE d.component_id = ? AND d.path = ? AND f.name = ?");
  st.bind(1, comp_id);
  st.bind(2, std::string_view(rel_dir));
  st.bind(3, std::string_view(name));
  if (!st.step()) {
    return std::nullopt;
  }
  return file_from(st);
}

std::optional<File> Storage::get_file_by_id(int64_t file_id) {
  auto st = db_.prepare(std::string("SELECT ") + kFileCols +
                        " FROM file WHERE id = ?");
  st.bind(1, file_id);
  if (!st.step()) {
    return std::nullopt;
  }
  return file_from(st);
}

std::optional<std::string> Storage::file_abs_path(int64_t file_id) {
  // SELECT c.path, c.version, c.repository_id so we can build the effective
  // root (clone-anchored when grouped).
  auto st =
      db_.prepare("SELECT c.path, c.version, c.repository_id, d.path AS rel, "
                  "f.name AS name "
                  "FROM file f JOIN directory d ON d.id = f.directory_id "
                  "JOIN component c ON c.id = d.component_id WHERE f.id = ?");
  st.bind(1, file_id);
  if (!st.step()) {
    return std::nullopt;
  }
  // Build effective root using stored path + version (contract §4.4 item 3).
  Component tmp;
  tmp.path = st.col_text(0);
  tmp.version = opt_text(st, 1);
  tmp.repository_id = opt_int64(st, 2);
  const std::string root = component_abs_base(tmp);
  return reconstruct_path(root, st.col_text(3), st.col_text(4));
}

std::optional<std::string> Storage::directory_abs_path(int64_t directory_id) {
  auto st =
      db_.prepare("SELECT c.path, c.version, c.repository_id, d.path AS rel "
                  "FROM directory d "
                  "JOIN component c ON c.id = d.component_id WHERE d.id = ?");
  st.bind(1, directory_id);
  if (!st.step()) {
    return std::nullopt;
  }
  Component tmp;
  tmp.path = st.col_text(0);
  tmp.version = opt_text(st, 1);
  tmp.repository_id = opt_int64(st, 2);
  const std::string root = component_abs_base(tmp);
  const std::string rel = st.col_text(3);
  return rel.empty() ? root : pathutil::join(root, rel);
}

std::vector<std::pair<File, std::string>>
Storage::list_files(const std::optional<int64_t> &component_id,
                    const std::optional<std::string> &dir_path,
                    const std::optional<std::string> &name,
                    const std::optional<bool> &indexed) {
  std::string sql =
      "SELECT f.id, f.directory_id, f.name, f.mtime, f.md5, f.compile_options, "
      "f.driver, f.indexed, f.indexed_at, f.args_overridden, "
      "c.path AS root, c.version, c.repository_id, d.path AS rel "
      "FROM file f JOIN directory d ON d.id = f.directory_id "
      "JOIN component c ON c.id = d.component_id";
  std::vector<std::string> where;
  std::vector<SqlValue> args;
  if (component_id) {
    where.emplace_back("d.component_id = ?");
    args.emplace_back(*component_id);
  }
  if (dir_path) {
    where.push_back(dir_scope_sql(*dir_path, args));
  }
  if (name && !name->empty()) {
    where.emplace_back("f.name LIKE ? ESCAPE '\\'");
    args.emplace_back(fuzzy_like(*name));
  }
  if (indexed) {
    where.emplace_back("f.indexed = ?");
    args.emplace_back(static_cast<int64_t>(*indexed ? 1 : 0));
  }
  if (!where.empty()) {
    sql += " WHERE " + join_strings(where, " AND ");
  }
  sql += " ORDER BY c.path, d.path, f.name";
  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < args.size(); ++i) {
    st.bind(static_cast<int>(i + 1), args[i]);
  }
  std::vector<std::pair<File, std::string>> out;
  while (st.step()) {
    // Columns 10=c.path, 11=c.version, 12=c.repository_id, 13=d.path (rel);
    // f.name is col 2.
    Component tmp;
    tmp.path = st.col_text(10);
    tmp.version = opt_text(st, 11);
    tmp.repository_id = opt_int64(st, 12);
    const std::string root = component_abs_base(tmp);
    out.emplace_back(file_from(st),
                     reconstruct_path(root, st.col_text(13), st.col_text(2)));
  }
  return out;
}

void Storage::mark_file_indexed(int64_t file_id,
                                const std::optional<double> &mtime) {
  const auto path = file_abs_path(file_id);
  const auto md5 = path ? md5_of(*path) : std::nullopt;
  auto st =
      db_.prepare("UPDATE file SET indexed = 1, indexed_at = datetime('now'), "
                  "  mtime = COALESCE(?, mtime), md5 = COALESCE(?, md5) "
                  "WHERE id = ?");
  bind_opt(st, 1, mtime);
  bind_opt(st, 2, md5);
  st.bind(3, file_id);
  st.step_done();
}

void Storage::set_file_indexed(int64_t file_id, bool indexed) {
  // Flip the indexed/pending flag in place; symbols are untouched. Setting
  // indexed=0 marks the file pending so the next `index` re-parses it
  // (regenerating graph edges) without losing its existing symbols.
  auto st = db_.prepare("UPDATE file SET indexed = ? WHERE id = ?");
  st.bind(1, static_cast<int64_t>(indexed ? 1 : 0));
  st.bind(2, file_id);
  st.step_done();
}

void Storage::set_file_compile_options(int64_t file_id,
                                       const std::vector<std::string> &options,
                                       const std::optional<std::string> &driver,
                                       bool update_driver) {
  // Replace a file's stored flags (and optionally its driver) and mark it
  // args_overridden=1 so a later `import` (without --force) keeps the edit.
  const std::string opts = json_min::encode_string_array(options);
  if (update_driver) {
    auto st = db_.prepare("UPDATE file SET compile_options = ?, driver = ?, "
                          "args_overridden = 1 WHERE id = ?");
    st.bind(1, std::string_view(opts));
    bind_opt(st, 2, driver);
    st.bind(3, file_id);
    st.step_done();
  } else {
    auto st = db_.prepare("UPDATE file SET compile_options = ?, "
                          "args_overridden = 1 WHERE id = ?");
    st.bind(1, std::string_view(opts));
    st.bind(2, file_id);
    st.step_done();
  }
}

void Storage::update_file_compile_options(
    int64_t file_id, const std::vector<std::string> &options) {
  // UPDATE compile_options WITHOUT setting args_overridden (realias semantics).
  // Port of storage.py update_file_compile_options.
  const std::string opts = json_min::encode_string_array(options);
  auto st = db_.prepare("UPDATE file SET compile_options = ? WHERE id = ?");
  st.bind(1, std::string_view(opts));
  st.bind(2, file_id);
  st.step_done();
}

bool Storage::is_file_indexed(const std::string &abs_path,
                              const std::optional<double> &mtime,
                              const std::optional<std::string> &md5) {
  const auto f = get_file(abs_path);
  if (!f || !f->indexed) {
    return false;
  }
  if (mtime && (!f->mtime || *f->mtime < *mtime)) {
    return false;
  }
  if (md5 && f->md5 != md5) {
    return false;
  }
  return true;
}

// -- diagnostics (v15)
// ----------------------------------------------------------------------

void Storage::replace_diagnostics(int64_t file_id,
                                  const std::vector<Diagnostic> &diags) {
  // Wholesale refresh: drop the file's stale rows, then insert in TU order so
  // ids follow the diagnostic sequence (parity with storage.py).
  {
    auto del = db_.prepare("DELETE FROM diagnostic WHERE file_id = ?");
    del.bind(1, file_id);
    del.step_done();
  }
  for (const Diagnostic &d : diags) {
    auto st = db_.prepare("INSERT INTO diagnostic "
                          "(file_id, severity, spelling, file_path, line, col) "
                          "VALUES (?, ?, ?, ?, ?, ?)");
    st.bind(1, file_id);
    st.bind(2, static_cast<int64_t>(d.severity));
    st.bind(3, std::string_view(d.spelling));
    bind_opt(st, 4, d.file_path);
    bind_opt(st, 5, d.line);
    bind_opt(st, 6, d.col);
    st.step_done();
  }
}

std::vector<Diagnostic> Storage::get_diagnostics(int64_t file_id) {
  auto st = db_.prepare(
      "SELECT id, file_id, severity, spelling, file_path, line, col "
      "FROM diagnostic WHERE file_id = ? ORDER BY id");
  st.bind(1, file_id);
  std::vector<Diagnostic> out;
  while (st.step()) {
    Diagnostic d;
    d.id = st.col_int64(0);
    d.file_id = st.col_int64(1);
    d.severity = static_cast<int>(st.col_int64(2));
    d.spelling = st.col_text(3);
    d.file_path = opt_text(st, 4);
    d.line = opt_int64(st, 5);
    d.col = opt_int64(st, 6);
    out.push_back(std::move(d));
  }
  return out;
}

std::map<int64_t, std::map<int, int64_t>> Storage::diagnostic_counts() {
  auto st = db_.prepare("SELECT file_id, severity, COUNT(*) FROM diagnostic "
                        "GROUP BY file_id, severity");
  std::map<int64_t, std::map<int, int64_t>> out;
  while (st.step()) {
    const int64_t fid = st.col_int64(0);
    const int sev = static_cast<int>(st.col_int64(1));
    out[fid][sev] = st.col_int64(2);
  }
  return out;
}

// -- symbols
// ----------------------------------------------------------------------

} // namespace cidx
