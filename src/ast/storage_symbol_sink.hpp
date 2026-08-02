// Storage-backed SymbolEmitter: writes SymbolRecords through the real
// cidx::Storage (add_symbol upsert, which also records decl_site rows) —
// the LT analogue of AstIndexer::store. The orchestrator sets the file_id of
// the file currently being walked (to_symbol's file_id parameter).
#pragma once

#include "ast/symbol_emitter.hpp"
#include "storage/records.hpp"

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
  // Publishes the sink and persistence spans this sink accumulated.
  ~StorageSymbolSink() override;
  StorageSymbolSink(const StorageSymbolSink &) = delete;
  StorageSymbolSink &operator=(const StorageSymbolSink &) = delete;
  StorageSymbolSink(StorageSymbolSink &&) = delete;
  StorageSymbolSink &operator=(StorageSymbolSink &&) = delete;

  void set_current_file_id(int64_t file_id);
  void set_identity_translation_unit_config_id(
      int64_t config_id, int64_t translation_unit_file_id = -1);
  void set_identity_translation_unit_file_id(int64_t file_id);

  // index_file_notxn counters: a cursor whose symbol already exists RESOLVED
  // counts as skipped (AstIndexer::store semantics).
  void reset_counters();
  void reset_all_counters();
  void set_metrics(PassMetrics *metrics);
  [[nodiscard]] int stored_count() const;
  [[nodiscard]] const std::vector<int64_t> &symbol_ids() const;
  [[nodiscard]] int stored_count(int64_t file_id) const;
  [[nodiscard]] const std::vector<int64_t> &symbol_ids(int64_t file_id) const;

  void emit(const SymbolRecord &symbol) override;

private:
  struct CachedResolvedIdentity {
    int64_t symbol_id;
    cidx::Symbol persisted;
  };
  struct FileBucket {
    int stored = 0;
    std::vector<int64_t> symbol_ids;
    std::unordered_set<int64_t> symbol_id_set;
  };

  // The semantic universe of a file is fixed by import; resolving it costs two
  // prepared statements, and every symbol of a file asks the same question.
  [[nodiscard]] int64_t semantic_universe_for_current_file();
  void clear_persisted_identities();
  void record_cached_decl_site(const cidx::Symbol &sym,
                               const std::string &identity_key,
                               FileBucket &bucket);
  void persist_symbol(const cidx::Symbol &sym, std::string identity_key,
                      FileBucket &bucket);

  cidx::storage::AstStoragePorts &ports_;
  int64_t current_file_id_ = -1;
  std::optional<std::string> identity_translation_unit_;
  std::unordered_map<int64_t, FileBucket> buckets_;
  PassMetrics *metrics_ = nullptr;
  // What this pass has already persisted, keyed by symbol identity. Bounded by
  // the symbols of the file being walked and cleared on every file switch and
  // counter reset.
  std::unordered_map<std::string, CachedResolvedIdentity>
      persisted_identity_cache_;
  std::unordered_set<std::size_t> persisted_identity_hashes_;
  std::uint64_t identity_reuses_ = 0;
  std::uint64_t identity_persists_ = 0;
  // Bounded by the routed files of one translation unit and cleared with the
  // rest of the per-translation-unit state by reset_all_counters().
  std::unordered_map<int64_t, int64_t> semantic_universe_by_file_;
  double sink_seconds_ = 0.0;
  double persistence_seconds_ = 0.0;
};

} // namespace cidx::ast
