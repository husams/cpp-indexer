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

#include <random>
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
  rule.bindings = {Binding{"call", EndpointDomain::expression},
                   Binding{"callee", EndpointDomain::declaration}};
  EmitOperation relation;
  relation.relation = EmitRelation{"audit", "logs_to", "call", "callee", true};
  rule.emits = {relation};
  rule.scope = PlanScope::main_file;
  rule.traversal = TraversalMode::as_is;
  rule.completeness = DeclaredCompleteness::complete;
  rule.budget = RuleBudget{1000, 1000, 100000, true};
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
  std::mt19937 rng(20260725);
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
      // expected failure mode
    }
  }
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
  plan.rules[0].emits[0].relation->to_binding = "not_declared";
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
  node_op.node = EmitNode{"audit", "call_site", "call",
                          IdentityRecipe{IdentityKind::composed, {}}};
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
  plan.rules[0].emits[0].relation->namespace_name = "run_shell_command";
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
    plan.rules[0].bindings = {Binding{"call", EndpointDomain::expression}};
    plan.rules[0].emits[0].relation->from_binding = "call";
    plan.rules[0].emits[0].relation->to_binding = "call";
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
        Binding{"never_bound", EndpointDomain::declaration});
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
    attribute_op.attribute =
        EmitAttribute{"audit", "raw_pointer", "call", "getAsOpaquePtr"};
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
    attribute_op.attribute =
        EmitAttribute{"audit", "purity", "call", "is_pure"};
    plan.rules[0].emits.push_back(attribute_op);
    ValidationResult result = validate_matchers(plan);
    bool found = false;
    for (const auto &error : result.errors) {
      found =
          found || error.code == ValidationErrorCode::endpoint_type_mismatch;
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
