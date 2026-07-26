// extraction_plan_test — HSE-64 ExtractionPlan IR, canonical JSON, plan
// identity, and validation.
//
// Default suite: pure IR/JSON/hash/matcher-catalog/structural-validation
// cases. None of these touch the Clang C++ API.
// "clang" suite: validate_matchers()/validate() -- constructs (never
// executes) Clang dynamic AST matchers via the registry (ADR-013).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "extract/matcher_catalog.hpp"
#include "extract/plan_identity.hpp"
#include "extract/plan_ir.hpp"
#include "extract/plan_json.hpp"
#include "extract/validator.hpp"

#include <optional>
#include <random>
#include <stdexcept>
#include <string>

using namespace cidx::extract;

namespace {

ExtractionRule make_rule(std::string id = "audit.log_call") {
  ExtractionRule rule;
  rule.id = std::move(id);
  rule.version = 1;
  rule.matcher_expression =
      "callExpr(callee(functionDecl(hasName(\"log_audit_event\")).bind("
      "\"callee\"))).bind(\"call\")";
  rule.bindings = {
      Binding{.name = "call", .domain = EndpointDomain::expression},
      Binding{.name = "callee", .domain = EndpointDomain::declaration}};
  EmitOperation relation;
  relation.relation = EmitRelation{.namespace_name = "audit",
                                   .relation_kind = "logs_to",
                                   .from_binding = "call",
                                   .to_binding = "callee",
                                   .with_evidence = true};
  rule.emits = {relation};
  rule.scope = PlanScope::main_file;
  rule.traversal = TraversalMode::as_is;
  rule.completeness = DeclaredCompleteness::complete;
  rule.budget = RuleBudget{.max_matches = 1000,
                           .max_emitted_facts = 1000,
                           .max_visited_nodes = 100000,
                           .declared = true};
  rule.producer_package = "banking.audit";
  rule.producer_version = 1;
  return rule;
}

ExtractionPlan make_plan() {
  ExtractionPlan plan;
  plan.plan_id = "banking.audit.plan";
  plan.plan_version = 1;
  plan.catalog_versions = {1};
  plan.rules = {make_rule()};
  return plan;
}

// Every test below mutates a relation this same fixture always constructs
// (make_rule() always sets emits[0].relation), so the guard here can never
// actually fire outside of a broken fixture -- it exists so the following
// dereference sits behind a real `if`/early-return the optional-access
// checker can follow, rather than a doctest assertion macro (REQUIRE/CHECK
// expand through machinery the checker's dataflow does not see through).
// The optional is bound to a single reference up front (rather than
// re-evaluating `rule.emits[0].relation` for both the guard and the
// dereference) so the checker can prove the checked-value-has-value and the
// dereferenced value are the same object.
EmitRelation &relation_of(ExtractionRule &rule) {
  std::optional<EmitRelation> &relation = rule.emits[0].relation;
  if (!relation.has_value()) {
    throw std::runtime_error("test fixture error: emits[0].relation not set");
  }
  return *relation;
}

} // namespace

TEST_CASE("canonical JSON round-trips through parse_plan_json") {
  ExtractionPlan plan = make_plan();
  const std::string text = canonical_json(plan);
  ExtractionPlan parsed = parse_plan_json(text);
  CHECK(canonical_json(parsed) == text);
  CHECK(plan_hash(parsed) == plan_hash(plan));
}

TEST_CASE("canonical JSON is deterministic across repeated calls") {
  ExtractionPlan plan = make_plan();
  CHECK(canonical_json(plan) == canonical_json(plan));
  CHECK(plan_hash(plan) == plan_hash(plan));
}

TEST_CASE("plan hash changes when a rule's matcher expression changes") {
  ExtractionPlan plan = make_plan();
  const std::string before = plan_hash(plan);
  plan.rules[0].matcher_expression =
      "callExpr(callee(functionDecl(hasName(\"log_other_event\")).bind("
      "\"callee\"))).bind(\"call\")";
  CHECK(plan_hash(plan) != before);
}

TEST_CASE("plan hash changes when traversal mode changes") {
  ExtractionPlan plan = make_plan();
  const std::string before = plan_hash(plan);
  plan.rules[0].traversal = TraversalMode::ignore_unless_spelled;
  CHECK(plan_hash(plan) != before);
}

TEST_CASE("plan hash changes when a budget changes") {
  ExtractionPlan plan = make_plan();
  const std::string before = plan_hash(plan);
  plan.rules[0].budget.max_matches = 2000;
  CHECK(plan_hash(plan) != before);
}

TEST_CASE("plan hash changes when catalog_versions changes") {
  ExtractionPlan plan = make_plan();
  const std::string before = plan_hash(plan);
  plan.catalog_versions.push_back(2);
  CHECK(plan_hash(plan) != before);
}

TEST_CASE("plan hash changes when producer_version changes") {
  ExtractionPlan plan = make_plan();
  const std::string before = plan_hash(plan);
  plan.rules[0].producer_version = 2;
  CHECK(plan_hash(plan) != before);
}

TEST_CASE("parse_plan_json rejects malformed JSON without crashing") {
  CHECK_THROWS_AS((void)parse_plan_json("not json at all"), PlanParseError);
  CHECK_THROWS_AS((void)parse_plan_json("{"), PlanParseError);
  CHECK_THROWS_AS((void)parse_plan_json("{}"), PlanParseError);
  CHECK_THROWS_AS((void)parse_plan_json(""), PlanParseError);
  CHECK_THROWS_AS((void)parse_plan_json("{\"schema_version\": 1.5}"),
                  PlanParseError);
}

TEST_CASE("parse_plan_json rejects unknown fields") {
  ExtractionPlan plan = make_plan();
  std::string text = canonical_json(plan);
  text.insert(text.size() - 1, ",\"bogus_field\":1");
  CHECK_THROWS_AS((void)parse_plan_json(text), PlanParseError);
}

TEST_CASE("parse_plan_json caps nesting depth") {
  std::string pathological(kExtractionPlanMaxJsonDepth + 10, '[');
  CHECK_THROWS_AS((void)parse_plan_json(pathological), PlanParseError);
}

TEST_CASE("parser fuzz: random mutations of a valid plan never crash and "
          "always fail closed") {
  const ExtractionPlan plan = make_plan();
  const std::string base = canonical_json(plan);
  // A fixed seed is intentional here: this is a reproducible regression
  // fixture, not security-relevant randomness. A random seed would make a
  // failure non-reproducible across CI runs, which is strictly worse for
  // debugging.
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(20260725);
  int rejected_mutations = 0;
  for (int i = 0; i < 500; ++i) {
    std::string mutated = base;
    const int mutation = static_cast<int>(rng() % 3);
    if (mutation == 0 && !mutated.empty()) {
      std::uniform_int_distribution<std::size_t> pos_dist(0,
                                                          mutated.size() - 1);
      mutated[pos_dist(rng)] = static_cast<char>(rng() % 256);
    } else if (mutation == 1 && mutated.size() > 1) {
      std::uniform_int_distribution<std::size_t> len_dist(0, mutated.size());
      mutated = mutated.substr(0, len_dist(rng));
    } else {
      std::uniform_int_distribution<std::size_t> pos_dist(0, mutated.size());
      std::uniform_int_distribution<int> char_dist(32, 126);
      mutated.insert(mutated.begin() + static_cast<long>(pos_dist(rng)),
                     static_cast<char>(char_dist(rng)));
    }
    // The only acceptable outcomes: parses to an equivalent plan (mutation
    // happened to be a no-op on meaning), or throws PlanParseError. Anything
    // else (a different exception type, or successfully producing a plan
    // that then can't round-trip) is a bug.
    try {
      ExtractionPlan reparsed = parse_plan_json(mutated);
      CHECK_NOTHROW((void)canonical_json(reparsed));
    } catch (const PlanParseError &) {
      ++rejected_mutations; // expected failure mode
    }
  }
  // Not a tautology: proves the loop actually exercises the reject path
  // across the 500 mutations rather than every mutation happening to
  // parse as an equivalent (or crash-free but never-thrown) plan.
  CHECK(rejected_mutations > 0);
}

TEST_CASE("matcher_catalog allow-list") {
  const auto &catalog = MatcherCatalog::default_catalog();
  CHECK(catalog.allows_matcher("functionDecl"));
  CHECK(catalog.allows_matcher("callExpr"));
  CHECK(catalog.allows_matcher("hasName"));
  CHECK_FALSE(catalog.allows_matcher("stmt"));
  CHECK_FALSE(catalog.allows_matcher("expr"));
  CHECK_FALSE(catalog.allows_matcher("decl"));
  CHECK(catalog.allows_property("spelling"));
  CHECK_FALSE(catalog.allows_property("getSourceRange"));
  // Removed (PR #66 review): a type-domain binding is a bare QualType with
  // no Decl/Expr/SourceLocation, so this property could never actually
  // resolve (engine.cpp's read_property()/default_identity() had nothing to
  // read) even though it was allow-listed and passed validation.
  CHECK_FALSE(catalog.allows_property("canonical_type_spelling"));
}

TEST_CASE("disallowed_matcher_calls flags catch-alls and unknown properties") {
  const auto &catalog = MatcherCatalog::default_catalog();
  auto disallowed = disallowed_matcher_calls("stmt().bind(\"x\")", catalog);
  REQUIRE(disallowed.size() == 1);
  CHECK(disallowed[0] == "stmt");

  CHECK(disallowed_matcher_calls("functionDecl(hasName(\"f\")).bind(\"x\")",
                                 catalog)
            .empty());

  auto method_disallowed =
      disallowed_matcher_calls("functionDecl().evilMethod(1)", catalog);
  REQUIRE(method_disallowed.size() == 1);
  CHECK(method_disallowed[0] == "evilMethod");
}

TEST_CASE("validate_structure rejects a missing declared budget") {
  ExtractionPlan plan = make_plan();
  plan.rules[0].budget.declared = false;
  ValidationResult result = validate_structure(plan);
  REQUIRE_FALSE(result.ok());
  CHECK(result.errors[0].code == ValidationErrorCode::excessive_budget);
}

TEST_CASE("validate_structure rejects excessive budgets") {
  ExtractionPlan plan = make_plan();
  plan.rules[0].budget.max_matches = 10'000'000'000;
  ValidationResult result = validate_structure(plan);
  bool found = false;
  for (const auto &error : result.errors) {
    found = found || error.code == ValidationErrorCode::excessive_budget;
  }
  CHECK(found);
}

TEST_CASE("validate_structure rejects unbounded workspace scope") {
  ExtractionPlan plan = make_plan();
  plan.rules[0].scope = PlanScope::workspace;
  plan.rules[0].budget.max_visited_nodes = 999'999'999'999;
  ValidationResult result = validate_structure(plan);
  bool found = false;
  for (const auto &error : result.errors) {
    found = found || error.code == ValidationErrorCode::unbounded_scope;
  }
  CHECK(found);
}

TEST_CASE("validate_structure rejects an emit that references an undeclared "
          "binding") {
  ExtractionPlan plan = make_plan();
  relation_of(plan.rules[0]).to_binding = "not_declared";
  ValidationResult result = validate_structure(plan);
  bool found = false;
  for (const auto &error : result.errors) {
    found = found || error.code == ValidationErrorCode::invalid_binding;
  }
  CHECK(found);
}

TEST_CASE("validate_structure rejects an unstable composed identity") {
  ExtractionPlan plan = make_plan();
  EmitOperation node_op;
  node_op.node =
      EmitNode{.namespace_name = "audit",
               .node_kind = "call_site",
               .binding = "call",
               .identity = IdentityRecipe{.kind = IdentityKind::composed,
                                          .components = {}}};
  plan.rules[0].emits.push_back(node_op);
  ValidationResult result = validate_structure(plan);
  bool found = false;
  for (const auto &error : result.errors) {
    found = found || error.code == ValidationErrorCode::unstable_identity;
  }
  CHECK(found);
}

TEST_CASE("validate_structure rejects duplicate rule ids") {
  ExtractionPlan plan = make_plan();
  plan.rules.push_back(make_rule());
  ValidationResult result = validate_structure(plan);
  bool found = false;
  for (const auto &error : result.errors) {
    found = found || error.code == ValidationErrorCode::malformed_plan;
  }
  CHECK(found);
}

TEST_CASE("validate_structure flags a forbidden-capability token even though "
          "the IR cannot execute one") {
  ExtractionPlan plan = make_plan();
  relation_of(plan.rules[0]).namespace_name = "run_shell_command";
  ValidationResult result = validate_structure(plan);
  bool found = false;
  for (const auto &error : result.errors) {
    found = found || error.code == ValidationErrorCode::forbidden_capability;
  }
  CHECK(found);
}

TEST_CASE("a fully valid plan passes validate_structure") {
  ExtractionPlan plan = make_plan();
  ValidationResult result = validate_structure(plan);
  CHECK(result.ok());
}

// --- PR #66 review regression coverage -------------------------------------

TEST_CASE("validate_structure rejects workspace scope: no cross-TU execution "
          "model exists to make it safe") {
  ExtractionPlan plan = make_plan();
  plan.rules[0].scope = PlanScope::workspace;
  ValidationResult result = validate_structure(plan);
  bool found = false;
  for (const auto &error : result.errors) {
    found = found || error.code == ValidationErrorCode::unbounded_scope;
  }
  CHECK(found);
}

TEST_CASE("validate_structure rejects a usr identity recipe on an "
          "expression-domain binding") {
  ExtractionPlan plan = make_plan();
  EmitOperation node_op;
  // "call" is declared EndpointDomain::expression in make_rule(); usr
  // identity can only ever be computed from a declaration.
  node_op.node = EmitNode{
      .namespace_name = "audit",
      .node_kind = "call_site",
      .binding = "call",
      .identity = IdentityRecipe{.kind = IdentityKind::usr, .components = {}}};
  plan.rules[0].emits.push_back(node_op);
  ValidationResult result = validate_structure(plan);
  bool found = false;
  for (const auto &error : result.errors) {
    found = found || error.code == ValidationErrorCode::endpoint_type_mismatch;
  }
  CHECK(found);
}

TEST_CASE("validate_structure rejects a relation endpoint bound to a "
          "type-domain binding") {
  ExtractionPlan plan = make_plan();
  plan.rules[0].bindings.push_back(
      Binding{.name = "t", .domain = EndpointDomain::type});
  relation_of(plan.rules[0]).to_binding = "t";
  ValidationResult result = validate_structure(plan);
  bool found = false;
  for (const auto &error : result.errors) {
    found = found || error.code == ValidationErrorCode::endpoint_type_mismatch;
  }
  CHECK(found);
}

TEST_CASE("artifact_identity changes when workspace or TU identity changes, "
          "independent of the plan") {
  ExtractionPlan plan = make_plan();
  const std::string base_hash = plan_hash(plan);
  const std::string a = artifact_identity(
      plan, ExecutionIdentityInput{.workspace_identity = "workspace:a",
                                   .tu_identity = "tu:1",
                                   .tu_content_fingerprint = "fp:1"});
  const std::string b = artifact_identity(
      plan, ExecutionIdentityInput{.workspace_identity = "workspace:b",
                                   .tu_identity = "tu:1",
                                   .tu_content_fingerprint = "fp:1"});
  const std::string c = artifact_identity(
      plan, ExecutionIdentityInput{.workspace_identity = "workspace:a",
                                   .tu_identity = "tu:2",
                                   .tu_content_fingerprint = "fp:1"});
  const std::string d = artifact_identity(
      plan, ExecutionIdentityInput{.workspace_identity = "workspace:a",
                                   .tu_identity = "tu:1",
                                   .tu_content_fingerprint = "fp:2"});
  CHECK(a != b);
  CHECK(a != c);
  CHECK(a != d);
  // artifact_identity is a distinct concept from plan_hash: the plan itself
  // never changed across a/b/c/d.
  CHECK(plan_hash(plan) == base_hash);
}

// --- PR #66 review round 3 regression coverage -----------------------------

TEST_CASE("validate_structure rejects an EmitOperation with two payloads set "
          "(round-3 repro: builder-constructed IR bypasses plan_json's "
          "exactly-one check, so validate(plan) passed while the plan could "
          "not even round-trip through its own canonical_json)") {
  ExtractionPlan plan = make_plan();
  EmitOperation &emit = plan.rules[0].emits[0];
  REQUIRE(emit.relation.has_value());
  // Directly set a SECOND payload alongside the existing relation -- no
  // JSON parsing involved, so plan_json.hpp's own "exactly one" check never
  // runs.
  emit.unknown = EmitUnknown{.namespace_name = "audit",
                             .reason_code = "also_unknown",
                             .binding = "call"};
  ValidationResult result = validate_structure(plan);
  bool found = false;
  for (const auto &error : result.errors) {
    found = found || error.code == ValidationErrorCode::malformed_plan;
  }
  CHECK(found);
}

TEST_CASE("validate_structure rejects an EmitOperation with no payload set") {
  ExtractionPlan plan = make_plan();
  plan.rules[0].emits[0] = EmitOperation{}; // all four payloads empty
  ValidationResult result = validate_structure(plan);
  bool found = false;
  for (const auto &error : result.errors) {
    found = found || error.code == ValidationErrorCode::malformed_plan;
  }
  CHECK(found);
}

TEST_SUITE("clang") {

  TEST_CASE("validate_matchers accepts a well-formed allow-listed rule") {
    ExtractionPlan plan = make_plan();
    ValidationResult result = validate_matchers(plan);
    CHECK(result.ok());
  }

  TEST_CASE("validate_matchers rejects a syntactically invalid matcher") {
    ExtractionPlan plan = make_plan();
    plan.rules[0].matcher_expression = "functionDecl(((";
    ValidationResult result = validate_matchers(plan);
    REQUIRE_FALSE(result.ok());
    CHECK(result.errors[0].code == ValidationErrorCode::unknown_matcher);
  }

  TEST_CASE("validate_matchers rejects a matcher not on the CIDX allow-list") {
    ExtractionPlan plan = make_plan();
    plan.rules[0].matcher_expression = "stmt().bind(\"call\")";
    plan.rules[0].bindings = {
        Binding{.name = "call", .domain = EndpointDomain::expression}};
    relation_of(plan.rules[0]).from_binding = "call";
    relation_of(plan.rules[0]).to_binding = "call";
    ValidationResult result = validate_matchers(plan);
    bool found = false;
    for (const auto &error : result.errors) {
      found = found || error.code == ValidationErrorCode::unknown_matcher;
    }
    CHECK(found);
  }

  TEST_CASE("validate_matchers rejects a declared binding never produced by "
            "the matcher expression") {
    ExtractionPlan plan = make_plan();
    plan.rules[0].bindings.push_back(
        Binding{.name = "never_bound", .domain = EndpointDomain::declaration});
    ValidationResult result = validate_matchers(plan);
    bool found = false;
    for (const auto &error : result.errors) {
      found = found || error.code == ValidationErrorCode::invalid_binding;
    }
    CHECK(found);
  }

  TEST_CASE("validate_matchers rejects an unknown attribute property") {
    ExtractionPlan plan = make_plan();
    EmitOperation attribute_op;
    attribute_op.attribute = EmitAttribute{.namespace_name = "audit",
                                           .attribute_name = "raw_pointer",
                                           .binding = "call",
                                           .ast_property = "getAsOpaquePtr"};
    plan.rules[0].emits.push_back(attribute_op);
    ValidationResult result = validate_matchers(plan);
    bool found = false;
    for (const auto &error : result.errors) {
      found = found || error.code == ValidationErrorCode::unknown_property;
    }
    CHECK(found);
  }

  TEST_CASE("validate_matchers rejects an endpoint-type mismatch") {
    ExtractionPlan plan = make_plan();
    EmitOperation attribute_op;
    // "is_pure" is declaration-only; "call" is bound as an expression.
    attribute_op.attribute = EmitAttribute{.namespace_name = "audit",
                                           .attribute_name = "purity",
                                           .binding = "call",
                                           .ast_property = "is_pure"};
    plan.rules[0].emits.push_back(attribute_op);
    ValidationResult result = validate_matchers(plan);
    bool found = false;
    for (const auto &error : result.errors) {
      found =
          found || error.code == ValidationErrorCode::endpoint_type_mismatch;
    }
    CHECK(found);
  }

  TEST_CASE(
      "validate_matchers rejects a binding declared as the wrong domain for "
      "the node its own matcher actually constructs (PR #66 review repro)") {
    // callExpr() constructs an Expr-kind node; declaring "x" as a
    // declaration binding must fail here, before Clang ever executes the
    // plan -- not resolve getNodeAs<Decl> to null at runtime.
    ExtractionPlan plan = make_plan();
    plan.rules[0].matcher_expression = "callExpr().bind(\"x\")";
    plan.rules[0].bindings = {
        Binding{.name = "x", .domain = EndpointDomain::declaration}};
    relation_of(plan.rules[0]).from_binding = "x";
    relation_of(plan.rules[0]).to_binding = "x";
    ValidationResult result = validate_matchers(plan);
    bool found = false;
    for (const auto &error : result.errors) {
      found =
          found || error.code == ValidationErrorCode::endpoint_type_mismatch;
    }
    CHECK(found);
  }

  // --- PR #66 review round 3 regression coverage ---------------------------

  TEST_CASE("validate_matchers rejects hasDescendant/hasAncestor nested inside "
            "one another (round-3 repro: nested combinators cause superlinear "
            "matchAST cost that the linear visited*(1+count) estimate cannot "
            "bound), but still accepts the same combinators used as SIBLINGS") {
    ExtractionPlan plan = make_plan();
    plan.rules[0].matcher_expression =
        "cxxRecordDecl(hasDescendant(cxxRecordDecl(hasDescendant(cxxMethodDecl("
        "hasName(\"never\"))))))"
        ".bind(\"call\")";
    plan.rules[0].bindings = {
        Binding{.name = "call", .domain = EndpointDomain::declaration}};
    relation_of(plan.rules[0]).from_binding = "call";
    relation_of(plan.rules[0]).to_binding = "call";
    ValidationResult nested_result = validate_matchers(plan);
    bool found_nested = false;
    for (const auto &error : nested_result.errors) {
      found_nested =
          found_nested || error.code == ValidationErrorCode::excessive_budget;
    }
    CHECK(found_nested);

    // Same two matchers, but as SIBLINGS joined by allOf() -- not nested
    // inside one another -- must NOT be rejected by this check.
    ExtractionPlan sibling_plan = make_plan();
    sibling_plan.rules[0].matcher_expression =
        "cxxRecordDecl(allOf(hasDescendant(cxxMethodDecl(hasName(\"a\"))), "
        "hasDescendant(cxxMethodDecl(hasName(\"b\"))))).bind(\"call\")";
    sibling_plan.rules[0].bindings = {
        Binding{.name = "call", .domain = EndpointDomain::declaration}};
    relation_of(sibling_plan.rules[0]).from_binding = "call";
    relation_of(sibling_plan.rules[0]).to_binding = "call";
    ValidationResult sibling_result = validate_matchers(sibling_plan);
    bool found_sibling_rejected = false;
    for (const auto &error : sibling_result.errors) {
      found_sibling_rejected =
          found_sibling_rejected ||
          error.code == ValidationErrorCode::excessive_budget;
    }
    CHECK_FALSE(found_sibling_rejected);
  }

  TEST_CASE(
      "validate_matchers rejects a main_file-scoped rule whose outermost "
      "matcher is not bound (round-3 repro: scope filtering previously "
      "anchored on the first-declared binding in rule.bindings order, which "
      "made scope filtering depend on that arbitrary order)") {
    ExtractionPlan plan = make_plan();
    // "call" (the outer callExpr()) is bound in make_rule(); rebuild the
    // matcher expression so ONLY the inner functionDecl is bound and the
    // outermost callExpr() carries no .bind(...) at all.
    plan.rules[0].matcher_expression =
        "callExpr(callee(functionDecl(hasName(\"log_audit_event\")).bind("
        "\"callee\")))";
    plan.rules[0].bindings = {
        Binding{.name = "callee", .domain = EndpointDomain::declaration}};
    relation_of(plan.rules[0]).from_binding = "callee";
    relation_of(plan.rules[0]).to_binding = "callee";
    plan.rules[0].scope = PlanScope::main_file;
    ValidationResult result = validate_matchers(plan);
    bool found = false;
    for (const auto &error : result.errors) {
      found = found || error.code == ValidationErrorCode::invalid_binding;
    }
    CHECK(found);
  }

  TEST_CASE("validate() merges structural and matcher validation") {
    ExtractionPlan plan = make_plan();
    plan.rules[0].budget.declared = false;       // structural failure
    plan.rules[0].matcher_expression = "stmt()"; // matcher failure
    ValidationResult result = validate(plan);
    bool has_budget_error = false;
    bool has_matcher_error = false;
    for (const auto &error : result.errors) {
      has_budget_error = has_budget_error ||
                         error.code == ValidationErrorCode::excessive_budget;
      has_matcher_error = has_matcher_error ||
                          error.code == ValidationErrorCode::unknown_matcher;
    }
    CHECK(has_budget_error);
    CHECK(has_matcher_error);
  }

} // TEST_SUITE("clang")
