#include "ast/storage_symbol_sink.hpp"

#include <algorithm>

#include "ast/kind_map.hpp"
#include "ast/pass_registry.hpp"

#include "storage/ports.hpp"

namespace cidx::ast {

StorageSymbolSink::StorageSymbolSink(cidx::storage::AstStoragePorts &ports)
    : ports_(ports) {}

void StorageSymbolSink::set_current_file_id(int64_t file_id) {
  current_file_id_ = file_id;
}

void StorageSymbolSink::set_identity_translation_unit_config_id(
    int64_t config_id, int64_t translation_unit_file_id) {
  identity_translation_unit_ =
      translation_unit_file_id >= 0
          ? ports_.workspace.portable_translation_unit_identity_for_config(
                config_id, translation_unit_file_id)
          : ports_.workspace.portable_translation_unit_identity_for_config(
                config_id);
}

void StorageSymbolSink::set_identity_translation_unit_file_id(int64_t file_id) {
  identity_translation_unit_ =
      file_id >= 0
          ? std::optional<std::string>(
                ports_.workspace.portable_translation_unit_identity_for_file(
                    file_id))
          : std::nullopt;
}

void StorageSymbolSink::reset_counters() {
  stored_ = 0;
  symbol_ids_.clear();
}

void StorageSymbolSink::set_metrics(PassMetrics *metrics) { metrics_ = metrics; }

int StorageSymbolSink::stored_count() const { return stored_; }

const std::vector<int64_t> &StorageSymbolSink::symbol_ids() const {
  return symbol_ids_;
}

void StorageSymbolSink::emit(const SymbolRecord &s) {
  const char *kind_name = cidx_kind_name_from_int(s.kind);
  if (kind_name == nullptr) {
    return;
  }
  if (metrics_ != nullptr) {
    metrics_->note_emitted();
  }

  cidx::Symbol sym;
  sym.usr = s.usr;
  sym.spelling = s.spelling;
  sym.kind = kind_name;
  sym.qual_name = s.qual_name;
  sym.display_name = s.display_name;
  sym.type_info = s.type_info;
  sym.file_id = current_file_id_;
  sym.line = s.line;
  sym.col = s.col;
  sym.end_line = s.end_line;
  sym.end_col = s.end_col;
  if (s.decl_line) { // declarations record their own site (to_symbol)
    sym.decl_file_id = current_file_id_;
    sym.decl_line = s.decl_line;
    sym.decl_col = s.decl_col;
  }
  sym.is_definition = s.is_definition;
  sym.is_pure = s.is_pure;
  sym.is_static = s.is_static;
  sym.is_instantiation = s.is_instantiation;
  sym.linkage = s.linkage;
  sym.access = s.access;
  sym.parent_usr = s.parent_usr;
  sym.const_value = s.const_value;
  sym.resolved = s.resolved;
  sym.semantic_universe_id =
      ports_.workspace.semantic_universe_for_file_id(current_file_id_);
  sym.identity_source = s.file;
  sym.identity_translation_unit = identity_translation_unit_;
  const std::optional<cidx::Symbol> existing =
      ports_.symbols_read.lookup_symbol(sym.usr, sym.semantic_universe_id,
                                        sym.identity_source,
                                        sym.identity_translation_unit);
  const int64_t symbol_id = ports_.symbols_write.add_symbol(sym);
  if (std::ranges::find(symbol_ids_, symbol_id) == symbol_ids_.end()) {
    symbol_ids_.push_back(symbol_id);
  }
  if (!(existing && existing->resolved)) {
    ++stored_; // AstIndexer::store: true = counted as "stored"
  }
}

} // namespace cidx::ast
