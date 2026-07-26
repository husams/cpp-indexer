// Clang-dependent half of validator.hpp: parses each rule's matcher
// expression through Clang's own dynamic AST matcher parser (ADR-013) --
// this CONSTRUCTS a matcher, it never executes one against a translation
// unit's AST -- and checks it against the CIDX allow-list and property/domain
// catalog.
//
// Binding domains are derived from the ACTUAL matcher tree, not inferred
// from text: BindingKindSema wraps the parser's default RegistrySema and
// records, for every `.bind(id)` the parser processes, the ASTNodeKind the
// matcher it was applied to actually constructs. That is compared against
// each rule's declared Binding::domain so a mismatch (e.g. `callExpr().bind(
// "x")` declared as a `declaration` binding) fails here, before Clang ever
// executes the plan, instead of resolving to a null pointer at runtime.
#include "extract/validator.hpp"

#include "extract/matcher_root_binding.hpp"

#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/Dynamic/Diagnostics.h"
#include "clang/ASTMatchers/Dynamic/Parser.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <map>

namespace cidx::extract {

namespace {

void add(ValidationResult &result, const std::string &rule_id,
         ValidationErrorCode code, std::string message) {
  result.errors.push_back(ValidationError{
      .rule_id = rule_id, .code = code, .message = std::move(message)});
}

const Binding *find_binding(const std::vector<Binding> &bindings,
                            const std::string &name) {
  auto it = std::ranges::find_if(
      bindings, [&](const Binding &binding) { return binding.name == name; });
  return it == bindings.end() ? nullptr : &*it;
}

// Maps the ASTNodeKind a matcher actually constructs to the EndpointDomain
// CIDX rules declare bindings against. Node kinds with no corresponding
// domain (NestedNameSpecifier, TemplateArgument, ...) return nullopt: no
// declared domain can ever be correct for them, so any binding of that kind
// is an endpoint_type_mismatch regardless of which domain was declared.
std::optional<EndpointDomain> domain_for_kind(clang::ASTNodeKind kind) {
  if (clang::ASTNodeKind::getFromNodeKind<clang::Decl>().isBaseOf(kind)) {
    return EndpointDomain::declaration;
  }
  if (clang::ASTNodeKind::getFromNodeKind<clang::Expr>().isBaseOf(kind)) {
    return EndpointDomain::expression;
  }
  if (clang::ASTNodeKind::getFromNodeKind<clang::QualType>().isSame(kind)) {
    return EndpointDomain::type;
  }
  return std::nullopt;
}

// Intercepts every matcher construction during parsing to record which
// ASTNodeKind each `.bind(id)` was applied to, then delegates to the
// registry's default construction behavior unchanged.
class BindingKindSema final
    : public clang::ast_matchers::dynamic::Parser::RegistrySema {
public:
  clang::ast_matchers::dynamic::VariantMatcher actOnMatcherExpression(
      clang::ast_matchers::dynamic::MatcherCtor ctor,
      clang::ast_matchers::dynamic::SourceRange name_range,
      llvm::StringRef bind_id,
      llvm::ArrayRef<clang::ast_matchers::dynamic::ParserValue> args,
      clang::ast_matchers::dynamic::Diagnostics *error) override {
    if (!bind_id.empty()) {
      bound_kinds_.emplace(bind_id.str(), nodeMatcherType(ctor));
    }
    return RegistrySema::actOnMatcherExpression(ctor, name_range, bind_id, args,
                                                error);
  }

  [[nodiscard]] const std::map<std::string, clang::ASTNodeKind> &
  bound_kinds() const {
    return bound_kinds_;
  }

private:
  std::map<std::string, clang::ASTNodeKind> bound_kinds_;
};

void validate_rule_matcher(ValidationResult &result, const ExtractionRule &rule,
                           const MatcherCatalog &catalog) {
  if (rule.matcher_expression.empty()) {
    // Already reported by validate_structure(); nothing further to check.
    return;
  }
  llvm::StringRef code(rule.matcher_expression);
  clang::ast_matchers::dynamic::Diagnostics diagnostics;
  BindingKindSema sema;
  auto matcher = clang::ast_matchers::dynamic::Parser::parseMatcherExpression(
      code, &sema, &diagnostics);
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
  // hasDescendant/hasAncestor nested inside one another (as opposed to
  // several SIBLING occurrences joined by allOf()/anyOf()) make Clang's
  // matchAST cost multiplicative in subtree size, not additive -- there is
  // no safe linear budget adjustment for arbitrary nesting depth (unlike
  // sibling occurrences, which engine.cpp's estimated-work check already
  // bounds). Reject outright rather than under-charge the runtime budget.
  if (has_nested_matcher_occurrences(rule.matcher_expression,
                                     traversal_work_combinators())) {
    add(result, rule.id, ValidationErrorCode::excessive_budget,
        "matcher expression nests hasDescendant/hasAncestor combinators "
        "inside one another, which can cause superlinear matcher-evaluation "
        "cost that no declared budget can safely bound; refactor to sibling "
        "allOf(...) combinators instead of nesting");
  }
  for (const auto &binding : rule.bindings) {
    const auto bound = sema.bound_kinds().find(binding.name);
    if (bound == sema.bound_kinds().end()) {
      add(result, rule.id, ValidationErrorCode::invalid_binding,
          "declared binding is never produced by .bind(...) in the matcher "
          "expression: " +
              binding.name);
      continue;
    }
    const auto observed_domain = domain_for_kind(bound->second);
    if (!observed_domain || *observed_domain != binding.domain) {
      add(result, rule.id, ValidationErrorCode::endpoint_type_mismatch,
          "binding '" + binding.name + "' is declared as domain " +
              to_string(binding.domain) +
              " but the matcher expression binds it to a " +
              bound->second.asStringRef().str() +
              (observed_domain
                   ? " (domain " + to_string(*observed_domain) + ")"
                   : " node, which has no corresponding CIDX endpoint domain"));
    }
  }
  // main_file scope filtering (engine.cpp) anchors on the OUTERMOST/root
  // matched node's location, not on binding declaration order (a rule's
  // bindings are an unordered set of names, so filtering that depended on
  // their declared order would make scope filtering non-deterministic
  // depending only on how a rule happened to list its bindings). That
  // requires the root matcher to actually be bound; reject main_file-scoped
  // rules whose root has no bind up front, instead of silently falling back
  // to an arbitrary bound node at execution time.
  if (rule.scope == PlanScope::main_file &&
      root_binding_of(rule.matcher_expression).empty()) {
    add(result, rule.id, ValidationErrorCode::invalid_binding,
        "main_file scope requires the matcher's outermost/root node to be "
        "bound (add .bind(...) to the top-level matcher) so scope filtering "
        "has a well-defined anchor");
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
