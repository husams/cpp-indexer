// Clang-dependent half of validator.hpp: parses each rule's matcher
// expression through Clang's own dynamic AST matcher parser (ADR-013) --
// this CONSTRUCTS a matcher, it never executes one against a translation
// unit's AST -- and checks it against the CIDX allow-list and property/domain
// catalog.
#include "extract/validator.hpp"

#include "clang/ASTMatchers/Dynamic/Diagnostics.h"
#include "clang/ASTMatchers/Dynamic/Parser.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>

namespace cidx::extract {

namespace {

void add(ValidationResult &result, const std::string &rule_id,
         ValidationErrorCode code, std::string message) {
  result.errors.push_back(ValidationError{
      .rule_id = rule_id, .code = code, .message = std::move(message)});
}

bool matcher_expression_binds(const std::string &expression,
                              const std::string &name) {
  return expression.contains(".bind(\"" + name + "\")");
}

const Binding *find_binding(const std::vector<Binding> &bindings,
                            const std::string &name) {
  auto it = std::ranges::find_if(
      bindings, [&](const Binding &binding) { return binding.name == name; });
  return it == bindings.end() ? nullptr : &*it;
}

void validate_rule_matcher(ValidationResult &result, const ExtractionRule &rule,
                           const MatcherCatalog &catalog) {
  if (rule.matcher_expression.empty()) {
    // Already reported by validate_structure(); nothing further to check.
    return;
  }
  llvm::StringRef code(rule.matcher_expression);
  clang::ast_matchers::dynamic::Diagnostics diagnostics;
  auto matcher = clang::ast_matchers::dynamic::Parser::parseMatcherExpression(
      code, &diagnostics);
  if (!matcher) {
    add(result, rule.id, ValidationErrorCode::unknown_matcher,
        "matcher expression failed to parse: " + diagnostics.toStringFull());
    return;
  }
  const llvm::StringRef remainder = code.trim();
  if (!remainder.empty()) {
    add(result, rule.id, ValidationErrorCode::unknown_matcher,
        "matcher expression has trailing content after the first matcher: " +
            remainder.str());
  }
  for (const auto &disallowed_id :
       disallowed_matcher_calls(rule.matcher_expression, catalog)) {
    add(result, rule.id, ValidationErrorCode::unknown_matcher,
        "matcher/predicate is not on the CIDX allow-list: " + disallowed_id);
  }
  for (const auto &binding : rule.bindings) {
    if (!matcher_expression_binds(rule.matcher_expression, binding.name)) {
      add(result, rule.id, ValidationErrorCode::invalid_binding,
          "declared binding is never produced by .bind(...) in the matcher "
          "expression: " +
              binding.name);
    }
  }
  for (const auto &emit : rule.emits) {
    if (!emit.attribute) {
      continue;
    }
    const std::string &property = emit.attribute->ast_property;
    if (!catalog.allows_property(property)) {
      add(result, rule.id, ValidationErrorCode::unknown_property,
          "attribute ast_property is not on the CIDX allow-list: " + property);
      continue;
    }
    const auto required = catalog.required_domain(property);
    if (!required) {
      continue;
    }
    const Binding *binding =
        find_binding(rule.bindings, emit.attribute->binding);
    if (binding != nullptr && binding->domain != *required) {
      add(result, rule.id, ValidationErrorCode::endpoint_type_mismatch,
          "attribute '" + property + "' requires endpoint domain " +
              to_string(*required) + " but binding '" +
              emit.attribute->binding + "' declares domain " +
              to_string(binding->domain));
    }
  }
}

} // namespace

ValidationResult validate_matchers(const ExtractionPlan &plan,
                                   const MatcherCatalog &catalog) {
  ValidationResult result;
  for (const auto &rule : plan.rules) {
    validate_rule_matcher(result, rule, catalog);
  }
  return result;
}

ValidationResult validate(const ExtractionPlan &plan,
                          const ValidationLimits &limits,
                          const MatcherCatalog &catalog) {
  ValidationResult result = validate_structure(plan, limits);
  result.merge(validate_matchers(plan, catalog));
  return result;
}

} // namespace cidx::extract
