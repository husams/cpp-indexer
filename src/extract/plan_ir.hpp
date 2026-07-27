// ExtractionPlan IR (HSE-64): the stable, versioned, Clang-free compatibility
// surface for the declarative AST extraction DSL. Plain data only -- no clang
// types leak out of the extraction layer (mirrors ast/symbol_record.hpp,
// diff/syntax_ir.hpp). A rule's matcher vocabulary is Clang's own dynamic AST
// matcher expression grammar (ADR-013); this header carries that expression as
// an opaque string plus the typed envelope (bindings, emits, scope, traversal,
// completeness, budgets, identity) that makes a plan safe to validate and run
// without ever giving a rule the ability to execute SQL, shell, Python, a
// shared library, or a user C++ callback.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cidx::extract {

// Bumped whenever a field is added/removed/reinterpreted in a way that
// changes canonical JSON or plan-hash identity. See docs/extraction-plan.md.
inline constexpr std::uint32_t kExtractionPlanSchemaVersion = 1;

// Explicit traversal semantics (docs/extraction-plan.md "Traversal modes").
// as_is mirrors the written AST including compiler-generated nodes;
// ignore_unless_spelled skips nodes with no direct source spelling (implicit
// constructors/conversions, some template-instantiation-only nodes). The two
// modes are REQUIRED to disagree on implicit/template constructs so a rule
// author can pick the one their fact family needs.
enum class TraversalMode : std::uint8_t { as_is, ignore_unless_spelled };

// How far a rule's matcher may be evaluated.
enum class PlanScope : std::uint8_t { main_file, translation_unit, workspace };

// Declared applicability of a rule's output, distinct from a per-run result
// status. `unknown_capable` means the rule is designed to emit explicit
// "unknown" findings for constructs it recognizes but cannot classify; it is
// not a promise that every construct is covered.
enum class DeclaredCompleteness : std::uint8_t {
  complete,
  partial,
  unknown_capable,
};

// The declared kind of AST entity a binding refers to. Used to reject
// endpoint-type mismatches (e.g. an emit operation that only makes sense for
// a declaration binding applied to an expression binding) before Clang ever
// executes the plan.
enum class EndpointDomain : std::uint8_t {
  declaration,
  expression,
  type,
  custom_node,
};

// Safe identity primitives (HSE-64 "safe identity primitives"). There is
// deliberately no "ast_pointer"/"address" variant: process-local AST handles
// can never become a plan's or a fact's identity by construction.
enum class IdentityKind : std::uint8_t {
  usr,            // canonical declaration USR
  source_anchor,  // file + expansion line/col
  owner_position, // an owning binding's identity + a stable position index
  type_key,       // canonical type spelling/key
  composed,       // deterministic composition of named component identities
};

struct IdentityRecipe {
  IdentityKind kind = IdentityKind::usr;
  // owner_position: component[0] = owner binding name, component[1] = the
  // literal position index (decimal string). composed: ordered list of
  // binding names whose own identity is concatenated/hashed together. Empty
  // for usr/source_anchor/type_key (those derive from `binding` alone).
  std::vector<std::string> components;

  friend bool operator==(const IdentityRecipe &,
                         const IdentityRecipe &) = default;
};

// A named capture produced by `.bind("name")` inside the rule's matcher
// expression (ADR-013). `domain` is the endpoint type the rule author
// declares for this capture; validation checks every use of the binding
// against it.
struct Binding {
  std::string name;
  EndpointDomain domain = EndpointDomain::declaration;

  friend bool operator==(const Binding &, const Binding &) = default;
};

// Mint one namespaced custom node from a bound capture.
struct EmitNode {
  std::string namespace_name; // package-qualified, e.g. "banking.appstate"
  std::string node_kind;      // local kind name within namespace_name
  std::string binding;        // source binding this node is minted from
  IdentityRecipe identity;

  friend bool operator==(const EmitNode &, const EmitNode &) = default;
};

// Emit one typed, namespaced relation between two bindings (either may name a
// core symbol binding or another EmitNode's binding).
struct EmitRelation {
  std::string namespace_name;
  std::string relation_kind;
  std::string from_binding;
  std::string to_binding;
  bool with_evidence = true; // attach source spelling/expansion evidence

  friend bool operator==(const EmitRelation &, const EmitRelation &) = default;
};

// Emit one namespaced attribute sourced from an allow-listed typed AST
// property (matcher_catalog.hpp) of a bound capture.
struct EmitAttribute {
  std::string namespace_name;
  std::string attribute_name;
  std::string binding;
  std::string ast_property;

  friend bool operator==(const EmitAttribute &,
                         const EmitAttribute &) = default;
};

// Record an explicit "recognized construct, but this rule cannot classify
// it" finding rather than silently dropping the match.
struct EmitUnknown {
  std::string namespace_name;
  std::string reason_code;
  std::string binding;

  friend bool operator==(const EmitUnknown &, const EmitUnknown &) = default;
};

// Exactly one payload is set; a plan.hpp-level helper enforces "exactly one"
// during parsing rather than at the language/type level, so the JSON shape
// stays a plain tagged object (docs/extraction-plan.md).
struct EmitOperation {
  std::optional<EmitNode> node;
  std::optional<EmitRelation> relation;
  std::optional<EmitAttribute> attribute;
  std::optional<EmitUnknown> unknown;
};

// Every rule declares explicit cardinality/resource budgets. `declared` must
// be true for a plan to pass validation (HSE-64: "require every rule to
// declare ... cardinality and resource budgets").
struct RuleBudget {
  std::int64_t max_matches = 0;
  std::int64_t max_emitted_facts = 0;
  std::int64_t max_visited_nodes = 0;
  bool declared = false;

  friend bool operator==(const RuleBudget &, const RuleBudget &) = default;
};

// One declarative extraction rule: a matcher expression (Clang's dynamic AST
// matcher grammar, ADR-013), its bindings, its emit operations, and the
// applicability/safety envelope every rule must declare.
struct ExtractionRule {
  std::string id;
  std::uint32_t version = 1;
  std::string matcher_expression;
  std::vector<Binding> bindings;
  std::vector<EmitOperation> emits;
  PlanScope scope = PlanScope::main_file;
  TraversalMode traversal = TraversalMode::as_is;
  DeclaredCompleteness completeness = DeclaredCompleteness::complete;
  RuleBudget budget;
  std::string producer_package; // package-qualified id, e.g. "banking.appstate"
  std::uint32_t producer_version = 1;
};

// The full plan: a versioned, ordered set of rules plus the core catalog
// versions it was authored against (HSE-59 catalogs). Two plans with
// identical canonical JSON always have identical plan hashes and vice versa;
// see plan_identity.hpp.
struct ExtractionPlan {
  std::uint32_t schema_version = kExtractionPlanSchemaVersion;
  std::string plan_id;
  std::uint32_t plan_version = 1;
  std::vector<std::uint32_t> catalog_versions;
  std::vector<ExtractionRule> rules;
};

// Header-only string projections (mirrors the model.contracts convention of
// carrying zero translation units: see ast/symbol_record.hpp,
// diff/syntax_ir.hpp). Used by canonical JSON, explain(), and diagnostics.
[[nodiscard]] inline std::string to_string(TraversalMode mode) {
  switch (mode) {
  case TraversalMode::as_is:
    return "as_is";
  case TraversalMode::ignore_unless_spelled:
    return "ignore_unless_spelled";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(PlanScope scope) {
  switch (scope) {
  case PlanScope::main_file:
    return "main_file";
  case PlanScope::translation_unit:
    return "translation_unit";
  case PlanScope::workspace:
    return "workspace";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(DeclaredCompleteness completeness) {
  switch (completeness) {
  case DeclaredCompleteness::complete:
    return "complete";
  case DeclaredCompleteness::partial:
    return "partial";
  case DeclaredCompleteness::unknown_capable:
    return "unknown_capable";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(EndpointDomain domain) {
  switch (domain) {
  case EndpointDomain::declaration:
    return "declaration";
  case EndpointDomain::expression:
    return "expression";
  case EndpointDomain::type:
    return "type";
  case EndpointDomain::custom_node:
    return "custom_node";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(IdentityKind kind) {
  switch (kind) {
  case IdentityKind::usr:
    return "usr";
  case IdentityKind::source_anchor:
    return "source_anchor";
  case IdentityKind::owner_position:
    return "owner_position";
  case IdentityKind::type_key:
    return "type_key";
  case IdentityKind::composed:
    return "composed";
  }
  return "unknown";
}

} // namespace cidx::extract
