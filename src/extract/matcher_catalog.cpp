#include "extract/matcher_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <functional>

namespace cidx::extract {

MatcherCatalog::MatcherCatalog(
    std::set<std::string> matcher_ids,
    std::vector<std::pair<std::string, EndpointDomain>> properties)
    : matcher_ids_(std::move(matcher_ids)), properties_(std::move(properties)) {
}

bool MatcherCatalog::allows_matcher(const std::string &matcher_id) const {
  return matcher_ids_.contains(matcher_id);
}

bool MatcherCatalog::allows_property(const std::string &property_name) const {
  return std::ranges::any_of(properties_, [&](const auto &entry) {
    return entry.first == property_name;
  });
}

std::optional<EndpointDomain>
MatcherCatalog::required_domain(const std::string &property_name) const {
  for (const auto &[name, domain] : properties_) {
    if (name == property_name) {
      return domain;
    }
  }
  return std::nullopt;
}

const MatcherCatalog &MatcherCatalog::default_catalog() {
  // Node matchers: the declaration/expression/type shapes a fact-emission
  // rule may bind. Deliberately excludes unrestricted catch-alls (stmt(),
  // expr(), decl()) with no bound parent scope -- every rule must anchor on
  // a concrete node kind.
  static const MatcherCatalog catalog(
      {
          // declaration matchers
          "functionDecl",
          "cxxMethodDecl",
          "cxxConstructorDecl",
          "cxxRecordDecl",
          "varDecl",
          "fieldDecl",
          "parmVarDecl",
          "namespaceDecl",
          "classTemplateDecl",
          "enumDecl",
          "namedDecl",
          // expression matchers
          "callExpr",
          "cxxMemberCallExpr",
          "cxxOperatorCallExpr",
          "cxxConstructExpr",
          "declRefExpr",
          "memberExpr",
          // narrowing predicates and bounded traversal
          "hasName",
          "matchesName",
          "hasAnyName",
          "callee",
          "callExpr",
          "hasArgument",
          "hasAnyArgument",
          "argumentCountIs",
          "hasAnyParameter",
          "hasParameter",
          "hasReturnType",
          "hasType",
          "pointsTo",
          "references",
          "ofClass",
          "hasDeclContext",
          "hasBody",
          "hasInitializer",
          "isDefinition",
          "isVirtual",
          "isConst",
          "isStatic",
          "isPrivate",
          "isPublic",
          "isProtected",
          "hasOverloadedOperatorName",
          "hasAncestor",
          "hasDescendant",
          "hasParent",
          "unless",
          "anyOf",
          "allOf",
      },
      {
          {"spelling", EndpointDomain::declaration},
          {"qualified_name", EndpointDomain::declaration},
          {"is_pure", EndpointDomain::declaration},
          {"is_static", EndpointDomain::declaration},
          {"is_virtual", EndpointDomain::declaration},
          {"access_spelling", EndpointDomain::declaration},
          {"storage_class", EndpointDomain::declaration},
          {"type_spelling", EndpointDomain::expression},
          {"value_kind", EndpointDomain::expression},
          {"canonical_type_spelling", EndpointDomain::type},
      });
  return catalog;
}

namespace {

// Shared string-literal-aware tokenizer: invokes `on_call(identifier,
// is_method_call)` for every `identifier(` or `.identifier(` call site
// found in `matcher_expression`, skipping quoted string contents. Both
// disallowed_matcher_calls() and count_matcher_occurrences() are built on
// this single scanner so they can never disagree about what counts as a
// "call site".
void for_each_call_site(
    const std::string &matcher_expression,
    const std::function<void(const std::string &, bool)> &on_call) {
  bool in_string = false;
  std::size_t i = 0;
  const std::size_t n = matcher_expression.size();
  while (i < n) {
    char c = matcher_expression[i];
    if (in_string) {
      if (c == '\\' && i + 1 < n) {
        i += 2;
        continue;
      }
      if (c == '"') {
        in_string = false;
      }
      ++i;
      continue;
    }
    if (c == '"') {
      in_string = true;
      ++i;
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
      std::size_t start = i;
      while (i < n && (std::isalnum(static_cast<unsigned char>(
                           matcher_expression[i])) != 0 ||
                       matcher_expression[i] == '_')) {
        ++i;
      }
      std::string ident = matcher_expression.substr(start, i - start);
      std::size_t j = i;
      while (j < n && std::isspace(static_cast<unsigned char>(
                          matcher_expression[j])) != 0) {
        ++j;
      }
      if (j < n && matcher_expression[j] == '(') {
        std::size_t k = start;
        while (k > 0 && std::isspace(static_cast<unsigned char>(
                            matcher_expression[k - 1])) != 0) {
          --k;
        }
        const bool method_call = (k > 0 && matcher_expression[k - 1] == '.');
        on_call(ident, method_call);
      }
      continue;
    }
    ++i;
  }
}

} // namespace

std::vector<std::string>
disallowed_matcher_calls(const std::string &matcher_expression,
                         const MatcherCatalog &catalog) {
  std::vector<std::string> disallowed;
  for_each_call_site(matcher_expression,
                     [&](const std::string &ident, bool method_call) {
                       if (method_call) {
                         if (ident != "bind") {
                           disallowed.push_back(ident);
                         }
                       } else if (!catalog.allows_matcher(ident)) {
                         disallowed.push_back(ident);
                       }
                     });
  return disallowed;
}

std::int64_t count_matcher_occurrences(const std::string &matcher_expression,
                                       const std::set<std::string> &names) {
  std::int64_t count = 0;
  for_each_call_site(matcher_expression,
                     [&](const std::string &ident, bool method_call) {
                       if (!method_call && names.contains(ident)) {
                         ++count;
                       }
                     });
  return count;
}

} // namespace cidx::extract
