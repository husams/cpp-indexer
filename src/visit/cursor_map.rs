//! Mapping from libclang `EntityKind` to our `NodeKind` (Phase 1 base set).
//!
//! Only the six M1 base node kinds are mapped here. M2 extensions
//! (`NAMESPACE`, `TEMPLATE_DECL`, `SPECIALIZATION`, `TYPEDEF`, `ENUM`, `HEADER`,
//! `MACRO`) are added in S14.
//!
//! Unmapped entity kinds (references, statements, expressions, …) return `None`
//! and are silently skipped by the visitor.

use clang::EntityKind;

use crate::schema::NodeKind;

/// Map a libclang [`EntityKind`] to a [`NodeKind`], or `None` if the kind is
/// not tracked in Phase 1 base.
///
/// # M1 base mappings
///
/// | libclang `EntityKind`         | `NodeKind`       |
/// |-------------------------------|------------------|
/// | `FunctionDecl`                | `Function`       |
/// | `Method`                      | `Method`         |
/// | `Constructor`                 | `Method`         |
/// | `Destructor`                  | `Method`         |
/// | `ConversionFunction`          | `Method`         |
/// | `FunctionTemplate`            | `Function`       |
/// | `ClassDecl`                   | `Class`          |
/// | `StructDecl`                  | `Class`          |
/// | `ClassTemplate`               | `Class`          |
/// | `ClassTemplatePartialSpecialization` | `Class`  |
/// | `FieldDecl`                   | `Field`          |
/// | `VarDecl` (file/ns scope)     | `GlobalVariable` |
/// | `TranslationUnit`             | `Module`         |
#[must_use]
pub fn entity_kind_to_node_kind(kind: EntityKind) -> Option<NodeKind> {
    match kind {
        // --- Module ---
        EntityKind::TranslationUnit => Some(NodeKind::Module),

        // --- Class / struct ---
        EntityKind::ClassDecl
        | EntityKind::StructDecl
        | EntityKind::ClassTemplate
        | EntityKind::ClassTemplatePartialSpecialization => Some(NodeKind::Class),

        // --- Free functions and function templates ---
        EntityKind::FunctionDecl | EntityKind::FunctionTemplate => Some(NodeKind::Function),

        // --- Member functions (including special members) ---
        EntityKind::Method
        | EntityKind::Constructor
        | EntityKind::Destructor
        | EntityKind::ConversionFunction => Some(NodeKind::Method),

        // --- Data members ---
        EntityKind::FieldDecl => Some(NodeKind::Field),

        // --- File/namespace-scope variables ---
        EntityKind::VarDecl => Some(NodeKind::GlobalVariable),

        // Everything else (references, statements, expressions, …) is skipped.
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn translation_unit_maps_to_module() {
        assert_eq!(
            entity_kind_to_node_kind(EntityKind::TranslationUnit),
            Some(NodeKind::Module)
        );
    }

    #[test]
    fn class_kinds_map_to_class() {
        for kind in [
            EntityKind::ClassDecl,
            EntityKind::StructDecl,
            EntityKind::ClassTemplate,
            EntityKind::ClassTemplatePartialSpecialization,
        ] {
            assert_eq!(
                entity_kind_to_node_kind(kind),
                Some(NodeKind::Class),
                "{kind:?} should map to Class"
            );
        }
    }

    #[test]
    fn function_kinds_map_to_function() {
        for kind in [EntityKind::FunctionDecl, EntityKind::FunctionTemplate] {
            assert_eq!(
                entity_kind_to_node_kind(kind),
                Some(NodeKind::Function),
                "{kind:?} should map to Function"
            );
        }
    }

    #[test]
    fn method_kinds_map_to_method() {
        for kind in [
            EntityKind::Method,
            EntityKind::Constructor,
            EntityKind::Destructor,
            EntityKind::ConversionFunction,
        ] {
            assert_eq!(
                entity_kind_to_node_kind(kind),
                Some(NodeKind::Method),
                "{kind:?} should map to Method"
            );
        }
    }

    #[test]
    fn field_decl_maps_to_field() {
        assert_eq!(
            entity_kind_to_node_kind(EntityKind::FieldDecl),
            Some(NodeKind::Field)
        );
    }

    #[test]
    fn var_decl_maps_to_global_variable() {
        assert_eq!(
            entity_kind_to_node_kind(EntityKind::VarDecl),
            Some(NodeKind::GlobalVariable)
        );
    }

    #[test]
    fn untracked_kinds_return_none() {
        for kind in [
            EntityKind::CallExpr,
            EntityKind::DeclRefExpr,
            EntityKind::TypeRef,
            EntityKind::Namespace,
            EntityKind::EnumDecl,
            EntityKind::TypedefDecl,
            EntityKind::ParmDecl,
        ] {
            assert!(
                entity_kind_to_node_kind(kind).is_none(),
                "{kind:?} should not be tracked in Phase 1 base"
            );
        }
    }
}
