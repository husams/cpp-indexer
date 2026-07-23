// astgraph — dump one TU's Clang C++ AST into a per-TU SQLite graph DB for
// Datalog (Soufflé) reasoning.  Design decisions (2026-07-09, user-approved;
// rebuilt 2026-07-12 on the Clang C++ AST, libclang removed):
//
//   * UNIFIED node space: declarations, statements/expressions AND types are
//     rows of `node`; every relation is a row of `edge(src,dst,rel,ord)`.
//     node_kind ids are CIDX-STABLE constants (schema.hpp), namespaced by
//     category: decls [1000,1999], stmts/exprs [2000,2999], types [3000,3999].
//   * NO NULLs anywhere — Soufflé's sqlite IO reads plain tables, so every
//     "absent" cell is a 0 sentinel (real ids start at 1) or ''.
//   * relation_kind is a FIXED catalog (RelKind below) — each relation maps to
//     a Clang C++ AST accessor (see astgraph.cpp process_* methods).
//   * symbol is deduped by USR (clang::index::generateUSRForDecl, byte-identical
//     to libclang) and joins against cidx index.db symbol.usr.
//
// The tool shares cidx's configuration: compile args + driver for the TU come
// from the cidx index.db `file` row (main.cpp); the AST is built by a ClangTool
// with the same toolchain flags + resource dir as the `cidx index` engine.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "astgraph/schema.hpp"

namespace cidx::astgraph {

// The fixed relation catalog.  1-8 relate cursor-space (decl/stmt) nodes, 10
// crosses type→decl, 11-19 relate type nodes.  Id 9 (has_type) is RETIRED: a
// node's own type is the `node.type_id` column, not an edge.  `ord` carries
// sibling / argument / template-argument position where noted.  Each relation
// maps to a Clang C++ AST accessor (astgraph.cpp).
enum RelKind : int {
  kRelChild = 1,          // structural AST containment (ord = child position)
  kRelReferences = 2,     // DeclRefExpr/MemberExpr/CallExpr callee etc.
  kRelDefinition = 3,     // Decl::getDefinition()
  kRelCanonical = 4,      // Decl::getCanonicalDecl()
  kRelSemanticParent = 5, // Decl::getDeclContext()  (decls only)
  kRelLexicalParent = 6,  // Decl::getLexicalDeclContext()  (decls only)
  kRelSpecializes = 7,    // getSpecializedTemplate() / getPrimaryTemplate()
  kRelOverrides = 8,      // CXXMethodDecl::overridden_methods (ord = position)
  // 9 = has_type, retired (see node.type_id)
  kRelTypeDecl = 10,      // Type -> its declaration node
  kRelCanonicalType = 11, // ASTContext::getCanonicalType()
  kRelPointee = 12,       // (Pointer|Reference|...)Type::getPointeeType()
  kRelElementType = 13,   // ArrayType/VectorType/ComplexType::getElementType()
  kRelResultType = 14,    // FunctionType::getReturnType()
  kRelArgType = 15,       // FunctionProtoType::getParamType (ord = position)
  kRelNamedType = 16,     // ElaboratedType::getNamedType()
  kRelUnderlyingType = 17,// TypedefNameDecl::getUnderlyingType (decl -> type)
  kRelTemplateArg = 18,   // TemplateSpecializationType arg (ord = position)
  kRelClassType = 19,     // MemberPointerType::getClass()
};

struct Options {
  // Restrict the STRUCTURAL walk to main-file declarations; header entities
  // still appear as shallow nodes when referenced (references/definition/...).
  bool main_only = false;
  // The normalized descriptor identity selected from index.db.  Astgraph
  // records it in the artifact so downstream analysis can prove that it used
  // the same configuration as indexing and diff.
  std::optional<std::string> descriptor_hash;
  std::optional<std::string> resource_dir;
};

struct DumpStats {
  int64_t cursor_nodes = 0; // decl + stmt/expr nodes
  int64_t type_nodes = 0;
  int64_t edges = 0;
  int64_t symbols = 0;
  int64_t files = 0;
};

// Stable identity of a dump's semantic inputs. Used for collision-safe default
// filenames and recorded in metadata; changing source, flags, driver, or
// main-only mode produces a different key.
std::string artifact_key(const std::string &source_path,
                         const std::vector<std::string> &args,
                         const std::optional<std::string> &driver,
                         const Options &opts);

// Atomically replace `out_db_path` with a fresh dump of the TU rooted at
// `source_path`, built by a ClangTool over `args` (+ toolchain flags) and
// `driver`.  A failed dump keeps any previous artifact intact.  `source_path` /
// `args` / `driver` are recorded in metadata for provenance.
// Throws StorageError / CidxError on failure (including a parse that produced
// no AST).
DumpStats dump_tu(const std::string &source_path,
                  const std::vector<std::string> &args,
                  const std::optional<std::string> &driver,
                  const std::string &out_db_path, const Options &opts);

} // namespace cidx::astgraph
