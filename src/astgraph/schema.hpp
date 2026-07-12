// Lightweight, dependency-free astgraph schema constants shared by the dumper,
// the native analysis runners, and the tests.
#pragma once

namespace cidx::astgraph {

// On-disk schema version of <TU>.db (meta key "schema_version").
// v3 (2026-07-12): the dumper was rebuilt on the Clang C++ AST (the libclang
// cursor/type API was removed). node_kind ids are now CIDX-STABLE constants
// (below) mapped from clang::Decl::Kind / Stmt::StmtClass / Type::TypeClass,
// NOT raw libclang CXCursorKind/CXTypeKind values. Relation ids (RelKind, in
// astgraph.hpp) are unchanged. The stable ids keep the generated Souffle rules
// (which hardcode kind literals) valid across LLVM majors (21 on RHEL, 22 on
// macOS), where the underlying clang enum values differ.
inline constexpr int kSchemaVersion = 3;

// --- CIDX-STABLE node-kind ids -----------------------------------------------
// Disjoint ranges so "cursor node vs type node" is a single threshold at 3000:
//   decls  [1000,1999]   stmts/exprs [2000,2999]   types [3000,3999]
// The mapper (astgraph.cpp) assigns these from the live clang enums; unmapped
// kinds fall into the per-category "Other" bucket so every node is classified.

// Declarations.
inline constexpr int kNodeTranslationUnit = 1000;
inline constexpr int kNodeFunction = 1001;
inline constexpr int kNodeCXXMethod = 1002;
inline constexpr int kNodeCXXConstructor = 1003;
inline constexpr int kNodeCXXDestructor = 1004;
inline constexpr int kNodeVar = 1005;
inline constexpr int kNodeParmVar = 1006;
inline constexpr int kNodeField = 1007;
inline constexpr int kNodeCXXRecord = 1008;
inline constexpr int kNodeRecord = 1009;
inline constexpr int kNodeEnum = 1010;
inline constexpr int kNodeEnumConstant = 1011;
inline constexpr int kNodeNamespace = 1012;
inline constexpr int kNodeTypedef = 1013;
inline constexpr int kNodeTypeAlias = 1014;
inline constexpr int kNodeFunctionTemplate = 1015;
inline constexpr int kNodeClassTemplate = 1016;
inline constexpr int kNodeOtherDecl = 1099;

// Statements / expressions.
inline constexpr int kNodeCallExpr = 2001; // ALL call exprs (isa<CallExpr>)
inline constexpr int kNodeCXXConstructExpr = 2003;
inline constexpr int kNodeDeclRefExpr = 2004;
inline constexpr int kNodeMemberExpr = 2005;
inline constexpr int kNodeCXXNewExpr = 2006;
inline constexpr int kNodeCXXDeleteExpr = 2007;
inline constexpr int kNodeOtherExpr = 2098;
inline constexpr int kNodeOtherStmt = 2099;

// Types. Everything at or above this is a type-space node.
inline constexpr int kTypeKindMin = 3000;
inline constexpr int kNodeBuiltinType = 3001;
inline constexpr int kNodePointerType = 3002;
inline constexpr int kNodeLValueReferenceType = 3003;
inline constexpr int kNodeRValueReferenceType = 3004;
inline constexpr int kNodeRecordType = 3005;
inline constexpr int kNodeEnumType = 3006;
inline constexpr int kNodeTypedefType = 3007;
inline constexpr int kNodeElaboratedType = 3008;
inline constexpr int kNodeFunctionProtoType = 3009;
inline constexpr int kNodeConstantArrayType = 3010;
inline constexpr int kNodeTemplateSpecializationType = 3011;
inline constexpr int kNodeMemberPointerType = 3012;
inline constexpr int kNodeOtherType = 3099;

} // namespace cidx::astgraph
