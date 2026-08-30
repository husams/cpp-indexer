// Immutable canonical fact batches and a bounded in-memory builder.
#pragma once

#include "ast/fact_emitters.hpp"
#include "ast/fact_identity.hpp"
#include "ast/spillable_identity_index.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cidx::ast {

template <typename Record> class SpillableFactBuffer;

class FactBatchArtifactCodec;

enum class FactFamily : std::uint8_t {
  symbols,
  declaration_sites,
  relations,
  edge_sites,
  call_arguments,
  template_parameters,
  template_arguments,
  types,
  type_edges,
  parameters,
  symbol_types,
  definitions,
  definition_edges,
  includes,
  macros,
  diagnostics,
  evidence,
  presentation_intents,
  lifecycle_cleanup,
  applicability,
};

struct FactRecords {
  std::vector<SymbolRecord> symbols;
  std::vector<DeclarationSiteRecord> declaration_sites;
  std::vector<EdgeRecord> relations;
  std::vector<EdgeSiteRecord> edge_sites;
  std::vector<CallArgRecord> call_args;
  std::vector<TemplateParamRecord> template_params;
  std::vector<TemplateArgRecord> template_args;
  std::vector<TypeNodeRecord> type_nodes;
  std::vector<TypeEdgeRecord> type_edges;
  std::vector<ParameterFactRecord> parameters;
  std::vector<SymbolTypeRecord> symbol_types;
  std::vector<DefinitionFactRecord> definitions;
  std::vector<DefinitionEdgeRecord> definition_edges;
  std::vector<IncludeDirectiveRecord> includes;
  std::vector<MacroUseRecord> macros;
  std::vector<DiagnosticFactRecord> diagnostics;
  std::vector<EvidenceRecord> evidence;
  std::vector<PresentationIntent> presentation_intents;
  std::vector<LifecycleCleanupIntent> lifecycle_cleanup;
  std::vector<ApplicabilityOwnershipRecord> applicability;
  std::vector<SymbolEmissionMetadata> symbol_order;
};

struct FileFactPartition {
  FactPartitionKey key;
  // Each index addresses the corresponding immutable FactRecords vector.
  std::map<FactFamily, std::vector<std::size_t>> members;
};

// Resource limits shared by the extractor's fact payload and its transient
// natural-key indexes.  The defaults are deliberately bounded; tests may set
// a smaller limit to force the same spill path used by production workers.
struct ExtractionFactLimits {
  std::uint64_t spill_threshold_bytes = 64ULL * 1024ULL * 1024ULL;
  std::uint64_t max_total_bytes = 512ULL * 1024ULL * 1024ULL;
  std::uint64_t max_resident_identity_bytes = 32ULL * 1024ULL * 1024ULL;
  std::uint64_t max_identity_entries = 1'000'000ULL;
  std::filesystem::path spill_directory;
};

// A cross-translation-unit symbol identity that extraction could not answer
// from the facts it produced itself.
//
// Extraction records the question against a batch handle rather than reading
// the authoritative database mid-parse: the answer depends on what other
// translation units have published, so resolving it during extraction makes
// identity a function of parse timing. The controlled writer answers it at
// publication instead, where batches are applied serially in the legacy apply
// order, which makes the answer a function of that order alone.
//
// A reference that still does not resolve at publication is not an error: it
// is the deferred form of the serial lookup returning nothing, and the facts
// that depend on it are dropped exactly as the serial visitor never emitted
// them.
struct PendingSymbolReference {
  std::int64_t handle = 0;
  std::string usr;
  // Empty when the probe was source-independent.
  std::string identity_source;
  std::string semantic_universe;
  std::string translation_unit;

  [[nodiscard]] auto stable_string() const -> std::string;
  // clang-format off
  // clang-format 22 rewrites `operator<=>` to `operator<= >`, which does not
  // compile; this is a formatter defect, not a style choice.
  friend auto operator<=>(const PendingSymbolReference &,
                          const PendingSymbolReference &) = default;
  // clang-format on
};

class FactBatch {
public:
  FactBatch();

  [[nodiscard]] auto producer() const -> const std::string &;
  [[nodiscard]] auto producer_version() const -> std::uint32_t;
  [[nodiscard]] auto completeness() const -> FactCompleteness;
  [[nodiscard]] auto is_canonical() const -> bool;
  [[nodiscard]] auto records() const -> const FactRecords &;
  [[nodiscard]] auto partitions() const
      -> const std::vector<FileFactPartition> &;
  [[nodiscard]] auto symbol_keys() const
      -> const std::map<std::int64_t, std::string> &;
  [[nodiscard]] auto relation_keys() const
      -> const std::map<std::int64_t, std::string> &;
  [[nodiscard]] auto type_keys() const
      -> const std::map<std::int64_t, std::string> &;
  [[nodiscard]] auto definition_keys() const
      -> const std::map<std::int64_t, std::string> &;
  [[nodiscard]] auto file_keys() const
      -> const std::map<std::int64_t, FactPartitionKey> &;
  [[nodiscard]] auto pending_symbol_references() const
      -> const std::vector<PendingSymbolReference> &;

private:
  struct Data {
    std::string producer;
    std::uint32_t producer_version = 1;
    FactCompleteness completeness = FactCompleteness::complete;
    bool canonical = false;
    FactRecords records;
    std::vector<FileFactPartition> partitions;
    std::map<std::int64_t, std::string> symbol_keys;
    std::map<std::int64_t, std::string> relation_keys;
    std::map<std::int64_t, std::string> type_keys;
    std::map<std::int64_t, std::string> definition_keys;
    std::map<std::int64_t, FactPartitionKey> file_keys;
    std::vector<PendingSymbolReference> pending_symbol_references;
  };

  explicit FactBatch(std::shared_ptr<const Data> data);
  [[nodiscard]] auto has_canonical_layout() const -> bool;
  std::shared_ptr<const Data> data_;

  friend class FactBatchRecorder;
  friend class FactBatchArtifactCodec;
};

struct FactBatchOperationCounters {
  std::map<std::string, std::uint64_t> calls;
  std::map<std::string, std::uint64_t> records_touched;

  void note(std::string_view operation, std::uint64_t touched = 0);
};

[[nodiscard]] auto stable_symbol_record_key(const SymbolRecord &record)
    -> std::string;

// Authoritative FactBatchRecorder operation audit (T-052).
// `k` is the selected keyed bucket, `r` is returned/copied output, and `n` is
// the whole batch. Hash-table bounds are amortised; ordered maps are O(log n).
//
// Operation                                          Bound
// set_partition                                      O(log n)
// set_completeness                                   O(1)
// emit(SymbolRecord), mint_symbol                    O(log n)
// emit(EvidenceRecord/DeclarationSiteRecord)         amortised O(1)
// emit(IncludeDirectiveRecord/MacroUseRecord)        amortised O(1)
// emit(DiagnosticFactRecord/LifecycleCleanupIntent)  amortised O(1)
// emit(ApplicabilityOwnershipRecord/PresentationIntent) amortised O(1)
// lookup_symbol_id                                   amortised O(1)
// file_id_for_path                                   amortised O(1)
// type_arg_candidates, symbol_ids_by_qual_name_kind  O(r)
// add_edge, ensure_edge                              O(log n)
// add_edge_site, add_call_arg                        O(log n)
// add_template_param, add_template_arg               amortised O(1)
// intern_type_node                                   O(log n)
// add_type_edge, add_symbol_type, add_def_edge       amortised O(1)
// replace_parameters                                 O(log n + input)
// get_or_create_definition                           O(log n)
// body_edge_count                                    amortised O(1)
// copy_body_edges_to_def_edge                        O(k)
// set_current_file_id                                O(log n)
// set_identity_translation_unit_config_id            O(log n)
// set_identity_translation_unit_file_id              O(log n)
// delete_edges_for_file, delete_definitions_for_file O(log n)
// lookup_display_name                                O(log n)
// update_display_name                                O(log n + k)
// snapshot, batch, canonical_batch                   O(n log n)
// counters                                           O(1)
//
// No emission scans a fact-family vector. Work above logarithmic is confined
// to caller input, returned output, one natural-key bucket, or finalization.
class FactBatchRecorder final : public SymbolFactEmitter,
                                public StatementFactPorts,
                                public DeclarationPassPorts,
                                public NamespacePassPorts,
                                public DefinitionScopeEmitter,
                                public IndexingLifecycle,
                                public PresentationNormalizer,
                                public PresentationIntentEmitter {
public:
  // Cross-translation-unit identity resolution deferred to the controlled
  // writer. With this enabled the recorder answers an unresolved lookup with a
  // batch handle and a recorded PendingSymbolReference, and performs no
  // database access of any kind.
  struct DeferredExternalIdentity {
    bool enabled = false;
    // The main file of the translation unit being extracted, in the identity
    // spelling the visitors use. The database-backed lookup deliberately
    // refused to answer for this unit's own declarations -- a previous
    // publication of the unit being replaced is stale input, not an external
    // identity oracle -- and deferral preserves that refusal without a read.
    std::string translation_unit_path;
  };

  explicit FactBatchRecorder(std::string producer = {},
                             const CollisionSafeHandleIndex::Hasher
                                 &primary_hasher = stable_fact_hash);
  ~FactBatchRecorder() override;

  FactBatchRecorder(const FactBatchRecorder &) = delete;
  auto operator=(const FactBatchRecorder &) -> FactBatchRecorder & = delete;
  FactBatchRecorder(FactBatchRecorder &&) = delete;
  auto operator=(FactBatchRecorder &&) -> FactBatchRecorder & = delete;

  void set_extraction_limits(ExtractionFactLimits limits);
  void set_completeness(FactCompleteness completeness);
  void set_deferred_external_identity(DeferredExternalIdentity deferral);
  void set_partition(
      FactPartitionKey partition,
      std::optional<std::int64_t> transient_file_handle = std::nullopt);
  void emit(const SymbolRecord &symbol) override;
  void emit(const EvidenceRecord &record) override;
  void emit(const DeclarationSiteRecord &record);
  void emit(const IncludeDirectiveRecord &record);
  void emit(const MacroUseRecord &record);
  void emit(const DiagnosticFactRecord &record);
  void emit(const LifecycleCleanupIntent &record);
  void emit(const ApplicabilityOwnershipRecord &record);

  auto lookup_symbol_id(
      const std::string &usr,
      const std::optional<std::string> &identity_source = std::nullopt)
      -> std::optional<std::int64_t> override;
  auto mint_symbol(const MintRequest &request) -> std::int64_t override;
  auto file_id_for_path(const std::string &path)
      -> std::optional<std::int64_t> override;
  auto type_arg_candidates(const std::string &name, bool qualified)
      -> std::vector<TypeArgCandidate> override;
  auto symbol_ids_by_qual_name_kind(const std::string &qual_name,
                                    const std::string &kind_name)
      -> std::vector<std::int64_t> override;

  auto add_edge(const EdgeRecord &edge) -> std::int64_t override;
  auto ensure_edge(const EdgeRecord &edge) -> std::int64_t override;
  void add_edge_site(const EdgeSiteRecord &site) override;
  void add_call_arg(const CallArgRecord &arg) override;
  void add_template_param(const TemplateParamRecord &param) override;
  void add_template_arg(const TemplateArgRecord &arg) override;

  auto intern_type_node(const TypeNodeRecord &node) -> std::int64_t override;
  void add_type_edge(std::int64_t src_id, std::int64_t kind,
                     std::int64_t position, std::int64_t dst_id) override;
  void
  replace_parameters(std::int64_t owner_id,
                     const std::vector<ParameterRecord> &parameters) override;
  void add_symbol_type(std::int64_t symbol_id, std::int64_t kind,
                       std::int64_t type_id) override;

  auto get_or_create_definition(std::int64_t symbol_id, std::int64_t file_id,
                                std::int64_t line, std::int64_t col,
                                std::int64_t end_line, std::int64_t end_col,
                                const std::optional<std::string> &init_text)
      -> std::int64_t override;
  void add_def_edge(std::int64_t definition_id, std::int64_t destination_id,
                    std::int64_t kind) override;
  auto body_edge_count(std::int64_t symbol_id) -> std::size_t override;
  void copy_body_edges_to_def_edge(std::int64_t definition_id,
                                   std::int64_t symbol_id) override;

  void set_current_file_id(std::int64_t file_id) override;
  void set_identity_translation_unit_config_id(
      std::int64_t config_id,
      std::int64_t translation_unit_file_id = -1) override;
  void set_identity_translation_unit_file_id(std::int64_t file_id) override;
  void delete_edges_for_file(std::int64_t file_id) override;
  void delete_definitions_for_file(std::int64_t file_id) override;

  auto lookup_display_name(std::int64_t symbol_id)
      -> std::optional<std::string> override;
  void update_display_name(std::int64_t symbol_id,
                           const std::string &display) override;
  void emit(const PresentationIntent &intent) override;

  [[nodiscard]] auto snapshot() const -> FactBatch;
  [[nodiscard]] auto batch() const -> FactBatch { return snapshot(); }
  [[nodiscard]] auto canonical_batch() const -> FactBatch;
  [[nodiscard]] auto counters() const -> const FactBatchOperationCounters &;
  [[nodiscard]] auto fact_payload_spilled() const -> bool;
  [[nodiscard]] auto identity_index_spilled() const -> bool;
  [[nodiscard]] auto pending_presentation_intents() const
      -> std::vector<PresentationIntent> {
    std::vector<PresentationIntent> result;
    result.reserve(presentation_intents_.size());
    std::ranges::transform(presentation_intents_, std::back_inserter(result),
                           &RoutedRecord<PresentationIntent>::record);
    return result;
  }

private:
  template <typename T> struct RoutedRecord {
    FactPartitionKey partition;
    T record;
  };

  using RoutedSymbol = RoutedRecord<SymbolRecord>;
  using RoutedEdge = RoutedRecord<EdgeRecord>;
  using ParameterBucket =
      std::pair<FactPartitionKey, std::vector<ParameterRecord>>;
  using Memberships = std::map<FactPartitionKey,
                               std::map<FactFamily, std::vector<std::size_t>>>;

  [[nodiscard]] auto partition_for_symbol(const SymbolRecord &symbol) const
      -> FactPartitionKey;
  [[nodiscard]] auto partition_for_file_handle(std::int64_t file_id) const
      -> FactPartitionKey;
  [[nodiscard]] static auto natural_key(const SymbolRecord &symbol,
                                        const FactPartitionKey &partition)
      -> SymbolNaturalKey;
  [[nodiscard]] static auto source_lookup_key(const FactPartitionKey &partition,
                                              std::string_view source,
                                              std::string_view usr)
      -> std::string;
  [[nodiscard]] static auto scope_lookup_key(const FactPartitionKey &partition,
                                             std::string_view usr)
      -> std::string;
  [[nodiscard]] static auto name_kind_key(std::string_view name,
                                          std::string_view kind) -> std::string;
  [[nodiscard]] auto source_independent_symbol_id(std::string_view usr)
      -> std::optional<std::int64_t>;
  void enrich_minted_symbol(std::int64_t symbol_id, const MintRequest &request);
  void add_mint_declaration(const std::vector<std::size_t> &positions,
                            const MintRequest &request);
  [[nodiscard]] auto
  create_minted_symbol(const MintRequest &request,
                       const std::optional<std::string> &source)
      -> std::int64_t;
  [[nodiscard]] auto
  defer_external_symbol(const std::string &usr,
                        const std::optional<std::string> &source)
      -> std::optional<std::int64_t>;
  [[nodiscard]] auto
  resolve_symbol_id(const std::string &usr,
                    const std::optional<std::string> &identity_source,
                    bool allow_deferral) -> std::optional<std::int64_t>;
  [[nodiscard]] static auto edge_key(const EdgeRecord &edge) -> std::string;
  void note_identity(FactIdentityKind kind, std::string key,
                     std::int64_t handle);
  [[nodiscard]] auto build_batch(bool canonical) const -> FactBatch;
  void append_symbol_records(FactBatch::Data &data, Memberships &memberships,
                             bool canonical) const;
  void append_type_records(FactBatch::Data &data, Memberships &memberships,
                           bool canonical) const;
  void append_auxiliary_records(FactBatch::Data &data, Memberships &memberships,
                                bool canonical) const;

  std::string producer_;
  FactCompleteness completeness_ = FactCompleteness::complete;
  FactPartitionKey current_partition_;
  std::map<std::int64_t, FactPartitionKey> partitions_by_file_handle_;
  std::unordered_map<std::string, std::int64_t> file_handles_by_path_;

  std::vector<RoutedSymbol> symbols_;
  std::vector<RoutedRecord<DeclarationSiteRecord>> declaration_sites_;
  std::vector<RoutedEdge> relations_;
  std::vector<RoutedRecord<EdgeSiteRecord>> edge_sites_;
  std::vector<RoutedRecord<CallArgRecord>> call_args_;
  std::vector<RoutedRecord<TemplateParamRecord>> template_params_;
  std::vector<RoutedRecord<TemplateArgRecord>> template_args_;
  std::vector<RoutedRecord<TypeNodeRecord>> type_nodes_;
  std::vector<RoutedRecord<TypeEdgeRecord>> type_edges_;
  std::map<std::int64_t, ParameterBucket> parameter_buckets_;
  std::vector<RoutedRecord<SymbolTypeRecord>> symbol_types_;
  std::vector<RoutedRecord<DefinitionFactRecord>> definitions_;
  std::vector<RoutedRecord<DefinitionEdgeRecord>> definition_edges_;
  std::vector<RoutedRecord<IncludeDirectiveRecord>> includes_;
  std::vector<RoutedRecord<MacroUseRecord>> macros_;
  std::vector<RoutedRecord<DiagnosticFactRecord>> diagnostics_;
  std::vector<RoutedRecord<EvidenceRecord>> evidence_;
  std::vector<RoutedRecord<PresentationIntent>> presentation_intents_;
  std::vector<RoutedRecord<LifecycleCleanupIntent>> lifecycle_cleanup_;
  std::vector<RoutedRecord<ApplicabilityOwnershipRecord>> applicability_;
  std::vector<SymbolEmissionMetadata> symbol_order_;

  CollisionSafeHandleIndex symbol_handles_;
  CollisionSafeHandleIndex edge_handles_;
  CollisionSafeHandleIndex type_handles_;
  CollisionSafeHandleIndex definition_handles_;
  CollisionSafeHandleIndex file_handles_;
  DeferredExternalIdentity deferred_external_identity_;
  std::vector<PendingSymbolReference> pending_symbol_references_;
  std::unordered_map<std::string, std::int64_t> external_ids_by_key_;
  std::unordered_map<std::string, std::int64_t> symbol_ids_by_source_usr_;
  std::unordered_map<std::string, std::set<std::int64_t>>
      symbol_ids_by_scope_usr_;
  std::unordered_map<std::int64_t, std::vector<std::size_t>>
      symbol_positions_by_id_;
  std::unordered_map<std::string, std::vector<TypeArgCandidate>>
      candidates_by_name_;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      candidate_keys_by_name_;
  std::unordered_map<std::string, std::vector<TypeArgCandidate>>
      candidates_by_qualified_name_;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      candidate_keys_by_qualified_name_;
  std::unordered_map<std::string, std::vector<std::int64_t>>
      symbol_ids_by_qualified_name_kind_;
  std::unordered_map<std::string, std::unordered_set<std::int64_t>>
      symbol_id_keys_by_qualified_name_kind_;
  std::unordered_map<std::string, std::size_t> edge_positions_by_key_;
  std::unordered_map<std::int64_t, std::vector<std::size_t>>
      body_edge_positions_by_source_;
  std::unordered_map<std::string, std::int64_t> definition_ids_by_key_;
  std::unordered_map<std::int64_t, FactPartitionKey>
      definition_partitions_by_id_;
  std::map<std::int64_t, std::string> display_names_;
  std::uint64_t next_emission_order_ = 0;
  FactBatchOperationCounters counters_;
  ExtractionFactLimits extraction_limits_;
  SpillableIdentityIndex identity_index_;
  std::unique_ptr<SpillableFactBuffer<std::string>> fact_payload_;
};

} // namespace cidx::ast
