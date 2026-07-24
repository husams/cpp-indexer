// Shared workspace and translation-unit resolution contracts.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "storage/records.hpp"
#include "util/errors.hpp"

namespace cidx {

class Toolchain;

TranslationUnitConfig resolve_translation_unit_config(
    const std::optional<std::string> &driver,
    const std::optional<std::string> &working_dir,
    const std::vector<std::string> &arguments,
    const std::optional<std::string> &language = std::nullopt,
    const std::optional<std::string> &resource_dir = std::nullopt,
    const std::optional<std::string> &diagnostics_policy = std::nullopt);

std::string
canonical_translation_unit_config_json(const TranslationUnitConfig &config);

struct IndexIdentity {
  int schema_version = 0;
  std::optional<std::string> source_revision;
  std::optional<std::string> source_fingerprint;
  std::optional<std::string> index_config;
  std::optional<std::string> index_config_fingerprint;
  std::string freshness = "unverifiable";
};

struct WorkspaceCompileCommand {
  std::string directory;
  std::string filename;
  std::string driver;
  std::vector<std::string> args;
};

class WorkspaceDataSource {
public:
  virtual ~WorkspaceDataSource() = default;

  virtual std::vector<Repository> list_repositories() = 0;
  virtual std::vector<Component> list_components() = 0;
  virtual std::vector<SemanticUniverse> list_semantic_universes() = 0;
  virtual IndexIdentity index_identity() = 0;
  virtual std::optional<Clone> clone_by_id(int64_t clone_id) = 0;
  virtual std::optional<File> file(const std::string &path) = 0;
  virtual std::vector<FileConfigApplicability>
  file_configs_for(int64_t file_id) = 0;
  virtual std::optional<TranslationUnitConfig>
  translation_unit_config_by_id(int64_t config_id) = 0;
  virtual std::vector<std::string>
  normalized_arguments(const std::vector<std::string> &arguments) = 0;
};

enum class WorkspaceErrorCode : std::uint8_t {
  unregistered_file,
  ambiguous_translation_unit,
  stale_index,
  missing_generated_input,
  unsupported_compiler,
  unreproducible_configuration,
};

class WorkspaceError final : public CidxError {
public:
  WorkspaceError(WorkspaceErrorCode code, const std::string &message,
                 std::vector<std::string> candidates = {});

  [[nodiscard]] WorkspaceErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::vector<std::string> &candidates() const noexcept {
    return candidates_;
  }

private:
  WorkspaceErrorCode code_;
  std::vector<std::string> candidates_;
};

struct WorkspaceSnapshot {
  std::vector<Repository> repositories;
  std::vector<Clone> active_clones;
  std::vector<Component> components;
  std::vector<SemanticUniverse> semantic_universes;
  IndexIdentity index_identity;
  std::string canonical_json;
  std::string identity;

  static WorkspaceSnapshot capture(WorkspaceDataSource &data_source);
  void recompute_identity();
};

enum class WorkspaceReadWriteMode : std::uint8_t { read_only, read_write };

class WorkspaceContext final {
public:
  static WorkspaceContext borrow(
      WorkspaceDataSource &data_source,
      WorkspaceReadWriteMode mode = WorkspaceReadWriteMode::read_write);

  WorkspaceContext(WorkspaceContext &&) noexcept;
  WorkspaceContext &operator=(WorkspaceContext &&) noexcept;
  WorkspaceContext(const WorkspaceContext &) = delete;
  WorkspaceContext &operator=(const WorkspaceContext &) = delete;
  ~WorkspaceContext();

  [[nodiscard]] const std::string &index_path() const noexcept {
    return index_path_;
  }
  [[nodiscard]] WorkspaceReadWriteMode mode() const noexcept { return mode_; }
  [[nodiscard]] const WorkspaceSnapshot &snapshot() const noexcept {
    return snapshot_;
  }
  [[nodiscard]] WorkspaceDataSource &data_source() noexcept {
    return *data_source_;
  }
  [[nodiscard]] const WorkspaceDataSource &data_source() const noexcept {
    return *data_source_;
  }

private:
  WorkspaceContext(std::string index_path, WorkspaceReadWriteMode mode,
                   WorkspaceDataSource *data_source,
                   WorkspaceSnapshot snapshot);

  std::string index_path_;
  WorkspaceReadWriteMode mode_;
  WorkspaceDataSource *data_source_ = nullptr;
  WorkspaceSnapshot snapshot_;
};

struct TranslationUnitDescriptor {
  std::string source_identity;
  std::string workspace_identity;
  TranslationUnitConfig configuration;
  std::string canonical_json;
  std::string semantic_hash;
};

class TranslationUnitConfigurationService final {
public:
  TranslationUnitConfigurationService(WorkspaceContext &context,
                                      Toolchain &toolchain,
                                      std::vector<WorkspaceCompileCommand>
                                          commands);
  TranslationUnitConfigurationService(WorkspaceContext &context,
                                      Toolchain &toolchain);

  [[nodiscard]] std::vector<TranslationUnitDescriptor>
  resolve_all(const std::string &source_path) const;
  [[nodiscard]] TranslationUnitDescriptor
  resolve(const std::string &source_path) const;
  [[nodiscard]] std::vector<std::string>
  normalized_arguments(const std::vector<std::string> &arguments) const;
  [[nodiscard]] static std::vector<std::string> invocation_arguments(
      const std::string &source_path,
      const TranslationUnitDescriptor &descriptor);

private:
  [[nodiscard]] TranslationUnitDescriptor
  descriptor_for(const std::string &source_path,
                TranslationUnitConfig config) const;
  [[nodiscard]] TranslationUnitConfig
  config_for_command(const WorkspaceCompileCommand &command) const;
  [[nodiscard]] TranslationUnitConfig
  config_for_file(const File &file, const std::string &source_path) const;
  static void validate(const TranslationUnitConfig &config);

  WorkspaceContext &context_;
  Toolchain &toolchain_;
  std::vector<WorkspaceCompileCommand> commands_;
};

[[nodiscard]] const char *workspace_error_code_name(
    WorkspaceErrorCode code) noexcept;

} // namespace cidx
