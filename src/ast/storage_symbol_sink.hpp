// Storage-backed SymbolEmitter: writes SymbolRecords through the real
// cidx::Storage (add_symbol upsert, which also records decl_site rows) —
// the LT analogue of AstIndexer::store. The orchestrator sets the file_id of
// the file currently being walked (to_symbol's file_id parameter).
#pragma once

#include "ast/symbol_emitter.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cidx::storage {
struct AstStoragePorts;
}

namespace cidx::ast {

struct PassMetrics;

class StorageSymbolSink : public SymbolEmitter {
public:
  explicit StorageSymbolSink(cidx::storage::AstStoragePorts &ports);

  void set_current_file_id(int64_t file_id);
  void set_identity_translation_unit_config_id(
      int64_t config_id, int64_t translation_unit_file_id = -1);
  void set_identity_translation_unit_file_id(int64_t file_id);

  // index_file_notxn counters: a cursor whose symbol already exists RESOLVED
  // counts as skipped (AstIndexer::store semantics).
  void reset_counters();
  void set_metrics(PassMetrics *metrics);
  [[nodiscard]] int stored_count() const;
  [[nodiscard]] const std::vector<int64_t> &symbol_ids() const;

  void emit(const SymbolRecord &symbol) override;

private:
  struct CachedResolvedIdentity {
    int64_t symbol_id;
    std::optional<int64_t> file_id;
    std::optional<int64_t> line;
    std::optional<int64_t> col;
    std::optional<int64_t> end_line;
    std::optional<int64_t> end_col;
  };

  cidx::storage::AstStoragePorts &ports_;
  int64_t current_file_id_ = -1;
  std::optional<std::string> identity_translation_unit_;
  int stored_ = 0;
  std::vector<int64_t> symbol_ids_;
  PassMetrics *metrics_ = nullptr;
  std::unordered_set<int64_t> symbol_id_set_;
  std::unordered_map<std::string, CachedResolvedIdentity>
      resolved_identity_cache_;
  std::unordered_set<std::size_t> resolved_identity_cache_hashes_;
  bool resolved_cache_active_ = false;
};

} // namespace cidx::ast
