#include "workspace/context.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <ranges>
#include <sstream>
#include <string_view>
#include <utility>

#include "toolchain/toolchain.hpp"
#include "util/hashing.hpp"
#include "util/pathutil.hpp"

namespace cidx {
namespace {

std::string json_string(std::string_view value) {
  std::string result;
  result.reserve(value.size() + 2);
  result.push_back('"');
  constexpr std::array hex = {'0', '1', '2', '3', '4', '5', '6', '7',
                              '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\b':
      result += "\\b";
      break;
    case '\f':
      result += "\\f";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (character < 0x20U) {
        result += "\\u00";
        result.push_back(hex[character >> 4U]);
        result.push_back(hex[character & 0x0fU]);
      } else {
        result.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  result.push_back('"');
  return result;
}

std::string optional_json(const std::optional<std::string> &value) {
  return value ? json_string(*value) : "null";
}

std::string workspace_snapshot_json(const WorkspaceSnapshot &snapshot) {
  std::vector<std::string> repositories;
  repositories.reserve(snapshot.repositories.size());
  for (const Repository &repository : snapshot.repositories) {
    std::ostringstream value;
    value << "{\"kind\":" << json_string(repository.kind)
          << ",\"name\":" << json_string(repository.name)
          << ",\"remote_url\":" << optional_json(repository.remote_url)
          << ",\"active_clone_id\":"
          << (repository.active_clone_id
                  ? std::to_string(*repository.active_clone_id)
                  : "null")
          << "}";
    repositories.push_back(value.str());
  }
  std::ranges::sort(repositories);

  std::vector<std::string> clones;
  clones.reserve(snapshot.active_clones.size());
  for (const Clone &clone : snapshot.active_clones) {
    clones.push_back("{\"label\":" + optional_json(clone.label) +
                     ",\"path\":" + json_string(clone.path) + "}");
  }
  std::ranges::sort(clones);

  std::vector<std::string> components;
  components.reserve(snapshot.components.size());
  for (const Component &component : snapshot.components) {
    std::ostringstream value;
    value << "{\"kind\":" << json_string(component.kind)
          << ",\"name\":" << json_string(component.name)
          << ",\"path\":" << json_string(component.path)
          << ",\"version\":" << optional_json(component.version) << "}";
    components.push_back(value.str());
  }
  std::ranges::sort(components);

  std::vector<std::string> universes;
  universes.reserve(snapshot.semantic_universes.size());
  for (const SemanticUniverse &universe : snapshot.semantic_universes) {
    universes.push_back("{\"key\":" + json_string(universe.key) +
                        ",\"name\":" + json_string(universe.name) +
                        ",\"policy\":" + json_string(universe.policy) +
                        "}");
  }
  std::ranges::sort(universes);

  std::ostringstream result;
  result << "{\"components\":[";
  for (std::size_t index = 0; index < components.size(); ++index) {
    if (index != 0) {
      result << ',';
    }
    result << components[index];
  }
  result << R"(],"index":{"freshness":)"
         << json_string(snapshot.index_identity.freshness)
         << ",\"schema_version\":"
         << snapshot.index_identity.schema_version
         << ",\"index_config\":"
         << optional_json(snapshot.index_identity.index_config)
         << ",\"index_config_fingerprint\":"
         << optional_json(snapshot.index_identity.index_config_fingerprint)
         << ",\"source_fingerprint\":"
         << optional_json(snapshot.index_identity.source_fingerprint)
         << ",\"source_revision\":"
         << optional_json(snapshot.index_identity.source_revision) << "}"
         << ",\"repositories\":[";
  for (std::size_t index = 0; index < repositories.size(); ++index) {
    if (index != 0) {
      result << ',';
    }
    result << repositories[index];
  }
  result << "],\"active_clones\":[";
  for (std::size_t index = 0; index < clones.size(); ++index) {
    if (index != 0) {
      result << ',';
    }
    result << clones[index];
  }
  result << "],\"semantic_universes\":[";
  for (std::size_t index = 0; index < universes.size(); ++index) {
    if (index != 0) {
      result << ',';
    }
    result << universes[index];
  }
  result << "]}";
  return result.str();
}

std::string absolute_source(const CompileCommand &command) {
  if (pathutil::isabs(command.filename)) {
    return pathutil::normpath(command.filename);
  }
  return pathutil::abspath(pathutil::join(command.directory, command.filename));
}

std::string absolute_input(const std::string &value,
                           const std::optional<std::string> &working_dir) {
  std::string resolved = pathutil::resolve_fs_path(value);
  if (pathutil::isabs(resolved)) {
    return resolved;
  }
  return pathutil::abspath(
      pathutil::join(working_dir.value_or(pathutil::getcwd()), resolved));
}

std::string descriptor_json(const TranslationUnitDescriptor &descriptor) {
  return "{\"configuration\":" +
         canonical_translation_unit_config_json(descriptor.configuration) +
         ",\"source_identity\":" + json_string(descriptor.source_identity) +
         ",\"workspace_identity\":" +
         json_string(descriptor.workspace_identity) + "}";
}

} // namespace

const char *workspace_error_code_name(WorkspaceErrorCode code) noexcept {
  switch (code) {
  case WorkspaceErrorCode::unregistered_file:
    return "unregistered-file";
  case WorkspaceErrorCode::ambiguous_translation_unit:
    return "ambiguous-translation-unit";
  case WorkspaceErrorCode::stale_index:
    return "stale-index";
  case WorkspaceErrorCode::missing_generated_input:
    return "missing-generated-input";
  case WorkspaceErrorCode::unsupported_compiler:
    return "unsupported-compiler";
  case WorkspaceErrorCode::unreproducible_configuration:
    return "unreproducible-configuration";
  }
  return "unknown-workspace-error";
}

WorkspaceError::WorkspaceError(WorkspaceErrorCode code,
                               const std::string &message,
                               std::vector<std::string> candidates)
    : CidxError(std::string(workspace_error_code_name(code)) + ": " +
                message),
      code_(code), candidates_(std::move(candidates)) {}

WorkspaceSnapshot WorkspaceSnapshot::capture(Storage &storage) {
  WorkspaceSnapshot snapshot;
  snapshot.repositories = storage.list_repositories();
  snapshot.components = storage.list_components();
  snapshot.semantic_universes = storage.list_semantic_universes();
  snapshot.index_identity = storage.index_identity();
  for (const Repository &repository : snapshot.repositories) {
    if (!repository.active_clone_id) {
      continue;
    }
    if (const auto clone = storage.get_clone_by_id(*repository.active_clone_id)) {
      snapshot.active_clones.push_back(*clone);
    }
  }
  snapshot.recompute_identity();
  return snapshot;
}

void WorkspaceSnapshot::recompute_identity() {
  canonical_json = workspace_snapshot_json(*this);
  identity = sha256_hex(canonical_json);
}

WorkspaceContext::WorkspaceContext(std::string index_path,
                                   WorkspaceReadWriteMode mode,
                                   Storage *storage,
                                   std::unique_ptr<Storage> owned_storage,
                                   WorkspaceSnapshot snapshot)
    : index_path_(std::move(index_path)), mode_(mode),
      storage_(storage), owned_storage_(std::move(owned_storage)),
      snapshot_(std::move(snapshot)) {}

WorkspaceContext WorkspaceContext::open(const std::string &index_path,
                                        WorkspaceReadWriteMode mode) {
  const auto storage_mode = mode == WorkspaceReadWriteMode::read_only
                                ? Storage::OpenMode::read_only
                                : Storage::OpenMode::read_write;
  auto storage = std::make_unique<Storage>(index_path, storage_mode);
  Storage *storage_ptr = storage.get();
  WorkspaceSnapshot snapshot = WorkspaceSnapshot::capture(*storage);
  return {index_path, mode, storage_ptr, std::move(storage),
          std::move(snapshot)};
}

WorkspaceContext WorkspaceContext::borrow(Storage &storage,
                                          WorkspaceReadWriteMode mode) {
  return {{}, mode, &storage, nullptr, WorkspaceSnapshot::capture(storage)};
}

WorkspaceContext::WorkspaceContext(WorkspaceContext &&) noexcept = default;
WorkspaceContext &WorkspaceContext::operator=(WorkspaceContext &&) noexcept =
    default;
WorkspaceContext::~WorkspaceContext() = default;

TranslationUnitConfigurationService::TranslationUnitConfigurationService(
    WorkspaceContext &context, Toolchain &toolchain,
    const std::string &compile_db_path)
    : TranslationUnitConfigurationService(context, toolchain) {
  commands_ = CompileDb::load(compile_db_path);
}

TranslationUnitConfigurationService::TranslationUnitConfigurationService(
    WorkspaceContext &context, Toolchain &toolchain)
    : context_(context), toolchain_(toolchain) {}

std::vector<std::string> resolved_options(
    Storage &storage, const std::vector<std::string> &arguments) {
  return CompileDb::resolve_options(
      CompileDb::sanitize(arguments),
      [&storage](const std::string &name) { return storage.get_alias(name); });
}

TranslationUnitConfig resolve_config(
    Toolchain &toolchain, const std::string &source_path,
    const std::optional<std::string> &driver,
    const std::optional<std::string> &working_dir,
    std::vector<std::string> options) {
  const bool cpp = Toolchain::is_cpp(source_path, options);
  const std::optional<std::string> language =
      cpp ? std::optional<std::string>("c++")
          : std::optional<std::string>("c");
  const std::vector<std::string> flags =
      toolchain.toolchain_flags(cpp, driver);
  options.insert(options.end(), flags.begin(), flags.end());
  options.emplace_back("-ferror-limit=0");
  return resolve_translation_unit_config(
      driver, working_dir, options, language, toolchain.resource_include(),
      std::string("error-limit=0"));
}

TranslationUnitConfig
TranslationUnitConfigurationService::config_for_command(
    const CompileCommand &command) const {
  if (command.driver.empty()) {
    throw WorkspaceError(WorkspaceErrorCode::unsupported_compiler,
                         "compile command has no driver");
  }
  return resolve_config(toolchain_, command.filename, command.driver,
                        command.directory, normalized_arguments(command.args));
}

TranslationUnitConfig
TranslationUnitConfigurationService::config_for_file(
    const File &file, const std::string &source_path) const {
  return resolve_config(
      toolchain_, source_path, file.driver, std::string("."),
      normalized_arguments(
          file.compile_options.value_or(std::vector<std::string>{})));
}

std::vector<std::string>
TranslationUnitConfigurationService::normalized_arguments(
    const std::vector<std::string> &arguments) const {
  return resolved_options(context_.storage(), arguments);
}

std::vector<std::string>
TranslationUnitConfigurationService::invocation_arguments(
    const std::string &source_path,
    const TranslationUnitDescriptor &descriptor) const {
  std::vector<std::string> arguments = descriptor.configuration.arguments;
  if (std::ranges::find(arguments, "-nostdinc") == arguments.end()) {
    const bool cpp = Toolchain::is_cpp(source_path, arguments);
    const std::vector<std::string> flags =
        toolchain_.toolchain_flags(cpp, descriptor.configuration.driver);
    arguments.insert(arguments.end(), flags.begin(), flags.end());
  }
  if (std::ranges::find(arguments, "-ferror-limit=0") == arguments.end()) {
    arguments.emplace_back("-ferror-limit=0");
  }
  return arguments;
}

void TranslationUnitConfigurationService::validate(
    const TranslationUnitConfig &config) {
  if (!config.driver || config.driver->empty()) {
    throw WorkspaceError(WorkspaceErrorCode::unsupported_compiler,
                         "translation unit has no compiler driver");
  }
  for (const std::string &input : config.generated_inputs) {
    const std::string path = absolute_input(input, config.working_dir);
    if (!std::filesystem::is_regular_file(path)) {
      throw WorkspaceError(
          WorkspaceErrorCode::missing_generated_input,
          "generated input is missing: " + path, {path});
    }
  }
  if (config.state != TranslationUnitConfigState::registered) {
    throw WorkspaceError(WorkspaceErrorCode::unreproducible_configuration,
                         "translation unit configuration is not registered");
  }
  if (config.association_state != TranslationUnitConfigState::registered) {
    throw WorkspaceError(
        WorkspaceErrorCode::unreproducible_configuration,
        "translation unit configuration association is not registered");
  }
}

TranslationUnitDescriptor
TranslationUnitConfigurationService::descriptor_for(
    const std::string &source_path, TranslationUnitConfig config) const {
  validate(config);
  TranslationUnitDescriptor descriptor;
  descriptor.source_identity = pathutil::normpath(source_path);
  descriptor.workspace_identity = context_.snapshot().identity;
  descriptor.configuration = std::move(config);
  descriptor.canonical_json = descriptor_json(descriptor);
  descriptor.semantic_hash = sha256_hex(descriptor.canonical_json);
  return descriptor;
}

std::vector<TranslationUnitDescriptor>
TranslationUnitConfigurationService::resolve_all(
    const std::string &source_path) const {
  if (context_.snapshot().index_identity.freshness == "stale") {
    throw WorkspaceError(WorkspaceErrorCode::stale_index,
                         "workspace index is stale");
  }
  const std::string normalized =
      pathutil::normpath(pathutil::abspath(source_path));
  std::vector<TranslationUnitDescriptor> descriptors;
  for (const CompileCommand &command : commands_) {
    if (absolute_source(command) == normalized) {
      descriptors.push_back(
          descriptor_for(normalized, config_for_command(command)));
    }
  }

  if (descriptors.empty()) {
    const auto file = context_.storage().get_file(normalized);
    if (file) {
      const auto configs =
          context_.storage().translation_unit_configs_for_file(file->id);
      if (configs.empty() && file->compile_options) {
        descriptors.push_back(
            descriptor_for(normalized, config_for_file(*file, normalized)));
      } else {
        for (const TranslationUnitConfig &config : configs) {
          descriptors.push_back(descriptor_for(normalized, config));
        }
      }
    }
  }
  if (descriptors.empty()) {
    throw WorkspaceError(WorkspaceErrorCode::unregistered_file,
                         "file is not registered in the workspace",
                         {normalized});
  }
  std::ranges::sort(
      descriptors, [](const TranslationUnitDescriptor &left,
                      const TranslationUnitDescriptor &right) {
        return left.semantic_hash < right.semantic_hash;
      });
  const auto unique_end = std::ranges::unique(
      descriptors, [](const TranslationUnitDescriptor &left,
                      const TranslationUnitDescriptor &right) {
        return left.semantic_hash == right.semantic_hash;
      });
  descriptors.erase(unique_end.begin(), descriptors.end());
  return descriptors;
}

TranslationUnitDescriptor TranslationUnitConfigurationService::resolve(
    const std::string &source_path) const {
  std::vector<TranslationUnitDescriptor> descriptors = resolve_all(source_path);
  if (descriptors.size() != 1U) {
    std::vector<std::string> candidates;
    candidates.reserve(descriptors.size());
    for (const TranslationUnitDescriptor &descriptor : descriptors) {
      candidates.push_back(descriptor.semantic_hash);
    }
    throw WorkspaceError(
        WorkspaceErrorCode::ambiguous_translation_unit,
        "file participates in multiple translation-unit configurations",
        std::move(candidates));
  }
  return std::move(descriptors.front());
}

} // namespace cidx
