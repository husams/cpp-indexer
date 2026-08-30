// Portable, database-independent identities used by immutable fact batches.
#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cidx::ast {

enum class FactIdentityKind : std::uint8_t {
  symbol,
  relation,
  type,
  definition,
  file,
  handle,
};

struct PortableFileIdentity {
  std::string component_path;
  std::string directory_path;
  std::string file_name;

  [[nodiscard]] auto portable_path() const -> std::string;
  auto operator<=>(const PortableFileIdentity &) const = default;
};

struct PortableConfigurationContent {
  std::optional<std::string> driver;
  std::optional<std::string> working_dir;
  std::vector<std::string> arguments;
  std::optional<std::string> lang_mode;
  std::optional<std::string> resource_dir;

  auto operator<=>(const PortableConfigurationContent &) const = default;
};

struct ConfigurationIdentity {
  std::string semantic_universe;
  std::string translation_unit;
  std::string normalized_configuration;
  std::string identity_source;
  // Reconstructable IncludeConfig inputs. normalized_configuration is their
  // portable digest; neither field may contain a database configuration id.
  PortableConfigurationContent content{};

  auto operator<=>(const ConfigurationIdentity &) const = default;
};

struct FactPartitionKey {
  PortableFileIdentity file;
  ConfigurationIdentity configuration;

  [[nodiscard]] auto stable_string() const -> std::string;
  auto operator<=>(const FactPartitionKey &) const = default;
};

struct SymbolNaturalKey {
  FactPartitionKey partition;
  std::string usr;
  std::optional<std::string> local_anchor;
  std::optional<std::string> linkage;

  [[nodiscard]] auto stable_string() const -> std::string;
  auto operator<=>(const SymbolNaturalKey &) const = default;
};

struct TypeNaturalKey {
  FactPartitionKey partition;
  std::string type_key;

  [[nodiscard]] auto stable_string() const -> std::string;
  auto operator<=>(const TypeNaturalKey &) const = default;
};

struct RelationNaturalKey {
  SymbolNaturalKey source;
  SymbolNaturalKey destination;
  std::int64_t kind = 0;
  std::optional<std::int64_t> base_access;
  std::optional<std::int64_t> is_virtual;

  [[nodiscard]] auto stable_string() const -> std::string;
  auto operator<=>(const RelationNaturalKey &) const = default;
};

// This is the exact legacy serial publication order. first_seen and
// conflict_ordinal retain the observable tie-break when natural identities
// coalesce but their payloads disagree.
struct LegacyApplyOrderKey {
  std::string component_path;
  std::string directory_path;
  std::string file_name;
  std::uint64_t first_seen = 0;
  std::uint64_t conflict_ordinal = 0;

  auto operator<=>(const LegacyApplyOrderKey &) const = default;
};

[[nodiscard]] auto stable_fact_hash(std::string_view value) -> std::uint64_t;

// Existing emitter ports exchange int64_t values. These handles are transient
// collision-safe references into the batch's natural-key dictionaries; they
// are never database row ids. Distinct natural keys are checked by equality,
// even if the injected primary fingerprint collides.
class CollisionSafeHandleIndex {
public:
  using Hasher = std::function<std::uint64_t(std::string_view)>;

  explicit CollisionSafeHandleIndex(Hasher primary = stable_fact_hash);

  auto find_or_insert(std::string key) -> std::int64_t;
  [[nodiscard]] auto find(std::string_view key) const
      -> std::optional<std::int64_t>;
  [[nodiscard]] auto key_for(std::int64_t handle) const
      -> std::optional<std::string>;
  [[nodiscard]] auto entries() const
      -> const std::map<std::int64_t, std::string> &;

private:
  [[nodiscard]] auto candidate_for(std::string_view key,
                                   std::uint64_t attempt) const -> std::int64_t;

  Hasher primary_;
  std::unordered_map<std::string, std::int64_t> handles_by_key_;
  std::map<std::int64_t, std::string> keys_by_handle_;
};

} // namespace cidx::ast
