// Focused typed extraction ports.
//
// EdgeSink remains the compatibility composition used by the current named
// visitors. These smaller ports make each responsibility independently
// substitutable for pass-level tests and future adapters.
#pragma once

#include "ast/edge_records.hpp"
#include "ast/fact_records.hpp"
#include "ast/symbol_emitter.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cidx::ast {

class DeclarationIdentityResolver {
public:
  virtual ~DeclarationIdentityResolver() = default;

  virtual auto lookup_symbol_id(
      const std::string &usr,
      const std::optional<std::string> &identity_source = std::nullopt)
      -> std::optional<std::int64_t> = 0;
  virtual auto mint_symbol(const MintRequest &request) -> std::int64_t = 0;
  virtual auto file_id_for_path(const std::string &path)
      -> std::optional<std::int64_t> = 0;
  virtual auto type_arg_candidates(const std::string &name, bool qualified)
      -> std::vector<TypeArgCandidate> = 0;
  virtual auto symbol_ids_by_qual_name_kind(const std::string &qual_name,
                                            const std::string &kind_name)
      -> std::vector<std::int64_t> = 0;
};

class RelationFactEmitter {
public:
  virtual ~RelationFactEmitter() = default;

  virtual auto add_edge(const EdgeRecord &edge) -> std::int64_t = 0;
  virtual auto ensure_edge(const EdgeRecord &edge) -> std::int64_t = 0;
  virtual void add_edge_site(const EdgeSiteRecord &site) = 0;
  virtual void add_call_arg(const CallArgRecord &arg) = 0;
  virtual void add_template_param(const TemplateParamRecord &param) = 0;
  virtual void add_template_arg(const TemplateArgRecord &arg) = 0;
};

class TypeFactEmitter {
public:
  virtual ~TypeFactEmitter() = default;

  virtual auto intern_type_node(const TypeNodeRecord &node) -> std::int64_t = 0;
  virtual void add_type_edge(std::int64_t src_id, std::int64_t kind,
                             std::int64_t position, std::int64_t dst_id) = 0;
  virtual void
  replace_parameters(std::int64_t owner_id,
                     const std::vector<ParameterRecord> &parameters) = 0;
  virtual void add_symbol_type(std::int64_t symbol_id, std::int64_t kind,
                               std::int64_t type_id) = 0;
};

class DefinitionScopeEmitter {
public:
  virtual ~DefinitionScopeEmitter() = default;

  virtual auto get_or_create_definition(
      std::int64_t symbol_id, std::int64_t file_id, std::int64_t line,
      std::int64_t col, std::int64_t end_line, std::int64_t end_col,
      const std::optional<std::string> &init_text) -> std::int64_t = 0;
  virtual void add_def_edge(std::int64_t definition_id,
                            std::int64_t destination_id, std::int64_t kind) = 0;
  virtual void copy_body_edges_to_def_edge(std::int64_t definition_id,
                                           std::int64_t symbol_id) = 0;
};

class EvidenceEmitter {
public:
  virtual ~EvidenceEmitter() = default;
  virtual void emit(const EvidenceRecord &evidence) = 0;
};

class IndexingLifecycle {
public:
  virtual ~IndexingLifecycle() = default;

  virtual void set_current_file_id(std::int64_t /*file_id*/) {}
  virtual void set_identity_translation_unit_config_id(
      std::int64_t /*config_id*/,
      std::int64_t /*translation_unit_file_id*/ = -1) {}
  virtual void set_identity_translation_unit_file_id(std::int64_t /*file_id*/) {
  }
  virtual void delete_edges_for_file(std::int64_t file_id) = 0;
  virtual void delete_definitions_for_file(std::int64_t file_id) = 0;
};

class PresentationNormalizer {
public:
  virtual ~PresentationNormalizer() = default;
  virtual auto lookup_display_name(std::int64_t symbol_id)
      -> std::optional<std::string> = 0;
  virtual void update_display_name(std::int64_t symbol_id,
                                   const std::string &display) = 0;
};

} // namespace cidx::ast
