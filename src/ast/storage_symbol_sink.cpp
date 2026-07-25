#include "ast/storage_symbol_sink.hpp"

#include <algorithm>
#include <functional>
#include <string_view>

#include "ast/kind_map.hpp"
#include "ast/pass_registry.hpp"

#include "storage/ports.hpp"

namespace cidx::ast {

namespace {

constexpr std::size_t kSymbolIdSetThreshold = 32;

bool record_symbol_id(std::vector<int64_t> &ids,
                      std::unordered_set<int64_t> &set, int64_t id) {
  const bool repeated = ids.size() < kSymbolIdSetThreshold
                            ? std::ranges::find(ids, id) != ids.end()
                            : set.contains(id);
  if (ids.size() < kSymbolIdSetThreshold) {
    if (!repeated) {
      ids.push_back(id);
      if (ids.size() == kSymbolIdSetThreshold) {
        set.insert(ids.begin(), ids.end());
      }
    }
  } else if (!repeated) {
    set.insert(id);
    ids.push_back(id);
  }
  return repeated;
}

std::string identity_cache_key(const cidx::Symbol &sym) {
  std::string key = sym.usr;
  key.push_back('\x1f');
  key += sym.identity_source.value_or("");
  key.push_back('\x1f');
  key += sym.identity_translation_unit.value_or("");
  key.push_back('\x1f');
  key += std::to_string(sym.semantic_universe_id);
  return key;
}

std::size_t identity_cache_hash(const cidx::Symbol &sym) {
  std::size_t hash = 0xcbf29ce484222325ULL;
  const auto mix = [&hash](std::size_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
  };
  const auto string_hash = std::hash<std::string_view>{};
  mix(string_hash(sym.usr));
  mix(static_cast<std::size_t>(sym.identity_source.has_value()));
  if (sym.identity_source) {
    mix(string_hash(*sym.identity_source));
  }
  mix(static_cast<std::size_t>(sym.identity_translation_unit.has_value()));
  if (sym.identity_translation_unit) {
    mix(string_hash(*sym.identity_translation_unit));
  }
  mix(std::hash<int64_t>{}(sym.semantic_universe_id));
  return hash;
}

std::optional<cidx::Symbol>
lookup_existing_symbol(cidx::storage::AstStoragePorts &ports,
                       const cidx::Symbol &sym, bool resolved_identity) {
  if (resolved_identity) {
    return std::nullopt;
  }
  return ports.symbols_read.lookup_symbol(sym.usr, sym.semantic_universe_id,
                                          sym.identity_source,
                                          sym.identity_translation_unit);
}

cidx::Symbol
make_symbol(const SymbolRecord &s, int64_t current_file_id,
            int64_t semantic_universe_id,
            const std::optional<std::string> &identity_translation_unit) {
  cidx::Symbol sym;
  sym.usr = s.usr;
  sym.spelling = s.spelling;
  sym.kind = cidx_kind_name_from_int(s.kind);
  sym.qual_name = s.qual_name;
  sym.display_name = s.display_name;
  sym.type_info = s.type_info;
  sym.file_id = current_file_id;
  sym.line = s.line;
  sym.col = s.col;
  sym.end_line = s.end_line;
  sym.end_col = s.end_col;
  if (s.decl_line) {
    sym.decl_file_id = current_file_id;
    sym.decl_line = s.decl_line;
    sym.decl_col = s.decl_col;
  }
  sym.is_definition = s.is_definition;
  sym.is_pure = s.is_pure;
  sym.is_static = s.is_static;
  sym.is_instantiation = s.is_instantiation;
  sym.callable_kind = s.callable_kind;
  sym.template_origin = s.template_origin;
  sym.template_form = s.template_form;
  sym.linkage = s.linkage;
  sym.access = s.access;
  sym.parent_usr = s.parent_usr;
  sym.const_value = s.const_value;
  sym.resolved = s.resolved;
  sym.semantic_universe_id = semantic_universe_id;
  sym.identity_source = s.file;
  sym.identity_translation_unit = identity_translation_unit;
  return sym;
}

} // namespace

StorageSymbolSink::StorageSymbolSink(cidx::storage::AstStoragePorts &ports)
    : ports_(ports) {}

void StorageSymbolSink::set_current_file_id(int64_t file_id) {
  current_file_id_ = file_id;
  resolved_identity_cache_.clear();
  resolved_identity_cache_hashes_.clear();
  resolved_cache_active_ = false;
}

void StorageSymbolSink::set_identity_translation_unit_config_id(
    int64_t config_id, int64_t translation_unit_file_id) {
  identity_translation_unit_ =
      translation_unit_file_id >= 0
          ? ports_.workspace.portable_translation_unit_identity_for_config(
                config_id, translation_unit_file_id)
          : ports_.workspace.portable_translation_unit_identity_for_config(
                config_id);
  resolved_identity_cache_.clear();
  resolved_identity_cache_hashes_.clear();
  resolved_cache_active_ = false;
}

void StorageSymbolSink::set_identity_translation_unit_file_id(int64_t file_id) {
  identity_translation_unit_ =
      file_id >= 0
          ? std::optional<std::string>(
                ports_.workspace.portable_translation_unit_identity_for_file(
                    file_id))
          : std::nullopt;
  resolved_identity_cache_.clear();
  resolved_identity_cache_hashes_.clear();
  resolved_cache_active_ = false;
}

void StorageSymbolSink::reset_counters() {
  stored_ = 0;
  symbol_ids_.clear();
  symbol_id_set_.clear();
  resolved_identity_cache_.clear();
  resolved_identity_cache_hashes_.clear();
  resolved_cache_active_ = false;
}

void StorageSymbolSink::set_metrics(PassMetrics *metrics) {
  metrics_ = metrics;
}

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
    metrics_->note_emitted(1 + (s.decl_line ? 1 : 0));
  }

  const cidx::Symbol sym = make_symbol(
      s, current_file_id_,
      ports_.workspace.semantic_universe_for_file_id(current_file_id_),
      identity_translation_unit_);
  const auto same_decl_site = [&](const CachedResolvedIdentity &cached) {
    return cached.file_id == sym.file_id && cached.line == sym.line &&
           cached.col == sym.col && cached.end_line == sym.end_line &&
           cached.end_col == sym.end_col;
  };
  std::string identity_key;
  const bool resolved_identity =
      resolved_cache_active_ &&
      resolved_identity_cache_hashes_.contains(identity_cache_hash(sym)) &&
      [&] {
        identity_key = identity_cache_key(sym);
        const auto cached = resolved_identity_cache_.find(identity_key);
        return cached != resolved_identity_cache_.end() &&
               (!sym.is_definition || same_decl_site(cached->second));
      }();
  if (resolved_identity) {
    const int64_t symbol_id =
        resolved_identity_cache_.at(identity_key).symbol_id;
    ports_.symbols_write.add_decl_site(symbol_id, sym);
    record_symbol_id(symbol_ids_, symbol_id_set_, symbol_id);
    return;
  }
  const std::optional<cidx::Symbol> existing =
      lookup_existing_symbol(ports_, sym, resolved_identity);
  const int64_t symbol_id = ports_.symbols_write.add_symbol(sym);
  record_symbol_id(symbol_ids_, symbol_id_set_, symbol_id);
  if (!(resolved_identity || (existing && existing->resolved))) {
    ++stored_; // AstIndexer::store: true = counted as "stored"
  }
  if (existing && existing->resolved && existing->is_definition) {
    resolved_cache_active_ = true;
    identity_key = identity_cache_key(sym);
    resolved_identity_cache_.insert_or_assign(
        std::move(identity_key),
        CachedResolvedIdentity{.symbol_id = symbol_id,
                               .file_id = sym.file_id,
                               .line = sym.line,
                               .col = sym.col,
                               .end_line = sym.end_line,
                               .end_col = sym.end_col});
    resolved_identity_cache_hashes_.insert(identity_cache_hash(sym));
  }
}

} // namespace cidx::ast
