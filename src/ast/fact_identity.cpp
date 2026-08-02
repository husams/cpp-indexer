#include "ast/fact_identity.hpp"

#include <bit>
#include <limits>
#include <utility>

namespace cidx::ast {

namespace {

void append_field(std::string &result, std::string_view field) {
  result += std::to_string(field.size());
  result += ':';
  result.append(field);
}

void append_optional(std::string &result,
                     const std::optional<std::string> &field) {
  if (!field) {
    result += "-;";
    return;
  }
  result += "+";
  append_field(result, *field);
  result += ';';
}

void append_optional(std::string &result,
                     const std::optional<std::int64_t> &field) {
  if (!field) {
    result += "-;";
    return;
  }
  result += "+" + std::to_string(*field) + ';';
}

} // namespace

auto PortableFileIdentity::portable_path() const -> std::string {
  std::string result = component_path;
  if (!result.empty() && !directory_path.empty()) {
    result += '/';
  }
  result += directory_path;
  if (!result.empty() && !file_name.empty()) {
    result += '/';
  }
  result += file_name;
  return result;
}

auto FactPartitionKey::stable_string() const -> std::string {
  std::string result;
  append_field(result, file.component_path);
  append_field(result, file.directory_path);
  append_field(result, file.file_name);
  append_field(result, configuration.semantic_universe);
  append_field(result, configuration.translation_unit);
  append_field(result, configuration.normalized_configuration);
  append_field(result, configuration.identity_source);
  append_optional(result, configuration.content.driver);
  append_optional(result, configuration.content.working_dir);
  result += std::to_string(configuration.content.arguments.size()) + ';';
  for (const std::string &argument : configuration.content.arguments) {
    append_field(result, argument);
  }
  append_optional(result, configuration.content.lang_mode);
  append_optional(result, configuration.content.resource_dir);
  return result;
}

auto SymbolNaturalKey::stable_string() const -> std::string {
  std::string result;
  append_field(result, partition.configuration.semantic_universe);
  const bool local =
      linkage && (*linkage == "internal" || *linkage == "no-linkage");
  if (local) {
    append_field(result, "local");
    append_field(result, partition.configuration.translation_unit);
    append_field(result, partition.configuration.identity_source);
    append_optional(result, local_anchor);
  }
  append_field(result, usr);
  return result;
}

auto TypeNaturalKey::stable_string() const -> std::string {
  std::string result = partition.stable_string();
  append_field(result, type_key);
  return result;
}

auto RelationNaturalKey::stable_string() const -> std::string {
  std::string result = source.stable_string();
  append_field(result, destination.stable_string());
  result += std::to_string(kind) + ';';
  append_optional(result, base_access);
  append_optional(result, is_virtual);
  return result;
}

auto stable_fact_hash(std::string_view value) -> std::uint64_t {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

CollisionSafeHandleIndex::CollisionSafeHandleIndex(Hasher primary)
    : primary_(std::move(primary)) {}

auto CollisionSafeHandleIndex::candidate_for(std::string_view key,
                                             std::uint64_t attempt) const
    -> std::int64_t {
  std::string salted(key);
  salted += '#';
  salted += std::to_string(attempt);
  const std::uint64_t primary = primary_(key);
  const std::uint64_t secondary = stable_fact_hash(salted);
  constexpr auto handle_mask =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  const std::uint64_t mixed = primary ^ std::rotl(secondary, 23);
  return static_cast<std::int64_t>((mixed & handle_mask) | 1ULL);
}

auto CollisionSafeHandleIndex::find_or_insert(std::string key) -> std::int64_t {
  if (const auto found = handles_by_key_.find(key);
      found != handles_by_key_.end()) {
    return found->second;
  }
  for (std::uint64_t attempt = 0;; ++attempt) {
    const std::int64_t candidate = candidate_for(key, attempt);
    const auto occupied = keys_by_handle_.find(candidate);
    if (occupied != keys_by_handle_.end() && occupied->second != key) {
      continue;
    }
    keys_by_handle_.emplace(candidate, key);
    handles_by_key_.emplace(std::move(key), candidate);
    return candidate;
  }
}

auto CollisionSafeHandleIndex::find(std::string_view key) const
    -> std::optional<std::int64_t> {
  const auto found = handles_by_key_.find(std::string(key));
  if (found == handles_by_key_.end()) {
    return std::nullopt;
  }
  return found->second;
}

auto CollisionSafeHandleIndex::key_for(std::int64_t handle) const
    -> std::optional<std::string> {
  const auto found = keys_by_handle_.find(handle);
  if (found == keys_by_handle_.end()) {
    return std::nullopt;
  }
  return found->second;
}

auto CollisionSafeHandleIndex::entries() const
    -> const std::map<std::int64_t, std::string> & {
  return keys_by_handle_;
}

} // namespace cidx::ast
