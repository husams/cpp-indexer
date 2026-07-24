// Shared workspace and translation-unit resolution contracts.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "compiledb/compiledb.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"

namespace cidx {

class Toolchain;

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

  static WorkspaceSnapshot capture(Storage &storage);
  void recompute_identity();
};

enum class WorkspaceReadWriteMode : std::uint8_t { read_only, read_write };

class WorkspaceContext final {
public:
  static WorkspaceContext open(
      const std::string &index_path,
      WorkspaceReadWriteMode mode = WorkspaceReadWriteMode::read_only);
  static WorkspaceContext borrow(
      Storage &storage,
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
  [[nodiscard]] Storage &storage() noexcept { return *storage_; }
  [[nodiscard]] const Storage &storage() const noexcept { return *storage_; }

private:
  WorkspaceContext(std::string index_path, WorkspaceReadWriteMode mode,
                   Storage *storage, std::unique_ptr<Storage> owned_storage,
                   WorkspaceSnapshot snapshot);

  std::string index_path_;
  WorkspaceReadWriteMode mode_;
  Storage *storage_ = nullptr;
  std::unique_ptr<Storage> owned_storage_;
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
                                      const std::string &compile_db_path);
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
  config_for_command(const CompileCommand &command) const;
  [[nodiscard]] TranslationUnitConfig
  config_for_file(const File &file, const std::string &source_path) const;
  static void validate(const TranslationUnitConfig &config);

  WorkspaceContext &context_;
  Toolchain &toolchain_;
  std::vector<CompileCommand> commands_;
};

[[nodiscard]] const char *workspace_error_code_name(
    WorkspaceErrorCode code) noexcept;

} // namespace cidx
