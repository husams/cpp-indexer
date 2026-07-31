#pragma once

#include "storage/records.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cidx::ast {

inline constexpr std::string_view kFrontEndReuseIdentityVersion =
    "front-end-reuse/v1";

enum class FrontEndReuseMechanism : std::uint8_t {
  none,
  generated_umbrella_pch,
  precompiled_preamble,
};

struct FrontEndReusePrefixIdentity {
  std::string path;
  std::string content_sha256;
  std::vector<std::string> dependency_sha256;
};

struct FrontEndReuseIdentity {
  std::string version;
  std::string mechanism;
  std::string canonical_bytes;
  std::string sha256;
};

struct FrontEndReusePlan {
  FrontEndReuseMechanism mechanism = FrontEndReuseMechanism::none;
  bool inject = false;
  std::string reason;
  FrontEndReuseIdentity identity;
};

[[nodiscard]] std::string_view
front_end_reuse_mechanism_name(FrontEndReuseMechanism mechanism) noexcept;

// The identity is deliberately derived from the complete normalized
// TranslationUnitConfig rather than a hand-picked cache-key subset. This
// makes every compiler, target, language, ABI, path, macro, environment,
// generated-input, diagnostic-policy, and invocation change invalidate a
// future reusable prefix. Prefix dependencies are canonicalized by path.
[[nodiscard]] FrontEndReuseIdentity make_front_end_reuse_identity(
    const TranslationUnitConfig &configuration,
    const std::vector<FrontEndReusePrefixIdentity> &prefixes = {},
    FrontEndReuseMechanism mechanism = FrontEndReuseMechanism::none);

// ADR-014 currently selects no shipped reuse mechanism. The plan remains a
// real contract seam for HSE-111: it always returns the versioned none
// identity and never constructs or injects a generated artifact.
[[nodiscard]] FrontEndReusePlan
plan_front_end_reuse(const TranslationUnitConfig &configuration,
                     bool explicitly_disabled = false);

} // namespace cidx::ast
