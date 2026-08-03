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

} // namespace

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
