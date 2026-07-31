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
#include "util/hashing.hpp"
#include "util/json_min.hpp"
#include "util/logger.hpp"
#include "util/pathutil.hpp"

namespace cidx {

using namespace detail;

namespace {
std::string json_quote(std::string_view value) {
  std::string out = "\"";
  for (const unsigned char ch : value) {
    switch (ch) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (ch < 0x20) {
        constexpr std::array hex = std::to_array("0123456789abcdef");
        out += "\\u00";
        out += hex[ch >> 4];
        out += hex[ch & 0xf];
      } else {
        out += static_cast<char>(ch);
      }
    }
  }
  return out + '"';
}

std::string json_array(const std::vector<std::string> &values) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += json_quote(values[i]);
  }
  return out + ']';
}

std::string config_state_name(TranslationUnitConfigState state) {
  switch (state) {
  case TranslationUnitConfigState::registered:
    return "registered";
  case TranslationUnitConfigState::unregistered:
    return "unregistered";
  case TranslationUnitConfigState::ambiguous:
    return "ambiguous";
  case TranslationUnitConfigState::stale:
    return "stale";
  case TranslationUnitConfigState::unavailable:
    return "unavailable";
  }
  return "unavailable";
}

TranslationUnitConfigState config_state(std::string_view name) {
  if (name == "registered") {
    return TranslationUnitConfigState::registered;
  }
  if (name == "unregistered") {
    return TranslationUnitConfigState::unregistered;
  }
  if (name == "ambiguous") {
    return TranslationUnitConfigState::ambiguous;
  }
  if (name == "stale") {
    return TranslationUnitConfigState::stale;
  }
  return TranslationUnitConfigState::unavailable;
}

TranslationUnitConfig normalized_from_include(const IncludeConfig &c) {
  return resolve_translation_unit_config(c.driver, c.working_dir, c.arguments,
                                         c.lang_mode, c.resource_dir,
                                         std::string("error-limit=0"));
}

std::string canonical_config_json(const TranslationUnitConfig &c) {
  std::vector<std::string> fields = {c.driver.value_or(""),
                                     c.working_dir.value_or(""),
                                     c.language.value_or(""),
                                     c.standard.value_or(""),
                                     c.target.value_or(""),
                                     json_array(c.abi_options),
                                     c.sysroot.value_or(""),
                                     c.resource_dir.value_or(""),
                                     json_array(c.include_paths),
                                     json_array(c.macro_state),
                                     json_array(c.relevant_environment),
                                     json_array(c.generated_inputs),
                                     c.diagnostics_policy.value_or(""),
                                     json_array(c.arguments)};
  std::string out = "[";
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    if (i == 5 || i == 8 || i == 9 || i == 10 || i == 11 || i == 13) {
      out += fields[i];
    } else {
      out += json_quote(fields[i]);
    }
  }
  return out + ']';
}

TranslationUnitConfig config_from_row(const SqliteStmt &st) {
  TranslationUnitConfig c;
  c.id = st.col_int64(0);
  c.descriptor_hash = st.col_text(1);
  c.descriptor_json = st.col_text(2);
  c.driver = opt_text(st, 3);
  c.working_dir = opt_text(st, 4);
  c.language = opt_text(st, 5);
  c.standard = opt_text(st, 6);
  c.target = opt_text(st, 7);
  c.abi_options = json_min::decode_string_array(st.col_text(8));
  c.sysroot = opt_text(st, 9);
  c.resource_dir = opt_text(st, 10);
  c.include_paths = json_min::decode_string_array(st.col_text(11));
  c.macro_state = json_min::decode_string_array(st.col_text(12));
  c.relevant_environment = json_min::decode_string_array(st.col_text(13));
  c.generated_inputs = json_min::decode_string_array(st.col_text(14));
  c.diagnostics_policy = opt_text(st, 15);
  c.arguments = json_min::decode_string_array(st.col_text(16));
  c.state = config_state(st.col_text(17));
  c.association_state = config_state(st.col_text(18));
  return c;
}
} // namespace

std::string
canonical_translation_unit_config_json(const TranslationUnitConfig &config) {
  return canonical_config_json(config);
}

std::string translation_unit_config_hash(const TranslationUnitConfig &config) {
  return sha1_hex(canonical_translation_unit_config_json(config));
}

int64_t SqliteStorageService::add_translation_unit_config(
    const TranslationUnitConfig &input) {
  TranslationUnitConfig c = resolve_translation_unit_config(
      input.driver, input.working_dir, input.arguments, input.language,
      input.resource_dir, input.diagnostics_policy);
  if (input.standard) {
    c.standard = input.standard;
  }
  if (input.target) {
    c.target = input.target;
  }
  if (!input.abi_options.empty()) {
    c.abi_options = input.abi_options;
  }
  if (input.sysroot) {
    c.sysroot = input.sysroot;
  }
  if (!input.include_paths.empty()) {
    c.include_paths = input.include_paths;
  }
  if (!input.macro_state.empty()) {
    c.macro_state = input.macro_state;
  }
  if (!input.relevant_environment.empty()) {
    c.relevant_environment = input.relevant_environment;
  }
  if (!input.generated_inputs.empty()) {
    c.generated_inputs = input.generated_inputs;
  }
  c.state = input.state;
  c.descriptor_json = canonical_translation_unit_config_json(c);
  c.descriptor_hash = translation_unit_config_hash(c);
  auto st = db_.prepare(
      "INSERT INTO translation_unit_config "
      "(descriptor_hash, descriptor_json, driver, working_dir, language, "
      "standard, "
      "target, abi_options, sysroot, resource_dir, include_paths, macro_state, "
      "relevant_environment, generated_inputs, diagnostics_policy, arguments, "
      "state) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(descriptor_hash) DO UPDATE SET "
      "descriptor_json=excluded.descriptor_json, "
      "driver=excluded.driver, working_dir=excluded.working_dir, "
      "language=excluded.language, "
      "standard=excluded.standard, target=excluded.target, "
      "abi_options=excluded.abi_options, "
      "sysroot=excluded.sysroot, resource_dir=excluded.resource_dir, "
      "include_paths=excluded.include_paths, macro_state=excluded.macro_state, "
      "relevant_environment=excluded.relevant_environment, "
      "generated_inputs=excluded.generated_inputs, "
      "diagnostics_policy=excluded.diagnostics_policy, "
      "arguments=excluded.arguments, "
      "state=excluded.state RETURNING id");
  st.bind(1, std::string_view(c.descriptor_hash));
  st.bind(2, std::string_view(c.descriptor_json));
  bind_opt(st, 3, c.driver);
  bind_opt(st, 4, c.working_dir);
  bind_opt(st, 5, c.language);
  bind_opt(st, 6, c.standard);
  bind_opt(st, 7, c.target);
  st.bind(8, std::string_view(json_array(c.abi_options)));
  bind_opt(st, 9, c.sysroot);
  bind_opt(st, 10, c.resource_dir);
  st.bind(11, std::string_view(json_array(c.include_paths)));
  st.bind(12, std::string_view(json_array(c.macro_state)));
  st.bind(13, std::string_view(json_array(c.relevant_environment)));
  st.bind(14, std::string_view(json_array(c.generated_inputs)));
  bind_opt(st, 15, c.diagnostics_policy);
  st.bind(16, std::string_view(json_array(c.arguments)));
  st.bind(17, std::string_view(config_state_name(c.state)));
  if (!st.step()) {
    throw StorageError("add_translation_unit_config: no id");
  }
  const int64_t id = st.col_int64(0);
  st.step_done();
  return id;
}

std::optional<TranslationUnitConfig>
SqliteStorageService::translation_unit_config_by_id(int64_t config_id) {
  auto st = db_.prepare(
      "SELECT id, descriptor_hash, descriptor_json, driver, working_dir, "
      "language, "
      "standard, target, abi_options, sysroot, resource_dir, include_paths, "
      "macro_state, relevant_environment, generated_inputs, "
      "diagnostics_policy, "
      "arguments, state, 'registered' FROM translation_unit_config WHERE id = "
      "?");
  st.bind(1, config_id);
  if (!st.step()) {
    return std::nullopt;
  }
  return config_from_row(st);
}

std::vector<TranslationUnitConfig>
SqliteStorageService::translation_unit_configs_for_file(int64_t file_id) {
  auto st = db_.prepare(
      "SELECT c.id, c.descriptor_hash, c.descriptor_json, c.driver, "
      "c.working_dir, "
      "c.language, c.standard, c.target, c.abi_options, c.sysroot, "
      "c.resource_dir, "
      "c.include_paths, c.macro_state, c.relevant_environment, "
      "c.generated_inputs, "
      "c.diagnostics_policy, c.arguments, c.state, f.state "
      "FROM translation_unit_config c "
      "JOIN translation_unit t ON t.config_id = c.id "
      "JOIN file_config f ON f.file_id = t.file_id AND f.config_id = c.id "
      "AND f.role = 'translation_unit' WHERE t.file_id = ? "
      "ORDER BY c.descriptor_hash");
  st.bind(1, file_id);
  std::vector<TranslationUnitConfig> out;
  while (st.step()) {
    out.push_back(config_from_row(st));
  }
  return out;
}

void SqliteStorageService::add_file_config(const FileConfigApplicability &a) {
  if (a.role != "translation_unit" && a.role != "header") {
    throw StorageError("unknown file configuration role: " + a.role);
  }
  auto st = db_.prepare(
      "INSERT INTO file_config(file_id, config_id, role, state, reason) VALUES "
      "(?, ?, ?, ?, ?) ON CONFLICT(file_id, config_id, role) DO UPDATE SET "
      "state=excluded.state, reason=excluded.reason");
  st.bind(1, a.file_id);
  st.bind(2, a.config_id);
  st.bind(3, std::string_view(a.role));
  st.bind(4, std::string_view(config_state_name(a.state)));
  bind_opt(st, 5, a.reason);
  st.step_done();
}

std::vector<FileConfigApplicability>
SqliteStorageService::file_configs_for(int64_t file_id) {
  auto st = db_.prepare(
      "SELECT file_id, config_id, role, state, reason FROM file_config "
      "WHERE file_id = ? ORDER BY config_id, role");
  st.bind(1, file_id);
  std::vector<FileConfigApplicability> out;
  while (st.step()) {
    FileConfigApplicability a;
    a.file_id = st.col_int64(0);
    a.config_id = st.col_int64(1);
    a.role = st.col_text(2);
    a.state = config_state(st.col_text(3));
    a.reason = opt_text(st, 4);
    out.push_back(std::move(a));
  }
  return out;
}

int64_t SqliteStorageService::add_include_config(const IncludeConfig &c) {
  const int64_t normalized_id =
      add_translation_unit_config(normalized_from_include(c));
  auto st = db_.prepare(
      "INSERT INTO include_config (tu_file_id, digest, driver, working_dir, "
      "                            arguments, lang_mode, resource_dir, "
      "                            translation_unit_config_id) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(tu_file_id, digest) DO UPDATE SET "
      "  driver       = excluded.driver, "
      "  working_dir  = excluded.working_dir, "
      "  arguments    = excluded.arguments, "
      "  lang_mode    = excluded.lang_mode, "
      "  resource_dir = excluded.resource_dir, "
      "  translation_unit_config_id = excluded.translation_unit_config_id "
      "RETURNING id");
  st.bind(1, c.tu_file_id);
  st.bind(2, std::string_view(c.digest));
  bind_opt(st, 3, c.driver);
  bind_opt(st, 4, c.working_dir);
  st.bind(5, std::string_view(json_min::encode_string_array(c.arguments)));
  bind_opt(st, 6, c.lang_mode);
  bind_opt(st, 7, c.resource_dir);
  st.bind(8, normalized_id);
  if (!st.step()) {
    throw StorageError("add_include_config: upsert returned no id");
  }
  const int64_t id = st.col_int64(0);
  st.step_done();
  auto tu = db_.prepare("INSERT OR IGNORE INTO translation_unit(file_id, "
                        "config_id) VALUES (?, ?)");
  tu.bind(1, c.tu_file_id);
  tu.bind(2, normalized_id);
  tu.step_done();
  add_file_config(
      FileConfigApplicability{.file_id = c.tu_file_id,
                              .config_id = normalized_id,
                              .role = "translation_unit",
                              .state = TranslationUnitConfigState::registered,
                              .reason = std::nullopt});
  return id;
}

std::optional<IncludeConfig>
SqliteStorageService::include_config_by_id(int64_t config_id) {
  auto st = db_.prepare(std::string("SELECT ") + kIncludeConfigCols +
                        " FROM include_config WHERE id = ?");
  st.bind(1, config_id);
  if (!st.step()) {
    return std::nullopt;
  }
  return include_config_from(st);
}

std::vector<IncludeConfig>
SqliteStorageService::include_configs_for_tu(int64_t tu_file_id) {
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

int64_t SqliteStorageService::add_include_edge(const IncludeEdge &e) {
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
      "  dst_file_id = COALESCE(excluded.dst_file_id, "
      "include_edge.dst_file_id), "
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

int64_t SqliteStorageService::add_include_site(const IncludeSite &s) {
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

void SqliteStorageService::add_include_macro_use(const IncludeMacroUse &m) {
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

IncludeDeletionStats
SqliteStorageService::delete_include_configs_for_tu(int64_t tu_file_id) {
  IncludeDeletionStats deleted;
  std::vector<int64_t> config_ids;
  auto ids = db_.prepare(
      "SELECT DISTINCT translation_unit_config_id FROM include_config "
      "WHERE tu_file_id = ? AND translation_unit_config_id IS NOT NULL");
  ids.bind(1, tu_file_id);
  while (ids.step()) {
    config_ids.push_back(ids.col_int64(0));
  }
  auto tu = db_.prepare(
      "DELETE FROM translation_unit WHERE file_id = ? RETURNING rowid");
  tu.bind(1, tu_file_id);
  while (tu.step()) {
    ++deleted.direct;
  }
  auto fc = db_.prepare("DELETE FROM file_config WHERE file_id = ? AND role = "
                        "'translation_unit' RETURNING file_id");
  fc.bind(1, tu_file_id);
  while (fc.step()) {
    ++deleted.direct;
  }
  auto sites = db_.prepare(
      "DELETE FROM include_site WHERE edge_id IN ("
      "SELECT id FROM include_edge WHERE config_id IN ("
      "SELECT id FROM include_config WHERE tu_file_id = ?)) RETURNING edge_id");
  sites.bind(1, tu_file_id);
  while (sites.step()) {
    ++deleted.cascade;
  }
  auto edges = db_.prepare(
      "DELETE FROM include_edge WHERE config_id IN ("
      "SELECT id FROM include_config WHERE tu_file_id = ?) RETURNING id");
  edges.bind(1, tu_file_id);
  while (edges.step()) {
    ++deleted.cascade;
  }
  auto macros = db_.prepare("DELETE FROM include_macro_use WHERE config_id IN ("
                            "SELECT id FROM include_config WHERE tu_file_id = "
                            "?) RETURNING config_id");
  macros.bind(1, tu_file_id);
  while (macros.step()) {
    ++deleted.cascade;
  }
  auto st = db_.prepare(
      "DELETE FROM include_config WHERE tu_file_id = ? RETURNING rowid");
  st.bind(1, tu_file_id);
  while (st.step()) {
    ++deleted.direct;
  }
  // A normalized descriptor may be shared by several TUs. Retire header
  // applicability only when no remaining TU or fact still supports it.
  for (const int64_t config_id : config_ids) {
    auto headers = db_.prepare(
        "DELETE FROM file_config WHERE config_id = ? AND role = 'header' "
        "AND NOT EXISTS (SELECT 1 FROM translation_unit "
        "                WHERE config_id = ?) "
        "AND NOT EXISTS (SELECT 1 FROM include_edge e "
        "                JOIN include_config ic ON ic.id = e.config_id "
        "                WHERE ic.translation_unit_config_id = ?) "
        "AND NOT EXISTS (SELECT 1 FROM include_macro_use m "
        "                JOIN include_config ic ON ic.id = m.config_id "
        "                WHERE ic.translation_unit_config_id = ?) "
        "RETURNING file_id");
    headers.bind(1, config_id);
    headers.bind(2, config_id);
    headers.bind(3, config_id);
    headers.bind(4, config_id);
    while (headers.step()) {
      ++deleted.direct;
    }
  }
  return deleted;
}

std::vector<IncludeEdge>
SqliteStorageService::include_edges_from(int64_t src_file_id,
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

std::vector<IncludeEdge> SqliteStorageService::include_edges_from_config(
    int64_t src_file_id, int64_t translation_unit_config_id,
    bool include_system) {
  std::string sql = std::string("SELECT ") + kIncludeEdgeCols +
                    " FROM include_edge e JOIN include_config c ON c.id = "
                    "e.config_id WHERE e.src_file_id = ? AND "
                    "c.translation_unit_config_id = ?";
  if (!include_system) {
    sql += " AND e.is_system = 0";
  }
  sql += " ORDER BY e.dst_path, c.digest";
  auto st = db_.prepare(sql);
  st.bind(1, src_file_id);
  st.bind(2, translation_unit_config_id);
  std::vector<IncludeEdge> out;
  while (st.step()) {
    out.push_back(include_edge_from(st));
  }
  return out;
}

ConfiguredIncludeEdges SqliteStorageService::invariant_include_edges(
    int64_t src_file_id, const std::vector<int64_t> &declared_config_ids,
    bool include_system) {
  ConfiguredIncludeEdges result;
  if (declared_config_ids.empty()) {
    return result;
  }
  std::map<std::string, IncludeEdge> common;
  bool initialized = false;
  for (const int64_t config_id : declared_config_ids) {
    auto coverage = db_.prepare(
        "SELECT 1 FROM translation_unit_config c JOIN file_config f "
        "ON f.config_id = c.id WHERE f.file_id = ? AND f.config_id = ? "
        "AND f.role = 'header' AND f.state = 'registered' "
        "AND c.state = 'registered'");
    coverage.bind(1, src_file_id);
    coverage.bind(2, config_id);
    if (!coverage.step()) {
      return result;
    }
    const auto edges =
        include_edges_from_config(src_file_id, config_id, include_system);
    std::map<std::string, IncludeEdge> current;
    for (const auto &edge : edges) {
      current.emplace(edge.dst_path, edge);
    }
    if (!initialized) {
      common = std::move(current);
      initialized = true;
    } else {
      for (auto it = common.begin(); it != common.end();) {
        if (!current.contains(it->first)) {
          it = common.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  result.coverage_complete = true;
  for (auto &[path, edge] : common) {
    (void)path;
    result.edges.push_back(edge);
  }
  return result;
}

std::vector<IncludeEdge>
SqliteStorageService::include_edges_to(int64_t dst_file_id) {
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
SqliteStorageService::include_edges_to_path(const std::string &dst_path) {
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

std::vector<IncludeEdge>
SqliteStorageService::all_include_edges(bool include_system) {
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

std::vector<IncludeSite>
SqliteStorageService::include_sites_for(int64_t edge_id) {
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
SqliteStorageService::include_macro_uses(int64_t src_file_id,
                                         const std::string &def_path) {
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

bool SqliteStorageService::include_graph_populated() {
  // A configuration row is written for EVERY translation unit the include tier
  // processed, even one with no #includes at all -- so an existing config is
  // the true "the tier has run" marker, where an edge is not. A fully indexed
  // project that happens to include nothing has configs but zero edges and must
  // read as populated; a DB that predates v31 or was never reindexed has
  // neither, and only that case is the vacuous "nothing to report".
  auto st = db_.prepare("SELECT 1 FROM include_config LIMIT 1");
  return st.step();
}

bool SqliteStorageService::include_tier_covers_file(int64_t file_id) {
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
