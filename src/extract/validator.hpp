// ExtractionPlan validation, run BEFORE a plan is ever executed against a
// Clang FrontendSession (HSE-64: "Enforce validation before Clang
// execution").
//
// Split in two so the plan-structure/policy checks stay entirely Clang-free:
//   - validate_structure(): schema version, budgets, scope bounds, binding
//     references, identity-recipe well-formedness, forbidden-capability
//     text scan. No Clang header is touched to answer these questions.
//   - validate_matchers(): parses every rule's matcher expression through
//     Clang's own dynamic AST matcher parser (constructs, never executes,
//     the matcher -- ADR-013) and checks the CIDX allow-list/property-domain
//     rules from matcher_catalog.hpp.
// validate() runs both and merges the errors. A plan is safe to run only
// when validate(plan).ok().
#pragma once

#include "extract/matcher_catalog.hpp"
#include "extract/plan_ir.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cidx::extract {

enum class ValidationErrorCode : std::uint8_t {
  malformed_plan,
  unsupported_schema_version,
  unknown_matcher,
  unknown_property,
  invalid_binding,
  endpoint_type_mismatch,
  unstable_identity,
  unbounded_scope,
  excessive_budget,
  forbidden_capability,
};

struct ValidationError {
  std::string rule_id; // empty for a plan-level error
  ValidationErrorCode code;
  std::string message;
};

struct ValidationResult {
  std::vector<ValidationError> errors;

  [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
  void merge(ValidationResult other);
};

struct ValidationLimits {
  std::int64_t max_matches_ceiling = 1'000'000;
  std::int64_t max_emitted_facts_ceiling = 1'000'000;
  std::int64_t max_visited_nodes_ceiling = 50'000'000;
  // workspace scope can traverse the whole repository, so it gets its own
  // (tighter) ceiling on top of the general one.
  std::int64_t workspace_max_visited_nodes_ceiling = 20'000'000;
};

[[nodiscard]] std::string to_string(ValidationErrorCode code);

[[nodiscard]] ValidationResult
validate_structure(const ExtractionPlan &plan,
                   const ValidationLimits &limits = {});

[[nodiscard]] ValidationResult validate_matchers(
    const ExtractionPlan &plan,
    const MatcherCatalog &catalog = MatcherCatalog::default_catalog());

[[nodiscard]] ValidationResult
validate(const ExtractionPlan &plan, const ValidationLimits &limits = {},
         const MatcherCatalog &catalog = MatcherCatalog::default_catalog());

} // namespace cidx::extract
