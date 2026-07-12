// Value-source classification for call receivers and arguments (Phase 2/3
// provenance) — the LibTooling analogues of ast_body.cpp's peel_expr,
// record_usr_of_type, type_is_value, decl_type_for_expr and
// classify_value_source.
#pragma once

#include "clangx_lt/edge_records2.hpp"

#include <string>

namespace clang {
class ASTContext;
class Expr;
class QualType;
} // namespace clang

#include "clang/AST/Type.h"

namespace cidx::lt {

// Peel implicit casts, parens, unary operators and C-style casts down to the
// underlying named sub-expression (at most 16 layers; peel_expr).
const clang::Expr *peel_expr(const clang::Expr *expr);

// USR of the record decl after stripping pointers/references/cv from the
// canonical type ("" for builtins; record_usr_of_type).
std::string record_usr_of_type(clang::QualType type);

// True iff loc_type holds dispatch_record_usr BY VALUE (type_is_value).
bool type_is_value(clang::QualType loc_type,
                   const std::string &dispatch_record_usr);

// The DECLARED type of the value source (not the use-site expression type,
// which auto-derefs lvalue references; decl_type_for_expr).
clang::QualType decl_type_for_expr(const clang::Expr *peeled);

// Provenance of a value expression (classify_value_source).
ValueSource classify_value_source(const clang::ASTContext &context,
                                  const clang::Expr *expr);

} // namespace cidx::lt
