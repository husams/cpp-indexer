// Value-provenance classification for call receivers and arguments: map a
// typed expression to the cidx provenance categories (local/global/member/
// this/construct/call_result/literal/unknown) recorded on edge_site and
// call_arg rows.
#pragma once

#include "ast/edge_records.hpp"

#include <string>

namespace clang {
class ASTContext;
class Expr;
class QualType;
} // namespace clang

#include "clang/AST/Type.h"

namespace cidx::ast {

// Strip the wrappers that do not change a value's provenance: parentheses,
// implicit casts, cleanups, temporary materialization/binding, explicit
// non-functional casts of a value (C-style and the named casts), and the
// provenance-preserving unary operators & and * (which still denote their
// operand's storage). Functional casts and every other spelled operator stay
// visible and classify as derived values.
const clang::Expr *normalize_value_expr(const clang::Expr *expr);

// USR of the record decl after stripping pointers/references/cv from the
// canonical type ("" for builtins).
std::string record_usr_of_type(clang::QualType type);

// True iff loc_type holds dispatch_record_usr BY VALUE.
bool type_is_value(clang::QualType loc_type,
                   const std::string &dispatch_record_usr);

// The DECLARED type of the value source (not the use-site expression type,
// which auto-derefs lvalue references).
clang::QualType decl_type_for_expr(const clang::Expr *normalized);

// Provenance of a value expression.
ValueSource classify_value_source(const clang::ASTContext &context,
                                  const clang::Expr *expr);

} // namespace cidx::ast
