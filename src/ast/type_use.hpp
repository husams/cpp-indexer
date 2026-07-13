// Signature-level `uses` emission: the LibTooling analogues of
// named_type_decl (ast_cursor.cpp:187) and emit_type_use (ast_cursor.cpp:213).
// Lookup-only: the named type's symbol must already be indexed; no stubs, no
// self-edges.
#pragma once

#include <cstdint>

#include "clang/AST/Type.h"

namespace clang {
class ASTContext;
class NamedDecl;
} // namespace clang

namespace cidx::lt {

class EdgeSink;
struct ExpansionLoc;

// Strip pointer/reference/array layers AS WRITTEN and return the named type's
// declaration (a typedef alias stays the alias), or nullptr for builtins.
const clang::NamedDecl *named_type_decl(clang::QualType type);

// Emit uses(7) src -> the record/enum/typedef named by `type`, grounded at
// `loc` (the site cursor's expansion location).
void emit_type_use(EdgeSink &sink, int64_t src_id, clang::QualType type,
                   int64_t file_id, const ExpansionLoc &loc, int conditional);

} // namespace cidx::lt
