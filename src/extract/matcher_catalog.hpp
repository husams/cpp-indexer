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

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cidx::extract {

// Combinators whose Clang implementation repeatedly re-traverses a subtree
// for every candidate node during matchAST (unlike a plain narrowing
// predicate such as hasName(), which is O(1) per node). Shared by the
// validator (which rejects these NESTED inside one another, since nesting
// makes their cost multiplicative rather than additive -- see
// has_nested_matcher_occurrences()) and the execution engine (which charges
// a proportionally smaller effective node budget per SIBLING occurrence).
[[nodiscard]] const std::set<std::string> &traversal_work_combinators();

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

// Counts bare-identifier call sites (same string-literal-aware scanner as
// disallowed_matcher_calls) whose identifier is in `names`. Used to bound the
// WORST-CASE evaluation work of a matcher expression: combinators like
// hasDescendant/hasAncestor cause Clang's MatchFinder to repeatedly
// re-traverse subtrees during matchAST, and each occurrence can (in the
// degenerate case) re-traverse up to the WHOLE TU for EACH top-level
// candidate -- quadratic, not linear, in node count -- so engine.cpp charges
// visited_nodes^2 per occurrence rather than dividing the budget evenly.
[[nodiscard]] std::int64_t
count_matcher_occurrences(const std::string &matcher_expression,
                          const std::set<std::string> &names);

// True if some call site in `names` (same string-literal-aware scanner)
// appears LEXICALLY NESTED inside another call site in `names` -- i.e. one
// of the combinator's own arguments (bounded by its matching parentheses)
// itself contains another combinator call, as opposed to several SIBLING
// calls joined by allOf()/anyOf(). Nesting hasDescendant/hasAncestor inside
// one another causes Clang's MatchFinder to re-run a full subtree traversal
// for every candidate the OUTER combinator considers, which is
// multiplicative (quadratic or worse) in subtree size -- not the roughly
// linear-per-combinator cost engine.cpp's estimated-work check assumes for
// sibling combinators. There is no safe linear bound for arbitrary nesting
// depth, so this is used to reject nested combinators outright at
// validation time rather than trying to model their real cost.
[[nodiscard]] bool
has_nested_matcher_occurrences(const std::string &matcher_expression,
                               const std::set<std::string> &names);

} // namespace cidx::extract
