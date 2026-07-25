// v30 signature/type tier: normalized type interning.
//
// TypeInterner walks a clang::QualType's structure and interns one type_node
// row per distinct shape (keyed by a deterministic structural encoding), plus
// the type_edge rows relating the layers (pointee/element/alias_of/return/
// param/template-arg). The DeclarationEdgeVisitor uses it to make callable
// signatures, variable/field types, and typedef targets traversable.
//
// Emission goes through the focused type port only; no storage or I/O here.
#pragma once

#include "ast/edge_records.hpp"

#include "clang/AST/Type.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace clang {
class ASTContext;
} // namespace clang

namespace cidx::ast {

class TypeFactEmitter;

// type_node.kind codes (mirrors the storage type_kind seed / cidx::kTypeKind*).
inline constexpr int64_t kTypeBuiltin = 1;
inline constexpr int64_t kTypeRecord = 2;
inline constexpr int64_t kTypeEnum = 3;
inline constexpr int64_t kTypeAlias = 4;
inline constexpr int64_t kTypePointer = 5;
inline constexpr int64_t kTypeLValueRef = 6;
inline constexpr int64_t kTypeRValueRef = 7;
inline constexpr int64_t kTypeArray = 8;
inline constexpr int64_t kTypeFunction = 9;
inline constexpr int64_t kTypeTemplateParam = 10;
inline constexpr int64_t kTypeOther = 11;
inline constexpr int64_t kTypeMemberDataPointer = 12;
inline constexpr int64_t kTypeMemberFunctionPointer = 13;
inline constexpr int64_t kTypePackExpansion = 14;

// type_edge.kind codes (mirrors the storage type_edge_kind seed).
inline constexpr int64_t kTypeEdgePointeeK = 1;
inline constexpr int64_t kTypeEdgeElementK = 2;
inline constexpr int64_t kTypeEdgeAliasOfK = 3;
inline constexpr int64_t kTypeEdgeReturnK = 4;
inline constexpr int64_t kTypeEdgeParamK = 5;
inline constexpr int64_t kTypeEdgeTemplateArgK = 6;
inline constexpr int64_t kTypeEdgeMemberOwnerK = 7;
inline constexpr int64_t kTypeEdgeMemberComponentK = 8;

// symbol_type.kind codes (mirrors the storage symbol_type_kind seed).
inline constexpr int64_t kSymTypeReturnsK = 1;
inline constexpr int64_t kSymTypeOfTypeK = 2;
inline constexpr int64_t kSymTypeUnderlyingK = 3;

class TypeInterner {
public:
  TypeInterner(clang::ASTContext &context, TypeFactEmitter &types);

  // Intern the full shape of `qt` (children first), returning the stable
  // type_node.id, or nullopt for a null type.
  std::optional<int64_t> intern(clang::QualType qt);

private:
  struct Result {
    int64_t id = -1;
    std::string key;
  };

  std::optional<Result> build(clang::QualType qt, int depth);
  // Intern one classified node, wiring canonical_id for sugared types.
  Result emit_node(clang::QualType qt, TypeNodeRecord rec, int depth);

  clang::ASTContext &context_;
  TypeFactEmitter &types_;
  // QualType opaque ptr -> interned result (per-pass memo; a QualType is
  // unique per (Type*, qualifiers) inside one ASTContext).
  std::unordered_map<const void *, Result> memo_;
};

} // namespace cidx::ast
