//! Mapping from libclang `EntityKind` to our `NodeKind` (M1 base + M2 extensions).
//!
//! M1 base: MODULE, CLASS, FUNCTION, METHOD, FIELD, GLOBAL_VARIABLE.
//! M2 extensions (S14): NAMESPACE, TEMPLATE_DECL, SPECIALIZATION, TYPEDEF, ENUM, HEADER.
//! `MACRO` (S22) is not mapped here.
//!
//! Unmapped entity kinds (references, statements, expressions, …) return `None`
//! and are silently skipped by the visitor.

use clang::EntityKind;

use crate::schema::NodeKind;

/// Map a libclang [`EntityKind`] to a [`NodeKind`], or `None` if the kind is
/// not tracked in Phase 1.
///
/// ## M1 base mappings
///
/// | libclang `EntityKind`               | `NodeKind`       |
/// |-------------------------------------|------------------|
/// | `FunctionDecl`                      | `Function`       |
/// | `Method`                            | `Method`         |
/// | `Constructor`                       | `Method`         |
/// | `Destructor`                        | `Method`         |
/// | `ConversionFunction`                | `Method`         |
/// | `FunctionTemplate`                  | `Function`       |
/// | `ClassDecl`                         | `Class`          |
/// | `StructDecl`                        | `Class`          |
/// | `ClassTemplate`                     | `Class`          |
/// | `ClassTemplatePartialSpecialization`| `Class`          |
/// | `FieldDecl`                         | `Field`          |
/// | `VarDecl` (file/ns scope)           | `GlobalVariable` |
/// | `TranslationUnit`                   | `Module`         |
///
/// ## M2 extension mappings (S14)
///
/// | libclang `EntityKind`               | `NodeKind`       |
/// |-------------------------------------|------------------|
/// | `Namespace`                         | `Namespace`      |
/// | `FunctionTemplate`                  | `TemplateDef`    | ← takes precedence over `Function`
/// | `ClassTemplate`                     | `TemplateDef`    | ← takes precedence over `Class`
/// | `TypeAliasTemplateDecl`             | `TemplateDef`    |
/// | `ClassTemplatePartialSpecialization`| `Specialization` |
/// | `TypedefDecl`                       | `Typedef`        |
/// | `TypeAliasDecl`                     | `Typedef`        |
/// | `EnumDecl`                          | `Enum`           |
/// | `InclusionDirective`                | `Header`         |
///
/// Note: `ClassTemplate` maps to `TemplateDef` (not `Class`) so that specializations
/// (`ClassTemplatePartialSpecialization`) can be cleanly distinguished.  Concrete
/// instantiated class nodes retain `Class`.
#[must_use]
pub fn entity_kind_to_node_kind(kind: EntityKind) -> Option<NodeKind> {
    match kind {
        // ── Module ──────────────────────────────────────────────────────────
        EntityKind::TranslationUnit => Some(NodeKind::Module),

        // ── Namespaces ──────────────────────────────────────────────────────
        EntityKind::Namespace => Some(NodeKind::Namespace),

        // ── Template declarations (before concrete class/function) ───────────
        // ClassTemplate and FunctionTemplate produce TEMPLATE_DECL nodes.
        EntityKind::ClassTemplate | EntityKind::TypeAliasTemplateDecl => {
            Some(NodeKind::TemplateDef)
        }
        EntityKind::FunctionTemplate => Some(NodeKind::TemplateDef),

        // ── Template specializations/instantiations ─────────────────────────
        EntityKind::ClassTemplatePartialSpecialization => Some(NodeKind::Specialization),

        // ── Concrete class / struct ─────────────────────────────────────────
        // ClassDecl and StructDecl that are NOT templates map to Class.
        // (ClassTemplate is handled above.)
        EntityKind::ClassDecl | EntityKind::StructDecl => Some(NodeKind::Class),

        // ── Free functions (non-template) ───────────────────────────────────
        EntityKind::FunctionDecl => Some(NodeKind::Function),

        // ── Member functions (including special members) ─────────────────────
        EntityKind::Method
        | EntityKind::Constructor
        | EntityKind::Destructor
        | EntityKind::ConversionFunction => Some(NodeKind::Method),

        // ── Data members ────────────────────────────────────────────────────
        EntityKind::FieldDecl => Some(NodeKind::Field),

        // ── File/namespace-scope variables ───────────────────────────────────
        EntityKind::VarDecl => Some(NodeKind::GlobalVariable),

        // ── Typedefs and using aliases ───────────────────────────────────────
        EntityKind::TypedefDecl | EntityKind::TypeAliasDecl => Some(NodeKind::Typedef),

        // ── Enumerations ────────────────────────────────────────────────────
        EntityKind::EnumDecl => Some(NodeKind::Enum),

        // ── Inclusion directives → HEADER nodes ─────────────────────────────
        EntityKind::InclusionDirective => Some(NodeKind::Header),

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
    fn class_decl_and_struct_decl_map_to_class() {
        for kind in [EntityKind::ClassDecl, EntityKind::StructDecl] {
            assert_eq!(
                entity_kind_to_node_kind(kind),
                Some(NodeKind::Class),
                "{kind:?} should map to Class"
            );
        }
    }

    #[test]
    fn template_kinds_map_to_template_def() {
        for kind in [
            EntityKind::ClassTemplate,
            EntityKind::FunctionTemplate,
            EntityKind::TypeAliasTemplateDecl,
        ] {
            assert_eq!(
                entity_kind_to_node_kind(kind),
                Some(NodeKind::TemplateDef),
                "{kind:?} should map to TemplateDef"
            );
        }
    }

    #[test]
    fn partial_specialization_maps_to_specialization() {
        assert_eq!(
            entity_kind_to_node_kind(EntityKind::ClassTemplatePartialSpecialization),
            Some(NodeKind::Specialization)
        );
    }

    #[test]
    fn function_decl_maps_to_function() {
        assert_eq!(
            entity_kind_to_node_kind(EntityKind::FunctionDecl),
            Some(NodeKind::Function)
        );
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
    fn namespace_maps_to_namespace() {
        assert_eq!(
            entity_kind_to_node_kind(EntityKind::Namespace),
            Some(NodeKind::Namespace)
        );
    }

    #[test]
    fn typedef_kinds_map_to_typedef() {
        for kind in [EntityKind::TypedefDecl, EntityKind::TypeAliasDecl] {
            assert_eq!(
                entity_kind_to_node_kind(kind),
                Some(NodeKind::Typedef),
                "{kind:?} should map to Typedef"
            );
        }
    }

    #[test]
    fn enum_decl_maps_to_enum() {
        assert_eq!(
            entity_kind_to_node_kind(EntityKind::EnumDecl),
            Some(NodeKind::Enum)
        );
    }

    #[test]
    fn inclusion_directive_maps_to_header() {
        assert_eq!(
            entity_kind_to_node_kind(EntityKind::InclusionDirective),
            Some(NodeKind::Header)
        );
    }

    #[test]
    fn untracked_kinds_return_none() {
        for kind in [
            EntityKind::CallExpr,
            EntityKind::DeclRefExpr,
            EntityKind::TypeRef,
            EntityKind::ParmDecl,
        ] {
            assert!(
                entity_kind_to_node_kind(kind).is_none(),
                "{kind:?} should not be tracked"
            );
        }
    }
}
