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

template <typename T> void coalesce(T &target, const T &incoming) {
  if (incoming.has_value()) {
    target = incoming;
  }
}

cidx::Symbol merged_symbol_state(const cidx::Symbol &persisted,
                                 const cidx::Symbol &incoming) {
  cidx::Symbol merged = persisted;
  merged.spelling = incoming.spelling;
  coalesce(merged.qual_name, incoming.qual_name);
  coalesce(merged.display_name, incoming.display_name);
  merged.kind = incoming.kind;
  coalesce(merged.type_info, incoming.type_info);
  if (!persisted.is_definition || incoming.is_definition) {
    merged.file_id = incoming.file_id;
    merged.line = incoming.line;
    merged.col = incoming.col;
    merged.end_line = incoming.end_line;
    merged.end_col = incoming.end_col;
  }
  coalesce(merged.decl_file_id, incoming.decl_file_id);
  coalesce(merged.decl_line, incoming.decl_line);
  coalesce(merged.decl_col, incoming.decl_col);
  merged.is_definition = persisted.is_definition || incoming.is_definition;
  merged.is_pure = persisted.is_pure || incoming.is_pure;
  merged.is_static = persisted.is_static || incoming.is_static;
  merged.is_instantiation = incoming.is_instantiation;
  coalesce(merged.linkage, incoming.linkage);
  coalesce(merged.access, incoming.access);
  coalesce(merged.parent_usr, incoming.parent_usr);
  coalesce(merged.callable_kind, incoming.callable_kind);
  coalesce(merged.template_origin, incoming.template_origin);
  coalesce(merged.template_form, incoming.template_form);
  merged.resolved = persisted.resolved || incoming.resolved;
  coalesce(merged.const_value, incoming.const_value);
  return merged;
}

bool same_add_symbol_result_except_decl_site(const cidx::Symbol &left,
                                             const cidx::Symbol &right) {
  // decl_path is deliberately absent: add_symbol() inserts it but never
  // updates it on conflict. The three decl_* fields are maintained by the
  // direct add_decl_site() operation below. Every other persisted field is
  // part of the conflict result and must match before bypassing add_symbol().
  return left.usr == right.usr && left.spelling == right.spelling &&
         left.kind == right.kind && left.qual_name == right.qual_name &&
         left.display_name == right.display_name &&
         left.type_info == right.type_info && left.file_id == right.file_id &&
         left.line == right.line && left.col == right.col &&
         left.end_line == right.end_line && left.end_col == right.end_col &&
         left.is_definition == right.is_definition &&
         left.is_pure == right.is_pure && left.is_static == right.is_static &&
         left.is_instantiation == right.is_instantiation &&
         left.linkage == right.linkage && left.access == right.access &&
         left.parent_usr == right.parent_usr &&
         left.callable_kind == right.callable_kind &&
         left.template_origin == right.template_origin &&
         left.template_form == right.template_form &&
         left.resolved == right.resolved &&
         left.const_value == right.const_value &&
         left.semantic_universe_id == right.semantic_universe_id &&
         left.identity_key == right.identity_key;
}

void apply_decl_site_to_cache(cidx::Symbol &persisted,
                              const cidx::Symbol &incoming) {
  coalesce(persisted.decl_file_id, incoming.decl_file_id);
  coalesce(persisted.decl_line, incoming.decl_line);
  coalesce(persisted.decl_col, incoming.decl_col);
}

bool cached_symbol_is_semantic_noop(const cidx::Symbol &persisted,
                                    const cidx::Symbol &incoming) {
  if (incoming.is_definition &&
      (persisted.file_id != incoming.file_id ||
       persisted.line != incoming.line || persisted.col != incoming.col ||
       persisted.end_line != incoming.end_line ||
       persisted.end_col != incoming.end_col)) {
    return false;
  }
  const cidx::Symbol merged = merged_symbol_state(persisted, incoming);
  return same_add_symbol_result_except_decl_site(persisted, merged);
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
  buckets_.erase(current_file_id_);
  resolved_identity_cache_.clear();
  resolved_identity_cache_hashes_.clear();
  resolved_cache_active_ = false;
}

void StorageSymbolSink::reset_all_counters() {
  buckets_.clear();
  resolved_identity_cache_.clear();
  resolved_identity_cache_hashes_.clear();
  resolved_cache_active_ = false;
}

void StorageSymbolSink::set_metrics(PassMetrics *metrics) {
  metrics_ = metrics;
}

int StorageSymbolSink::stored_count() const {
  return stored_count(current_file_id_);
}

const std::vector<int64_t> &StorageSymbolSink::symbol_ids() const {
  return symbol_ids(current_file_id_);
}

int StorageSymbolSink::stored_count(int64_t file_id) const {
  const auto bucket = buckets_.find(file_id);
  return bucket == buckets_.end() ? 0 : bucket->second.stored;
}

const std::vector<int64_t> &
StorageSymbolSink::symbol_ids(int64_t file_id) const {
  static const std::vector<int64_t> empty;
  const auto bucket = buckets_.find(file_id);
  return bucket == buckets_.end() ? empty : bucket->second.symbol_ids;
}

void StorageSymbolSink::emit(const SymbolRecord &s) {
  const char *kind_name = cidx_kind_name_from_int(s.kind);
  if (kind_name == nullptr) {
    return;
  }
  FileBucket &bucket = buckets_[current_file_id_];
  if (metrics_ != nullptr) {
    metrics_->note_emitted(1 + (s.decl_line ? 1 : 0));
  }

  const cidx::Symbol sym = make_symbol(
      s, current_file_id_,
      ports_.workspace.semantic_universe_for_file_id(current_file_id_),
      identity_translation_unit_);
  std::string identity_key;
  const bool resolved_identity =
      resolved_cache_active_ &&
      resolved_identity_cache_hashes_.contains(identity_cache_hash(sym)) &&
      [&] {
        identity_key = identity_cache_key(sym);
        const auto cached = resolved_identity_cache_.find(identity_key);
        if (cached == resolved_identity_cache_.end()) {
          return false;
        }
        return cached_symbol_is_semantic_noop(cached->second.persisted, sym);
      }();
  if (resolved_identity) {
    auto &cached = resolved_identity_cache_.at(identity_key);
    const int64_t symbol_id = cached.symbol_id;
    ports_.symbols_write.add_decl_site(symbol_id, sym);
    apply_decl_site_to_cache(cached.persisted, sym);
    record_symbol_id(bucket.symbol_ids, bucket.symbol_id_set, symbol_id);
    return;
  }
  const std::optional<cidx::Symbol> existing =
      lookup_existing_symbol(ports_, sym, resolved_identity);
  const int64_t symbol_id = ports_.symbols_write.add_symbol(sym);
  record_symbol_id(bucket.symbol_ids, bucket.symbol_id_set, symbol_id);
  if (!(resolved_identity || (existing && existing->resolved))) {
    ++bucket.stored; // AstIndexer::store: true = counted as "stored"
  }
  if (existing && existing->resolved && existing->is_definition) {
    resolved_cache_active_ = true;
    identity_key = identity_cache_key(sym);
    cidx::Symbol persisted = merged_symbol_state(*existing, sym);
    persisted.id = symbol_id;
    resolved_identity_cache_.insert_or_assign(
        std::move(identity_key),
        CachedResolvedIdentity{.symbol_id = symbol_id,
                               .persisted = std::move(persisted)});
    resolved_identity_cache_hashes_.insert(identity_cache_hash(sym));
  }
}

} // namespace cidx::ast
