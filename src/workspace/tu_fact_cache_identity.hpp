// Canonical identity for one translation-unit FactBatch cache entry.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "util/version.hpp"

namespace cidx {

inline constexpr std::string_view kTuFactCacheIdentityVersion =
    "tu-fact-cache/v1";
inline constexpr std::string_view kNoFrontEndReuseIdentity =
    "front-end-reuse/v1:none";

struct TuFactDependencyIdentity {
  std::string path;
  std::string content_sha256;
  std::string kind = "include";
  std::string conditional_context;
};

struct TuFactEnvironmentIdentity {
  std::string name;
  std::string value_sha256;
};

struct TuFactCacheVersionIdentity {
  std::string product_version = std::string(version::kFullProductVersion);
  int artifact_version = version::kArtifactVersion;
  int extractor_version = 1;
  int pass_version = 1;
  int storage_schema_version = version::kDatabaseSchemaVersion;
  int catalog_version = version::kCatalogVersion;
  std::string catalog_hash = std::string(version::kCatalogHash);
};

struct TuFactCacheIdentityInput {
  std::string main_source_path;
  std::string main_source_sha256;
  std::string translation_unit_descriptor;
  std::string clang_identity;
  std::string front_end_reuse_identity = std::string(kNoFrontEndReuseIdentity);
  TuFactCacheVersionIdentity versions;
  std::vector<TuFactEnvironmentIdentity> environment;
  std::vector<TuFactDependencyIdentity> dependencies;
};

struct TuFactCacheIdentity {
  std::string version;
  std::string canonical_bytes;
  std::string sha256;
};

[[nodiscard]] auto canonical_tu_cache_path(std::string_view path)
    -> std::string;

// The constructor deliberately has no timestamp, size, or database-row-id
// input. Cache validity is content- and configuration-addressed only.
[[nodiscard]] auto make_tu_fact_cache_identity(TuFactCacheIdentityInput input)
    -> TuFactCacheIdentity;

} // namespace cidx
