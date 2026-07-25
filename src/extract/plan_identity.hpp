// Deterministic ExtractionPlan identity: sha256("sha256:<hex>") over the
// plan's canonical JSON (plan_json.hpp). Any change to a rule's matcher
// expression, bindings, emits, scope, traversal mode, budgets, catalog
// versions, or producer/package identity changes the canonical JSON and
// therefore this hash -- HSE-64 "changing source/configuration, rule,
// traversal mode, catalog, budget-affecting semantics ... changes artifact
// identity".
#pragma once

#include "extract/plan_ir.hpp"

#include <string>

namespace cidx::extract {

[[nodiscard]] std::string plan_hash(const ExtractionPlan &plan);

} // namespace cidx::extract
