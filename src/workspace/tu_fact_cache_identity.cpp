#include "workspace/tu_fact_cache_identity.hpp"

#include "util/hashing.hpp"
#include "util/pathutil.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <sstream>
#include <tuple>

namespace cidx {

namespace {

auto json_string(std::string_view value) -> std::string {
  std::ostringstream output;
  output << '"';
  for (const unsigned char ch : value) {
    switch (ch) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (ch < 0x20U) {
        constexpr auto digits = std::to_array("0123456789abcdef");
        output << "\\u00" << digits[(ch >> 4U) & 0x0fU] << digits[ch & 0x0fU];
      } else {
        output << static_cast<char>(ch);
      }
    }
  }
  output << '"';
  return output.str();
}

auto dependency_json(const TuFactDependencyIdentity &dependency)
    -> std::string {
  return "{\"conditional_context\":" +
         json_string(dependency.conditional_context) +
         ",\"content_sha256\":" + json_string(dependency.content_sha256) +
         ",\"kind\":" + json_string(dependency.kind) +
         ",\"path\":" + json_string(dependency.path) + "}";
}

auto environment_json(const TuFactEnvironmentIdentity &environment)
    -> std::string {
  return "{\"name\":" + json_string(environment.name) +
         ",\"value_sha256\":" + json_string(environment.value_sha256) + "}";
}

} // namespace

auto canonical_tu_cache_path(std::string_view path) -> std::string {
  if (path.empty()) {
    return {};
  }
  return pathutil::normpath(pathutil::abspath(std::string(path)));
}

auto make_tu_fact_cache_identity(TuFactCacheIdentityInput input)
    -> TuFactCacheIdentity {
  input.main_source_path = canonical_tu_cache_path(input.main_source_path);
  for (TuFactDependencyIdentity &dependency : input.dependencies) {
    dependency.path = canonical_tu_cache_path(dependency.path);
  }
  std::ranges::sort(input.dependencies, {}, [](const auto &dependency) {
    return std::tie(dependency.path, dependency.kind,
                    dependency.conditional_context, dependency.content_sha256);
  });
  input.dependencies.erase(
      std::ranges::unique(input.dependencies, {},
                          [](const auto &dependency) {
                            return std::tie(dependency.path, dependency.kind,
                                            dependency.conditional_context,
                                            dependency.content_sha256);
                          })
          .begin(),
      input.dependencies.end());
  std::ranges::sort(input.environment, {}, [](const auto &environment) {
    return std::tie(environment.name, environment.value_sha256);
  });
  input.environment.erase(std::ranges::unique(input.environment, {},
                                              [](const auto &environment) {
                                                return std::tie(
                                                    environment.name,
                                                    environment.value_sha256);
                                              })
                              .begin(),
                          input.environment.end());

  std::string dependencies = "[";
  for (std::size_t index = 0; index < input.dependencies.size(); ++index) {
    if (index != 0U) {
      dependencies += ',';
    }
    dependencies += dependency_json(input.dependencies[index]);
  }
  dependencies += ']';

  std::string environment = "[";
  for (std::size_t index = 0; index < input.environment.size(); ++index) {
    if (index != 0U) {
      environment += ',';
    }
    environment += environment_json(input.environment[index]);
  }
  environment += ']';

  const TuFactCacheVersionIdentity &versions = input.versions;
  const std::string canonical =
      "{\"clang_identity\":" + json_string(input.clang_identity) +
      ",\"dependencies\":" + dependencies + ",\"environment\":" + environment +
      ",\"front_end_reuse_identity\":" +
      json_string(input.front_end_reuse_identity) +
      R"(,"main_source":{"content_sha256":)" +
      json_string(input.main_source_sha256) +
      ",\"path\":" + json_string(input.main_source_path) +
      "},\"translation_unit_descriptor\":" +
      json_string(input.translation_unit_descriptor) +
      ",\"version\":" + json_string(kTuFactCacheIdentityVersion) +
      R"(,"versions":{"artifact":)" +
      std::to_string(versions.artifact_version) +
      ",\"catalog\":" + std::to_string(versions.catalog_version) +
      ",\"catalog_hash\":" + json_string(versions.catalog_hash) +
      ",\"extractor\":" + std::to_string(versions.extractor_version) +
      ",\"pass\":" + std::to_string(versions.pass_version) +
      ",\"product\":" + json_string(versions.product_version) +
      ",\"storage_schema\":" + std::to_string(versions.storage_schema_version) +
      "}}";

  return {.version = std::string(kTuFactCacheIdentityVersion),
          .canonical_bytes = canonical,
          .sha256 = sha256_hex(canonical)};
}

} // namespace cidx
