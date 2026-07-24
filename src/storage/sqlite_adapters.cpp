#include "storage/sqlite_adapters.hpp"

#include "storage/storage.hpp"

namespace cidx::storage {

SqliteWorkspaceCatalogAdapter::SqliteWorkspaceCatalogAdapter(Storage &db)
    : db_(&db) {}

std::optional<SemanticUniverse>
SqliteWorkspaceCatalogAdapter::get_semantic_universe_by_id(int64_t id) {
  return db_->get_semantic_universe_by_id(id);
}

std::optional<SemanticUniverse>
SqliteWorkspaceCatalogAdapter::get_semantic_universe_by_key(
    const std::string &key) {
  return db_->get_semantic_universe_by_key(key);
}

std::vector<SemanticUniverse>
SqliteWorkspaceCatalogAdapter::list_semantic_universes() {
  return db_->list_semantic_universes();
}

std::optional<Component>
SqliteWorkspaceCatalogAdapter::get_component(const std::string &path) {
  return db_->get_component(path);
}

std::optional<Component>
SqliteWorkspaceCatalogAdapter::get_component_by_id(int64_t id) {
  return db_->get_component_by_id(id);
}

std::optional<Component>
SqliteWorkspaceCatalogAdapter::component_for_path(const std::string &path) {
  return db_->component_for_path(path);
}

std::vector<Component> SqliteWorkspaceCatalogAdapter::list_components(
    const std::optional<std::string> &name,
    const std::optional<std::string> &kind) {
  return db_->list_components(name, kind);
}

std::optional<Repository>
SqliteWorkspaceCatalogAdapter::get_repository_by_id(int64_t id) {
  return db_->get_repository_by_id(id);
}

std::optional<Repository>
SqliteWorkspaceCatalogAdapter::get_repository_by_name(const std::string &name) {
  return db_->get_repository_by_name(name);
}

std::vector<Repository> SqliteWorkspaceCatalogAdapter::list_repositories(
    const std::optional<std::string> &name,
    const std::optional<std::string> &kind) {
  return db_->list_repositories(name, kind);
}

std::optional<Clone>
SqliteWorkspaceCatalogAdapter::get_clone_by_id(int64_t id) {
  return db_->get_clone_by_id(id);
}

std::optional<Clone>
SqliteWorkspaceCatalogAdapter::get_clone_by_path(const std::string &path) {
  return db_->get_clone_by_path(path);
}

std::vector<Clone> SqliteWorkspaceCatalogAdapter::list_clones(
    const std::optional<int64_t> &repository_id) {
  return db_->list_clones(repository_id);
}

int64_t SqliteWorkspaceCatalogAdapter::add_semantic_universe(
    const std::string &key, const std::string &name,
    const std::string &policy) {
  return db_->add_semantic_universe(key, name, policy);
}

int64_t SqliteWorkspaceCatalogAdapter::add_component(
    const ComponentWriteRecord &component) {
  return db_->add_component(component.name, component.path, component.kind,
                            component.version);
}

void SqliteWorkspaceCatalogAdapter::delete_component(int64_t id) {
  db_->delete_component(id);
}

int64_t SqliteWorkspaceCatalogAdapter::add_repository(
    const RepositoryWriteRecord &repository) {
  return db_->add_repository(repository.name, repository.kind,
                             repository.remote_url,
                             repository.semantic_universe_id);
}

void SqliteWorkspaceCatalogAdapter::delete_repository(int64_t id) {
  db_->delete_repository(id);
}

int64_t SqliteWorkspaceCatalogAdapter::add_clone(
    int64_t repository_id, const std::string &path,
    const std::optional<std::string> &label) {
  return db_->add_clone(repository_id, path, label);
}

void SqliteWorkspaceCatalogAdapter::delete_clone(int64_t id) {
  db_->delete_clone(id);
}

SqliteSourceStoreAdapter::SqliteSourceStoreAdapter(Storage &db) : db_(&db) {}

std::optional<File>
SqliteSourceStoreAdapter::get_file(const std::string &path) {
  return db_->get_file(path);
}

std::optional<File> SqliteSourceStoreAdapter::get_file_by_id(int64_t id) {
  return db_->get_file_by_id(id);
}

std::optional<std::string> SqliteSourceStoreAdapter::file_abs_path(int64_t id) {
  return db_->file_abs_path(id);
}

bool SqliteSourceStoreAdapter::is_file_indexed(
    const std::string &path, const std::optional<double> &mtime,
    const std::optional<std::string> &md5) {
  return db_->is_file_indexed(path, mtime, md5);
}

std::vector<Diagnostic>
SqliteSourceStoreAdapter::get_diagnostics(int64_t file_id) {
  return db_->get_diagnostics(file_id);
}

std::map<int64_t, std::map<int, int64_t>>
SqliteSourceStoreAdapter::diagnostic_counts() {
  return db_->diagnostic_counts();
}

int64_t SqliteSourceStoreAdapter::add_file(
    int64_t directory_id, const std::string &name,
    const std::optional<double> &mtime, const std::optional<std::string> &md5,
    const std::optional<std::vector<std::string>> &compile_options,
    const std::optional<std::string> &driver) {
  return db_->add_file(directory_id, name, mtime, md5, compile_options, driver);
}

int64_t SqliteSourceStoreAdapter::add_file_path(
    const std::string &path, const std::optional<double> &mtime,
    const std::optional<std::string> &md5,
    const std::optional<std::vector<std::string>> &compile_options,
    const std::optional<std::string> &driver) {
  return db_->add_file_path(path, mtime, md5, compile_options, driver);
}

void SqliteSourceStoreAdapter::delete_file(int64_t id) { db_->delete_file(id); }

void SqliteSourceStoreAdapter::mark_file_indexed(
    int64_t id, const std::optional<double> &mtime,
    const std::optional<std::string> &md5) {
  db_->mark_file_indexed(id, mtime, md5);
}

void SqliteSourceStoreAdapter::set_file_indexed(int64_t id, bool indexed) {
  db_->set_file_indexed(id, indexed);
}

void SqliteSourceStoreAdapter::replace_diagnostics(
    int64_t file_id, const std::vector<Diagnostic> &diagnostics) {
  db_->replace_diagnostics(file_id, diagnostics);
}

SqliteSymbolStoreAdapter::SqliteSymbolStoreAdapter(Storage &db) : db_(&db) {}

std::optional<Symbol> SqliteSymbolStoreAdapter::lookup_symbol(
    const std::string &usr,
    const std::optional<int64_t> &semantic_universe_id) {
  return db_->lookup_symbol(usr, semantic_universe_id);
}

std::optional<Symbol>
SqliteSymbolStoreAdapter::lookup_symbol_by_id(int64_t id) {
  return db_->lookup_symbol_by_id(id);
}

std::vector<Symbol> SqliteSymbolStoreAdapter::lookup_symbols_by_usr(
    const std::string &usr,
    const std::optional<int64_t> &semantic_universe_id) {
  return db_->lookup_symbols_by_usr(usr, semantic_universe_id);
}

std::vector<Symbol> SqliteSymbolStoreAdapter::lookup_symbols_by_name(
    const std::string &name, const std::optional<std::string> &kind,
    const std::optional<int64_t> &semantic_universe_id) {
  return db_->lookup_symbols_by_name(name, kind, semantic_universe_id);
}

std::vector<Symbol> SqliteSymbolStoreAdapter::lookup_symbols_by_qual_name(
    const std::string &name, const std::optional<std::string> &kind,
    const std::optional<int64_t> &semantic_universe_id) {
  return db_->lookup_symbols_by_qual_name(name, kind, semantic_universe_id);
}

std::vector<Symbol> SqliteSymbolStoreAdapter::symbols_in_file(int64_t file_id) {
  return db_->symbols_in_file(file_id);
}

int64_t SqliteSymbolStoreAdapter::add_symbol(const Symbol &symbol) {
  return db_->add_symbol(symbol);
}

int64_t
SqliteSymbolStoreAdapter::mint_symbol_id(const SymbolIdentityRecord &symbol) {
  return db_->mint_symbol_id(
      symbol.usr, symbol.spelling, symbol.qual_name, symbol.display_name,
      symbol.kind, symbol.decl_file_id, symbol.decl_line, symbol.decl_col,
      symbol.decl_path, symbol.is_instantiation, symbol.is_named_instance,
      symbol.type_info, symbol.semantic_universe_id, symbol.identity_source,
      symbol.linkage, symbol.identity_translation_unit);
}

bool SqliteSymbolStoreAdapter::update_symbol_by_id(
    int64_t id,
    const std::vector<std::pair<std::string, SymbolValue>> &values) {
  return db_->update_symbol_by_id(id, values);
}

void SqliteSymbolStoreAdapter::delete_symbol(int64_t id) {
  db_->delete_symbol(id);
}

void SqliteSymbolStoreAdapter::delete_symbols_for_file(int64_t file_id) {
  db_->delete_symbols_for_file(file_id);
}

SqliteTypeStoreAdapter::SqliteTypeStoreAdapter(Storage &db) : db_(&db) {}

std::optional<TypeNode> SqliteTypeStoreAdapter::type_node_by_id(int64_t id) {
  return db_->type_node_by_id(id);
}

std::vector<Parameter>
SqliteTypeStoreAdapter::parameters_of(int64_t symbol_id) {
  return db_->parameters_of(symbol_id);
}

std::optional<int64_t> SqliteTypeStoreAdapter::symbol_type_of(int64_t symbol_id,
                                                              int64_t kind) {
  return db_->symbol_type_of(symbol_id, kind);
}

int64_t SqliteTypeStoreAdapter::intern_type_node(const TypeNode &node) {
  return db_->intern_type_node(node);
}

void SqliteTypeStoreAdapter::add_type_edge(int64_t src_id, int64_t kind,
                                           int64_t position, int64_t dst_id) {
  db_->add_type_edge(src_id, kind, position, dst_id);
}

void SqliteTypeStoreAdapter::replace_parameters(
    int64_t owner_id, const std::vector<Parameter> &parameters) {
  db_->replace_parameters(owner_id, parameters);
}

void SqliteTypeStoreAdapter::add_symbol_type(int64_t symbol_id, int64_t kind,
                                             int64_t type_id) {
  db_->add_symbol_type(symbol_id, kind, type_id);
}

SqliteFactStoreAdapter::SqliteFactStoreAdapter(Storage &db) : db_(&db) {}

std::vector<GraphEdgeRecord> SqliteFactStoreAdapter::graph_edges(
    int64_t symbol_id, const std::string &direction,
    const std::vector<int64_t> &kind_ids, bool count_resolved, int limit) {
  std::vector<GraphEdgeRecord> result;
  for (const auto &row : db_->graph_edges(symbol_id, direction, kind_ids,
                                          count_resolved, limit)) {
    result.push_back({.edge_id = row.eid,
                      .src_id = row.src_id,
                      .dst_id = row.dst_id,
                      .kind = row.ekind,
                      .count = row.ecount,
                      .raw_count = row.rawcount,
                      .base_access = row.base_access,
                      .is_virtual = row.is_virtual,
                      .target = row.sym});
  }
  return result;
}

std::map<int64_t, std::vector<GraphEdgeSiteRecord>>
SqliteFactStoreAdapter::edge_sites_for(const std::vector<int64_t> &edge_ids) {
  std::map<int64_t, std::vector<GraphEdgeSiteRecord>> result;
  for (const auto &[edge_id, rows] : db_->edge_sites_for(edge_ids)) {
    auto &converted = result[edge_id];
    converted.reserve(rows.size());
    for (const auto &row : rows) {
      converted.push_back({.edge_id = row.edge_id,
                           .file_id = row.file_id,
                           .line = row.line,
                           .col = row.col,
                           .conditional = row.conditional,
                           .args_sig = row.args_sig,
                           .recv_src_kind = row.recv_src_kind,
                           .recv_type_usr = row.recv_type_usr,
                           .recv_decl_usr = row.recv_decl_usr,
                           .recv_param_pos = row.recv_param_pos,
                           .recv_type_is_value = row.recv_type_is_value});
    }
  }
  return result;
}

int64_t SqliteFactStoreAdapter::add_edge(const Edge &edge) {
  return db_->add_edge(edge);
}

int64_t SqliteFactStoreAdapter::ensure_edge(const Edge &edge) {
  return db_->ensure_edge(edge);
}

void SqliteFactStoreAdapter::add_edge_site(const EdgeSite &site) {
  db_->add_edge_site(site);
}

void SqliteFactStoreAdapter::add_call_arg(const CallArg &arg) {
  db_->add_call_arg(arg);
}

void SqliteFactStoreAdapter::add_template_param(const TemplateParam &param) {
  db_->add_template_param(param);
}

void SqliteFactStoreAdapter::add_template_arg(const TemplateArg &arg) {
  db_->add_template_arg(arg);
}

SqliteDefinitionStoreAdapter::SqliteDefinitionStoreAdapter(Storage &db)
    : db_(&db) {}

std::vector<DefinitionRecord>
SqliteDefinitionStoreAdapter::definitions_of(int64_t symbol_id) {
  std::vector<DefinitionRecord> result;
  for (const auto &row : db_->definitions_of(symbol_id)) {
    result.push_back({.symbol_id = row.symbol_id,
                      .file_id = row.file_id,
                      .line = row.line,
                      .col = row.col,
                      .end_line = row.end_line,
                      .end_col = row.end_col,
                      .init_text = row.init_text});
  }
  return result;
}

std::vector<DefinitionRecord>
SqliteDefinitionStoreAdapter::possible_callees_of(int64_t symbol_id) {
  std::vector<DefinitionRecord> result;
  for (const auto &row : db_->possible_callees_of(symbol_id)) {
    result.push_back({.symbol_id = row.symbol_id,
                      .file_id = row.file_id,
                      .line = row.line,
                      .col = row.col,
                      .end_line = row.end_line,
                      .end_col = row.end_col,
                      .init_text = row.init_text});
  }
  return result;
}

int64_t SqliteDefinitionStoreAdapter::get_or_create_definition(
    int64_t symbol_id, std::optional<int64_t> file_id,
    std::optional<int64_t> line, std::optional<int64_t> col,
    std::optional<int64_t> end_line, std::optional<int64_t> end_col,
    const std::optional<std::string> &init_text) {
  return db_->get_or_create_definition(symbol_id, file_id, line, col, end_line,
                                       end_col, init_text);
}

void SqliteDefinitionStoreAdapter::add_def_edge(int64_t definition_id,
                                                int64_t destination_id,
                                                int64_t kind) {
  db_->add_def_edge(definition_id, destination_id, kind);
}

void SqliteDefinitionStoreAdapter::copy_body_edges_to_def_edge(
    int64_t definition_id, int64_t symbol_id) {
  db_->copy_body_edges_to_def_edge(definition_id, symbol_id);
}

void SqliteDefinitionStoreAdapter::delete_edges_for_file(int64_t file_id) {
  db_->delete_edges_for_file(file_id);
}

void SqliteDefinitionStoreAdapter::delete_definitions_for_file(
    int64_t file_id) {
  db_->delete_definitions_for_file(file_id);
}

SqliteIncludeStoreAdapter::SqliteIncludeStoreAdapter(Storage &db) : db_(&db) {}

std::optional<IncludeConfig>
SqliteIncludeStoreAdapter::include_config_by_id(int64_t id) {
  return db_->include_config_by_id(id);
}

std::vector<IncludeConfig>
SqliteIncludeStoreAdapter::include_configs_for_tu(int64_t file_id) {
  return db_->include_configs_for_tu(file_id);
}

std::vector<IncludeEdge>
SqliteIncludeStoreAdapter::include_edges_from(int64_t file_id,
                                              bool include_system) {
  return db_->include_edges_from(file_id, include_system);
}

std::vector<IncludeSite>
SqliteIncludeStoreAdapter::include_sites_for(int64_t edge_id) {
  return db_->include_sites_for(edge_id);
}

int64_t
SqliteIncludeStoreAdapter::add_include_config(const IncludeConfig &config) {
  return db_->add_include_config(config);
}

int64_t SqliteIncludeStoreAdapter::add_include_edge(const IncludeEdge &edge) {
  return db_->add_include_edge(edge);
}

int64_t SqliteIncludeStoreAdapter::add_include_site(const IncludeSite &site) {
  return db_->add_include_site(site);
}

void SqliteIncludeStoreAdapter::add_include_macro_use(
    const IncludeMacroUse &use) {
  db_->add_include_macro_use(use);
}

void SqliteIncludeStoreAdapter::delete_include_configs_for_tu(int64_t file_id) {
  db_->delete_include_configs_for_tu(file_id);
}

SqliteSchemaCatalogAdapter::SqliteSchemaCatalogAdapter(Storage &db)
    : db_(&db) {}

Stats SqliteSchemaCatalogAdapter::stats() { return db_->stats(); }

bool SqliteSchemaCatalogAdapter::integrity_ok() { return db_->integrity_ok(); }

bool SqliteSchemaCatalogAdapter::foreign_keys_ok() {
  return db_->foreign_keys_ok();
}

bool SqliteSchemaCatalogAdapter::graph_resolved() {
  return db_->graph_resolved();
}

SqliteUnitOfWork::SqliteUnitOfWork(Storage &db)
    : transaction_(std::make_unique<Transaction>(db)) {}

SqliteUnitOfWork::~SqliteUnitOfWork() = default;

void SqliteUnitOfWork::commit() { transaction_->commit(); }

void SqliteUnitOfWork::rollback() { transaction_->rollback(); }

SqliteUnitOfWorkFactory::SqliteUnitOfWorkFactory(Storage &db) : db_(&db) {}

std::unique_ptr<UnitOfWork> SqliteUnitOfWorkFactory::begin() {
  return std::make_unique<SqliteUnitOfWork>(*db_);
}

SqliteStoragePorts::SqliteStoragePorts(Storage &db)
    : catalog_(db), source_(db), symbols_(db), types_(db), facts_(db),
      definitions_(db), includes_(db), schema_(db), units_(db) {}

} // namespace cidx::storage
