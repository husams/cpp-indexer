#pragma once

#include "catalogs/generated_catalog.hpp"
#include "storage/sqlite.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cidx {

class Storage;
class ArtifactStore;
struct ArtifactAttachmentLifetime;

enum class ArtifactCompleteness : std::uint8_t { complete, partial, unknown };
enum class ArtifactTruncation : std::uint8_t { none, truncated, unknown };
enum class ArtifactTrust : std::uint8_t {
  unverified,
  producer_verified,
  reader_verified
};

struct ArtifactSpec {
  std::string logical_id;
  std::string kind;
  std::string artifact_schema = "cidx-artifact/v1";
  std::int64_t catalog_version = catalog::kCatalogVersion;
  std::string catalog_hash = std::string(catalog::kCatalogHash);
  std::string producer_version = "unknown";
  std::string engine_version = "unknown";
  std::string workspace_identity;
  std::string tu_identity;
  std::string configuration_identity;
  std::string input_fact_set_identity;
  ArtifactCompleteness completeness = ArtifactCompleteness::unknown;
  ArtifactTruncation truncation = ArtifactTruncation::unknown;
  ArtifactTrust trust = ArtifactTrust::unverified;
  std::string evidence = "source";
  std::string attachment_name;
  std::vector<std::string> exposed_relations;
  std::string retention_policy = "retain";
};

struct ArtifactRecord {
  std::int64_t id = 0;
  ArtifactSpec spec;
  std::string relative_path;
  std::string content_hash;
  std::int64_t byte_size = 0;
  std::string state;
  std::string created_at;
  std::string published_at;
};

struct ArtifactDiagnostic {
  std::string code;
  std::string message;
};

struct ArtifactValidation {
  std::optional<ArtifactRecord> manifest;
  std::vector<ArtifactDiagnostic> diagnostics;

  [[nodiscard]] bool usable() const {
    return manifest.has_value() && diagnostics.empty();
  }
};

class ArtifactAttachment {
public:
  ~ArtifactAttachment() noexcept;
  ArtifactAttachment(const ArtifactAttachment &) = delete;
  ArtifactAttachment &operator=(const ArtifactAttachment &) = delete;
  ArtifactAttachment(ArtifactAttachment &&other) noexcept;
  ArtifactAttachment &operator=(ArtifactAttachment &&other) noexcept;

  [[nodiscard]] std::string_view name() const { return name_; }

private:
  friend class ArtifactStore;
  ArtifactAttachment(std::shared_ptr<ArtifactAttachmentLifetime> lifetime,
                     std::string name, bool previous_query_only);
  void reset() noexcept;

  std::shared_ptr<ArtifactAttachmentLifetime> lifetime_;
  std::string name_;
  bool previous_query_only_ = false;
};

class ArtifactStore {
public:
  using SidecarWriter = std::function<void(SqliteDb &)>;

  explicit ArtifactStore(Storage &storage, std::filesystem::path root = {},
                         std::size_t max_attached = 8);
  ~ArtifactStore();
  ArtifactStore(const ArtifactStore &) = delete;
  ArtifactStore &operator=(const ArtifactStore &) = delete;

  [[nodiscard]] ArtifactValidation validate(std::string_view logical_id) const;
  [[nodiscard]] std::optional<ArtifactRecord>
  current(std::string_view logical_id) const;

  ArtifactRecord publish(const ArtifactSpec &spec, const SidecarWriter &writer);
  // Adopt an already-built SQLite sidecar (for example, a per-TU astgraph)
  // into the same content-addressed, manifest-governed publication path.
  ArtifactRecord publish_existing(const ArtifactSpec &spec,
                                  const std::filesystem::path &source_path);
  using IdentityMappingWriter = std::function<void(const ArtifactRecord &)>;
  ArtifactRecord publish_existing(const ArtifactSpec &spec,
                                  const std::filesystem::path &source_path,
                                  const IdentityMappingWriter &mapping_writer);
  [[nodiscard]] std::unique_ptr<ArtifactAttachment>
  attach_current(std::string_view logical_id);

  void record_identity_mapping(std::string_view logical_id,
                               std::string_view local_identity,
                               std::string_view identity_kind,
                               std::string_view stable_identity,
                               std::string_view resolution_state,
                               std::optional<std::int64_t> core_symbol_id,
                               std::string_view diagnostic = {});
  void lease(std::string_view logical_id, std::string_view lease_id,
             std::string_view purpose);
  void unlease(std::string_view logical_id, std::string_view lease_id);
  void pin(std::string_view logical_id, std::string_view pin_id,
           std::string_view reason);
  void unpin(std::string_view logical_id, std::string_view pin_id);

  // Returns paths and diagnostics for backup/export. Required current
  // artifacts are always reported; optional artifacts are included only when
  // requested. No bytes are copied by this method.
  [[nodiscard]] std::pair<std::vector<ArtifactRecord>,
                          std::vector<ArtifactDiagnostic>>
  export_plan(bool include_optional = true) const;

  // Removes only unreferenced stale/retired artifacts and orphaned files.
  // Current artifacts, leases, and pins are never eligible.
  [[nodiscard]] std::size_t recover();

private:
  friend class ArtifactAttachment;

  [[nodiscard]] ArtifactRecord read_record(SqliteStmt &statement) const;
  [[nodiscard]] ArtifactValidation
  validate_record(const ArtifactRecord &record) const;
  [[nodiscard]] ArtifactRecord
  publish_staged(const ArtifactSpec &spec, int staging_fd, int staged_fd,
                 std::string_view staged_name,
                 const IdentityMappingWriter &mapping_writer = {});
  void release_attachment(std::string_view name,
                          bool previous_query_only) noexcept;
  void reset_query_only(bool previous) noexcept;

  Storage &storage_;
  std::filesystem::path root_;
  int root_fd_ = -1;
  std::size_t max_attached_;
  std::vector<std::string> attached_names_;
  std::optional<bool> query_only_before_attach_;
  std::shared_ptr<ArtifactAttachmentLifetime> attachment_lifetime_;
};

} // namespace cidx
