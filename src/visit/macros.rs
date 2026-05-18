//! Phase 1 macro visitor: `MACRO` nodes and `EXPANDS_TO` edges (AC-M5-1..4, S26).
//!
//! ## Overview
//!
//! When `detailed_preprocessing_record(true)` is passed to libclang, the AST walk
//! includes two extra entity kinds:
//!
//! - `MacroDefinition` — the `#define` site; one per macro name per file.
//! - `MacroExpansion` — every invocation of a macro; can appear at file scope
//!   (top-level) or nested inside another macro's body.
//!
//! This module provides two independent collection passes that are called from
//! `visit::shallow::Collector`:
//!
//! 1. **`collect_macro_definition`** — called when the visitor encounters a
//!    `MacroDefinition` entity.  Emits a `MACRO` node.
//!
//! 2. **`collect_macro_expansion`** — called when the visitor encounters a
//!    `MacroExpansion` entity.  Emits an `EXPANDS_TO` edge from the enclosing
//!    lexical context (function / method / module USR) to the `MACRO` node.
//!    Only **top-level** expansions are emitted (AC-M5-3): an expansion is
//!    top-level when its *spelling* location and its *expansion* location are in
//!    the same file (nested expansions have a spelling location inside the
//!    expanding macro's body, which is a synthesised virtual location whose file
//!    differs from the physical expansion site).
//!
//! ## USR synthesis for macros
//!
//! libclang does not assign USRs to `MacroDefinition` or `MacroExpansion`
//! entities.  We synthesise a stable USR as:
//!
//! ```text
//! macro:<absolute_file_path>:<macro_name>
//! ```
//!
//! This is stable across TU re-parses as long as the definition site does not
//! move.  Phase 3 deduplicates by USR, so multiple TUs including the same
//! header emit the same `MACRO` node without duplication.
//!
//! ## Edge count bound (AC-M5-4)
//!
//! By limiting to top-level expansions and using USR-level deduplication of MACRO
//! nodes, the number of `EXPANDS_TO` edges per source file is bounded by the
//! number of macro invocations visible in that file, which in practice is
//! O(source_lines).  X-macro `.def` files that contain only `HANDLE_*(…)` lines
//! produce exactly one edge per invocation line — well within the ≤ 10× bound.

use clang::{Entity, EntityKind};

use crate::schema::{EdgeKind, EdgeRecord, NodeKind, NodeRecord};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Collect a `MACRO` `NodeRecord` from a `MacroDefinition` entity.
///
/// Returns `None` if the entity is a builtin macro (no physical file) or if
/// the entity does not have a usable name or location.
///
/// The caller is responsible for pushing the returned record into its node
/// accumulator.
pub(crate) fn collect_macro_definition(
    entity: &Entity<'_>,
    repo_name: &str,
    tu_hash: [u8; 32],
    partial: bool,
    tu_file_path: &str,
    skip_system_headers: bool,
) -> Option<NodeRecord> {
    debug_assert_eq!(entity.get_kind(), EntityKind::MacroDefinition);

    // Builtins have no physical location — skip them.
    if entity.is_builtin_macro() {
        return None;
    }

    // System-header filter: honour the same flag as the AST visitor.
    if skip_system_headers && entity.is_in_system_header() {
        return None;
    }

    let name = entity.get_name()?;
    if name.is_empty() {
        return None;
    }

    // Spelling location gives the physical `#define` site.
    let loc = entity.get_location()?;
    let spell = loc.get_spelling_location();
    let file_path = spell
        .file
        .as_ref()
        .map(|f| f.get_path().to_string_lossy().into_owned())
        .unwrap_or_else(|| tu_file_path.to_owned());

    // Synthesise stable USR: "macro:<file>:<name>".
    let usr = macro_usr(&file_path, &name);

    let line = if spell.line > 0 {
        Some(spell.line)
    } else {
        None
    };
    let col = if spell.column > 0 {
        Some(spell.column)
    } else {
        None
    };

    let attrs_json = build_macro_attrs(entity);

    Some(NodeRecord {
        usr,
        kind: NodeKind::Macro,
        name,
        qualified_name: file_path.clone(),
        mangled_name: None,
        file_path,
        line,
        col,
        repo_name: repo_name.to_owned(),
        attrs_json,
        partial,
        phase: 1,
        tu_hash,
        // M8 promoted fields — MACRO nodes carry none of these
        return_type: None,
        params: None,
        signature: None,
        code: None,
        code_truncated: None,
        template_params: None,
        template_args: None,
        is_virtual: None,
        is_pure_virtual: None,
        is_static: None,
    })
}

/// Collect an `EXPANDS_TO` `EdgeRecord` from a `MacroExpansion` entity.
///
/// Returns `None` when:
/// - The expansion is nested inside another macro's body (AC-M5-3).
/// - The referenced `MacroDefinition` cannot be resolved.
/// - The expansion is in a system header and `skip_system_headers` is set.
///
/// `enclosing_usr` is the USR of the closest lexical ancestor that is a
/// function, method, or module.  Pass the module USR for file-scope
/// expansions.
pub(crate) fn collect_macro_expansion(
    entity: &Entity<'_>,
    enclosing_usr: &str,
    repo_name: &str,
    tu_hash: [u8; 32],
    tu_file_path: &str,
    skip_system_headers: bool,
) -> Option<EdgeRecord> {
    debug_assert_eq!(entity.get_kind(), EntityKind::MacroExpansion);

    // System-header filter.
    if skip_system_headers && entity.is_in_system_header() {
        return None;
    }

    // Top-level filter (AC-M5-3): a nested expansion has a spelling location
    // whose *file* differs from its expansion location file (libclang sets the
    // spelling file to the macro's definition file for nested expansions).
    if !is_top_level_expansion(entity) {
        return None;
    }

    // Resolve the MacroDefinition via get_reference().
    let macro_def = entity.get_reference()?;
    if macro_def.get_kind() != EntityKind::MacroDefinition {
        return None;
    }

    // Build the macro USR from the definition's location.
    let macro_name = macro_def.get_name()?;
    let def_loc = macro_def.get_location()?;
    let def_spell = def_loc.get_spelling_location();
    let def_file = def_spell
        .file
        .as_ref()
        .map(|f| f.get_path().to_string_lossy().into_owned())
        .unwrap_or_else(|| tu_file_path.to_owned());

    let macro_usr = macro_usr(&def_file, &macro_name);

    Some(EdgeRecord {
        src_usr: enclosing_usr.to_owned(),
        dst_usr: Some(macro_usr),
        dst_placeholder: None,
        kind: EdgeKind::ExpandsTo,
        resolved: false,
        cross_repo_candidate: false,
        repo_name: repo_name.to_owned(),
        attrs_json: "{}".to_owned(),
        tu_hash,
        // M8 promoted fields — EXPANDS_TO edges carry none of these
        source_association_type: None,
        target_association_type: None,
    })
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Synthesise a stable USR for a macro definition.
///
/// Format: `"macro:<absolute_file_path>:<macro_name>"`.
pub(crate) fn macro_usr(file_path: &str, name: &str) -> String {
    format!("macro:{file_path}:{name}")
}

/// Determine whether a `MacroExpansion` is top-level (not nested inside another
/// macro's body).
///
/// libclang represents nested macro expansions with a *spelling* location whose
/// file is the definition file of the enclosing macro (a synthesised virtual
/// location).  For a top-level expansion, the spelling location and the
/// expansion location both point to the same physical file.
///
/// Returns `true` for top-level; `false` for nested.
fn is_top_level_expansion(entity: &Entity<'_>) -> bool {
    let loc = match entity.get_location() {
        Some(l) => l,
        None => return false,
    };

    let spell = loc.get_spelling_location();
    let expansion = loc.get_expansion_location();

    // Compare file identity: if both locations have a file and the paths match,
    // the expansion is top-level.
    match (spell.file, expansion.file) {
        (Some(sf), Some(ef)) => sf.get_path() == ef.get_path(),
        // No file on either side (virtual / builtin) — skip.
        _ => false,
    }
}

/// Build `attrs_json` for a `MacroDefinition` entity.
///
/// Captures:
/// - `"is_function_like"`: `true` for function-like macros.
/// - `"params"`: JSON array of parameter name strings for function-like macros;
///   `null` for object-like macros (absent key encodes as `null`).
/// - `"is_builtin"`: always `false` here (builtins are filtered before this call).
fn build_macro_attrs(entity: &Entity<'_>) -> String {
    use std::collections::BTreeMap;

    let is_function_like = entity.is_function_like_macro();
    let mut map: BTreeMap<&str, serde_json::Value> = BTreeMap::new();
    map.insert(
        "is_function_like",
        serde_json::Value::Bool(is_function_like),
    );
    map.insert("is_builtin", serde_json::Value::Bool(false));

    // `clang-rs` does not expose macro parameter names directly; we record
    // `null` for now rather than attempting token-level parsing.
    map.insert("params", serde_json::Value::Null);

    serde_json::to_string(&map).unwrap_or_else(|_| "{}".to_owned())
}

// ---------------------------------------------------------------------------
// Unit tests (AC-M5-1..4)
// ---------------------------------------------------------------------------

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    // ── USR synthesis ────────────────────────────────────────────────────────

    #[test]
    fn macro_usr_format() {
        let u = macro_usr("/repo/src/foo.h", "MY_MACRO");
        assert_eq!(u, "macro:/repo/src/foo.h:MY_MACRO");
    }

    #[test]
    fn macro_usr_stable_across_calls() {
        let u1 = macro_usr("/a/b.h", "FOO");
        let u2 = macro_usr("/a/b.h", "FOO");
        assert_eq!(u1, u2);
    }

    #[test]
    fn macro_usr_different_names() {
        let u1 = macro_usr("/a/b.h", "FOO");
        let u2 = macro_usr("/a/b.h", "BAR");
        assert_ne!(u1, u2);
    }

    // ── attrs_json content ───────────────────────────────────────────────────

    /// Integration-level attrs parsing helper (used by integration fixture tests).
    #[allow(dead_code)]
    pub fn parse_macro_attrs(attrs: &str) -> serde_json::Value {
        serde_json::from_str(attrs).expect("attrs_json must be valid JSON")
    }

    // ── NodeKind / EdgeKind round-trip ───────────────────────────────────────

    #[test]
    fn macro_node_kind_round_trips() {
        use crate::schema::NodeKind;
        let s = NodeKind::Macro.as_str();
        assert_eq!(s, "MACRO");
        assert_eq!(NodeKind::try_from_arrow_str(s), Some(NodeKind::Macro));
    }

    #[test]
    fn expands_to_edge_kind_round_trips() {
        use crate::schema::EdgeKind;
        let s = EdgeKind::ExpandsTo.as_str();
        assert_eq!(s, "EXPANDS_TO");
        assert_eq!(EdgeKind::try_from_arrow_str(s), Some(EdgeKind::ExpandsTo));
    }

    #[test]
    fn macro_in_node_kind_all() {
        use crate::schema::NodeKind;
        assert!(
            NodeKind::all().contains(&NodeKind::Macro),
            "NodeKind::Macro must appear in NodeKind::all()"
        );
    }

    #[test]
    fn expands_to_in_edge_kind_all() {
        use crate::schema::EdgeKind;
        assert!(
            EdgeKind::all().contains(&EdgeKind::ExpandsTo),
            "EdgeKind::ExpandsTo must appear in EdgeKind::all()"
        );
    }

    // ── Schema version bump ──────────────────────────────────────────────────

    #[test]
    fn schema_version_bumped_to_5() {
        use crate::schema::version::SCHEMA_VERSION;
        assert_eq!(
            SCHEMA_VERSION, 5,
            "SCHEMA_VERSION must be 5 after S40-S43 (M8 native fields)"
        );
    }

    // ── Edge record shape ────────────────────────────────────────────────────

    #[test]
    fn expands_to_edge_record_fields() {
        use crate::schema::EdgeKind;
        let edge = EdgeRecord {
            src_usr: "tu:/repo/main.cpp".to_owned(),
            dst_usr: Some("macro:/repo/main.cpp:MY_MACRO".to_owned()),
            dst_placeholder: None,
            kind: EdgeKind::ExpandsTo,
            resolved: false,
            cross_repo_candidate: false,
            repo_name: "repo".to_owned(),
            attrs_json: "{}".to_owned(),
            tu_hash: [0u8; 32],
            source_association_type: None,
            target_association_type: None,
        };
        assert_eq!(edge.kind, EdgeKind::ExpandsTo);
        assert!(edge.dst_usr.as_deref().unwrap().starts_with("macro:"));
        assert!(!edge.resolved);
        assert!(!edge.cross_repo_candidate);
    }
}
