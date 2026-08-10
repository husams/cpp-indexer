// Transitional CLI adapter. Migrated commands return typed requests directly;
// commands without an application service remain an explicitly tagged
// compatibility request and are parsed by the legacy grammar in main().
#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "application/requests.hpp"
#include "cli/commands.hpp"

namespace cidx::cli {

struct CompatibilityRequest {
  std::vector<std::string> argv;
};

struct ApplicationParseResult {
  std::variant<application::CommandRequest, CompatibilityRequest> value;
};

[[nodiscard]] std::optional<application::CommandRequest>
parse_request(const std::vector<std::string> &argv);

[[nodiscard]] ApplicationParseResult
parse_application_request(const std::vector<std::string> &argv);

int run_application_request(const application::CommandRequest &request,
                            Context &ctx);

} // namespace cidx::cli
