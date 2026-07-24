#include "storage/storage.hpp"

#include "compiledb/compiledb.hpp"

namespace cidx {

std::vector<Repository> StorageWorkspaceAdapter::list_repositories() {
  return storage_.list_repositories();
}

std::vector<Component> StorageWorkspaceAdapter::list_components() {
  return storage_.list_components();
}

std::vector<SemanticUniverse>
StorageWorkspaceAdapter::list_semantic_universes() {
  return storage_.list_semantic_universes();
}

IndexIdentity StorageWorkspaceAdapter::index_identity() {
  return storage_.index_identity();
}

std::optional<Clone>
StorageWorkspaceAdapter::clone_by_id(const int64_t clone_id) {
  return storage_.get_clone_by_id(clone_id);
}

std::optional<File> StorageWorkspaceAdapter::file(const std::string &path) {
  return storage_.get_file(path);
}

std::vector<FileConfigApplicability>
StorageWorkspaceAdapter::file_configs_for(const int64_t file_id) {
  return storage_.file_configs_for(file_id);
}

std::optional<TranslationUnitConfig>
StorageWorkspaceAdapter::translation_unit_config_by_id(
    const int64_t config_id) {
  return storage_.translation_unit_config_by_id(config_id);
}

std::vector<std::string> StorageWorkspaceAdapter::normalized_arguments(
    const std::vector<std::string> &arguments) {
  return CompileDb::resolve_options(
      CompileDb::sanitize(arguments),
      [this](const std::string &name) { return storage_.get_alias(name); });
}

} // namespace cidx
