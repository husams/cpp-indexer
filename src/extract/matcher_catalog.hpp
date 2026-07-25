// The CIDX allow-list layer over Clang's dynamic AST matcher registry
// (ADR-013). A matcher expression may construct perfectly validly through
// Clang's own parser and still be rejected here because it is not on the
// catalog CIDX has reviewed for fact-emission use (no unbounded catch-alls,
// no matchers whose only purpose is clang-query REPL exploration).
//
// This is deliberately a small, explicit, checked-in set rather than "every
// matcher Clang knows about" -- growing it is a reviewed catalog change.
#pragma once

#include "extract/plan_ir.hpp"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cidx::extract {

class MatcherCatalog {
public:
  [[nodiscard]] static const MatcherCatalog &default_catalog();

  // A bare matcher/narrowing-predicate call, e.g. `functionDecl(...)` or
  // `hasName(...)`. `.bind("x")` is checked separately (it is always
  // permitted; it is the binding mechanism itself, not a matcher).
  [[nodiscard]] bool allows_matcher(const std::string &matcher_id) const;

  // A typed AST property named by an EmitAttribute.
  [[nodiscard]] bool allows_property(const std::string &property_name) const;

  // The endpoint domain a property is only meaningful for, if the catalog
  // constrains it; nullopt means the property applies to any domain.
  [[nodiscard]] std::optional<EndpointDomain>
  required_domain(const std::string &property_name) const;

  MatcherCatalog(
      std::set<std::string> matcher_ids,
      std::vector<std::pair<std::string, EndpointDomain>> properties);

private:
  std::set<std::string> matcher_ids_;
  std::vector<std::pair<std::string, EndpointDomain>> properties_;
};

// Lexically scans `matcher_expression` for bare-identifier call sites
// (`identifier(`) and `.bind(` method calls, ignoring string-literal
// contents, and returns every disallowed identifier found. An empty result
// means every call site in the expression is on the catalog (or is `bind`).
// This is a conservative textual check layered in front of -- not instead
// of -- Clang's own matcher-expression parser (validator.hpp).
[[nodiscard]] std::vector<std::string>
disallowed_matcher_calls(const std::string &matcher_expression,
                         const MatcherCatalog &catalog);

} // namespace cidx::extract
