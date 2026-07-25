// Clang-free half of validator.hpp: plan structure and safety-envelope
// checks that never need to construct or run a Clang matcher.
#include "extract/validator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <set>
#include <string_view>

namespace cidx::extract {

void ValidationResult::merge(ValidationResult other) {
  errors.insert(errors.end(), std::make_move_iterator(other.errors.begin()),
                std::make_move_iterator(other.errors.end()));
}

std::string to_string(ValidationErrorCode code) {
  switch (code) {
  case ValidationErrorCode::malformed_plan:
    return "malformed_plan";
  case ValidationErrorCode::unsupported_schema_version:
    return "unsupported_schema_version";
  case ValidationErrorCode::unknown_matcher:
    return "unknown_matcher";
  case ValidationErrorCode::unknown_property:
    return "unknown_property";
  case ValidationErrorCode::invalid_binding:
    return "invalid_binding";
  case ValidationErrorCode::endpoint_type_mismatch:
    return "endpoint_type_mismatch";
  case ValidationErrorCode::unstable_identity:
    return "unstable_identity";
  case ValidationErrorCode::unbounded_scope:
    return "unbounded_scope";
  case ValidationErrorCode::excessive_budget:
    return "excessive_budget";
  case ValidationErrorCode::forbidden_capability:
    return "forbidden_capability";
  }
  return "unknown";
}

namespace {

void add(ValidationResult &result, const std::string &rule_id,
         ValidationErrorCode code, std::string message) {
  result.errors.push_back(ValidationError{
      .rule_id = rule_id, .code = code, .message = std::move(message)});
}

// Mirrors the Python SDK's FORBIDDEN_CAPABILITIES
// (python/indexer/extensions.py): belt-and-braces text scan. The IR itself has
// no field that can name an executable, shared library, or interpreter to
// invoke, so this can never be the ONLY thing standing between a rule and code
// execution -- it exists so a namespace/relation/attribute name cannot even
// spell out an intent to.
constexpr std::array<std::string_view, 6> kForbiddenTokens = {
    "sql", "executor", "shell", "python", "shared_library", "native_plugin"};

bool contains_forbidden_token(std::string_view text) {
  std::string lowered(text);
  std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return std::ranges::any_of(kForbiddenTokens, [&](std::string_view token) {
    return lowered.contains(token);
  });
}

void scan_forbidden(ValidationResult &result, const std::string &rule_id,
                    std::string_view field_name, const std::string &value) {
  if (contains_forbidden_token(value)) {
    add(result, rule_id, ValidationErrorCode::forbidden_capability,
        "field '" + std::string(field_name) +
            "' names a forbidden capability: " + value);
  }
}

bool binding_exists(const std::vector<Binding> &bindings,
                    const std::string &name) {
  return std::ranges::any_of(
      bindings, [&](const Binding &binding) { return binding.name == name; });
}

void validate_identity(ValidationResult &result, const ExtractionRule &rule,
                       const IdentityRecipe &identity) {
  switch (identity.kind) {
  case IdentityKind::usr:
  case IdentityKind::source_anchor:
  case IdentityKind::type_key:
    if (!identity.components.empty()) {
      add(result, rule.id, ValidationErrorCode::malformed_plan,
          "identity kind '" + to_string(identity.kind) +
              "' must not declare components");
    }
    return;
  case IdentityKind::owner_position: {
    if (identity.components.size() != 2) {
      add(result, rule.id, ValidationErrorCode::unstable_identity,
          "owner_position identity requires exactly [owner_binding, "
          "position_index]");
      return;
    }
    if (!binding_exists(rule.bindings, identity.components[0])) {
      add(result, rule.id, ValidationErrorCode::invalid_binding,
          "owner_position identity references undeclared binding: " +
              identity.components[0]);
    }
    const std::string &position = identity.components[1];
    int parsed = 0;
    auto [ptr, ec] = std::from_chars(position.data(),
                                     position.data() + position.size(), parsed);
    if (ec != std::errc{} || ptr != position.data() + position.size() ||
        parsed < 0) {
      add(result, rule.id, ValidationErrorCode::unstable_identity,
          "owner_position identity position index is not a non-negative "
          "integer: " +
              position);
    }
    return;
  }
  case IdentityKind::composed:
    if (identity.components.empty()) {
      add(result, rule.id, ValidationErrorCode::unstable_identity,
          "composed identity must reference at least one component binding");
      return;
    }
    for (const auto &component : identity.components) {
      if (!binding_exists(rule.bindings, component)) {
        add(result, rule.id, ValidationErrorCode::invalid_binding,
            "composed identity references undeclared binding: " + component);
      }
    }
    return;
  }
  add(result, rule.id, ValidationErrorCode::malformed_plan,
      "identity recipe has an unrecognized kind");
}

void validate_emit(ValidationResult &result, const ExtractionRule &rule,
                   const EmitOperation &emit) {
  if (emit.node) {
    scan_forbidden(result, rule.id, "node.namespace",
                   emit.node->namespace_name);
    scan_forbidden(result, rule.id, "node.node_kind", emit.node->node_kind);
    if (!binding_exists(rule.bindings, emit.node->binding)) {
      add(result, rule.id, ValidationErrorCode::invalid_binding,
          "emit node references undeclared binding: " + emit.node->binding);
    }
    validate_identity(result, rule, emit.node->identity);
  }
  if (emit.relation) {
    scan_forbidden(result, rule.id, "relation.namespace",
                   emit.relation->namespace_name);
    scan_forbidden(result, rule.id, "relation.relation_kind",
                   emit.relation->relation_kind);
    if (!binding_exists(rule.bindings, emit.relation->from_binding)) {
      add(result, rule.id, ValidationErrorCode::invalid_binding,
          "emit relation references undeclared from_binding: " +
              emit.relation->from_binding);
    }
    if (!binding_exists(rule.bindings, emit.relation->to_binding)) {
      add(result, rule.id, ValidationErrorCode::invalid_binding,
          "emit relation references undeclared to_binding: " +
              emit.relation->to_binding);
    }
  }
  if (emit.attribute) {
    scan_forbidden(result, rule.id, "attribute.namespace",
                   emit.attribute->namespace_name);
    scan_forbidden(result, rule.id, "attribute.attribute_name",
                   emit.attribute->attribute_name);
    if (!binding_exists(rule.bindings, emit.attribute->binding)) {
      add(result, rule.id, ValidationErrorCode::invalid_binding,
          "emit attribute references undeclared binding: " +
              emit.attribute->binding);
    }
  }
  if (emit.unknown) {
    scan_forbidden(result, rule.id, "unknown.namespace",
                   emit.unknown->namespace_name);
    if (!binding_exists(rule.bindings, emit.unknown->binding)) {
      add(result, rule.id, ValidationErrorCode::invalid_binding,
          "emit unknown references undeclared binding: " +
              emit.unknown->binding);
    }
  }
}

void validate_budget(ValidationResult &result, const ExtractionRule &rule,
                     const ValidationLimits &limits) {
  if (!rule.budget.declared) {
    add(result, rule.id, ValidationErrorCode::excessive_budget,
        "rule does not declare a resource budget");
    return;
  }
  auto check = [&](std::int64_t value, std::int64_t ceiling, const char *name) {
    if (value <= 0) {
      add(result, rule.id, ValidationErrorCode::excessive_budget,
          std::string(name) + " must be a positive declared budget");
    } else if (value > ceiling) {
      add(result, rule.id, ValidationErrorCode::excessive_budget,
          std::string(name) + " exceeds the validation ceiling");
    }
  };
  check(rule.budget.max_matches, limits.max_matches_ceiling, "max_matches");
  check(rule.budget.max_emitted_facts, limits.max_emitted_facts_ceiling,
        "max_emitted_facts");
  const std::int64_t node_ceiling =
      rule.scope == PlanScope::workspace
          ? limits.workspace_max_visited_nodes_ceiling
          : limits.max_visited_nodes_ceiling;
  check(rule.budget.max_visited_nodes, node_ceiling, "max_visited_nodes");
  if (rule.scope == PlanScope::workspace &&
      rule.budget.max_visited_nodes > node_ceiling) {
    add(result, rule.id, ValidationErrorCode::unbounded_scope,
        "workspace-scoped rule exceeds the workspace visited-node ceiling");
  }
}

} // namespace

ValidationResult validate_structure(const ExtractionPlan &plan,
                                    const ValidationLimits &limits) {
  ValidationResult result;
  if (plan.schema_version != kExtractionPlanSchemaVersion) {
    add(result, "", ValidationErrorCode::unsupported_schema_version,
        "unsupported ExtractionPlan schema_version: " +
            std::to_string(plan.schema_version));
  }
  if (plan.plan_id.empty()) {
    add(result, "", ValidationErrorCode::malformed_plan,
        "plan_id must not be empty");
  }
  std::set<std::string> seen_rule_ids;
  for (const auto &rule : plan.rules) {
    if (rule.id.empty()) {
      add(result, rule.id, ValidationErrorCode::malformed_plan,
          "rule id must not be empty");
    } else if (!seen_rule_ids.insert(rule.id).second) {
      add(result, rule.id, ValidationErrorCode::malformed_plan,
          "duplicate rule id: " + rule.id);
    }
    if (rule.matcher_expression.empty()) {
      add(result, rule.id, ValidationErrorCode::malformed_plan,
          "matcher_expression must not be empty");
    }
    scan_forbidden(result, rule.id, "matcher_expression",
                   rule.matcher_expression);
    if (rule.producer_package.empty()) {
      add(result, rule.id, ValidationErrorCode::malformed_plan,
          "producer_package must not be empty");
    }
    if (rule.producer_version == 0) {
      add(result, rule.id, ValidationErrorCode::malformed_plan,
          "producer_version must be positive");
    }
    std::set<std::string> seen_bindings;
    for (const auto &binding : rule.bindings) {
      if (binding.name.empty()) {
        add(result, rule.id, ValidationErrorCode::malformed_plan,
            "binding name must not be empty");
      } else if (!seen_bindings.insert(binding.name).second) {
        add(result, rule.id, ValidationErrorCode::malformed_plan,
            "duplicate binding name: " + binding.name);
      }
    }
    if (rule.emits.empty()) {
      add(result, rule.id, ValidationErrorCode::malformed_plan,
          "rule must declare at least one emit operation");
    }
    for (const auto &emit : rule.emits) {
      validate_emit(result, rule, emit);
    }
    validate_budget(result, rule, limits);
  }
  return result;
}

} // namespace cidx::extract
