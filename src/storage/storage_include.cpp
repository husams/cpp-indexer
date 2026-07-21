// The include tier: configs, include edges, sites and macro uses.
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


int64_t Storage::add_include_config(const IncludeConfig &c) {
  auto st = db_.prepare(
      "INSERT INTO include_config (tu_file_id, digest, driver, working_dir, "
      "                            arguments, lang_mode, resource_dir) "
      "VALUES (?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(tu_file_id, digest) DO UPDATE SET "
      "  driver       = excluded.driver, "
      "  working_dir  = excluded.working_dir, "
      "  arguments    = excluded.arguments, "
      "  lang_mode    = excluded.lang_mode, "
      "  resource_dir = excluded.resource_dir "
      "RETURNING id");
  st.bind(1, c.tu_file_id);
  st.bind(2, std::string_view(c.digest));
  bind_opt(st, 3, c.driver);
  bind_opt(st, 4, c.working_dir);
  st.bind(5, std::string_view(json_min::encode_string_array(c.arguments)));
  bind_opt(st, 6, c.lang_mode);
  bind_opt(st, 7, c.resource_dir);
  if (!st.step()) {
    throw StorageError("add_include_config: upsert returned no id");
  }
  const int64_t id = st.col_int64(0);
  st.step_done();
  return id;
}

std::optional<IncludeConfig> Storage::include_config_by_id(int64_t config_id) {
  auto st = db_.prepare(std::string("SELECT ") + kIncludeConfigCols +
                        " FROM include_config WHERE id = ?");
  st.bind(1, config_id);
  if (!st.step()) {
    return std::nullopt;
  }
  return include_config_from(st);
}

std::vector<IncludeConfig> Storage::include_configs_for_tu(int64_t tu_file_id) {
  auto st = db_.prepare(std::string("SELECT ") + kIncludeConfigCols +
                        " FROM include_config WHERE tu_file_id = ? "
                        "ORDER BY digest");
  st.bind(1, tu_file_id);
  std::vector<IncludeConfig> out;
  while (st.step()) {
    out.push_back(include_config_from(st));
  }
  return out;
}

int64_t Storage::add_include_edge(const IncludeEdge &e) {
  // count ACCUMULATES: the same header included twice in one file under one
  // configuration is two occurrences of one collapsed edge (each gets its own
  // include_site row). dst_file_id COALESCEs so a later caller that has
  // resolved the target can fill it in, but a known id is never nulled.
  auto st = db_.prepare(
      "INSERT INTO include_edge (src_file_id, dst_file_id, dst_path, "
      "                          config_id, is_system, is_generated, count) "
      "VALUES (?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(src_file_id, dst_path, config_id) DO UPDATE SET "
      "  count       = include_edge.count + excluded.count, "
      "  dst_file_id = COALESCE(excluded.dst_file_id, include_edge.dst_file_id), "
      "  is_system   = excluded.is_system, "
      "  is_generated = excluded.is_generated "
      "RETURNING id");
  st.bind(1, e.src_file_id);
  bind_opt(st, 2, e.dst_file_id);
  st.bind(3, std::string_view(e.dst_path));
  st.bind(4, e.config_id);
  st.bind(5, static_cast<int64_t>(e.is_system ? 1 : 0));
  st.bind(6, static_cast<int64_t>(e.is_generated ? 1 : 0));
  st.bind(7, e.count);
  if (!st.step()) {
    throw StorageError("add_include_edge: upsert returned no id");
  }
  const int64_t id = st.col_int64(0);
  st.step_done();
  return id;
}

int64_t Storage::add_include_site(const IncludeSite &s) {
  auto st = db_.prepare(
      "INSERT INTO include_site (edge_id, line, col, begin_offset, end_offset, "
      "                          spelling, is_angled, directive, "
      "                          cond_fingerprint, resolved, guarded) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(edge_id, begin_offset) DO UPDATE SET "
      "  line = excluded.line, col = excluded.col, "
      "  end_offset = excluded.end_offset, spelling = excluded.spelling, "
      "  is_angled = excluded.is_angled, directive = excluded.directive, "
      "  cond_fingerprint = excluded.cond_fingerprint, "
      "  resolved = excluded.resolved, guarded = excluded.guarded "
      "RETURNING id");
  st.bind(1, s.edge_id);
  st.bind(2, s.line);
  st.bind(3, s.col);
  st.bind(4, s.begin_offset);
  st.bind(5, s.end_offset);
  st.bind(6, std::string_view(s.spelling));
  st.bind(7, static_cast<int64_t>(s.is_angled ? 1 : 0));
  st.bind(8, s.directive);
  st.bind(9, std::string_view(s.cond_fingerprint));
  st.bind(10, static_cast<int64_t>(s.resolved ? 1 : 0));
  st.bind(11, static_cast<int64_t>(s.guarded ? 1 : 0));
  if (!st.step()) {
    throw StorageError("add_include_site: upsert returned no id");
  }
  const int64_t id = st.col_int64(0);
  st.step_done();
  return id;
}

void Storage::add_include_macro_use(const IncludeMacroUse &m) {
  auto st = db_.prepare(
      "INSERT INTO include_macro_use (src_file_id, def_path, name, config_id, "
      "                               count) "
      "VALUES (?, ?, ?, ?, ?) "
      "ON CONFLICT(src_file_id, def_path, name, config_id) DO UPDATE SET "
      "  count = include_macro_use.count + excluded.count");
  st.bind(1, m.src_file_id);
  st.bind(2, std::string_view(m.def_path));
  st.bind(3, std::string_view(m.name));
  st.bind(4, m.config_id);
  st.bind(5, m.count);
  st.step_done();
}

void Storage::delete_include_configs_for_tu(int64_t tu_file_id) {
  auto st = db_.prepare("DELETE FROM include_config WHERE tu_file_id = ?");
  st.bind(1, tu_file_id);
  st.step_done();
}

std::vector<IncludeEdge> Storage::include_edges_from(int64_t src_file_id,
                                                     bool include_system) {
  std::string sql = std::string("SELECT ") + kIncludeEdgeCols +
                    " FROM include_edge e "
                    "JOIN include_config c ON c.id = e.config_id "
                    "WHERE e.src_file_id = ?";
  if (!include_system) {
    sql += " AND e.is_system = 0";
  }
  sql += " ORDER BY e.dst_path, c.digest";
  auto st = db_.prepare(sql);
  st.bind(1, src_file_id);
  std::vector<IncludeEdge> out;
  while (st.step()) {
    out.push_back(include_edge_from(st));
  }
  return out;
}

std::vector<IncludeEdge> Storage::include_edges_to(int64_t dst_file_id) {
  auto st = db_.prepare(std::string("SELECT ") + kIncludeEdgeCols +
                        " FROM include_edge e "
                        "JOIN include_config c ON c.id = e.config_id "
                        "WHERE e.dst_file_id = ? "
                        "ORDER BY e.src_file_id, c.digest");
  st.bind(1, dst_file_id);
  std::vector<IncludeEdge> out;
  while (st.step()) {
    out.push_back(include_edge_from(st));
  }
  return out;
}

std::vector<IncludeEdge>
Storage::include_edges_to_path(const std::string &dst_path) {
  auto st = db_.prepare(std::string("SELECT ") + kIncludeEdgeCols +
                        " FROM include_edge e "
                        "JOIN include_config c ON c.id = e.config_id "
                        "WHERE e.dst_path = ? "
                        "ORDER BY e.src_file_id, c.digest");
  st.bind(1, std::string_view(dst_path));
  std::vector<IncludeEdge> out;
  while (st.step()) {
    out.push_back(include_edge_from(st));
  }
  return out;
}

std::vector<IncludeEdge> Storage::all_include_edges(bool include_system) {
  std::string sql = std::string("SELECT ") + kIncludeEdgeCols +
                    " FROM include_edge e "
                    "JOIN include_config c ON c.id = e.config_id";
  if (!include_system) {
    sql += " WHERE e.is_system = 0";
  }
  sql += " ORDER BY e.src_file_id, e.dst_path, c.digest";
  auto st = db_.prepare(sql);
  std::vector<IncludeEdge> out;
  while (st.step()) {
    out.push_back(include_edge_from(st));
  }
  return out;
}

std::vector<IncludeSite> Storage::include_sites_for(int64_t edge_id) {
  auto st = db_.prepare(
      "SELECT id, edge_id, line, col, begin_offset, end_offset, spelling, "
      "       is_angled, directive, cond_fingerprint, resolved, guarded "
      "FROM include_site WHERE edge_id = ? ORDER BY begin_offset");
  st.bind(1, edge_id);
  std::vector<IncludeSite> out;
  while (st.step()) {
    IncludeSite s;
    s.id = st.col_int64(0);
    s.edge_id = st.col_int64(1);
    s.line = st.col_int64(2);
    s.col = st.col_int64(3);
    s.begin_offset = st.col_int64(4);
    s.end_offset = st.col_int64(5);
    s.spelling = st.col_text(6);
    s.is_angled = st.col_int64(7) != 0;
    s.directive = st.col_int64(8);
    s.cond_fingerprint = st.col_text(9);
    s.resolved = st.col_int64(10) != 0;
    s.guarded = st.col_int64(11) != 0;
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<IncludeMacroUse>
Storage::include_macro_uses(int64_t src_file_id, const std::string &def_path) {
  auto st = db_.prepare(
      "SELECT src_file_id, def_path, name, config_id, count "
      "FROM include_macro_use WHERE src_file_id = ? AND def_path = ? "
      "ORDER BY name, config_id");
  st.bind(1, src_file_id);
  st.bind(2, std::string_view(def_path));
  std::vector<IncludeMacroUse> out;
  while (st.step()) {
    IncludeMacroUse m;
    m.src_file_id = st.col_int64(0);
    m.def_path = st.col_text(1);
    m.name = st.col_text(2);
    m.config_id = st.col_int64(3);
    m.count = st.col_int64(4);
    out.push_back(std::move(m));
  }
  return out;
}

bool Storage::include_graph_populated() {
  // A configuration row is written for EVERY translation unit the include tier
  // processed, even one with no #includes at all -- so an existing config is
  // the true "the tier has run" marker, where an edge is not. A fully indexed
  // project that happens to include nothing has configs but zero edges and must
  // read as populated; a DB that predates v31 or was never reindexed has
  // neither, and only that case is the vacuous "nothing to report".
  auto st = db_.prepare("SELECT 1 FROM include_config LIMIT 1");
  return st.step();
}

bool Storage::include_tier_covers_file(int64_t file_id) {
  // A configuration row is written for every TU the tier processed (even one
  // with no #includes), so it -- together with either direction of an include
  // edge -- is the completion marker for "this file was seen".
  auto st = db_.prepare(
      "SELECT 1 FROM include_config WHERE tu_file_id = ? "
      "UNION ALL SELECT 1 FROM include_edge WHERE src_file_id = ? "
      "UNION ALL SELECT 1 FROM include_edge WHERE dst_file_id = ? LIMIT 1");
  st.bind(1, file_id);
  st.bind(2, file_id);
  st.bind(3, file_id);
  return st.step();
}

} // namespace cidx
