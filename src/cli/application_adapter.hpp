// Transitional CLI adapter for the typed application boundary.  The legacy
// ParsedArgs path remains available for commands that have not migrated yet.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "application/requests.hpp"
#include "cli/args.hpp"
#include "cli/commands.hpp"

namespace cidx::cli {

[[nodiscard]] std::optional<application::CommandRequest>
try_build_application_request(const ParsedArgs &args);

// Typed parser entry point for migrated operations. Commands without a
// migrated service return nullopt and remain on the compatibility adapter.
[[nodiscard]] std::optional<application::CommandRequest>
parse_request(const std::vector<std::string> &argv);

int run_application_request(const application::CommandRequest &request,
                            Context &ctx);

} // namespace cidx::cli
