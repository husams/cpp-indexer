#include "storage/storage.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "storage/storage_detail.hpp"

namespace cidx {
namespace {

using detail::kSymbolColsS;
using detail::symbol_from_offset;

void ensure_applicability_staging(SqliteDb &db) {
  db.exec("CREATE TEMP TABLE IF NOT EXISTS cidx_applicability_ids ("
          "bucket TEXT NOT NULL, fact_id INTEGER NOT NULL, "
          "PRIMARY KEY(bucket, fact_id));"
          "CREATE TEMP TABLE IF NOT EXISTS cidx_applicability_generation ("
          "generation INTEGER NOT NULL);");
}

auto stage_ids(SqliteDb &db, std::string_view bucket,
               const std::vector<int64_t> &ids) -> std::uint64_t {
  if (ids.empty()) {
    return 0;
  }
  auto st = db.prepare(
      "INSERT OR IGNORE INTO temp.cidx_applicability_ids(bucket, fact_id) "
      "VALUES (?, ?)");
  std::uint64_t inserted = 0;
  for (const int64_t id : ids) {
    st.bind(1, bucket);
    st.bind(2, id);
    st.step_done();
    inserted += static_cast<std::uint64_t>(db.changes());
    st.reset();
  }
  return inserted;
}

auto insert_staged(SqliteDb &db, std::string_view fact_kind,
                   std::string_view table, std::string_view id_expression,
                   std::string_view filter_column, std::string_view bucket,
                   int64_t file_id, int64_t config_id, int64_t generation)
    -> std::uint64_t {
  auto st = db.prepare(
      "INSERT OR IGNORE INTO fact_applicability "
      "(fact_kind, fact_id, file_id, config_id, generation) "
      "SELECT ?, t." +
      std::string(id_expression) + ", ?, ?, ? FROM " + std::string(table) +
      " t JOIN temp.cidx_applicability_ids ids ON ids.bucket = ? AND " +
      "ids.fact_id = t." + std::string(filter_column));
  st.bind(1, fact_kind);
  st.bind(2, file_id);
  st.bind(3, config_id);
  st.bind(4, generation);
  st.bind(5, bucket);
  st.step_done();
  return static_cast<std::uint64_t>(db.changes());
}

auto run_derived(SqliteDb &db, std::string_view sql, int64_t file_id,
                 int64_t config_id, int64_t generation) -> std::uint64_t {
  auto st = db.prepare(sql);
  st.bind(1, file_id);
  st.bind(2, config_id);
  st.bind(3, generation);
  st.bind(4, file_id);
  st.bind(5, config_id);
  st.bind(6, generation);
  st.step_done();
  return static_cast<std::uint64_t>(db.changes());
}

} // namespace

AssociationStats SqliteStorageService::associate_facts_for_file(
    int64_t file_id, int64_t config_id, const std::vector<int64_t> &symbol_ids,
    const std::vector<int64_t> &edge_ids,
    const std::vector<int64_t> &definition_ids) {
  AssociationStats stats;
  stats.attempted = symbol_ids.size() + edge_ids.size() + definition_ids.size();
  ensure_applicability_staging(db_);

  auto reset_generation =
      db_.prepare("DELETE FROM temp.cidx_applicability_generation");
  reset_generation.step_done();
  auto seed_generation = db_.prepare(
      "INSERT INTO temp.cidx_applicability_generation(generation) "
      "SELECT COALESCE(MAX(generation), 0) + 1 FROM fact_applicability "
      "WHERE file_id = ? AND config_id = ?");
  seed_generation.bind(1, file_id);
  seed_generation.bind(2, config_id);
  seed_generation.step_done();
  auto generation_row =
      db_.prepare("SELECT generation FROM temp.cidx_applicability_generation");
  if (!generation_row.step()) {
    throw StorageError("applicability generation staging failed");
  }
  const int64_t generation = generation_row.col_int64(0);

  auto clear = db_.prepare(
      "DELETE FROM fact_applicability WHERE file_id = ? AND config_id = ?");
  clear.bind(1, file_id);
  clear.bind(2, config_id);
  clear.step_done();
  stats.deleted = static_cast<std::uint64_t>(db_.changes());

  auto reset_ids = db_.prepare("DELETE FROM temp.cidx_applicability_ids");
  reset_ids.step_done();
  const std::uint64_t symbol_rows = stage_ids(db_, "symbol", symbol_ids);
  const std::uint64_t edge_rows = stage_ids(db_, "edge", edge_ids);
  const std::uint64_t definition_rows =
      stage_ids(db_, "definition", definition_ids);
  stats.temporary_rows = symbol_rows + edge_rows + definition_rows;

  std::uint64_t base_inserted = 0;
  base_inserted += insert_staged(db_, "symbol", "symbol", "id", "id", "symbol",
                                 file_id, config_id, generation);
  base_inserted += insert_staged(db_, "definition", "definition", "id", "id",
                                 "definition", file_id, config_id, generation);
  base_inserted += insert_staged(db_, "edge", "edge", "id", "id", "edge",
                                 file_id, config_id, generation);
  base_inserted +=
      insert_staged(db_, "def_edge", "def_edge", "rowid", "src_def_id",
                    "definition", file_id, config_id, generation);
  stats.inserted = base_inserted;

  {
    auto st = db_.prepare(
        "INSERT OR IGNORE INTO fact_applicability "
        "(fact_kind, fact_id, file_id, config_id, generation) "
        "SELECT 'decl_site', ds.rowid, ?, ?, ? FROM decl_site ds JOIN "
        "fact_applicability fa ON fa.fact_kind = 'symbol' AND "
        "fa.fact_id = ds.symbol_id AND fa.file_id = ? AND fa.config_id = ? "
        "AND fa.generation = ? WHERE ds.file_id = ?");
    st.bind(1, file_id);
    st.bind(2, config_id);
    st.bind(3, generation);
    st.bind(4, file_id);
    st.bind(5, config_id);
    st.bind(6, generation);
    st.bind(7, file_id);
    st.step_done();
    stats.inserted += static_cast<std::uint64_t>(db_.changes());
  }
  stats.inserted += run_derived(
      db_,
      "INSERT OR IGNORE INTO fact_applicability "
      "(fact_kind, fact_id, file_id, config_id, generation) "
      "SELECT 'parameter', p.owner_id, ?, ?, ? FROM parameter p JOIN "
      "fact_applicability fa ON fa.fact_kind = 'symbol' AND fa.fact_id = "
      "p.owner_id AND fa.file_id = ? AND fa.config_id = ? AND fa.generation = "
      "?",
      file_id, config_id, generation);
  stats.inserted += run_derived(
      db_,
      "INSERT OR IGNORE INTO fact_applicability "
      "(fact_kind, fact_id, file_id, config_id, generation) "
      "SELECT 'symbol_type', st.symbol_id, ?, ?, ? FROM symbol_type st JOIN "
      "fact_applicability fa ON fa.fact_kind = 'symbol' AND fa.fact_id = "
      "st.symbol_id AND fa.file_id = ? AND fa.config_id = ? AND fa.generation "
      "= ?",
      file_id, config_id, generation);
  stats.inserted += run_derived(
      db_,
      "INSERT OR IGNORE INTO fact_applicability "
      "(fact_kind, fact_id, file_id, config_id, generation) "
      "SELECT DISTINCT 'type_node', st.type_id, ?, ?, ? FROM symbol_type st "
      "JOIN fact_applicability fa ON fa.fact_kind = 'symbol' AND "
      "fa.fact_id = st.symbol_id AND fa.file_id = ? AND fa.config_id = ? AND "
      "fa.generation = ? WHERE st.type_id IS NOT NULL",
      file_id, config_id, generation);
  stats.inserted += run_derived(
      db_,
      "INSERT OR IGNORE INTO fact_applicability "
      "(fact_kind, fact_id, file_id, config_id, generation) "
      "SELECT DISTINCT 'type_edge', te.src_id, ?, ?, ? FROM type_edge te JOIN "
      "fact_applicability fa ON fa.fact_kind = 'type_node' AND fa.fact_id = "
      "te.src_id AND fa.file_id = ? AND fa.config_id = ? AND fa.generation = ?",
      file_id, config_id, generation);
  stats.inserted += run_derived(
      db_,
      "INSERT OR IGNORE INTO fact_applicability "
      "(fact_kind, fact_id, file_id, config_id, generation) "
      "SELECT 'entity_node', en.id, ?, ?, ? FROM entity_node en JOIN "
      "fact_applicability fa ON fa.fact_kind = 'symbol' AND fa.fact_id = "
      "en.id AND fa.file_id = ? AND fa.config_id = ? AND fa.generation = ?",
      file_id, config_id, generation);
  stats.inserted += run_derived(
      db_,
      "INSERT OR IGNORE INTO fact_applicability "
      "(fact_kind, fact_id, file_id, config_id, generation) "
      "SELECT DISTINCT 'entity_edge', ee.rowid, ?, ?, ? FROM entity_edge ee "
      "JOIN fact_applicability fa ON fa.fact_kind = 'symbol' AND "
      "fa.fact_id = ee.src_id AND fa.file_id = ? AND fa.config_id = ? AND "
      "fa.generation = ?",
      file_id, config_id, generation);
  stats.inserted += run_derived(
      db_,
      "INSERT OR IGNORE INTO fact_applicability "
      "(fact_kind, fact_id, file_id, config_id, generation) "
      "SELECT 'template_param', tp.owner_id, ?, ?, ? FROM template_param tp "
      "JOIN fact_applicability fa ON fa.fact_kind = 'symbol' AND "
      "fa.fact_id = tp.owner_id AND fa.file_id = ? AND fa.config_id = ? AND "
      "fa.generation = ?",
      file_id, config_id, generation);
  stats.inserted += run_derived(
      db_,
      "INSERT OR IGNORE INTO fact_applicability "
      "(fact_kind, fact_id, file_id, config_id, generation) "
      "SELECT 'template_arg', ta.owner_id, ?, ?, ? FROM template_arg ta JOIN "
      "fact_applicability fa ON fa.fact_kind = 'symbol' AND fa.fact_id = "
      "ta.owner_id AND fa.file_id = ? AND fa.config_id = ? AND fa.generation = "
      "?",
      file_id, config_id, generation);
  stats.inserted += run_derived(
      db_,
      "INSERT OR IGNORE INTO fact_applicability "
      "(fact_kind, fact_id, file_id, config_id, generation) "
      "SELECT 'call_arg', ca.edge_id, ?, ?, ? FROM call_arg ca JOIN "
      "fact_applicability fa ON fa.fact_kind = 'edge' AND fa.fact_id = "
      "ca.edge_id AND fa.file_id = ? AND fa.config_id = ? AND fa.generation = "
      "?",
      file_id, config_id, generation);
  stats.inserted += run_derived(
      db_,
      "INSERT OR IGNORE INTO fact_applicability "
      "(fact_kind, fact_id, file_id, config_id, generation) "
      "SELECT 'possible_call', pc.src_def_id, ?, ?, ? FROM possible_call pc "
      "JOIN fact_applicability fa ON fa.fact_kind = 'definition' AND "
      "fa.fact_id = pc.src_def_id AND fa.file_id = ? AND fa.config_id = ? AND "
      "fa.generation = ?",
      file_id, config_id, generation);
  {
    auto st = db_.prepare(
        "INSERT OR IGNORE INTO fact_applicability "
        "(fact_kind, fact_id, file_id, config_id, generation) "
        "SELECT 'diagnostic', id, ?, ?, ? FROM diagnostic WHERE file_id = ?");
    st.bind(1, file_id);
    st.bind(2, config_id);
    st.bind(3, generation);
    st.bind(4, file_id);
    st.step_done();
    stats.inserted += static_cast<std::uint64_t>(db_.changes());
  }
  stats.attempted += stats.inserted - base_inserted;
  stats.ignored = stats.attempted - std::min(stats.attempted, stats.inserted);
  return stats;
}

ConfiguredSymbols
SqliteStorageService::symbols_for_config(int64_t file_id,
                                         const std::vector<int64_t> &config_ids,
                                         FactCoverage coverage) {
  ConfiguredSymbols result;
  if (config_ids.empty()) {
    return result;
  }
  const auto covered = [this, file_id](int64_t config_id) {
    auto st = db_.prepare(
        "SELECT 1 FROM translation_unit_config c JOIN file_config f ON "
        "f.config_id = c.id WHERE f.file_id = ? AND f.config_id = ? AND "
        "f.state = 'registered' AND c.state = 'registered' LIMIT 1");
    st.bind(1, file_id);
    st.bind(2, config_id);
    return st.step();
  };
  for (const int64_t config_id : config_ids) {
    if (!covered(config_id)) {
      return result;
    }
  }

  const auto read = [this, file_id](int64_t config_id) {
    auto st = db_.prepare(std::string("SELECT ") + kSymbolColsS +
                          " FROM symbol s JOIN fact_applicability fa ON "
                          "fa.fact_kind = 'symbol' AND fa.fact_id = s.id AND "
                          "fa.file_id = ? AND fa.config_id = ? ORDER BY s.usr");
    st.bind(1, file_id);
    st.bind(2, config_id);
    std::map<int64_t, Symbol> out;
    while (st.step()) {
      Symbol symbol = symbol_from_offset(st, 0);
      out.emplace(symbol.id, std::move(symbol));
    }
    return out;
  };

  std::map<int64_t, Symbol> selected;
  bool initialized = false;
  for (const int64_t config_id : config_ids) {
    auto current = read(config_id);
    if (coverage == FactCoverage::one) {
      selected = std::move(current);
      break;
    }
    if (!initialized) {
      selected = std::move(current);
      initialized = true;
      continue;
    }
    if (coverage == FactCoverage::all) {
      selected.insert(current.begin(), current.end());
    } else {
      for (auto it = selected.begin(); it != selected.end();) {
        if (!current.contains(it->first)) {
          it = selected.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  result.coverage_complete = true;
  for (auto &[id, symbol] : selected) {
    (void)id;
    result.symbols.push_back(std::move(symbol));
  }
  return result;
}

ConfiguredFactIds SqliteStorageService::fact_ids_for_config(
    int64_t file_id, const std::string &fact_kind,
    const std::vector<int64_t> &config_ids, FactCoverage coverage) {
  ConfiguredFactIds result;
  if (config_ids.empty()) {
    return result;
  }
  std::map<int64_t, bool> selected;
  bool initialized = false;
  for (const int64_t config_id : config_ids) {
    auto covered = db_.prepare(
        "SELECT 1 FROM translation_unit_config c JOIN file_config f ON "
        "f.config_id = c.id WHERE f.file_id = ? AND f.config_id = ? AND "
        "f.state = 'registered' AND c.state = 'registered' LIMIT 1");
    covered.bind(1, file_id);
    covered.bind(2, config_id);
    if (!covered.step()) {
      return result;
    }
    auto st = db_.prepare(
        "SELECT fact_id FROM fact_applicability WHERE fact_kind = ? AND "
        "file_id = ? AND config_id = ? ORDER BY fact_id");
    st.bind(1, std::string_view(fact_kind));
    st.bind(2, file_id);
    st.bind(3, config_id);
    std::map<int64_t, bool> current;
    while (st.step()) {
      current.emplace(st.col_int64(0), true);
    }
    if (coverage == FactCoverage::one) {
      selected = std::move(current);
      break;
    }
    if (!initialized) {
      selected = std::move(current);
      initialized = true;
    } else if (coverage == FactCoverage::all) {
      selected.insert(current.begin(), current.end());
    } else {
      for (auto it = selected.begin(); it != selected.end();) {
        if (!current.contains(it->first)) {
          it = selected.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  result.coverage_complete = true;
  for (const auto &[id, present] : selected) {
    (void)present;
    result.ids.push_back(id);
  }
  return result;
}

} // namespace cidx
