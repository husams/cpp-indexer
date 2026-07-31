#include "ast/front_end_reuse.hpp"

#include "util/hashing.hpp"
#include "util/json_min.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

namespace cidx::ast {

namespace {

std::string json_quote(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (character < 0x20U) {
        constexpr std::string_view hex = "0123456789abcdef";
        out += "\\u00";
        out.push_back(hex[(character >> 4U) & 0x0fU]);
        out.push_back(hex[character & 0x0fU]);
      } else {
        out.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  out.push_back('"');
  return out;
}

std::string optional_json(const std::optional<std::string> &value) {
  return json_quote(value.value_or(""));
}

std::string configuration_json(const TranslationUnitConfig &configuration) {
  return "[" + optional_json(configuration.driver) + "," +
         optional_json(configuration.working_dir) + "," +
         optional_json(configuration.language) + "," +
         optional_json(configuration.standard) + "," +
         optional_json(configuration.target) + "," +
         json_min::encode_string_array(configuration.abi_options) + "," +
         optional_json(configuration.sysroot) + "," +
         optional_json(configuration.resource_dir) + "," +
         json_min::encode_string_array(configuration.include_paths) + "," +
         json_min::encode_string_array(configuration.macro_state) + "," +
         json_min::encode_string_array(configuration.relevant_environment) +
         "," + json_min::encode_string_array(configuration.generated_inputs) +
         "," + optional_json(configuration.diagnostics_policy) + "," +
         json_min::encode_string_array(configuration.arguments) + "]";
}

std::string
prefixes_json(const std::vector<FrontEndReusePrefixIdentity> &prefixes) {
  std::vector<FrontEndReusePrefixIdentity> ordered = prefixes;
  std::ranges::sort(ordered, [](const auto &left, const auto &right) {
    return std::tie(left.path, left.content_sha256, left.dependency_sha256) <
           std::tie(right.path, right.content_sha256, right.dependency_sha256);
  });
  std::string out = "[";
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    if (index != 0) {
      out.push_back(',');
    }
    auto dependencies = ordered[index].dependency_sha256;
    std::ranges::sort(dependencies);
    out += "{\"path\":" + json_quote(ordered[index].path) +
           ",\"content_sha256\":" + json_quote(ordered[index].content_sha256) +
           ",\"dependency_sha256\":" +
           json_min::encode_string_array(dependencies) + "}";
  }
  out.push_back(']');
  return out;
}

} // namespace

std::string_view
front_end_reuse_mechanism_name(FrontEndReuseMechanism mechanism) noexcept {
  switch (mechanism) {
  case FrontEndReuseMechanism::none:
    return "none";
  case FrontEndReuseMechanism::generated_umbrella_pch:
    return "generated-umbrella-pch";
  case FrontEndReuseMechanism::precompiled_preamble:
    return "precompiled-preamble";
  }
  return "none";
}

FrontEndReuseIdentity make_front_end_reuse_identity(
    const TranslationUnitConfig &configuration,
    const std::vector<FrontEndReusePrefixIdentity> &prefixes,
    FrontEndReuseMechanism mechanism) {
  FrontEndReuseIdentity identity;
  identity.version = std::string(kFrontEndReuseIdentityVersion);
  identity.mechanism = std::string(front_end_reuse_mechanism_name(mechanism));
  identity.canonical_bytes =
      "{\"version\":" + json_quote(identity.version) +
      ",\"mechanism\":" + json_quote(identity.mechanism) +
      ",\"configuration\":" + configuration_json(configuration) +
      ",\"prefixes\":" + prefixes_json(prefixes) + "}";
  identity.sha256 = sha256_hex(identity.canonical_bytes);
  return identity;
}

FrontEndReusePlan
plan_front_end_reuse(const TranslationUnitConfig &configuration,
                     bool explicitly_disabled) {
  return FrontEndReusePlan{
      .mechanism = FrontEndReuseMechanism::none,
      .inject = false,
      .reason =
          explicitly_disabled
              ? "front-end reuse explicitly disabled; using none"
              : "ADR-014 do-not-ship: no reusable artifact is constructed",
      .identity = make_front_end_reuse_identity(configuration),
  };
}

} // namespace cidx::ast
