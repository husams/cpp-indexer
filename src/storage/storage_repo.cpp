// Repository, clone and component rows: the corpus-identity tier.
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

int64_t Storage::add_semantic_universe(const std::string &key,
                                       const std::string &name,
                                       const std::string &policy) {
  if (key.empty()) {
    throw StorageError("semantic universe key must not be empty");
  }
  auto st = db_.prepare(
      "INSERT INTO semantic_universe (key, name, policy) VALUES (?, ?, ?) "
      "ON CONFLICT(key) DO UPDATE SET name = excluded.name, "
      "policy = excluded.policy RETURNING id");
  st.bind(1, std::string_view(key));
  st.bind(2, std::string_view(name.empty() ? key : name));
  st.bind(3, std::string_view(policy));
  if (!st.step()) {
    throw StorageError("semantic universe insert returned no id");
  }
  const int64_t id = st.col_int64(0);
  st.step_done();
  return id;
}

std::optional<SemanticUniverse>
Storage::get_semantic_universe_by_id(int64_t universe_id) {
  auto st = db_.prepare("SELECT id, key, name, policy FROM semantic_universe "
                        "WHERE id = ?");
  st.bind(1, universe_id);
  if (!st.step()) {
    return std::nullopt;
  }
  SemanticUniverse u;
  u.id = st.col_int64(0);
  u.key = st.col_text(1);
  u.name = st.col_text(2);
  u.policy = st.col_text(3);
  return u;
}

std::optional<SemanticUniverse>
Storage::get_semantic_universe_by_key(const std::string &key) {
  auto st = db_.prepare("SELECT id, key, name, policy FROM semantic_universe "
                        "WHERE key = ?");
  st.bind(1, std::string_view(key));
  if (!st.step()) {
    return std::nullopt;
  }
  SemanticUniverse u;
  u.id = st.col_int64(0);
  u.key = st.col_text(1);
  u.name = st.col_text(2);
  u.policy = st.col_text(3);
  return u;
}

std::vector<SemanticUniverse> Storage::list_semantic_universes() {
  auto st = db_.prepare("SELECT id, key, name, policy FROM semantic_universe "
                        "ORDER BY key");
  std::vector<SemanticUniverse> out;
  while (st.step()) {
    SemanticUniverse u;
    u.id = st.col_int64(0);
    u.key = st.col_text(1);
    u.name = st.col_text(2);
    u.policy = st.col_text(3);
    out.push_back(std::move(u));
  }
  return out;
}

void Storage::set_repository_semantic_universe(
    int64_t repository_id, const std::optional<int64_t> &universe_id) {
  auto st = db_.prepare(
      "UPDATE repository SET semantic_universe_id = COALESCE(?, 1) "
      "WHERE id = ?");
  bind_opt(st, 1, universe_id);
  st.bind(2, repository_id);
  st.step_done();
}

void Storage::set_component_semantic_universe(
    int64_t component_id, const std::optional<int64_t> &universe_id) {
  auto st = db_.prepare(
      "UPDATE component SET semantic_universe_id = ? WHERE id = ?");
  bind_opt(st, 1, universe_id);
  st.bind(2, component_id);
  st.step_done();
}

int64_t Storage::default_semantic_universe_id() {
  auto st = db_.prepare(
      "SELECT id FROM semantic_universe WHERE key = 'legacy'");
  if (st.step()) {
    return st.col_int64(0);
  }
  return add_semantic_universe("legacy", "Legacy single-workspace universe",
                              "legacy");
}

int64_t Storage::semantic_universe_for_file(
    const std::optional<int64_t> &file_id) {
  if (!file_id) {
    return default_semantic_universe_id();
  }
  auto st = db_.prepare(
      "SELECT COALESCE(c.semantic_universe_id, r.semantic_universe_id, ?) "
      "FROM file f JOIN directory d ON d.id = f.directory_id "
      "JOIN component c ON c.id = d.component_id "
      "LEFT JOIN repository r ON r.id = c.repository_id WHERE f.id = ?");
  st.bind(1, default_semantic_universe_id());
  st.bind(2, *file_id);
  if (st.step() && !st.col_is_null(0)) {
    return st.col_int64(0);
  }
  return default_semantic_universe_id();
}

std::string Storage::symbol_identity_key(
    const Symbol &sym, int64_t universe_id,
    const std::optional<int64_t> &file_id) {
  const auto universe = get_semantic_universe_by_id(universe_id);
  const std::string key = universe ? universe->key : "legacy";
  const bool local = sym.linkage &&
                     (*sym.linkage == "internal" ||
                      *sym.linkage == "no-linkage");
  std::string out = key + '\x1f';
  if (local) {
    out += "file:" + std::to_string(file_id.value_or(0)) + '\x1f';
  }
  out += sym.usr;
  return out;
}

int64_t Storage::add_component(const std::string &name, const std::string &path,
                               const std::string &kind,
                               const std::optional<std::string> &version) {
  // Preserve indirected (portable) paths verbatim; absolutize plain paths.
  // Mirrors Python: if "$" not in path and "<" not in path: path = abspath(path)
  const std::string abs = (!path.contains('$') && !path.contains('<'))
                              ? pathutil::abspath(path)
                              : path;
  // v24: `path` is no longer globally UNIQUE (it is UNIQUE per repository, so
  // grouped components can share a '.' root), so the dedup is done here in code
  // rather than via ON CONFLICT(path). Idempotent on the exact stored path
  // string: an existing row has name/kind updated (version COALESCE-preserved
  // when none supplied). Low-level primitive -- callers pass the ABSOLUTE base
  // before grouping; re-resolving an already-relativized component is the
  // caller's job (get_component / component_for_path are clone-aware). Mirrors
  // Python add_component.
  {
    auto sel = db_.prepare("SELECT id FROM component WHERE path = ?");
    sel.bind(1, std::string_view(abs));
    if (sel.step()) {
      const int64_t cid = sel.col_int64(0);
      auto upd = db_.prepare("UPDATE component SET name = ?, kind = ?, "
                             "version = COALESCE(?, version) WHERE id = ?");
      upd.bind(1, std::string_view(name));
      upd.bind(2, std::string_view(kind));
      bind_opt(upd, 3, version);
      upd.bind(4, cid);
      upd.step_done();
      return cid;
    }
  }
  auto st = db_.prepare("INSERT INTO component (name, path, kind, version) "
                        "VALUES (?, ?, ?, ?) RETURNING id");
  st.bind(1, std::string_view(name));
  st.bind(2, std::string_view(abs));
  st.bind(3, std::string_view(kind));
  bind_opt(st, 4, version);
  if (!st.step()) {
    throw StorageError("component insert returned no id");
  }
  const int64_t cid = st.col_int64(0);
  st.step_done();
  return cid;
}

void Storage::update_component_meta(int64_t component_id,
                                    const std::string &name,
                                    const std::string &kind,
                                    const std::optional<std::string> &version) {
  // Refresh an EXISTING (already-grouped, possibly clone-relative) component's
  // name/kind in place without touching its stored path; version COALESCE-kept.
  // Mirrors Python Storage.update_component_meta.
  auto st = db_.prepare("UPDATE component SET name = ?, kind = ?, "
                        "version = COALESCE(?, version) WHERE id = ?");
  st.bind(1, std::string_view(name));
  st.bind(2, std::string_view(kind));
  bind_opt(st, 3, version);
  st.bind(4, component_id);
  st.step_done();
}

bool Storage::set_component_version(const std::string &name,
                                    const std::optional<std::string> &version) {
  // Explicit clear goes through this path (not add_component) to avoid the
  // COALESCE guard that would no-op a NULL-clear.
  auto st = db_.prepare(
      "UPDATE component SET version = ? WHERE name = ?");
  bind_opt(st, 1, version);
  st.bind(2, std::string_view(name));
  st.step_done();
  return db_.changes() > 0;
}

bool Storage::set_component_effective_version(const std::string &name,
                                              const std::string &version) {
  // Mirrors Python Storage.set_component_effective_version: only act when the
  // name resolves to exactly one row; pick property-vs-embedded by splitting
  // the STORED path (so portable <label>/$VAR prefixes survive the rewrite).
  std::vector<Component> rows;
  for (const auto &c : list_components()) {
    if (c.name == name) {
      rows.push_back(c);
    }
  }
  if (rows.size() != 1) {
    return false;
  }
  const Component &comp = rows.front();
  const auto [base, seg] = CompileDb::split_base_version(comp.path);
  if (!seg.empty()) {
    // version embedded in the path: swap the trailing segment.
    std::string new_path = pathutil::normpath(pathutil::join(base, version));
    if (!new_path.contains('$') && !new_path.contains('<')) {
      new_path = pathutil::abspath(new_path);
    }
    auto st = db_.prepare(
        "UPDATE component SET path = ?, version = NULL WHERE id = ?");
    st.bind(1, std::string_view(new_path));
    st.bind(2, comp.id);
    st.step_done();
  } else {
    auto st = db_.prepare("UPDATE component SET version = ? WHERE id = ?");
    st.bind(1, std::string_view(version));
    st.bind(2, comp.id);
    st.step_done();
  }
  return true;
}

// static
std::string Storage::effective_root(const Component &comp) {
  // Stored effective root (NOT resolved; may contain $VAR).
  if (!comp.version || comp.version->empty()) {
    return comp.path;
  }
  return pathutil::normpath(pathutil::join(comp.path, *comp.version));
}

// Resolved absolute path of a repository's active clone, or nullopt when the
// component is ungrouped / the repository has no live clone. Mirrors Python
// Storage._active_clone_root.
std::optional<std::string>
Storage::active_clone_root(const std::optional<int64_t> &repository_id) {
  if (!repository_id) {
    return std::nullopt;
  }
  const auto repo = get_repository_by_id(*repository_id);
  if (!repo || !repo->active_clone_id) {
    return std::nullopt;
  }
  const auto clone = get_clone_by_id(*repo->active_clone_id);
  if (!clone) {
    return std::nullopt;
  }
  return pathutil::abspath(pathutil::resolve_fs_path(clone->path));
}

std::string Storage::component_abs_base(const Component &comp) {
  const std::string eff = effective_root(comp);
  if (comp.repository_id && !pathutil::isabs(comp.path) &&
      !comp.path.contains('<') && !comp.path.contains('$')) {
    const auto root = active_clone_root(comp.repository_id);
    if (root) {
      return pathutil::abspath(pathutil::join(*root, eff));
    }
  }
  return pathutil::abspath(pathutil::resolve_fs_path(eff));
}

void Storage::relativize_component(int64_t component_id,
                                   const std::string &clone_root) {
  const auto comp = get_component_by_id(component_id);
  if (!comp) {
    return;
  }
  if (comp->path.contains('<') || comp->path.contains('$') ||
      !pathutil::isabs(comp->path)) {
    return;
  }
  if (!CompileDb::split_base_version(comp->path).second.empty()) {
    // version-in-path representation: the version segment is part of the
    // absolute path identity (set_component_effective_version rewrites it in
    // place), so keep it absolute -- relativizing would drop it.
    return;
  }
  std::string root = pathutil::abspath(pathutil::resolve_fs_path(clone_root));
  while (!root.empty() && root.back() == '/') {
    root.pop_back();
  }
  std::string base = pathutil::abspath(comp->path);
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  std::string rel;
  if (base == root) {
    rel = ".";
  } else if (base.starts_with(root + "/")) {
    rel = pathutil::relpath(base, root);
  } else {
    return; // component lives outside this clone -> keep it absolute
  }
  auto upd = db_.prepare("UPDATE component SET path = ? WHERE id = ?");
  upd.bind(1, std::string_view(rel));
  upd.bind(2, component_id);
  upd.step_done();
}

std::optional<Component> Storage::get_component(const std::string &path) {
  const std::string abs = pathutil::abspath(path);
  // Step 1: exact match on stored BASE path (fast-path for unversioned comps).
  {
    auto st = db_.prepare(std::string("SELECT ") + kComponentCols +
                          " FROM component WHERE path = ?");
    st.bind(1, std::string_view(abs));
    if (st.step()) {
      return component_from(st);
    }
  }
  // Step 2: scan all components and match against effective root (required
  // when version-detection split the trailing segment off the stored base).
  {
    auto st =
        db_.prepare(std::string("SELECT ") + kComponentCols + " FROM component");
    while (st.step()) {
      Component c = component_from(st);
      const std::string root = component_abs_base(c);
      if (root == abs) {
        return c;
      }
    }
  }
  return std::nullopt;
}

std::optional<Component>
Storage::get_component_by_name(const std::string &name) {
  auto st = db_.prepare(std::string("SELECT ") + kComponentCols +
                        " FROM component WHERE name = ?");
  st.bind(1, std::string_view(name));
  if (!st.step()) {
    return std::nullopt;
  }
  return component_from(st);
}

std::optional<Component> Storage::get_component_by_id(int64_t component_id) {
  auto st = db_.prepare(std::string("SELECT ") + kComponentCols +
                        " FROM component WHERE id = ?");
  st.bind(1, component_id);
  if (!st.step()) {
    return std::nullopt;
  }
  return component_from(st);
}

std::optional<Component>
Storage::component_for_path(const std::string &abs_path) {
  const std::string abs = pathutil::abspath(abs_path);
  std::optional<Component> best;
  std::string best_root;
  auto st =
      db_.prepare(std::string("SELECT ") + kComponentCols + " FROM component");
  while (st.step()) {
    Component c = component_from(st);
    // Use the resolved effective root (base+version, clone-anchored when
    // grouped) for prefix matching (contract §4.4 item 1 / §2 hazard). Strip
    // trailing slashes.
    std::string root = component_abs_base(c);
    while (!root.empty() && root.back() == '/') {
      root.pop_back(); // rstrip(os.sep)
    }
    const bool owns = abs == root || abs.starts_with(root + "/");
    if (owns && (!best || root.size() > best_root.size())) {
      best = std::move(c);
      best_root = root;
    }
  }
  return best;
}

void Storage::delete_component(int64_t component_id) {
  // Symbols reference files with ON DELETE SET NULL, so remove them before the
  // cascade nulls their file ids -- otherwise they linger as file-less orphans.
  // Directories and files then vanish via the component's ON DELETE CASCADE.
  static const char kSub[] =
      "SELECT f.id FROM file f JOIN directory d ON f.directory_id = d.id "
      "WHERE d.component_id = ?";
  auto del_sym =
      db_.prepare(std::string("DELETE FROM symbol WHERE file_id IN (") + kSub +
                  ") OR decl_file_id IN (" + kSub + ")");
  del_sym.bind(1, component_id);
  del_sym.bind(2, component_id);
  del_sym.step_done();
  auto del_comp = db_.prepare("DELETE FROM component WHERE id = ?");
  del_comp.bind(1, component_id);
  del_comp.step_done();
}

void Storage::delete_directory(int64_t directory_id) {
  // Files cascade on directory delete; symbols (ON DELETE SET NULL) would
  // linger file-less, so delete them first.
  static const char kSub[] = "SELECT id FROM file WHERE directory_id = ?";
  auto del_sym =
      db_.prepare(std::string("DELETE FROM symbol WHERE file_id IN (") + kSub +
                  ") OR decl_file_id IN (" + kSub + ")");
  del_sym.bind(1, directory_id);
  del_sym.bind(2, directory_id);
  del_sym.step_done();
  auto del_dir = db_.prepare("DELETE FROM directory WHERE id = ?");
  del_dir.bind(1, directory_id);
  del_dir.step_done();
}

void Storage::delete_file(int64_t file_id) {
  // Symbols reference files with ON DELETE SET NULL; delete them first so they
  // do not linger file-less.
  auto del_sym =
      db_.prepare("DELETE FROM symbol WHERE file_id = ? OR decl_file_id = ?");
  del_sym.bind(1, file_id);
  del_sym.bind(2, file_id);
  del_sym.step_done();
  auto del_file = db_.prepare("DELETE FROM file WHERE id = ?");
  del_file.bind(1, file_id);
  del_file.step_done();
}

void Storage::delete_symbol(int64_t symbol_id) {
  auto del = db_.prepare("DELETE FROM symbol WHERE id = ?");
  del.bind(1, symbol_id);
  del.step_done();
}

std::vector<Component>
Storage::list_components(const std::optional<std::string> &name,
                         const std::optional<std::string> &kind) {
  std::string sql = std::string("SELECT ") + kComponentCols + " FROM component";
  std::vector<std::string> where;
  std::vector<SqlValue> args;
  if (name && !name->empty()) {
    where.emplace_back("name LIKE ? ESCAPE '\\'");
    args.emplace_back(fuzzy_like(*name));
  }
  if (kind) {
    where.emplace_back("kind = ?");
    args.emplace_back(*kind);
  }
  if (!where.empty()) {
    sql += " WHERE " + join_strings(where, " AND ");
  }
  sql += " ORDER BY name, path";
  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < args.size(); ++i) {
    st.bind(static_cast<int>(i + 1), args[i]);
  }
  std::vector<Component> out;
  while (st.step()) {
    out.push_back(component_from(st));
  }
  return out;
}

void Storage::set_component_repository(
    int64_t component_id, const std::optional<int64_t> &repository_id) {
  auto st = db_.prepare("UPDATE component SET repository_id = ? WHERE id = ?");
  if (repository_id) {
    st.bind(1, *repository_id);
  } else {
    st.bind_null(1);
  }
  st.bind(2, component_id);
  st.step_done();
}

std::vector<Component>
Storage::components_for_repository(int64_t repository_id) {
  auto st = db_.prepare(std::string("SELECT ") + kComponentCols +
                        " FROM component WHERE repository_id = ? "
                        "ORDER BY name, path");
  st.bind(1, repository_id);
  std::vector<Component> out;
  while (st.step()) {
    out.push_back(component_from(st));
  }
  return out;
}

// -- repositories / clones (v23)
// -----------------------------------------------------------------

int64_t Storage::add_repository(const std::string &name,
                                const std::string &kind,
                                const std::optional<std::string> &remote_url,
                                const std::optional<int64_t> &universe_id) {
  auto st = db_.prepare(
      "INSERT INTO repository (name, kind, remote_url, semantic_universe_id) "
      "VALUES (?, ?, ?, COALESCE(?, 1)) "
      "ON CONFLICT(name) DO UPDATE SET kind = excluded.kind, "
      "remote_url = COALESCE(excluded.remote_url, repository.remote_url), "
      "semantic_universe_id = COALESCE(excluded.semantic_universe_id, "
      "repository.semantic_universe_id) "
      "RETURNING id");
  st.bind(1, std::string_view(name));
  st.bind(2, std::string_view(kind));
  bind_opt(st, 3, remote_url);
  bind_opt(st, 4, universe_id);
  if (!st.step()) {
    throw StorageError("repository upsert returned no id");
  }
  const int64_t rid = st.col_int64(0);
  st.step_done();
  return rid;
}

std::optional<Repository>
Storage::get_repository_by_name(const std::string &name) {
  auto st = db_.prepare(std::string("SELECT ") + kRepositoryCols +
                        " FROM repository WHERE name = ?");
  st.bind(1, std::string_view(name));
  if (!st.step()) {
    return std::nullopt;
  }
  return repository_from(st);
}

std::optional<Repository> Storage::get_repository_by_id(int64_t repository_id) {
  auto st = db_.prepare(std::string("SELECT ") + kRepositoryCols +
                        " FROM repository WHERE id = ?");
  st.bind(1, repository_id);
  if (!st.step()) {
    return std::nullopt;
  }
  return repository_from(st);
}

std::optional<Repository>
Storage::get_repository_by_remote(const std::string &remote_url) {
  auto st = db_.prepare(std::string("SELECT ") + kRepositoryCols +
                        " FROM repository WHERE remote_url = ? "
                        "ORDER BY id LIMIT 1");
  st.bind(1, std::string_view(remote_url));
  if (!st.step()) {
    return std::nullopt;
  }
  return repository_from(st);
}

std::vector<Repository>
Storage::list_repositories(const std::optional<std::string> &name,
                           const std::optional<std::string> &kind) {
  std::string sql =
      std::string("SELECT ") + kRepositoryCols + " FROM repository";
  std::vector<std::string> where;
  std::vector<SqlValue> args;
  if (name && !name->empty()) {
    where.emplace_back("name LIKE ? ESCAPE '\\'");
    args.emplace_back(fuzzy_like(*name));
  }
  if (kind) {
    where.emplace_back("kind = ?");
    args.emplace_back(*kind);
  }
  if (!where.empty()) {
    sql += " WHERE " + join_strings(where, " AND ");
  }
  sql += " ORDER BY name";
  auto st = db_.prepare(sql);
  for (std::size_t i = 0; i < args.size(); ++i) {
    st.bind(static_cast<int>(i + 1), args[i]);
  }
  std::vector<Repository> out;
  while (st.step()) {
    out.push_back(repository_from(st));
  }
  return out;
}

void Storage::set_active_clone(int64_t repository_id,
                               const std::optional<int64_t> &clone_id) {
  auto st =
      db_.prepare("UPDATE repository SET active_clone_id = ? WHERE id = ?");
  if (clone_id) {
    st.bind(1, *clone_id);
  } else {
    st.bind_null(1);
  }
  st.bind(2, repository_id);
  st.step_done();
}

void Storage::delete_repository(int64_t repository_id) {
  auto st = db_.prepare("DELETE FROM repository WHERE id = ?");
  st.bind(1, repository_id);
  st.step_done();
}

int64_t Storage::add_clone(int64_t repository_id, const std::string &path,
                           const std::optional<std::string> &label) {
  // Mirror Python: absolutize plain paths; preserve portable ($/<) verbatim.
  const std::string abs = (!path.contains('$') && !path.contains('<'))
                              ? pathutil::abspath(path)
                              : path;
  auto st = db_.prepare(
      "INSERT INTO clone (repository_id, path, label) VALUES (?, ?, ?) "
      "ON CONFLICT(path) DO UPDATE SET repository_id = excluded.repository_id, "
      "label = COALESCE(excluded.label, clone.label) RETURNING id");
  st.bind(1, repository_id);
  st.bind(2, std::string_view(abs));
  bind_opt(st, 3, label);
  if (!st.step()) {
    throw StorageError("clone upsert returned no id");
  }
  const int64_t cid = st.col_int64(0);
  st.step_done();
  return cid;
}

std::optional<Clone> Storage::get_clone_by_id(int64_t clone_id) {
  auto st = db_.prepare(std::string("SELECT ") + kCloneCols +
                        " FROM clone WHERE id = ?");
  st.bind(1, clone_id);
  if (!st.step()) {
    return std::nullopt;
  }
  return clone_from(st);
}

std::optional<Clone> Storage::get_clone_by_path(const std::string &path) {
  const std::string abs = (!path.contains('$') && !path.contains('<'))
                              ? pathutil::abspath(path)
                              : path;
  auto st = db_.prepare(std::string("SELECT ") + kCloneCols +
                        " FROM clone WHERE path = ?");
  st.bind(1, std::string_view(abs));
  if (!st.step()) {
    return std::nullopt;
  }
  return clone_from(st);
}

std::vector<Clone>
Storage::list_clones(const std::optional<int64_t> &repository_id) {
  std::string sql = std::string("SELECT ") + kCloneCols + " FROM clone";
  if (repository_id) {
    sql += " WHERE repository_id = ?";
  }
  sql += " ORDER BY id";
  auto st = db_.prepare(sql);
  if (repository_id) {
    st.bind(1, *repository_id);
  }
  std::vector<Clone> out;
  while (st.step()) {
    out.push_back(clone_from(st));
  }
  return out;
}

void Storage::delete_clone(int64_t clone_id) {
  auto clr = db_.prepare("UPDATE repository SET active_clone_id = NULL "
                         "WHERE active_clone_id = ?");
  clr.bind(1, clone_id);
  clr.step_done();
  auto del = db_.prepare("DELETE FROM clone WHERE id = ?");
  del.bind(1, clone_id);
  del.step_done();
}


// -- directories
// -----------------------------------------------------------------

} // namespace cidx
