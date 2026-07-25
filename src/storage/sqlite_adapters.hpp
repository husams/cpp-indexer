// SQLite bindings for the focused persistence ports.
//
// This header is an adapter boundary: consumers that only need a port should
// include storage/ports.hpp instead and remain independent of the SQLite
// service implementation.
#pragma once

#include "storage/ports.hpp"

#include <memory>

namespace cidx {
class SqliteStorageService;
class Transaction;
} // namespace cidx

namespace cidx::storage {

class SqliteWorkspaceCatalogAdapter final : public WorkspaceCatalogReadPort,
                                            public WorkspaceCatalogWritePort {
public:
  explicit SqliteWorkspaceCatalogAdapter(SqliteStorageService &db);

  std::optional<SemanticUniverse>
  get_semantic_universe_by_id(int64_t id) override;
  std::optional<SemanticUniverse>
  get_semantic_universe_by_key(const std::string &key) override;
  std::vector<SemanticUniverse> list_semantic_universes() override;
  std::optional<Component> get_component(const std::string &path) override;
  std::optional<Component> get_component_by_id(int64_t id) override;
  std::optional<Component> component_for_path(const std::string &path) override;
  std::vector<Component> list_components(
      const std::optional<std::string> &name = std::nullopt,
      const std::optional<std::string> &kind = std::nullopt) override;
  std::optional<std::string> get_alias(const std::string &name) override;
  std::string portable_translation_unit_identity_for_config(
      int64_t config_id,
      std::optional<int64_t> translation_unit_file_id = std::nullopt) override;
  std::string
  portable_translation_unit_identity_for_file(int64_t file_id) override;
  int64_t semantic_universe_for_file_id(int64_t file_id) override;
  std::optional<Repository> get_repository_by_id(int64_t id) override;
  std::optional<Repository>
  get_repository_by_name(const std::string &name) override;
  std::vector<Repository> list_repositories(
      const std::optional<std::string> &name = std::nullopt,
      const std::optional<std::string> &kind = std::nullopt) override;
  std::optional<Clone> get_clone_by_id(int64_t id) override;
  std::optional<Clone> get_clone_by_path(const std::string &path) override;
  std::vector<Clone> list_clones(
      const std::optional<int64_t> &repository_id = std::nullopt) override;

  int64_t add_semantic_universe(const std::string &key, const std::string &name,
                                const std::string &policy) override;
  int64_t add_component(const ComponentWriteRecord &component) override;
  void delete_component(int64_t id) override;
  int64_t add_repository(const RepositoryWriteRecord &repository) override;
  void delete_repository(int64_t id) override;
  int64_t add_clone(int64_t repository_id, const std::string &path,
                    const std::optional<std::string> &label) override;
  void delete_clone(int64_t id) override;

private:
  SqliteStorageService *db_;
};

class SqliteSourceStoreAdapter final : public SourceStoreReadPort,
                                       public SourceStoreWritePort {
public:
  explicit SqliteSourceStoreAdapter(SqliteStorageService &db);

  std::optional<File> get_file(const std::string &path) override;
  std::optional<File> get_file_by_id(int64_t id) override;
  std::optional<std::string> file_abs_path(int64_t id) override;
  bool is_file_indexed(
      const std::string &path,
      const std::optional<double> &mtime = std::nullopt,
      const std::optional<std::string> &md5 = std::nullopt) override;
  std::vector<Diagnostic> get_diagnostics(int64_t file_id) override;
  std::map<int64_t, std::map<int, int64_t>> diagnostic_counts() override;
  std::vector<FileConfigApplicability>
  file_configs_for(int64_t file_id) override;
  std::optional<TranslationUnitConfig>
  translation_unit_config_by_id(int64_t config_id) override;

  int64_t
  add_file(int64_t directory_id, const std::string &name,
           const std::optional<double> &mtime = std::nullopt,
           const std::optional<std::string> &md5 = std::nullopt,
           const std::optional<std::vector<std::string>> &compile_options =
               std::nullopt,
           const std::optional<std::string> &driver = std::nullopt) override;
  int64_t add_file_path(
      const std::string &path,
      const std::optional<double> &mtime = std::nullopt,
      const std::optional<std::string> &md5 = std::nullopt,
      const std::optional<std::vector<std::string>> &compile_options =
          std::nullopt,
      const std::optional<std::string> &driver = std::nullopt) override;
  void delete_file(int64_t id) override;
  void mark_file_indexed(
      int64_t id, const std::optional<double> &mtime = std::nullopt,
      const std::optional<std::string> &md5 = std::nullopt) override;
  void set_file_indexed(int64_t id, bool indexed) override;
  void replace_diagnostics(int64_t file_id,
                           const std::vector<Diagnostic> &diagnostics) override;

private:
  SqliteStorageService *db_;
};

class SqliteSymbolStoreAdapter final : public SymbolReadPort,
                                       public SymbolWritePort {
public:
  explicit SqliteSymbolStoreAdapter(SqliteStorageService &db,
                                    FailureInjector *injector = nullptr);

  std::optional<Symbol> lookup_symbol(
      const std::string &usr,
      const std::optional<int64_t> &semantic_universe_id = std::nullopt,
      const std::optional<std::string> &identity_source = std::nullopt,
      const std::optional<std::string> &identity_translation_unit =
          std::nullopt) override;
  std::optional<Symbol> lookup_symbol_by_id(int64_t id) override;
  std::vector<Symbol>
  lookup_symbols_by_usr(const std::string &usr,
                        const std::optional<int64_t> &semantic_universe_id =
                            std::nullopt) override;
  std::vector<Symbol>
  lookup_symbols_by_name(const std::string &name,
                         const std::optional<std::string> &kind = std::nullopt,
                         const std::optional<int64_t> &semantic_universe_id =
                             std::nullopt) override;
  std::vector<Symbol> lookup_symbols_by_qual_name(
      const std::string &name,
      const std::optional<std::string> &kind = std::nullopt,
      const std::optional<int64_t> &semantic_universe_id =
          std::nullopt) override;
  std::vector<Symbol> symbols_in_file(int64_t file_id) override;

  int64_t add_symbol(const Symbol &symbol) override;
  void add_decl_site(int64_t symbol_id, const Symbol &symbol) override;
  int64_t mint_symbol_id(const SymbolIdentityRecord &symbol) override;
  bool update_symbol_by_id(
      int64_t id,
      const std::vector<std::pair<std::string, SymbolValue>> &values) override;
  void delete_symbol(int64_t id) override;
  void delete_symbols_for_file(int64_t file_id) override;

private:
  SqliteStorageService *db_;
  FailureInjector *injector_;
};

class SqliteTypeStoreAdapter final : public TypeReadPort, public TypeWritePort {
public:
  explicit SqliteTypeStoreAdapter(SqliteStorageService &db);

  std::optional<TypeNode> type_node_by_id(int64_t id) override;
  std::vector<Parameter> parameters_of(int64_t symbol_id) override;
  std::optional<int64_t> symbol_type_of(int64_t symbol_id,
                                        int64_t kind) override;
  int64_t intern_type_node(const TypeNode &node) override;
  void add_type_edge(int64_t src_id, int64_t kind, int64_t position,
                     int64_t dst_id) override;
  void replace_parameters(int64_t owner_id,
                          const std::vector<Parameter> &parameters) override;
  void add_symbol_type(int64_t symbol_id, int64_t kind,
                       int64_t type_id) override;

private:
  SqliteStorageService *db_;
};

class SqliteFactStoreAdapter final : public FactReadPort, public FactWritePort {
public:
  explicit SqliteFactStoreAdapter(SqliteStorageService &db);

  std::vector<GraphEdgeRecord> graph_edges(int64_t symbol_id,
                                           const std::string &direction,
                                           const std::vector<int64_t> &kind_ids,
                                           bool count_resolved,
                                           int limit) override;
  std::map<int64_t, std::vector<GraphEdgeSiteRecord>>
  edge_sites_for(const std::vector<int64_t> &edge_ids) override;
  int64_t add_edge(const Edge &edge) override;
  int64_t ensure_edge(const Edge &edge) override;
  void add_edge_site(const EdgeSite &site) override;
  void add_call_arg(const CallArg &arg) override;
  void add_template_param(const TemplateParam &param) override;
  void add_template_arg(const TemplateArg &arg) override;

private:
  SqliteStorageService *db_;
};

class SqliteDefinitionStoreAdapter final : public DefinitionReadPort,
                                           public DefinitionWritePort {
public:
  explicit SqliteDefinitionStoreAdapter(SqliteStorageService &db);

  std::vector<DefinitionRecord> definitions_of(int64_t symbol_id) override;
  std::vector<DefinitionRecord> possible_callees_of(int64_t symbol_id) override;
  int64_t get_or_create_definition(
      int64_t symbol_id, std::optional<int64_t> file_id,
      std::optional<int64_t> line, std::optional<int64_t> col,
      std::optional<int64_t> end_line, std::optional<int64_t> end_col,
      const std::optional<std::string> &init_text) override;
  void add_def_edge(int64_t definition_id, int64_t destination_id,
                    int64_t kind) override;
  auto body_edge_count(int64_t symbol_id) -> std::size_t override;
  void copy_body_edges_to_def_edge(int64_t definition_id,
                                   int64_t symbol_id) override;
  void delete_edges_for_file(int64_t file_id) override;
  void delete_definitions_for_file(int64_t file_id) override;

private:
  SqliteStorageService *db_;
};

class SqliteIncludeStoreAdapter final : public IncludeReadPort,
                                        public IncludeWritePort {
public:
  explicit SqliteIncludeStoreAdapter(SqliteStorageService &db);

  std::optional<IncludeConfig> include_config_by_id(int64_t id) override;
  std::vector<IncludeConfig> include_configs_for_tu(int64_t file_id) override;
  std::vector<IncludeEdge> include_edges_from(int64_t file_id,
                                              bool include_system) override;
  std::vector<IncludeSite> include_sites_for(int64_t edge_id) override;
  int64_t add_include_config(const IncludeConfig &config) override;
  int64_t add_include_edge(const IncludeEdge &edge) override;
  int64_t add_include_site(const IncludeSite &site) override;
  void add_include_macro_use(const IncludeMacroUse &use) override;
  void delete_include_configs_for_tu(int64_t file_id) override;

private:
  SqliteStorageService *db_;
};

class SqliteSchemaCatalogAdapter final : public SchemaCatalogReadPort {
public:
  explicit SqliteSchemaCatalogAdapter(SqliteStorageService &db);

  Stats stats() override;
  bool integrity_ok() override;
  bool foreign_keys_ok() override;
  bool graph_resolved() override;

private:
  SqliteStorageService *db_;
};

class SqliteUnitOfWork final : public UnitOfWork {
public:
  explicit SqliteUnitOfWork(SqliteStorageService &db,
                            FailureInjector *injector = nullptr);
  ~SqliteUnitOfWork() override;

  void commit() override;
  void rollback() override;

private:
  std::unique_ptr<Transaction> transaction_;
  FailureInjector *injector_;
};

class SqliteUnitOfWorkFactory final : public UnitOfWorkFactory {
public:
  explicit SqliteUnitOfWorkFactory(SqliteStorageService &db,
                                   FailureInjector *injector = nullptr);
  std::unique_ptr<UnitOfWork> begin() override;

private:
  SqliteStorageService *db_;
  FailureInjector *injector_;
};

// The compatibility façade owns one adapter set so application code can
// migrate port-by-port without constructing parallel SQLite bindings.
class SqliteStoragePorts final {
public:
  explicit SqliteStoragePorts(SqliteStorageService &db,
                              FailureInjector *injector = nullptr);

  WorkspaceCatalogReadPort &workspace_catalog_read() { return catalog_; }
  WorkspaceCatalogWritePort &workspace_catalog_write() { return catalog_; }
  SourceStoreReadPort &source_read() { return source_; }
  SourceStoreWritePort &source_write() { return source_; }
  SymbolReadPort &symbol_read() { return symbols_; }
  SymbolWritePort &symbol_write() { return symbols_; }
  TypeReadPort &type_read() { return types_; }
  TypeWritePort &type_write() { return types_; }
  FactReadPort &fact_read() { return facts_; }
  FactWritePort &fact_write() { return facts_; }
  DefinitionReadPort &definition_read() { return definitions_; }
  DefinitionWritePort &definition_write() { return definitions_; }
  IncludeReadPort &include_read() { return includes_; }
  IncludeWritePort &include_write() { return includes_; }
  SchemaCatalogReadPort &schema_read() { return schema_; }
  UnitOfWorkFactory &unit_of_work() { return units_; }

private:
  SqliteWorkspaceCatalogAdapter catalog_;
  SqliteSourceStoreAdapter source_;
  SqliteSymbolStoreAdapter symbols_;
  SqliteTypeStoreAdapter types_;
  SqliteFactStoreAdapter facts_;
  SqliteDefinitionStoreAdapter definitions_;
  SqliteIncludeStoreAdapter includes_;
  SqliteSchemaCatalogAdapter schema_;
  SqliteUnitOfWorkFactory units_;
};

} // namespace cidx::storage
