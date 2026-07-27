// Determines which `.bind(id)` in a matcher expression is bound to the
// OUTERMOST (root) matcher construction -- the single point of truth shared
// by validator_matchers.cpp (which requires main_file-scoped rules to bind
// their root) and engine.cpp (which anchors main_file scope filtering on
// that same binding's resolved location, instead of an arbitrary
// first-declared binding). Living in one place means validation-time and
// execution-time root-binding determination can never drift apart.
#pragma once

#include <string>

namespace cidx::extract {

// Parses `matcher_expression` (assumed already known to parse successfully;
// callers that have not yet validated it should still handle an empty
// result) and returns the bind id attached to its outermost matcher, or an
// empty string if the root has no bind (or the expression fails to parse).
//
// Clang::ast_matchers::dynamic::Parser::actOnMatcherExpression is invoked
// bottom-up: a matcher's arguments must be constructed and bound before the
// matcher itself is. So during a single top-level parse, the LAST
// actOnMatcherExpression call corresponds to the outermost/root matcher --
// this simply records the most recent bind id seen.
[[nodiscard]] std::string
root_binding_of(const std::string &matcher_expression);

} // namespace cidx::extract
