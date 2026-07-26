// Clang-free half of validator.hpp: plan structure and safety-envelope
// checks that never need to construct or run a Clang matcher.
#include "extract/validator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
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

std::optional<EndpointDomain> domain_of(const std::vector<Binding> &bindings,
                                        const std::string &name) {
  for (const auto &binding : bindings) {
    if (binding.name == name) {
      return binding.domain;
    }
  }
  return std::nullopt;
}

// Every identity primitive and relation endpoint ultimately runs through
// engine.cpp's default_identity()/compute_identity(), which only resolves a
// declaration (USR/anchor) or an expression (anchor); a `type`-domain binding
// has no location and a `custom_node`-domain binding is not resolved by this
// engine version at all. Checking that here means an incompatible domain
// fails validation instead of silently resolving to "unresolved" at runtime.
bool domain_supports_default_identity(EndpointDomain domain) {
  return domain == EndpointDomain::declaration ||
         domain == EndpointDomain::expression;
}

// usr/source_anchor/type_key identity kinds are declared FOR one specific
// binding (the emit's own `binding` field); check that binding's declared
// domain can actually produce the requested primitive.
void validate_identity_domain(ValidationResult &result,
                              const ExtractionRule &rule,
                              const std::string &owning_binding,
                              IdentityKind kind) {
  const auto domain = domain_of(rule.bindings, owning_binding);
  if (!domain) {
    return; // already reported as invalid_binding elsewhere
  }
  bool compatible = true;
  switch (kind) {
  case IdentityKind::usr:
    compatible = *domain == EndpointDomain::declaration;
    break;
  case IdentityKind::source_anchor:
    compatible = domain_supports_default_identity(*domain);
    break;
  case IdentityKind::type_key:
    compatible = *domain == EndpointDomain::declaration ||
                 *domain == EndpointDomain::expression ||
                 *domain == EndpointDomain::type;
    break;
  case IdentityKind::owner_position:
  case IdentityKind::composed:
    return; // checked per-component below, not against owning_binding
  }
  if (!compatible) {
    add(result, rule.id, ValidationErrorCode::endpoint_type_mismatch,
        "identity kind '" + to_string(kind) + "' is not compatible with " +
            "binding '" + owning_binding + "' declared as domain " +
            to_string(*domain));
  }
}

void validate_identity(ValidationResult &result, const ExtractionRule &rule,
                       const std::string &owning_binding,
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
    validate_identity_domain(result, rule, owning_binding, identity.kind);
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
    } else if (const auto domain =
                   domain_of(rule.bindings, identity.components[0]);
               domain && !domain_supports_default_identity(*domain)) {
      add(result, rule.id, ValidationErrorCode::endpoint_type_mismatch,
          "owner_position identity owner binding '" + identity.components[0] +
              "' declared as domain " + to_string(*domain) +
              " cannot produce a stable identity");
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
        continue;
      }
      if (const auto domain = domain_of(rule.bindings, component);
          domain && !domain_supports_default_identity(*domain)) {
        add(result, rule.id, ValidationErrorCode::endpoint_type_mismatch,
            "composed identity component '" + component +
                "' declared as domain " + to_string(*domain) +
                " cannot produce a stable identity");
      }
    }
    return;
  }
  add(result, rule.id, ValidationErrorCode::malformed_plan,
      "identity recipe has an unrecognized kind");
}

void validate_emit(ValidationResult &result, const ExtractionRule &rule,
                   const EmitOperation &emit) {
  // EmitOperation is a tagged union over 4 independent std::optional
  // payloads; plan_json.hpp's emit_from_json() already rejects a
  // multi-payload emit when PARSING json, but a plan assembled directly in
  // C++ (via builder code, bypassing the JSON codec entirely) can set more
  // than one of these fields with nothing to stop it -- validate(plan) would
  // pass, execution would silently pick whichever branch its if/else-if
  // chain checks first, and canonical_json(plan) would serialize BOTH set
  // fields, producing JSON that parse_plan_json() then rejects on its own
  // round trip. Enforce the same "exactly one" invariant here so it holds
  // for every ExtractionPlan regardless of how it was constructed.
  const int payload_count = (emit.node ? 1 : 0) + (emit.relation ? 1 : 0) +
                            (emit.attribute ? 1 : 0) + (emit.unknown ? 1 : 0);
  if (payload_count != 1) {
    add(result, rule.id, ValidationErrorCode::malformed_plan,
        "emit operation must set exactly one of node/relation/attribute/"
        "unknown (found " +
            std::to_string(payload_count) + ")");
  }
  if (emit.node) {
    scan_forbidden(result, rule.id, "node.namespace",
                   emit.node->namespace_name);
    scan_forbidden(result, rule.id, "node.node_kind", emit.node->node_kind);
    if (!binding_exists(rule.bindings, emit.node->binding)) {
      add(result, rule.id, ValidationErrorCode::invalid_binding,
          "emit node references undeclared binding: " + emit.node->binding);
    }
    validate_identity(result, rule, emit.node->binding, emit.node->identity);
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
    } else if (const auto domain =
                   domain_of(rule.bindings, emit.relation->from_binding);
               domain && !domain_supports_default_identity(*domain)) {
      add(result, rule.id, ValidationErrorCode::endpoint_type_mismatch,
          "emit relation from_binding '" + emit.relation->from_binding +
              "' declared as domain " + to_string(*domain) +
              " cannot produce a stable identity");
    }
    if (!binding_exists(rule.bindings, emit.relation->to_binding)) {
      add(result, rule.id, ValidationErrorCode::invalid_binding,
          "emit relation references undeclared to_binding: " +
              emit.relation->to_binding);
    } else if (const auto domain =
                   domain_of(rule.bindings, emit.relation->to_binding);
               domain && !domain_supports_default_identity(*domain)) {
      add(result, rule.id, ValidationErrorCode::endpoint_type_mismatch,
          "emit relation to_binding '" + emit.relation->to_binding +
              "' declared as domain " + to_string(*domain) +
              " cannot produce a stable identity");
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
  // workspace scope is unconditionally rejected: this engine executes a
  // plan against exactly one ASTContext/TU, and has no cross-TU/repository
  // execution model. Rather than silently treat workspace the same as
  // translation_unit (which would misrepresent the rule's actual reach),
  // refuse it at validation -- HSE-64 accepts either implementing real
  // workspace routing or rejecting unsupported scope/budget combinations.
  if (rule.scope == PlanScope::workspace) {
    add(result, rule.id, ValidationErrorCode::unbounded_scope,
        "workspace scope is not supported by this execution engine version "
        "(one plan runs against exactly one ASTContext/TU); use "
        "translation_unit or main_file");
  }
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
