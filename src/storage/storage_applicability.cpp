#include "storage/storage.hpp"

#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "storage/storage_detail.hpp"

namespace cidx {
namespace {

using detail::kSymbolColsS;
using detail::symbol_from_offset;

void insert_simple(SqliteDb &db, std::string_view sql, int64_t file_id,
                   int64_t config_id, int64_t generation) {
  auto st = db.prepare(sql);
  st.bind(1, file_id);
  st.bind(2, config_id);
  st.bind(3, generation);
  st.bind(4, file_id);
  st.step_done();
}

void insert_fact_ids(SqliteDb &db, std::string_view fact_kind,
                     std::string_view table, std::string_view id_expression,
                     std::string_view filter_column, int64_t file_id,
                     int64_t config_id, int64_t generation,
                     const std::vector<int64_t> &ids) {
  if (ids.empty()) {
    return;
  }
  std::string sql = "INSERT OR IGNORE INTO fact_applicability "
                    "(fact_kind, fact_id, file_id, config_id, generation) "
                    "SELECT ?, " +
                    std::string(id_expression) + ", ?, ?, ? FROM " +
                    std::string(table) + " WHERE " +
                    std::string(filter_column) + " IN (";
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i != 0) {
      sql += ",";
    }
    sql += "?";
  }
  sql += ")";
  auto st = db.prepare(sql);
  st.bind(1, fact_kind);
  st.bind(2, file_id);
  st.bind(3, config_id);
  st.bind(4, generation);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    st.bind(static_cast<int>(i + 5), ids[i]);
  }
  st.step_done();
}

} // namespace

void SqliteStorageService::associate_facts_for_file(
    int64_t file_id, int64_t config_id, const std::vector<int64_t> &symbol_ids,
    const std::vector<int64_t> &edge_ids,
    const std::vector<int64_t> &definition_ids) {
  auto next = db_.prepare(
      "SELECT COALESCE(MAX(generation), 0) + 1 FROM fact_applicability "
      "WHERE file_id = ? AND config_id = ?");
  next.bind(1, file_id);
  next.bind(2, config_id);
  const int64_t generation = next.step() ? next.col_int64(0) : 1;

  auto clear = db_.prepare(
      "DELETE FROM fact_applicability WHERE file_id = ? AND config_id = ?");
  clear.bind(1, file_id);
  clear.bind(2, config_id);
  clear.step_done();

  if (!symbol_ids.empty()) {
    std::string sql = "INSERT OR IGNORE INTO fact_applicability "
                      "(fact_kind, fact_id, file_id, config_id, generation) "
                      "SELECT 'symbol', id, ?, ?, ? FROM symbol WHERE id IN (";
    for (std::size_t i = 0; i < symbol_ids.size(); ++i) {
      if (i != 0) {
        sql += ",";
      }
      sql += "?";
    }
    sql += ")";
    auto st = db_.prepare(sql);
    st.bind(1, file_id);
    st.bind(2, config_id);
    st.bind(3, generation);
    for (std::size_t i = 0; i < symbol_ids.size(); ++i) {
      st.bind(static_cast<int>(i + 4), symbol_ids[i]);
    }
    st.step_done();
  }
  {
    auto st = db_.prepare(
        "INSERT OR IGNORE INTO fact_applicability "
        "(fact_kind, fact_id, file_id, config_id, generation) "
        "SELECT 'decl_site', ds.rowid, ?, ?, ? FROM decl_site ds JOIN "
        "fact_applicability fa ON fa.fact_kind = 'symbol' AND "
        "fa.fact_id = ds.symbol_id AND fa.file_id = ? AND fa.config_id = ? "
        "WHERE ds.file_id = ?");
    st.bind(1, file_id);
    st.bind(2, config_id);
    st.bind(3, generation);
    st.bind(4, file_id);
    st.bind(5, config_id);
    st.bind(6, file_id);
    st.step_done();
  }
  insert_fact_ids(db_, "definition", "definition", "id", "id", file_id,
                  config_id, generation, definition_ids);
  insert_fact_ids(db_, "edge", "edge", "id", "id", file_id, config_id,
                  generation, edge_ids);
  insert_fact_ids(db_, "def_edge", "def_edge", "rowid", "src_def_id", file_id,
                  config_id, generation, definition_ids);
  insert_simple(
      db_,
      "INSERT OR IGNORE INTO fact_applicability "
      "(fact_kind, fact_id, file_id, config_id, generation) "
      "SELECT 'diagnostic', id, ?, ?, ? FROM diagnostic WHERE file_id = ?",
      file_id, config_id, generation);

  // Signature and type facts inherit the applicability of their owning
  // symbols. The same rule is used for derived entity facts below.
  {
    const auto run = [&](std::string_view sql) {
      auto st = db_.prepare(sql);
      st.bind(1, file_id);
      st.bind(2, config_id);
      st.bind(3, generation);
      st.bind(4, file_id);
      st.bind(5, config_id);
      st.step_done();
    };
    run("INSERT OR IGNORE INTO fact_applicability "
        "(fact_kind, fact_id, file_id, config_id, generation) "
        "SELECT 'parameter', p.owner_id, ?, ?, ? FROM parameter p JOIN "
        "fact_applicability fa ON fa.fact_kind = 'symbol' AND fa.fact_id = "
        "p.owner_id AND fa.file_id = ? AND fa.config_id = ?");
    run("INSERT OR IGNORE INTO fact_applicability "
        "(fact_kind, fact_id, file_id, config_id, generation) "
        "SELECT 'symbol_type', st.symbol_id, ?, ?, ? FROM symbol_type st JOIN "
        "fact_applicability fa ON fa.fact_kind = 'symbol' AND fa.fact_id = "
        "st.symbol_id AND fa.file_id = ? AND fa.config_id = ?");
    run("INSERT OR IGNORE INTO fact_applicability "
        "(fact_kind, fact_id, file_id, config_id, generation) "
        "SELECT DISTINCT 'type_node', st.type_id, ?, ?, ? FROM symbol_type st "
        "JOIN fact_applicability fa ON fa.fact_kind = 'symbol' AND "
        "fa.fact_id = st.symbol_id AND fa.file_id = ? AND fa.config_id = ? "
        "WHERE st.type_id IS NOT NULL");
    run("INSERT OR IGNORE INTO fact_applicability "
        "(fact_kind, fact_id, file_id, config_id, generation) "
        "SELECT 'type_edge', te.src_id, ?, ?, ? FROM type_edge te JOIN "
        "fact_applicability fa ON fa.fact_kind = 'type_node' AND fa.fact_id = "
        "te.src_id AND fa.file_id = ? AND fa.config_id = ?");
    run("INSERT OR IGNORE INTO fact_applicability "
        "(fact_kind, fact_id, file_id, config_id, generation) "
        "SELECT 'entity_node', en.id, ?, ?, ? FROM entity_node en JOIN "
        "fact_applicability fa ON fa.fact_kind = 'symbol' AND fa.fact_id = "
        "en.id AND fa.file_id = ? AND fa.config_id = ?");
    run("INSERT OR IGNORE INTO fact_applicability "
        "(fact_kind, fact_id, file_id, config_id, generation) "
        "SELECT 'entity_edge', ee.rowid, ?, ?, ? FROM entity_edge ee JOIN "
        "fact_applicability fa ON fa.fact_kind = 'symbol' AND fa.fact_id = "
        "ee.src_id AND fa.file_id = ? AND fa.config_id = ?");
    run("INSERT OR IGNORE INTO fact_applicability "
        "(fact_kind, fact_id, file_id, config_id, generation) "
        "SELECT 'template_param', tp.owner_id, ?, ?, ? FROM template_param tp "
        "JOIN fact_applicability fa ON fa.fact_kind = 'symbol' AND "
        "fa.fact_id = tp.owner_id AND fa.file_id = ? AND fa.config_id = ?");
    run("INSERT OR IGNORE INTO fact_applicability "
        "(fact_kind, fact_id, file_id, config_id, generation) "
        "SELECT 'template_arg', ta.owner_id, ?, ?, ? FROM template_arg ta "
        "JOIN fact_applicability fa ON fa.fact_kind = 'symbol' AND "
        "fa.fact_id = ta.owner_id AND fa.file_id = ? AND fa.config_id = ?");
    run("INSERT OR IGNORE INTO fact_applicability "
        "(fact_kind, fact_id, file_id, config_id, generation) "
        "SELECT 'call_arg', ca.edge_id, ?, ?, ? FROM call_arg ca JOIN "
        "fact_applicability fa ON fa.fact_kind = 'edge' AND fa.fact_id = "
        "ca.edge_id AND fa.file_id = ? AND fa.config_id = ?");
    run("INSERT OR IGNORE INTO fact_applicability "
        "(fact_kind, fact_id, file_id, config_id, generation) "
        "SELECT 'possible_call', pc.src_def_id, ?, ?, ? FROM possible_call pc "
        "JOIN fact_applicability fa ON fa.fact_kind = 'definition' AND "
        "fa.fact_id = pc.src_def_id AND fa.file_id = ? AND fa.config_id = ?");
  }
}

auto SqliteStorageService::association_fact_count(
    int64_t file_id, const std::vector<int64_t> &symbol_ids,
    const std::vector<int64_t> &edge_ids,
    const std::vector<int64_t> &definition_ids) -> std::size_t {
  const auto count_with_ids =
      [this](std::string_view sql,
             const std::vector<int64_t> &ids) -> std::size_t {
    if (ids.empty()) {
      return std::size_t{0};
    }
    std::string statement(sql);
    statement += " (";
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (i != 0) {
        statement += ",";
      }
      statement += "?";
    }
    statement += ")";
    auto query = db_.prepare(statement);
    for (std::size_t i = 0; i < ids.size(); ++i) {
      query.bind(static_cast<int>(i + 1), ids[i]);
    }
    return query.step() ? static_cast<std::size_t>(query.col_int64(0)) : 0;
  };
  const auto count_file = [this](std::string_view sql,
                                 int64_t id) -> std::size_t {
    auto query = db_.prepare(sql);
    query.bind(1, id);
    return query.step() ? static_cast<std::size_t>(query.col_int64(0)) : 0;
  };
  const auto count_decl_sites =
      [this, file_id](const std::vector<int64_t> &ids) -> std::size_t {
    if (ids.empty()) {
      return std::size_t{0};
    }
    std::string statement =
        "SELECT COUNT(*) FROM decl_site WHERE file_id = ? AND symbol_id IN (";
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (i != 0) {
        statement += ",";
      }
      statement += "?";
    }
    statement += ")";
    auto query = db_.prepare(statement);
    query.bind(1, file_id);
    for (std::size_t i = 0; i < ids.size(); ++i) {
      query.bind(static_cast<int>(i + 2), ids[i]);
    }
    return query.step() ? static_cast<std::size_t>(query.col_int64(0)) : 0;
  };

  std::size_t total = 0;
  total +=
      count_with_ids("SELECT COUNT(*) FROM symbol WHERE id IN", symbol_ids);
  total += count_decl_sites(symbol_ids);
  total += count_with_ids("SELECT COUNT(*) FROM definition WHERE id IN",
                          definition_ids);
  total += count_with_ids("SELECT COUNT(*) FROM edge WHERE id IN", edge_ids);
  total += count_with_ids("SELECT COUNT(*) FROM def_edge WHERE src_def_id IN",
                          definition_ids);
  total +=
      count_file("SELECT COUNT(*) FROM diagnostic WHERE file_id = ?", file_id);

  total += count_with_ids(
      "SELECT COUNT(DISTINCT owner_id) FROM parameter WHERE owner_id IN",
      symbol_ids);
  total += count_with_ids(
      "SELECT COUNT(DISTINCT symbol_id) FROM symbol_type WHERE symbol_id IN",
      symbol_ids);
  total += count_with_ids(
      "SELECT COUNT(DISTINCT type_id) FROM symbol_type WHERE symbol_id IN",
      symbol_ids);
  total +=
      count_with_ids("SELECT COUNT(DISTINCT te.src_id) FROM type_edge te JOIN "
                     "symbol_type st "
                     "ON st.type_id = te.src_id WHERE st.symbol_id IN",
                     symbol_ids);
  total += count_with_ids(
      "SELECT COUNT(DISTINCT id) FROM entity_node WHERE id IN", symbol_ids);
  total += count_with_ids(
      "SELECT COUNT(DISTINCT ee.rowid) FROM entity_edge ee WHERE ee.src_id IN",
      symbol_ids);
  total += count_with_ids(
      "SELECT COUNT(DISTINCT owner_id) FROM template_param WHERE owner_id IN",
      symbol_ids);
  total += count_with_ids(
      "SELECT COUNT(DISTINCT owner_id) FROM template_arg WHERE owner_id IN",
      symbol_ids);
  total += count_with_ids(
      "SELECT COUNT(DISTINCT edge_id) FROM call_arg WHERE edge_id IN",
      edge_ids);
  total += count_with_ids("SELECT COUNT(DISTINCT src_def_id) FROM "
                          "possible_call WHERE src_def_id IN",
                          definition_ids);
  return total;
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
