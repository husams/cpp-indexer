// Focused persistence ports. These contracts expose domain records only; a
// consumer of a port does not need to know that SQLite is the current adapter.
#pragma once

#include "storage/records.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace cidx::storage {

class SqliteReadDb;

enum class FailurePoint : std::uint8_t {
  begin,
  adapter,
  partial_transform,
  commit,
};

class FailureInjector {
public:
  virtual ~FailureInjector() = default;
  virtual void inject(FailurePoint point) = 0;
};

class GraphReadPort {
public:
  virtual ~GraphReadPort() = default;
  virtual SqliteReadDb &read_db() = 0;
  virtual int64_t edge_count() = 0;
  virtual bool graph_resolved() = 0;
  virtual std::string component_abs_base(const Component &component) = 0;
  virtual std::optional<SemanticUniverse>
  get_semantic_universe_by_id(int64_t id) = 0;
  virtual std::optional<Symbol> graph_symbol_by_usr(const std::string &usr) = 0;
  virtual std::optional<Symbol> graph_symbol_by_id(int64_t id) = 0;
  virtual std::vector<Symbol> lookup_symbols_by_usr(const std::string &usr) = 0;
  virtual std::vector<Symbol>
  find_symbols(const std::string &pattern,
               const std::optional<std::string> &kind, int limit) = 0;
  virtual std::vector<GraphEdgeRow>
  graph_edges(int64_t mine_id, const std::string &direction,
              const std::vector<int64_t> &kind_ids, bool count_resolved,
              int limit) = 0;
  virtual std::map<int64_t, std::vector<EdgeSiteRow>>
  edge_sites_for(const std::vector<int64_t> &edge_ids) = 0;
  virtual std::vector<EdgeSiteRow> edge_sites_one(int64_t edge_id,
                                                  int limit) = 0;
  // Bounded, delivery-order-correct (path, line, col) page over one edge's
  // sites -- see SqliteStorageService::edge_sites_page().
  virtual std::vector<EdgeSiteRow> edge_sites_page(int64_t edge_id, int offset,
                                                   int limit) = 0;
  virtual std::optional<EdgeSiteRow> edge_site_by_key(int64_t edge_id,
                                                      int64_t file_id,
                                                      int64_t line,
                                                      int64_t col) = 0;
  // Whether ANY of an edge's sites is config-conditional -- an indexed
  // EXISTS aggregate over the edge's own (primary-key-bounded) site rows,
  // never materializing the site list, so this stays exact and cheap
  // regardless of how many sites the edge has.
  virtual bool edge_has_conditional_site(int64_t edge_id) = 0;
  // Exact edge lookup by (src_id, dst_id, kind) -- an indexed point lookup
  // against `edge`'s own UNIQUE(src_id, dst_id, kind) constraint, never a
  // bounded adjacency scan that can miss a real edge past its own limit.
  virtual std::optional<int64_t> edge_id_for(int64_t src_id, int64_t dst_id,
                                             int64_t kind) = 0;
  virtual std::vector<Symbol> redefined_symbols(int limit) = 0;
  virtual std::vector<DefinitionRow> definitions_of(int64_t symbol_id) = 0;
  virtual std::vector<DefinitionRow> possible_callees_of(int64_t symbol_id) = 0;
  virtual std::optional<TypeNode> type_node_by_id(int64_t id) = 0;
  virtual std::optional<int64_t> symbol_type_of(int64_t symbol_id,
                                                int64_t kind) = 0;
  virtual std::vector<Parameter> parameters_of(int64_t symbol_id) = 0;
  virtual std::vector<int64_t>
  type_ids_reaching(const std::string &decl_usr) = 0;
  virtual std::vector<std::pair<int64_t, int64_t>>
  param_owners_of_types(const std::vector<int64_t> &type_ids) = 0;
  virtual std::vector<std::pair<int64_t, int64_t>>
  symbol_type_owners_of_types(const std::vector<int64_t> &type_ids) = 0;
};

class WorkspaceCatalogReadPort {
public:
  virtual ~WorkspaceCatalogReadPort() = default;

  virtual std::optional<SemanticUniverse>
  get_semantic_universe_by_id(int64_t id) = 0;
  virtual std::optional<SemanticUniverse>
  get_semantic_universe_by_key(const std::string &key) = 0;
  virtual std::vector<SemanticUniverse> list_semantic_universes() = 0;
  virtual std::optional<Component> get_component(const std::string &path) = 0;
  virtual std::optional<Component> get_component_by_id(int64_t id) = 0;
  virtual std::optional<Component>
  component_for_path(const std::string &path) = 0;
  virtual std::vector<Component>
  list_components(const std::optional<std::string> &name = std::nullopt,
                  const std::optional<std::string> &kind = std::nullopt) = 0;
  virtual std::optional<std::string> get_alias(const std::string &name) = 0;
  virtual std::string portable_translation_unit_identity_for_config(
      int64_t config_id,
      std::optional<int64_t> translation_unit_file_id = std::nullopt) = 0;
  virtual std::string
  portable_translation_unit_identity_for_file(int64_t file_id) = 0;
  virtual int64_t semantic_universe_for_file_id(int64_t file_id) = 0;
  virtual std::optional<Repository> get_repository_by_id(int64_t id) = 0;
  virtual std::optional<Repository>
  get_repository_by_name(const std::string &name) = 0;
  virtual std::vector<Repository>
  list_repositories(const std::optional<std::string> &name = std::nullopt,
                    const std::optional<std::string> &kind = std::nullopt) = 0;
  virtual std::optional<Clone> get_clone_by_id(int64_t id) = 0;
  virtual std::optional<Clone> get_clone_by_path(const std::string &path) = 0;
  virtual std::vector<Clone>
  list_clones(const std::optional<int64_t> &repository_id = std::nullopt) = 0;
};

class WorkspaceCatalogWritePort {
public:
  virtual ~WorkspaceCatalogWritePort() = default;

  virtual int64_t add_semantic_universe(const std::string &key,
                                        const std::string &name,
                                        const std::string &policy) = 0;
  virtual int64_t add_component(const ComponentWriteRecord &component) = 0;
  virtual void delete_component(int64_t id) = 0;
  virtual int64_t add_repository(const RepositoryWriteRecord &repository) = 0;
  virtual void delete_repository(int64_t id) = 0;
  virtual int64_t add_clone(int64_t repository_id, const std::string &path,
                            const std::optional<std::string> &label) = 0;
  virtual void delete_clone(int64_t id) = 0;
};

class SourceStoreReadPort {
public:
  virtual ~SourceStoreReadPort() = default;

  virtual std::optional<File> get_file(const std::string &path) = 0;
  virtual std::optional<File> get_file_by_id(int64_t id) = 0;
  virtual std::optional<std::string> file_abs_path(int64_t id) = 0;
  virtual bool
  is_file_indexed(const std::string &path,
                  const std::optional<double> &mtime = std::nullopt,
                  const std::optional<std::string> &md5 = std::nullopt) = 0;
  virtual std::vector<Diagnostic> get_diagnostics(int64_t file_id) = 0;
  virtual std::map<int64_t, std::map<int, int64_t>> diagnostic_counts() = 0;
  virtual std::vector<FileConfigApplicability>
  file_configs_for(int64_t file_id) = 0;
  virtual std::optional<TranslationUnitConfig>
  translation_unit_config_by_id(int64_t config_id) = 0;
};

class SourceStoreWritePort {
public:
  virtual ~SourceStoreWritePort() = default;

  virtual int64_t
  add_file(int64_t directory_id, const std::string &name,
           const std::optional<double> &mtime = std::nullopt,
           const std::optional<std::string> &md5 = std::nullopt,
           const std::optional<std::vector<std::string>> &compile_options =
               std::nullopt,
           const std::optional<std::string> &driver = std::nullopt) = 0;
  virtual int64_t
  add_file_path(const std::string &path,
                const std::optional<double> &mtime = std::nullopt,
                const std::optional<std::string> &md5 = std::nullopt,
                const std::optional<std::vector<std::string>> &compile_options =
                    std::nullopt,
                const std::optional<std::string> &driver = std::nullopt) = 0;
  virtual void delete_file(int64_t id) = 0;
  virtual void
  mark_file_indexed(int64_t id,
                    const std::optional<double> &mtime = std::nullopt,
                    const std::optional<std::string> &md5 = std::nullopt) = 0;
  virtual void set_file_indexed(int64_t id, bool indexed) = 0;
  virtual void
  replace_diagnostics(int64_t file_id,
                      const std::vector<Diagnostic> &diagnostics) = 0;
};

class SymbolReadPort {
public:
  virtual ~SymbolReadPort() = default;

  virtual std::optional<Symbol> lookup_symbol(
      const std::string &usr,
      const std::optional<int64_t> &semantic_universe_id = std::nullopt,
      const std::optional<std::string> &identity_source = std::nullopt,
      const std::optional<std::string> &identity_translation_unit =
          std::nullopt) = 0;
  virtual std::optional<Symbol> lookup_symbol_by_id(int64_t id) = 0;
  virtual std::vector<Symbol> lookup_symbols_by_usr(
      const std::string &usr,
      const std::optional<int64_t> &semantic_universe_id = std::nullopt) = 0;
  virtual std::vector<Symbol> lookup_symbols_by_name(
      const std::string &name,
      const std::optional<std::string> &kind = std::nullopt,
      const std::optional<int64_t> &semantic_universe_id = std::nullopt) = 0;
  virtual std::vector<Symbol> lookup_symbols_by_qual_name(
      const std::string &name,
      const std::optional<std::string> &kind = std::nullopt,
      const std::optional<int64_t> &semantic_universe_id = std::nullopt) = 0;
  virtual std::vector<Symbol> symbols_in_file(int64_t file_id) = 0;
};

class SymbolWritePort {
public:
  virtual ~SymbolWritePort() = default;

  virtual int64_t add_symbol(const Symbol &symbol) = 0;
  virtual void add_decl_site(int64_t symbol_id, const Symbol &symbol) = 0;
  virtual int64_t mint_symbol_id(const SymbolIdentityRecord &symbol) = 0;
  virtual bool update_symbol_by_id(
      int64_t id,
      const std::vector<std::pair<std::string, SymbolValue>> &values) = 0;
  virtual void delete_symbol(int64_t id) = 0;
  virtual void delete_symbols_for_file(int64_t file_id) = 0;
};

class TypeReadPort {
public:
  virtual ~TypeReadPort() = default;

  virtual std::optional<TypeNode> type_node_by_id(int64_t id) = 0;
  virtual std::vector<Parameter> parameters_of(int64_t symbol_id) = 0;
  virtual std::optional<int64_t> symbol_type_of(int64_t symbol_id,
                                                int64_t kind) = 0;
};

class TypeWritePort {
public:
  virtual ~TypeWritePort() = default;

  virtual int64_t intern_type_node(const TypeNode &node) = 0;
  virtual void add_type_edge(int64_t src_id, int64_t kind, int64_t position,
                             int64_t dst_id) = 0;
  virtual void replace_parameters(int64_t owner_id,
                                  const std::vector<Parameter> &parameters) = 0;
  virtual void add_symbol_type(int64_t symbol_id, int64_t kind,
                               int64_t type_id) = 0;
};

class FactWritePort {
public:
  virtual ~FactWritePort() = default;

  virtual int64_t add_edge(const Edge &edge) = 0;
  virtual int64_t ensure_edge(const Edge &edge) = 0;
  virtual void add_edge_site(const EdgeSite &site) = 0;
  virtual void add_call_arg(const CallArg &arg) = 0;
  virtual void add_template_param(const TemplateParam &param) = 0;
  virtual void add_template_arg(const TemplateArg &arg) = 0;
};

class FactReadPort {
public:
  virtual ~FactReadPort() = default;

  virtual std::vector<GraphEdgeRecord>
  graph_edges(int64_t symbol_id, const std::string &direction,
              const std::vector<int64_t> &kind_ids, bool count_resolved,
              int limit) = 0;
  virtual std::map<int64_t, std::vector<GraphEdgeSiteRecord>>
  edge_sites_for(const std::vector<int64_t> &edge_ids) = 0;
};

class DefinitionReadPort {
public:
  virtual ~DefinitionReadPort() = default;

  virtual std::vector<DefinitionRecord> definitions_of(int64_t symbol_id) = 0;
  virtual std::vector<DefinitionRecord>
  possible_callees_of(int64_t symbol_id) = 0;
};

class DefinitionWritePort {
public:
  virtual ~DefinitionWritePort() = default;

  virtual int64_t get_or_create_definition(
      int64_t symbol_id, std::optional<int64_t> file_id,
      std::optional<int64_t> line, std::optional<int64_t> col,
      std::optional<int64_t> end_line, std::optional<int64_t> end_col,
      const std::optional<std::string> &init_text) = 0;
  virtual void add_def_edge(int64_t definition_id, int64_t destination_id,
                            int64_t kind) = 0;
  virtual auto body_edge_count(int64_t symbol_id) -> std::size_t = 0;
  virtual void copy_body_edges_to_def_edge(int64_t definition_id,
                                           int64_t symbol_id) = 0;
  virtual void delete_edges_for_file(int64_t file_id) = 0;
  virtual void delete_definitions_for_file(int64_t file_id) = 0;
};

class IncludeReadPort {
public:
  virtual ~IncludeReadPort() = default;

  virtual std::optional<IncludeConfig> include_config_by_id(int64_t id) = 0;
  virtual std::vector<IncludeConfig>
  include_configs_for_tu(int64_t file_id) = 0;
  virtual std::vector<IncludeEdge> include_edges_from(int64_t file_id,
                                                      bool include_system) = 0;
  virtual std::vector<IncludeSite> include_sites_for(int64_t edge_id) = 0;
};

class IncludeWritePort {
public:
  virtual ~IncludeWritePort() = default;

  virtual int64_t add_include_config(const IncludeConfig &config) = 0;
  virtual int64_t add_include_edge(const IncludeEdge &edge) = 0;
  virtual int64_t add_include_site(const IncludeSite &site) = 0;
  virtual void add_include_macro_use(const IncludeMacroUse &use) = 0;
  virtual void delete_include_configs_for_tu(int64_t file_id) = 0;
};

class SchemaCatalogReadPort {
public:
  virtual ~SchemaCatalogReadPort() = default;

  virtual Stats stats() = 0;
  virtual bool integrity_ok() = 0;
  virtual bool foreign_keys_ok() = 0;
  virtual bool graph_resolved() = 0;
};

class UnitOfWork {
public:
  virtual ~UnitOfWork() = default;
  virtual void commit() = 0;
  virtual void rollback() = 0;
};

class UnitOfWorkFactory {
public:
  virtual ~UnitOfWorkFactory() = default;
  virtual std::unique_ptr<UnitOfWork> begin() = 0;
};

// Capability bundle used by the AST extraction layer. It contains only the
// ports needed to publish one translation unit; the extractor never receives
// the Storage compatibility façade or a raw SQLite connection.
struct AstStoragePorts {
  WorkspaceCatalogReadPort &workspace;
  SourceStoreReadPort &source;
  SymbolReadPort &symbols_read;
  SymbolWritePort &symbols_write;
  TypeWritePort &types_write;
  FactWritePort &facts_write;
  DefinitionWritePort &definitions_write;
  UnitOfWorkFactory &unit_of_work;
};

} // namespace cidx::storage
