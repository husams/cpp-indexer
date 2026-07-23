// Storage-backed EdgeSink: writes through the real cidx::Storage, so the LT
// edge pass produces byte-identical `edge`/`template_param`/`template_arg`
// rows in an index.db. The only file in the LT layer that includes storage
// headers.
#pragma once

#include "ast/edge_sink.hpp"

#include <unordered_map>

namespace cidx {
class Storage;
}

namespace cidx::ast {

class StorageEdgeSink : public EdgeSink {
public:
  explicit StorageEdgeSink(cidx::Storage &db);

  std::optional<int64_t>
  lookup_symbol_id(const std::string &usr,
                   const std::optional<std::string> &identity_source =
                       std::nullopt) override;
  void set_current_file_id(int64_t file_id) override;
  void set_identity_translation_unit_file_id(int64_t file_id) override;
  int64_t mint_symbol(const MintRequest &req) override;
  int64_t add_edge(const EdgeRecord &edge) override;
  int64_t ensure_edge(const EdgeRecord &edge) override;
  void add_edge_site(const EdgeSiteRecord &site) override;
  void add_call_arg(const CallArgRecord &arg) override;
  void add_template_param(const TemplateParamRecord &param) override;
  void add_template_arg(const TemplateArgRecord &arg) override;
  int64_t intern_type_node(const TypeNodeRecord &node) override;
  void add_type_edge(int64_t src_id, int64_t kind, int64_t position,
                     int64_t dst_id) override;
  void replace_parameters(int64_t owner_id,
                          const std::vector<ParameterRecord> &params) override;
  void add_symbol_type(int64_t symbol_id, int64_t kind,
                       int64_t type_id) override;

  void reset_fact_ids();
  [[nodiscard]] const std::vector<int64_t> &edge_ids() const {
    return edge_ids_;
  }
  [[nodiscard]] const std::vector<int64_t> &definition_ids() const {
    return definition_ids_;
  }
  void delete_edges_for_file(int64_t file_id) override;
  void delete_definitions_for_file(int64_t file_id) override;
  int64_t get_or_create_definition(
      int64_t symbol_id, int64_t file_id, int64_t line, int64_t col,
      int64_t end_line, int64_t end_col,
      const std::optional<std::string> &init_text) override;
  void add_def_edge(int64_t def_id, int64_t dst_id, int64_t kind) override;
  void copy_body_edges_to_def_edge(int64_t def_id, int64_t src_id) override;
  std::optional<int64_t> file_id_for_path(const std::string &path) override;
  std::vector<TypeArgCandidate> type_arg_candidates(const std::string &name,
                                                    bool qualified) override;
  std::optional<std::string> lookup_display_name(int64_t id) override;
  void update_display_name(int64_t id, const std::string &display) override;
  std::vector<int64_t>
  symbol_ids_by_qual_name_kind(const std::string &qual_name,
                               const std::string &kind_name) override;

private:
  cidx::Storage &db_;
  std::vector<int64_t> edge_ids_;
  std::vector<int64_t> definition_ids_;
  int64_t current_file_id_ = -1;
  std::optional<int64_t> current_universe_id_;
  std::optional<std::string> identity_translation_unit_;
  std::unordered_map<std::string, std::optional<int64_t>> lookup_cache_;
};

} // namespace cidx::ast
