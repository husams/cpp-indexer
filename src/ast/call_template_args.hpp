// As-written template arguments at a call site.
//
// The call site is never the source of truth for WHICH template arguments a
// callable specialization has — that is getTemplateSpecializationArgs() on the
// callee, handled by emit_callable_template_identity. The explicit `<...>`
// written at the site only contributes the as-written TYPE spellings, overlaid
// positionally onto the full argument list.
#pragma once

#include "clang/AST/Type.h"

#include <vector>

namespace clang {
class Expr;
} // namespace clang

namespace cidx::ast {

// The types written inside the callee's explicit `<...>` (DeclRefExpr or
// MemberExpr), position-aligned with the leading specialization arguments.
// Non-type positions and sites without explicit arguments yield null entries /
// an empty vector.
std::vector<clang::QualType> written_template_args(const clang::Expr *site);

} // namespace cidx::ast
