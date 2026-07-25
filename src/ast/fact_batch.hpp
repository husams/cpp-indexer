// Immutable canonical fact batches and an in-memory recording implementation.
#pragma once

#include "ast/fact_emitters.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cidx::ast {

struct FactBatch {
  std::string producer;
  std::uint32_t producer_version = 1;
  FactCompleteness completeness = FactCompleteness::complete;
  std::vector<SymbolRecord> symbols;
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
  std::vector<EvidenceRecord> evidence;

  // Sorts and removes duplicate canonical records. It is intentionally
  // explicit so writers, rather than traversal order, own determinism.
  void canonicalize();
};

class FactBatchRecorder final : public SymbolFactEmitter,
                                public DeclarationIdentityResolver,
                                public RelationFactEmitter,
                                public TypeFactEmitter,
                                public DefinitionScopeEmitter,
                                public EvidenceEmitter,
                                public IndexingLifecycle,
                                public PresentationNormalizer {
public:
  explicit FactBatchRecorder(std::string producer = {});

  void emit(const SymbolRecord &symbol) override;
  void emit(const EvidenceRecord &evidence) override;

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
  void copy_body_edges_to_def_edge(std::int64_t definition_id,
                                   std::int64_t symbol_id) override;

  void delete_edges_for_file(std::int64_t /*file_id*/) override {}
  void delete_definitions_for_file(std::int64_t /*file_id*/) override {}

  auto lookup_display_name(std::int64_t symbol_id)
      -> std::optional<std::string> override;
  void update_display_name(std::int64_t symbol_id,
                           const std::string &display) override;

  [[nodiscard]] auto batch() const -> const FactBatch & { return batch_; }
  [[nodiscard]] auto canonical_batch() const -> FactBatch;

private:
  static auto edge_key(const EdgeRecord &edge) -> std::string;
  std::int64_t next_id_ = 1;
  FactBatch batch_;
  std::unordered_map<std::string, std::int64_t> symbol_ids_;
  std::unordered_map<std::string, std::int64_t> edge_ids_;
  std::unordered_map<std::string, std::int64_t> type_ids_;
  std::map<std::int64_t, std::string> display_names_;
};

} // namespace cidx::ast
