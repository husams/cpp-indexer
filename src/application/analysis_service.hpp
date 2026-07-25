#pragma once

#include <string>

#include "application/requests.hpp"
#include "query/result_protocol.hpp"

namespace cidx::cli {

// Shared production analysis entry point. The legacy CLI and the typed
// application adapter use the same Souffle implementation and result shape.
[[nodiscard]] protocol::ResultEnvelope
run_analysis_application(const application::AnalysisRequest &request,
                         const std::string &index_path);

} // namespace cidx::cli
