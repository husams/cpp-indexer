// Canonical, versioned FactBatch wire artifacts with bounded spill and decode.
#pragma once

#include "ast/fact_batch.hpp"
#include "catalogs/generated_catalog.hpp"
#include "util/artifact_io.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cidx::ast {

inline constexpr std::uint32_t kFactBatchArtifactWireVersion = 1;
inline constexpr std::uint32_t kFactBatchPassVersion = 1;
inline constexpr std::uint32_t kFactBatchExtractorVersion = 1;
inline constexpr std::uint32_t kFactBatchStorageSchemaVersion = 40;
inline constexpr std::uint64_t kDefaultMaxFactBatchArtifactBytes =
    512ULL * 1024ULL * 1024ULL;

enum class FactBatchArtifactErrorCode : std::uint8_t {
  missing,
  stale,
  corrupt,
  incompatible,
  partial,
  truncated,
  limit_exceeded,
  io_error,
  spill_failure,
  noncanonical,
};

[[nodiscard]] auto to_string(FactBatchArtifactErrorCode code)
    -> std::string_view;

class FactBatchArtifactError final : public std::runtime_error {
public:
  FactBatchArtifactError(FactBatchArtifactErrorCode code,
                         const std::string &message);
  [[nodiscard]] auto code() const noexcept -> FactBatchArtifactErrorCode;

private:
  FactBatchArtifactErrorCode code_;
};

struct FactBatchArtifactMetadata {
  std::uint32_t pass_version = kFactBatchPassVersion;
  std::uint32_t extractor_version = kFactBatchExtractorVersion;
  std::uint32_t storage_schema_version = kFactBatchStorageSchemaVersion;
  std::int64_t catalog_version = cidx::catalog::kCatalogVersion;
  std::string catalog_hash = std::string(cidx::catalog::kCatalogHash);
  FactTrust trust = FactTrust::trusted;
  bool truncated = false;
  std::vector<std::string> dependency_identities;
};

struct FactBatchArtifactInfo {
  std::uint32_t wire_version = kFactBatchArtifactWireVersion;
  std::string producer;
  std::uint32_t producer_version = 0;
  std::uint32_t pass_version = 0;
  std::uint32_t extractor_version = 0;
  std::uint32_t storage_schema_version = 0;
  std::int64_t catalog_version = 0;
  std::string catalog_hash;
  FactCompleteness completeness = FactCompleteness::unknown;
  FactTrust trust = FactTrust::provisional;
  bool truncated = false;
  std::vector<std::string> dependency_identities;
  std::string content_digest;
  std::uint64_t byte_size = 0;
};

struct FactBatchArtifactEncodeOptions {
  FactBatchArtifactMetadata metadata;
  std::size_t spill_threshold_bytes = std::size_t{8} * 1024U * 1024U;
  std::string spill_directory;
  std::uint64_t max_artifact_bytes = kDefaultMaxFactBatchArtifactBytes;
};

struct FactBatchArtifactDecodeLimits {
  std::uint64_t max_artifact_bytes = kDefaultMaxFactBatchArtifactBytes;
  std::uint64_t max_string_bytes = 16ULL * 1024ULL * 1024ULL;
  std::uint64_t max_collection_entries = 10'000'000ULL;
  std::uint64_t max_total_entries = 20'000'000ULL;
};

struct FactBatchArtifactCompatibility {
  std::uint32_t wire_version = kFactBatchArtifactWireVersion;
  std::optional<std::string> producer;
  std::optional<std::uint32_t> producer_version;
  std::uint32_t pass_version = kFactBatchPassVersion;
  std::uint32_t extractor_version = kFactBatchExtractorVersion;
  std::uint32_t storage_schema_version = kFactBatchStorageSchemaVersion;
  std::int64_t catalog_version = cidx::catalog::kCatalogVersion;
  std::string catalog_hash = std::string(cidx::catalog::kCatalogHash);
  std::optional<std::vector<std::string>> dependency_identities;
  bool allow_partial = false;
  bool allow_truncated = false;
  bool require_trusted = true;
  FactBatchArtifactDecodeLimits limits;
};

class FactBatchArtifact {
public:
  FactBatchArtifact() = default;

  [[nodiscard]] static auto from_bytes(std::vector<std::byte> bytes)
      -> FactBatchArtifact;
  [[nodiscard]] static auto from_file(std::string_view path)
      -> FactBatchArtifact;

  [[nodiscard]] auto byte_size() const -> std::uint64_t;
  [[nodiscard]] auto spilled() const -> bool;
  [[nodiscard]] auto peak_memory_bytes() const -> std::size_t;
  void write_to(std::ostream &output) const;
  void materialize(std::string_view path) const;

private:
  explicit FactBatchArtifact(util::ImmutableByteArtifact storage);
  util::ImmutableByteArtifact storage_;

  friend class FactBatchArtifactCodec;
  friend class FactBatchArtifactInput;
};

struct FactBatchArtifactDiagnostic {
  FactBatchArtifactErrorCode code = FactBatchArtifactErrorCode::corrupt;
  std::string message;
};

struct FactBatchArtifactDecodeResult {
  std::optional<FactBatch> batch;
  std::optional<FactBatchArtifactInfo> info;
  std::vector<FactBatchArtifactDiagnostic> diagnostics;

  [[nodiscard]] auto usable() const -> bool {
    return batch.has_value() && diagnostics.empty();
  }
};

[[nodiscard]] auto
encode_fact_batch_artifact(const FactBatch &batch,
                           const FactBatchArtifactEncodeOptions &options = {})
    -> FactBatchArtifact;

[[nodiscard]] auto decode_fact_batch_artifact(
    const FactBatchArtifact &artifact,
    const FactBatchArtifactCompatibility &compatibility = {})
    -> FactBatchArtifactDecodeResult;

} // namespace cidx::ast
