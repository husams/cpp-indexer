#include "ast/fact_batch.hpp"

#include "ast/kind_map.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace cidx::ast {

namespace {

template <typename T>
auto optional_text(const std::optional<T> &value) -> std::string {
  if (!value) {
    return "-";
  }
  if constexpr (std::same_as<T, std::string>) {
    return std::to_string(value->size()) + ':' + *value;
  } else {
    return std::to_string(*value);
  }
}

auto bool_text(bool value) -> std::string { return value ? "1" : "0"; }

auto portable_file_key(const PortableFileIdentity &file) -> std::string {
  return file.component_path + '\x1f' + file.directory_path + '\x1f' +
         file.file_name;
}

auto optional_file_key(const std::optional<PortableFileIdentity> &file)
    -> std::string {
  return file ? portable_file_key(*file) : "-";
}

template <typename Routed, typename T, typename Key, typename Memberships>
void append_records(const std::vector<Routed> &source,
                    std::vector<T> &destination, FactFamily family, Key key,
                    Memberships &memberships, bool canonical) {
  std::vector<Routed> routed = source;
  if (canonical) {
    std::ranges::sort(routed, [&key](const Routed &left, const Routed &right) {
      if (left.partition != right.partition) {
        return left.partition < right.partition;
      }
      return key(left.record) < key(right.record);
    });
    routed.erase(std::unique(routed.begin(), routed.end(),
                             [&key](const Routed &left, const Routed &right) {
                               return left.partition == right.partition &&
                                      key(left.record) == key(right.record);
                             }),
                 routed.end());
  }
  for (const Routed &item : routed) {
    const std::size_t index = destination.size();
    destination.push_back(item.record);
    memberships[item.partition][family].push_back(index);
  }
}

auto symbol_record_key(const SymbolRecord &record) -> std::string {
  return record.file + '\x1f' + record.usr + '\x1f' + record.spelling + '\x1f' +
         std::to_string(record.kind) + '\x1f' + record.kind_name + '\x1f' +
         optional_text(record.qual_name) + '\x1f' +
         optional_text(record.display_name) + '\x1f' +
         optional_text(record.type_info) + '\x1f' +
         std::to_string(record.line) + ':' + std::to_string(record.col) + ':' +
         std::to_string(record.end_line) + ':' +
         std::to_string(record.end_col) + ':' +
         optional_text(record.decl_line) + ':' +
         optional_text(record.decl_col) + ':' +
         bool_text(record.is_definition) + bool_text(record.is_pure) +
         bool_text(record.is_static) + bool_text(record.is_instantiation) +
         '\x1f' + optional_text(record.linkage) + '\x1f' +
         optional_text(record.access) + '\x1f' +
         optional_text(record.parent_usr) + '\x1f' +
         optional_text(record.const_value) + '\x1f' +
         bool_text(record.resolved);
}

auto edge_record_key(const EdgeRecord &record) -> std::string {
  return std::to_string(record.src_id) + ':' + std::to_string(record.dst_id) +
         ':' + std::to_string(record.kind) + ':' +
         std::to_string(record.count) + ':' +
         optional_text(record.base_access) + ':' +
         optional_text(record.is_virtual);
}

auto edge_site_key(const EdgeSiteRecord &record) -> std::string {
  return std::to_string(record.edge_id) + ':' + std::to_string(record.file_id) +
         ':' + std::to_string(record.line) + ':' + std::to_string(record.col) +
         ':' + std::to_string(record.conditional) + ':' +
         optional_text(record.recv_src_kind) + ':' +
         optional_text(record.recv_type_usr) + ':' +
         optional_text(record.recv_decl_usr) + ':' +
         optional_text(record.recv_param_pos) + ':' +
         optional_text(record.recv_type_is_value);
}

auto call_arg_key(const CallArgRecord &record) -> std::string {
  return std::to_string(record.edge_id) + ':' +
         std::to_string(record.position) + ':' +
         std::to_string(record.file_id) + ':' + std::to_string(record.line) +
         ':' + std::to_string(record.col) + ':' + record.src_kind + ':' +
         optional_text(record.type_usr) + ':' + optional_text(record.decl_usr) +
         ':' + optional_text(record.callee_usr) + ':' +
         optional_text(record.type_is_value);
}

auto template_param_key(const TemplateParamRecord &record) -> std::string {
  return std::to_string(record.owner_id) + ':' +
         std::to_string(record.position) + ':' +
         std::to_string(record.param_kind) + ':' + optional_text(record.name) +
         ':' + optional_text(record.default_txt) + ':' +
         optional_text(record.type_id) + ':' +
         optional_text(record.default_type_id) + ':' +
         optional_text(record.default_ref_id);
}

auto template_arg_key(const TemplateArgRecord &record) -> std::string {
  return std::to_string(record.owner_id) + ':' +
         std::to_string(record.position) + ':' +
         std::to_string(record.pack_index) + ':' +
         std::to_string(record.arg_kind) + ':' + optional_text(record.ref_id) +
         ':' + optional_text(record.literal) + ':' +
         optional_text(record.type_id);
}

auto type_node_key(const TypeNodeRecord &record) -> std::string {
  return record.type_key + '\x1f' + record.spelling + '\x1f' +
         std::to_string(record.kind) + ':' + bool_text(record.is_const) +
         bool_text(record.is_volatile) + bool_text(record.is_restrict) + ':' +
         optional_text(record.decl_usr) + ':' +
         optional_text(record.canonical_id) + ':' +
         optional_text(record.extent);
}

auto type_edge_key(const TypeEdgeRecord &record) -> std::string {
  return std::to_string(record.src_id) + ':' + std::to_string(record.kind) +
         ':' + std::to_string(record.position) + ':' +
         std::to_string(record.dst_id);
}

auto parameter_key(const ParameterFactRecord &record) -> std::string {
  const ParameterRecord &parameter = record.parameter;
  return std::to_string(record.owner_id) + ':' +
         std::to_string(parameter.position) + ':' +
         std::to_string(parameter.pack_index) + ':' +
         optional_text(parameter.name) + ':' +
         optional_text(parameter.type_id) + ':' +
         optional_text(parameter.declared_type_id) + ':' +
         optional_text(parameter.adjusted_type_id) + ':' +
         optional_text(parameter.default_text) + ':' +
         optional_text(parameter.default_origin) + ':' +
         optional_text(parameter.reference_semantics) + ':' +
         optional_text(parameter.file_id) + ':' +
         optional_text(parameter.line) + ':' + optional_text(parameter.col);
}

auto symbol_type_key(const SymbolTypeRecord &record) -> std::string {
  return std::to_string(record.symbol_id) + ':' + std::to_string(record.kind) +
         ':' + std::to_string(record.type_id);
}

auto definition_key(const DefinitionFactRecord &record) -> std::string {
  return std::to_string(record.id) + ':' + std::to_string(record.symbol_id) +
         ':' + std::to_string(record.file_id) + ':' +
         std::to_string(record.line) + ':' + std::to_string(record.col) + ':' +
         std::to_string(record.end_line) + ':' +
         std::to_string(record.end_col) + ':' + optional_text(record.init_text);
}

auto definition_edge_key(const DefinitionEdgeRecord &record) -> std::string {
  return std::to_string(record.definition_id) + ':' +
         std::to_string(record.destination_id) + ':' +
         std::to_string(record.kind);
}

auto evidence_key(const EvidenceRecord &record) -> std::string {
  return record.producer + '\x1f' + record.construct + '\x1f' + record.file +
         '\x1f' + std::to_string(record.line) + ':' +
         std::to_string(record.col) + ':' +
         std::to_string(static_cast<unsigned>(record.completeness)) + ':' +
         std::to_string(static_cast<unsigned>(record.trust)) + '\x1f' +
         record.detail;
}

auto canonical_symbol_order(
    const std::vector<SymbolEmissionMetadata> &emissions)
    -> std::vector<SymbolEmissionMetadata> {
  std::map<std::string, std::vector<SymbolEmissionMetadata>> groups;
  for (const SymbolEmissionMetadata &metadata : emissions) {
    groups[metadata.symbol.stable_string()].push_back(metadata);
  }
  using GroupOrder =
      std::tuple<std::string, std::string, std::string, std::string>;
  std::vector<std::pair<GroupOrder, std::vector<SymbolEmissionMetadata>>>
      ordered_groups;
  ordered_groups.reserve(groups.size());
  for (auto &[natural, entries] : groups) {
    std::ranges::sort(entries, {}, &SymbolEmissionMetadata::first_seen);
    const auto first_file = std::ranges::min_element(
        entries, {}, [](const SymbolEmissionMetadata &metadata) {
          return std::tuple(metadata.apply_order.component_path,
                            metadata.apply_order.directory_path,
                            metadata.apply_order.file_name);
        });
    ordered_groups.emplace_back(
        GroupOrder{first_file->apply_order.component_path,
                   first_file->apply_order.directory_path,
                   first_file->apply_order.file_name, natural},
        std::move(entries));
  }
  std::ranges::sort(
      ordered_groups, {},
      [](const auto &group) -> const GroupOrder & { return group.first; });
  std::vector<SymbolEmissionMetadata> result;
  result.reserve(emissions.size());
  for (auto &group : ordered_groups) {
    auto &entries = group.second;
    for (std::size_t index = 0; index < entries.size(); ++index) {
      entries[index].first_seen = index;
      entries[index].last_seen = index;
      entries[index].apply_order.first_seen = index;
      entries[index].apply_order.conflict_ordinal = index;
    }
    result.insert(result.end(), std::make_move_iterator(entries.begin()),
                  std::make_move_iterator(entries.end()));
  }
  return result;
}

auto candidate_key(const TypeArgCandidate &candidate) -> std::string {
  return std::to_string(candidate.id) + ':' + candidate.kind_name + ':' +
         (candidate.is_instantiation ? "1" : "0");
}

template <typename T, typename Key>
void append_indexed_unique(std::vector<T> &values,
                           std::unordered_set<Key> &membership, Key key,
                           const T &value,
                           FactBatchOperationCounters &counters) {
  counters.note("emit_candidate_membership", 1);
  if (membership.insert(std::move(key)).second) {
    values.push_back(value);
  }
}

auto file_identity_from_path(std::string_view value,
                             std::string_view component_path)
    -> PortableFileIdentity {
  std::string_view relative = value;
  const bool within_component = !component_path.empty() &&
                                value.size() > component_path.size() &&
                                value.starts_with(component_path) &&
                                (value[component_path.size()] == '/' ||
                                 value[component_path.size()] == '\\');
  if (within_component) {
    relative.remove_prefix(component_path.size() + 1);
  }
  const std::size_t separator = relative.find_last_of("/\\");
  const std::string directory =
      separator == std::string_view::npos
          ? std::string{}
          : std::string(relative.substr(0, separator));
  const std::string file = separator == std::string_view::npos
                               ? std::string(relative)
                               : std::string(relative.substr(separator + 1));
  return {.component_path =
              within_component ? std::string(component_path) : std::string{},
          .directory_path = directory,
          .file_name = file};
}

} // namespace

auto stable_symbol_record_key(const SymbolRecord &record) -> std::string {
  return symbol_record_key(record);
}

FactBatch::FactBatch() : data_(std::make_shared<const Data>()) {}

FactBatch::FactBatch(std::shared_ptr<const Data> data)
    : data_(std::move(data)) {}

auto FactBatch::producer() const -> const std::string & {
  return data_->producer;
}
auto FactBatch::producer_version() const -> std::uint32_t {
  return data_->producer_version;
}
auto FactBatch::completeness() const -> FactCompleteness {
  return data_->completeness;
}
auto FactBatch::records() const -> const FactRecords & {
  return data_->records;
}
auto FactBatch::partitions() const -> const std::vector<FileFactPartition> & {
  return data_->partitions;
}
auto FactBatch::symbol_keys() const
    -> const std::map<std::int64_t, std::string> & {
  return data_->symbol_keys;
}
auto FactBatch::relation_keys() const
    -> const std::map<std::int64_t, std::string> & {
  return data_->relation_keys;
}
auto FactBatch::type_keys() const
    -> const std::map<std::int64_t, std::string> & {
  return data_->type_keys;
}
auto FactBatch::definition_keys() const
    -> const std::map<std::int64_t, std::string> & {
  return data_->definition_keys;
}
auto FactBatch::file_keys() const
    -> const std::map<std::int64_t, FactPartitionKey> & {
  return data_->file_keys;
}

void FactBatchOperationCounters::note(std::string_view operation,
                                      std::uint64_t touched) {
  ++calls[std::string(operation)];
  records_touched[std::string(operation)] += touched;
}

FactBatchRecorder::FactBatchRecorder(
    std::string producer,
    const CollisionSafeHandleIndex::Hasher &primary_hasher)
    : producer_(std::move(producer)), symbol_handles_(primary_hasher),
      edge_handles_(primary_hasher), type_handles_(primary_hasher),
      definition_handles_(primary_hasher), file_handles_(primary_hasher) {}

void FactBatchRecorder::set_partition(
    FactPartitionKey partition,
    std::optional<std::int64_t> transient_file_handle) {
  current_partition_ = std::move(partition);
  const std::string path = current_partition_.file.portable_path();
  const std::int64_t natural_handle = file_handles_.find_or_insert(
      "file:" + current_partition_.stable_string());
  if (!path.empty()) {
    file_handles_by_path_[path] = natural_handle;
  }
  partitions_by_file_handle_[natural_handle] = current_partition_;
  if (transient_file_handle) {
    partitions_by_file_handle_[*transient_file_handle] = current_partition_;
    if (!path.empty()) {
      file_handles_by_path_[path] = *transient_file_handle;
    }
  }
}

auto FactBatchRecorder::partition_for_symbol(const SymbolRecord &symbol) const
    -> FactPartitionKey {
  FactPartitionKey result = current_partition_;
  if (!symbol.file.empty() && result.file.portable_path() != symbol.file) {
    if (const auto handle = file_handles_by_path_.find(symbol.file);
        handle != file_handles_by_path_.end()) {
      result = partition_for_file_handle(handle->second);
    } else {
      result.file =
          file_identity_from_path(symbol.file, result.file.component_path);
    }
  }
  if (!symbol.semantic_universe.empty()) {
    result.configuration.semantic_universe = symbol.semantic_universe;
  }
  if (!symbol.normalized_configuration.empty()) {
    result.configuration.normalized_configuration =
        symbol.normalized_configuration;
  }
  if (symbol.identity_source) {
    result.configuration.identity_source = *symbol.identity_source;
  } else if (result.configuration.identity_source.empty()) {
    result.configuration.identity_source = symbol.file;
  }
  if (symbol.identity_translation_unit) {
    result.configuration.translation_unit = *symbol.identity_translation_unit;
  }
  return result;
}

auto FactBatchRecorder::partition_for_file_handle(std::int64_t file_id) const
    -> FactPartitionKey {
  const auto found = partitions_by_file_handle_.find(file_id);
  return found == partitions_by_file_handle_.end() ? current_partition_
                                                   : found->second;
}

auto FactBatchRecorder::natural_key(const SymbolRecord &symbol,
                                    const FactPartitionKey &partition)
    -> SymbolNaturalKey {
  return {.partition = partition,
          .usr = symbol.usr,
          .local_anchor = symbol.local_anchor,
          .linkage = symbol.linkage};
}

auto FactBatchRecorder::source_lookup_key(const FactPartitionKey &partition,
                                          std::string_view source,
                                          std::string_view usr) -> std::string {
  return partition.configuration.semantic_universe + '\n' +
         partition.configuration.translation_unit + '\n' + std::string(source) +
         '\n' + std::string(usr);
}

auto FactBatchRecorder::scope_lookup_key(const FactPartitionKey &partition,
                                         std::string_view usr) -> std::string {
  return partition.configuration.semantic_universe + '\n' +
         partition.configuration.translation_unit + '\n' + std::string(usr);
}

auto FactBatchRecorder::name_kind_key(std::string_view name,
                                      std::string_view kind) -> std::string {
  return std::string(name) + '\n' + std::string(kind);
}

void FactBatchRecorder::emit(const SymbolRecord &symbol) {
  const FactPartitionKey partition = partition_for_symbol(symbol);
  const SymbolNaturalKey key = natural_key(symbol, partition);
  const std::int64_t id =
      symbol_handles_.find_or_insert("symbol:" + key.stable_string());
  const std::size_t position = symbols_.size();
  symbols_.push_back({.partition = partition, .record = symbol});
  symbol_positions_by_id_[id].push_back(position);

  const std::string source = symbol.identity_source.value_or(symbol.file);
  symbol_ids_by_source_usr_.try_emplace(
      source_lookup_key(partition, source, symbol.usr), id);
  symbol_ids_by_scope_usr_[scope_lookup_key(partition, symbol.usr)].insert(id);

  const char *mapped_kind = cidx_kind_name_from_int(symbol.kind);
  std::string kind = symbol.kind_name;
  if (kind.empty() && mapped_kind != nullptr) {
    kind = mapped_kind;
  }
  const TypeArgCandidate candidate{
      .id = id, .kind_name = kind, .is_instantiation = symbol.is_instantiation};
  append_indexed_unique(candidates_by_name_[symbol.spelling],
                        candidate_keys_by_name_[symbol.spelling],
                        candidate_key(candidate), candidate, counters_);
  if (symbol.qual_name) {
    append_indexed_unique(candidates_by_qualified_name_[*symbol.qual_name],
                          candidate_keys_by_qualified_name_[*symbol.qual_name],
                          candidate_key(candidate), candidate, counters_);
    const std::string qualified_kind = name_kind_key(*symbol.qual_name, kind);
    append_indexed_unique(
        symbol_ids_by_qualified_name_kind_[qualified_kind],
        symbol_id_keys_by_qualified_name_kind_[qualified_kind], id, id,
        counters_);
  }

  const std::uint64_t emission = next_emission_order_++;
  symbol_order_.push_back(
      {.symbol = key,
       .apply_order = {.component_path = partition.file.component_path,
                       .directory_path = partition.file.directory_path,
                       .file_name = partition.file.file_name,
                       .first_seen = emission,
                       .conflict_ordinal = static_cast<std::uint64_t>(
                           symbol_positions_by_id_[id].size() - 1)},
       .record_key = symbol_record_key(symbol),
       .first_seen = emission,
       .last_seen = emission});
  counters_.note("emit_symbol");
}

void FactBatchRecorder::emit(const EvidenceRecord &record) {
  evidence_.push_back({.partition = current_partition_, .record = record});
  counters_.note("emit_evidence");
}

void FactBatchRecorder::emit(const DeclarationSiteRecord &record) {
  declaration_sites_.push_back(
      {.partition = record.partition, .record = record});
  counters_.note("emit_declaration_site");
}

void FactBatchRecorder::emit(const IncludeDirectiveRecord &record) {
  includes_.push_back({.partition = record.partition, .record = record});
  counters_.note("emit_include");
}

void FactBatchRecorder::emit(const MacroUseRecord &record) {
  macros_.push_back({.partition = record.partition, .record = record});
  counters_.note("emit_macro");
}

void FactBatchRecorder::emit(const DiagnosticFactRecord &record) {
  diagnostics_.push_back({.partition = record.partition, .record = record});
  counters_.note("emit_diagnostic");
}

void FactBatchRecorder::emit(const LifecycleCleanupIntent &record) {
  lifecycle_cleanup_.push_back(
      {.partition = record.partition, .record = record});
  counters_.note("emit_lifecycle_cleanup");
}

void FactBatchRecorder::emit(const ApplicabilityOwnershipRecord &record) {
  applicability_.push_back({.partition = record.partition, .record = record});
  counters_.note("emit_applicability");
}

auto FactBatchRecorder::lookup_symbol_id(
    const std::string &usr, const std::optional<std::string> &identity_source)
    -> std::optional<std::int64_t> {
  counters_.note(identity_source ? "lookup_symbol_exact"
                                 : "lookup_symbol_sourceless");
  if (identity_source) {
    const auto found = symbol_ids_by_source_usr_.find(
        source_lookup_key(current_partition_, *identity_source, usr));
    if (found == symbol_ids_by_source_usr_.end()) {
      return std::nullopt;
    }
    ++counters_.records_touched["lookup_symbol_exact"];
    return found->second;
  }
  const auto found =
      symbol_ids_by_scope_usr_.find(scope_lookup_key(current_partition_, usr));
  if (found == symbol_ids_by_scope_usr_.end() || found->second.empty()) {
    return std::nullopt;
  }
  counters_.records_touched["lookup_symbol_sourceless"] +=
      std::min<std::size_t>(found->second.size(), 2);
  if (found->second.size() != 1) {
    throw std::runtime_error("ambiguous symbol USR within translation unit: " +
                             usr);
  }
  return *found->second.begin();
}

auto FactBatchRecorder::mint_symbol(const MintRequest &request)
    -> std::int64_t {
  std::optional<std::string> source = request.identity_source;
  if ((!source || source->empty()) && request.decl_path &&
      !request.decl_path->empty()) {
    source = request.decl_path;
  }
  if (source && source->empty()) {
    source.reset();
  }
  if (const auto existing = lookup_symbol_id(request.usr, source)) {
    return *existing;
  }
  SymbolRecord symbol;
  symbol.file = source.value_or("");
  symbol.usr = request.usr;
  symbol.spelling = request.spelling;
  symbol.kind = -1;
  symbol.qual_name = request.qual_name;
  symbol.display_name = request.display_name;
  symbol.type_info = request.type_info;
  symbol.decl_line = request.decl_line;
  symbol.decl_col = request.decl_col;
  symbol.is_instantiation = request.is_instantiation;
  symbol.linkage = request.linkage;
  symbol.identity_source = source;
  symbol.kind_name = request.kind_name;
  emit(symbol);
  const auto created = lookup_symbol_id(request.usr, source);
  if (!created) {
    throw std::logic_error("minted symbol is not indexed");
  }
  return *created;
}

auto FactBatchRecorder::file_id_for_path(const std::string &path)
    -> std::optional<std::int64_t> {
  counters_.note("lookup_file");
  const auto found = file_handles_by_path_.find(path);
  return found == file_handles_by_path_.end()
             ? std::nullopt
             : std::optional<std::int64_t>(found->second);
}

auto FactBatchRecorder::type_arg_candidates(const std::string &name,
                                            bool qualified)
    -> std::vector<TypeArgCandidate> {
  counters_.note("type_arg_candidates");
  const auto &index =
      qualified ? candidates_by_qualified_name_ : candidates_by_name_;
  const auto found = index.find(name);
  if (found == index.end()) {
    return {};
  }
  counters_.records_touched["type_arg_candidates"] += found->second.size();
  return found->second;
}

auto FactBatchRecorder::symbol_ids_by_qual_name_kind(
    const std::string &qual_name, const std::string &kind_name)
    -> std::vector<std::int64_t> {
  counters_.note("symbol_ids_by_qual_name_kind");
  const auto found = symbol_ids_by_qualified_name_kind_.find(
      name_kind_key(qual_name, kind_name));
  if (found == symbol_ids_by_qualified_name_kind_.end()) {
    return {};
  }
  counters_.records_touched["symbol_ids_by_qual_name_kind"] +=
      found->second.size();
  return found->second;
}

auto FactBatchRecorder::edge_key(const EdgeRecord &edge) -> std::string {
  return std::to_string(edge.src_id) + ':' + std::to_string(edge.dst_id) + ':' +
         std::to_string(edge.kind) + ':' + optional_text(edge.base_access) +
         ':' + optional_text(edge.is_virtual);
}

auto FactBatchRecorder::add_edge(const EdgeRecord &edge) -> std::int64_t {
  const std::string natural =
      current_partition_.stable_string() + edge_key(edge);
  const auto found = edge_positions_by_key_.find(natural);
  if (found != edge_positions_by_key_.end()) {
    relations_[found->second].record.count += edge.count;
    counters_.note("add_edge", 1);
    return edge_handles_.find_or_insert("edge:" + natural);
  }
  const std::int64_t id = edge_handles_.find_or_insert("edge:" + natural);
  const std::size_t position = relations_.size();
  edge_positions_by_key_.emplace(natural, position);
  relations_.push_back({.partition = current_partition_, .record = edge});
  if (edge.kind == 1 || edge.kind == 7) {
    body_edge_positions_by_source_[edge.src_id].push_back(position);
  }
  counters_.note("add_edge");
  return id;
}

auto FactBatchRecorder::ensure_edge(const EdgeRecord &edge) -> std::int64_t {
  const std::string natural =
      current_partition_.stable_string() + edge_key(edge);
  if (edge_positions_by_key_.contains(natural)) {
    counters_.note("ensure_edge");
    return edge_handles_.find_or_insert("edge:" + natural);
  }
  const EdgeRecord single{.src_id = edge.src_id,
                          .dst_id = edge.dst_id,
                          .kind = edge.kind,
                          .count = edge.count,
                          .base_access = edge.base_access,
                          .is_virtual = edge.is_virtual};
  return add_edge(single);
}

void FactBatchRecorder::add_edge_site(const EdgeSiteRecord &site) {
  edge_sites_.push_back(
      {.partition = partition_for_file_handle(site.file_id), .record = site});
  counters_.note("add_edge_site");
}

void FactBatchRecorder::add_call_arg(const CallArgRecord &arg) {
  call_args_.push_back(
      {.partition = partition_for_file_handle(arg.file_id), .record = arg});
  counters_.note("add_call_arg");
}

void FactBatchRecorder::add_template_param(const TemplateParamRecord &param) {
  template_params_.push_back(
      {.partition = current_partition_, .record = param});
  counters_.note("add_template_param");
}

void FactBatchRecorder::add_template_arg(const TemplateArgRecord &arg) {
  template_args_.push_back({.partition = current_partition_, .record = arg});
  counters_.note("add_template_arg");
}

auto FactBatchRecorder::intern_type_node(const TypeNodeRecord &node)
    -> std::int64_t {
  const TypeNaturalKey key{.partition = current_partition_,
                           .type_key = node.type_key};
  const std::string stable = "type:" + key.stable_string();
  if (const auto found = type_handles_.find(stable)) {
    counters_.note("intern_type_node", 1);
    return *found;
  }
  const std::int64_t id = type_handles_.find_or_insert(stable);
  type_nodes_.push_back({.partition = current_partition_, .record = node});
  counters_.note("intern_type_node");
  return id;
}

void FactBatchRecorder::add_type_edge(std::int64_t src_id, std::int64_t kind,
                                      std::int64_t position,
                                      std::int64_t dst_id) {
  type_edges_.push_back({.partition = current_partition_,
                         .record = {.src_id = src_id,
                                    .kind = kind,
                                    .position = position,
                                    .dst_id = dst_id}});
  counters_.note("add_type_edge");
}

void FactBatchRecorder::replace_parameters(
    std::int64_t owner_id, const std::vector<ParameterRecord> &parameters) {
  parameter_buckets_[owner_id] = {current_partition_, parameters};
  counters_.note("replace_parameters", parameters.size());
}

void FactBatchRecorder::add_symbol_type(std::int64_t symbol_id,
                                        std::int64_t kind,
                                        std::int64_t type_id) {
  symbol_types_.push_back(
      {.partition = current_partition_,
       .record = {.symbol_id = symbol_id, .kind = kind, .type_id = type_id}});
  counters_.note("add_symbol_type");
}

auto FactBatchRecorder::get_or_create_definition(
    std::int64_t symbol_id, std::int64_t file_id, std::int64_t line,
    std::int64_t col, std::int64_t end_line, std::int64_t end_col,
    const std::optional<std::string> &init_text) -> std::int64_t {
  const FactPartitionKey partition = partition_for_file_handle(file_id);
  const std::string natural =
      partition.stable_string() + ':' + std::to_string(symbol_id) + ':' +
      std::to_string(line) + ':' + std::to_string(col) + ':' +
      std::to_string(end_line) + ':' + std::to_string(end_col) + ':' +
      optional_text(init_text);
  if (const auto found = definition_ids_by_key_.find(natural);
      found != definition_ids_by_key_.end()) {
    counters_.note("get_or_create_definition", 1);
    return found->second;
  }
  const std::int64_t id =
      definition_handles_.find_or_insert("definition:" + natural);
  definition_ids_by_key_.emplace(natural, id);
  definition_partitions_by_id_.emplace(id, partition);
  definitions_.push_back({.partition = partition,
                          .record = {.id = id,
                                     .symbol_id = symbol_id,
                                     .file_id = file_id,
                                     .line = line,
                                     .col = col,
                                     .end_line = end_line,
                                     .end_col = end_col,
                                     .init_text = init_text}});
  counters_.note("get_or_create_definition");
  return id;
}

void FactBatchRecorder::add_def_edge(std::int64_t definition_id,
                                     std::int64_t destination_id,
                                     std::int64_t kind) {
  const auto partition = definition_partitions_by_id_.find(definition_id);
  const FactPartitionKey &owner_partition =
      partition == definition_partitions_by_id_.end() ? current_partition_
                                                      : partition->second;
  definition_edges_.push_back({.partition = owner_partition,
                               .record = {.definition_id = definition_id,
                                          .destination_id = destination_id,
                                          .kind = kind}});
  counters_.note("add_def_edge");
}

auto FactBatchRecorder::body_edge_count(std::int64_t symbol_id) -> std::size_t {
  const auto found = body_edge_positions_by_source_.find(symbol_id);
  const std::size_t count =
      found == body_edge_positions_by_source_.end() ? 0 : found->second.size();
  counters_.note("body_edge_count", count);
  return count;
}

void FactBatchRecorder::copy_body_edges_to_def_edge(std::int64_t definition_id,
                                                    std::int64_t symbol_id) {
  const auto found = body_edge_positions_by_source_.find(symbol_id);
  if (found == body_edge_positions_by_source_.end()) {
    counters_.note("copy_body_edges_to_def_edge");
    return;
  }
  counters_.note("copy_body_edges_to_def_edge", found->second.size());
  for (const std::size_t position : found->second) {
    const EdgeRecord &edge = relations_[position].record;
    add_def_edge(definition_id, edge.dst_id, edge.kind);
  }
}

void FactBatchRecorder::set_current_file_id(std::int64_t file_id) {
  current_partition_ = partition_for_file_handle(file_id);
}

void FactBatchRecorder::set_identity_translation_unit_config_id(
    std::int64_t /*config_id*/, std::int64_t translation_unit_file_id) {
  if (translation_unit_file_id >= 0) {
    current_partition_.configuration.translation_unit =
        partition_for_file_handle(translation_unit_file_id)
            .file.portable_path();
  }
}

void FactBatchRecorder::set_identity_translation_unit_file_id(
    std::int64_t file_id) {
  current_partition_.configuration.translation_unit =
      partition_for_file_handle(file_id).file.portable_path();
}

void FactBatchRecorder::delete_edges_for_file(std::int64_t file_id) {
  const FactPartitionKey partition = partition_for_file_handle(file_id);
  emit(LifecycleCleanupIntent{.partition = partition,
                              .kind = LifecycleCleanupKind::relations,
                              .target = partition.file});
}

void FactBatchRecorder::delete_definitions_for_file(std::int64_t file_id) {
  const FactPartitionKey partition = partition_for_file_handle(file_id);
  emit(LifecycleCleanupIntent{.partition = partition,
                              .kind = LifecycleCleanupKind::definitions,
                              .target = partition.file});
}

auto FactBatchRecorder::lookup_display_name(std::int64_t symbol_id)
    -> std::optional<std::string> {
  counters_.note("lookup_display_name");
  const auto found = display_names_.find(symbol_id);
  return found == display_names_.end()
             ? std::nullopt
             : std::optional<std::string>(found->second);
}

void FactBatchRecorder::update_display_name(std::int64_t symbol_id,
                                            const std::string &display) {
  display_names_[symbol_id] = display;
  const auto found = symbol_positions_by_id_.find(symbol_id);
  if (found == symbol_positions_by_id_.end()) {
    counters_.note("update_display_name");
    return;
  }
  counters_.note("update_display_name", found->second.size());
  for (const std::size_t position : found->second) {
    symbols_[position].record.display_name = display;
  }
}

void FactBatchRecorder::emit(const PresentationIntent &intent) {
  presentation_intents_.push_back(
      {.partition = current_partition_, .record = intent});
  counters_.note("emit_presentation_intent");
}

void FactBatchRecorder::append_symbol_records(FactBatch::Data &data,
                                              Memberships &memberships,
                                              bool canonical) const {
  append_records(symbols_, data.records.symbols, FactFamily::symbols,
                 symbol_record_key, memberships, canonical);
  append_records(
      declaration_sites_, data.records.declaration_sites,
      FactFamily::declaration_sites,
      [](const DeclarationSiteRecord &record) {
        return record.symbol.stable_string() + ':' +
               std::to_string(record.line) + ':' + std::to_string(record.col) +
               ':' + std::to_string(record.end_line) + ':' +
               std::to_string(record.end_col) + ':' +
               bool_text(record.is_definition);
      },
      memberships, canonical);
  append_records(relations_, data.records.relations, FactFamily::relations,
                 edge_record_key, memberships, canonical);
  append_records(edge_sites_, data.records.edge_sites, FactFamily::edge_sites,
                 edge_site_key, memberships, canonical);
  append_records(call_args_, data.records.call_args, FactFamily::call_arguments,
                 call_arg_key, memberships, canonical);
  append_records(template_params_, data.records.template_params,
                 FactFamily::template_parameters, template_param_key,
                 memberships, canonical);
  append_records(template_args_, data.records.template_args,
                 FactFamily::template_arguments, template_arg_key, memberships,
                 canonical);
}

void FactBatchRecorder::append_type_records(FactBatch::Data &data,
                                            Memberships &memberships,
                                            bool canonical) const {
  append_records(type_nodes_, data.records.type_nodes, FactFamily::types,
                 type_node_key, memberships, canonical);
  append_records(type_edges_, data.records.type_edges, FactFamily::type_edges,
                 type_edge_key, memberships, canonical);
  std::vector<RoutedRecord<ParameterFactRecord>> parameters;
  for (const auto &[owner, bucket] : parameter_buckets_) {
    for (const ParameterRecord &parameter : bucket.second) {
      parameters.push_back(
          {.partition = bucket.first,
           .record = {.owner_id = owner, .parameter = parameter}});
    }
  }
  append_records(parameters, data.records.parameters, FactFamily::parameters,
                 parameter_key, memberships, canonical);
  append_records(symbol_types_, data.records.symbol_types,
                 FactFamily::symbol_types, symbol_type_key, memberships,
                 canonical);
  append_records(definitions_, data.records.definitions,
                 FactFamily::definitions, definition_key, memberships,
                 canonical);
  append_records(definition_edges_, data.records.definition_edges,
                 FactFamily::definition_edges, definition_edge_key, memberships,
                 canonical);
}

void FactBatchRecorder::append_auxiliary_records(FactBatch::Data &data,
                                                 Memberships &memberships,
                                                 bool canonical) const {
  append_records(
      includes_, data.records.includes, FactFamily::includes,
      [](const IncludeDirectiveRecord &record) {
        return record.partition.stable_string() + ':' +
               portable_file_key(record.source) + ':' +
               optional_file_key(record.destination) + ':' +
               record.destination_path + ':' + record.spelling + ':' +
               std::to_string(std::to_underlying(record.directive)) + ':' +
               std::to_string(record.line) + ':' + std::to_string(record.col) +
               ':' + std::to_string(record.begin_offset) + ':' +
               std::to_string(record.end_offset) + ':' +
               record.conditional_fingerprint + ':' +
               bool_text(record.is_angled) + bool_text(record.resolved) +
               bool_text(record.is_system) + bool_text(record.guarded);
      },
      memberships, canonical);
  append_records(
      macros_, data.records.macros, FactFamily::macros,
      [](const MacroUseRecord &record) {
        return record.partition.stable_string() + ':' +
               portable_file_key(record.source) + ':' +
               optional_file_key(record.definition) + ':' +
               record.definition_path + ':' + record.name + ':' +
               std::to_string(record.count);
      },
      memberships, canonical);
  append_records(
      diagnostics_, data.records.diagnostics, FactFamily::diagnostics,
      [](const DiagnosticFactRecord &record) {
        return record.partition.stable_string() + ':' +
               std::to_string(static_cast<unsigned>(record.severity)) + ':' +
               record.spelling + ':' + optional_file_key(record.location_file) +
               ':' + optional_text(record.line) + ':' +
               optional_text(record.col);
      },
      memberships, canonical);
  append_records(evidence_, data.records.evidence, FactFamily::evidence,
                 evidence_key, memberships, canonical);
  append_records(
      presentation_intents_, data.records.presentation_intents,
      FactFamily::presentation_intents,
      [](const PresentationIntent &record) {
        std::string result = std::to_string(record.symbol_id);
        for (const std::string &argument : record.display_args) {
          result += ':' + std::to_string(argument.size()) + ':' + argument;
        }
        return result;
      },
      memberships, canonical);
  append_records(
      lifecycle_cleanup_, data.records.lifecycle_cleanup,
      FactFamily::lifecycle_cleanup,
      [](const LifecycleCleanupIntent &record) {
        return record.partition.stable_string() + ':' +
               std::to_string(static_cast<unsigned>(record.kind)) + ':' +
               record.target.portable_path() + ':' +
               record.prior_generation.token;
      },
      memberships, canonical);
  append_records(
      applicability_, data.records.applicability, FactFamily::applicability,
      [](const ApplicabilityOwnershipRecord &record) {
        return record.partition.stable_string() + ':' +
               record.file.portable_path() + ':' +
               std::to_string(static_cast<unsigned>(record.role)) + ':' +
               std::to_string(static_cast<unsigned>(record.state)) + ':' +
               optional_text(record.reason) + ':' + record.generation.token;
      },
      memberships, canonical);
}

auto FactBatchRecorder::build_batch(bool canonical) const -> FactBatch {
  auto data = std::make_shared<FactBatch::Data>();
  data->producer = producer_;
  data->symbol_keys = symbol_handles_.entries();
  data->relation_keys = edge_handles_.entries();
  data->type_keys = type_handles_.entries();
  data->definition_keys = definition_handles_.entries();
  data->file_keys.insert(partitions_by_file_handle_.begin(),
                         partitions_by_file_handle_.end());
  Memberships memberships;
  append_symbol_records(*data, memberships, canonical);
  append_type_records(*data, memberships, canonical);
  append_auxiliary_records(*data, memberships, canonical);
  data->records.symbol_order =
      canonical ? canonical_symbol_order(symbol_order_) : symbol_order_;
  for (auto &[partition, members] : memberships) {
    data->partitions.push_back(
        {.key = partition, .members = std::move(members)});
  }
  return FactBatch(std::move(data));
}

auto FactBatchRecorder::snapshot() const -> FactBatch {
  return build_batch(false);
}

auto FactBatchRecorder::canonical_batch() const -> FactBatch {
  return build_batch(true);
}

auto FactBatchRecorder::counters() const -> const FactBatchOperationCounters & {
  return counters_;
}

} // namespace cidx::ast
