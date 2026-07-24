// query/cxq.hpp -- dependency-free textual front end for the CXQ pipeline.
#pragma once

#include <string>

#include "query/plan.hpp"

namespace cidx::query {

// Parse the textual CXQ pipeline directly into the immutable QueryPlan IR.
// Syntax errors use the stable E_PARSE PlanError code; semantic errors are
// reported by the existing validate() pass.
Plan parse_cxq(const std::string &text);

} // namespace cidx::query
