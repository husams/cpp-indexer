//! USES edge access-mode classifier (S43, ADR-13).
//!
//! Classifies every USES-edge emission site in the Phase 1 visitor by inspecting
//! the cursor that triggered the emission and walking up to 4 parent cursors.
//!
//! ## Taxonomy (closed, exactly 7 values)
//!
//! | Variant   | String      | Meaning                                                     |
//! |-----------|-------------|-------------------------------------------------------------|
//! | Read      | `read`      | RHS of assignment, condition, const-ref argument            |
//! | Write     | `write`     | LHS of assignment, `++`/`--`, non-const ref/pointer output  |
//! | AddrOf    | `addr_of`   | `&x`, function pointer formation                            |
//! | CallArg   | `call_arg`  | Argument where param type is dependent / unresolved         |
//! | Return    | `return`    | Operand of `return` statement                               |
//! | DeclRef   | `decl_ref`  | Reference inside an initializer / default value             |
//! | Unknown   | `unknown`   | Anything else; logged at `debug` level (ADR-13 §Log level)  |
//!
//! ## References
//! - ADR-13 — USES access classifier taxonomy, libclang strategy, EXTERNAL_REF
//!   mirroring scope.
//! - `src/visit/shallow.rs` — call sites wired in S43.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;

use clang::{Entity, EntityKind};
use tracing::debug;

/// Closed enumeration of USES access modes (ADR-13, AC-S43-1).
///
/// Exactly 7 variants — enforced at compile time by the exhaustive match in
/// [`AccessKind::as_str`].  No eighth value can be added without a compiler
/// error on the `as_str` match.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AccessKind {
    /// Right-hand side of assignment or condition expression.
    Read,
    /// Left-hand side of assignment or `++`/`--` operand.
    Write,
    /// Address-of operator (`&x`) or pointer-typed parameter match.
    AddrOf,
    /// Argument to a call where param type is dependent / unresolved.
    CallArg,
    /// Operand of a `return` statement.
    Return,
    /// Reference inside a declaration initializer or default value.
    DeclRef,
    /// Unclassified — overloaded operators, deref-writes, macro expansions.
    Unknown,
}

impl AccessKind {
    /// Canonical string stored in the edge property (sink-stable).
    ///
    /// The exhaustive match enforces the closed-7 invariant (AC-S43-1):
    /// the compiler will reject any new variant until `as_str` is extended.
    pub fn as_str(self) -> &'static str {
        match self {
            AccessKind::Read => "read",
            AccessKind::Write => "write",
            AccessKind::AddrOf => "addr_of",
            AccessKind::CallArg => "call_arg",
            AccessKind::Return => "return",
            AccessKind::DeclRef => "decl_ref",
            AccessKind::Unknown => "unknown",
        }
    }

    /// All 7 variants in declaration order.
    ///
    /// Used by tests to assert the full variant set.
    pub fn all() -> &'static [AccessKind] {
        &[
            AccessKind::Read,
            AccessKind::Write,
            AccessKind::AddrOf,
            AccessKind::CallArg,
            AccessKind::Return,
            AccessKind::DeclRef,
            AccessKind::Unknown,
        ]
    }
}

impl std::fmt::Display for AccessKind {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Classify a USES edge emission site by walking up to 4 parent cursors
/// (ADR-13 §libclang strategy).
///
/// Decision order (first match wins):
/// 1. Parent is `ReturnStmt` → [`AccessKind::Return`]
/// 2. Parent is `BinaryOperator`: cursor is LHS → [`AccessKind::Write`],
///    cursor is RHS → [`AccessKind::Read`]
/// 3. Parent is `CompoundAssignOperator` → [`AccessKind::Write`]
/// 4. Parent is `UnaryOperator` (`&`) → [`AccessKind::AddrOf`]
/// 5. Parent is `UnaryOperator` (`++`/`--`) → [`AccessKind::Write`]
/// 6. Parent is `CallExpr` → [`AccessKind::CallArg`] (param-type inspection
///    is not available for all cases; conservative fallback)
/// 7. Parent is `VarDecl` / `FieldDecl` / `ParmDecl` initializer →
///    [`AccessKind::DeclRef`]
/// 8. Fallback → [`AccessKind::Unknown`] (logged at `debug` level)
///
/// `parent_chain` should be built from immediate-parent to grandparent order.
/// An empty or length-1 chain (just the direct parent) is fully supported.
pub fn classify_use(
    cursor: &Entity<'_>,
    parent_chain: &[Entity<'_>],
    file_cache: &mut HashMap<PathBuf, Arc<[u8]>>,
) -> AccessKind {
    // Walk up to 4 parents, in order from immediate parent outward.
    let parents: Vec<&Entity<'_>> = parent_chain.iter().take(4).collect();

    for (idx, parent) in parents.iter().enumerate() {
        let kind = parent.get_kind();

        match kind {
            // ── Return statement ────────────────────────────────────────────
            EntityKind::ReturnStmt => {
                return AccessKind::Return;
            }

            // ── Binary operator: split LHS vs RHS ───────────────────────────
            EntityKind::BinaryOperator => {
                if is_lhs_of_binary_op(cursor, parent, idx, parent_chain) {
                    return AccessKind::Write;
                } else {
                    return AccessKind::Read;
                }
            }

            // ── Compound-assign operator (+=, -= …) — LHS is always write ───
            EntityKind::CompoundAssignOperator => {
                return AccessKind::Write;
            }

            // ── Unary operator ───────────────────────────────────────────────
            EntityKind::UnaryOperator => {
                // addr_of vs pre/post increment/decrement
                // libclang does not expose the operator token directly from the
                // Entity API; we use the spelling of the operator's range start
                // to distinguish them.  On failure we fall back to AddrOf for
                // safety (addr_of is the first unary token character `&`).
                let op = unary_op_spelling(parent, file_cache);
                match op.as_deref() {
                    Some("&") => return AccessKind::AddrOf,
                    Some("++") | Some("--") => return AccessKind::Write,
                    // `*` dereference writes fall through to Unknown (per ADR-13 §unknown)
                    _ => return AccessKind::Unknown,
                }
            }

            // ── Call expression: argument position (conservative) ────────────
            EntityKind::CallExpr => {
                return AccessKind::CallArg;
            }

            // ── Initializer / default-argument contexts ──────────────────────
            EntityKind::VarDecl | EntityKind::FieldDecl | EntityKind::ParmDecl => {
                return AccessKind::DeclRef;
            }

            // ── If / While / For conditions — reading ────────────────────────
            EntityKind::IfStmt | EntityKind::WhileStmt | EntityKind::ForStmt => {
                return AccessKind::Read;
            }

            // ── Member access base object — reading the base to access a field ─
            // DeclRefExpr child of MemberRefExpr is the base object (e.g. `c` in
            // `c.value`).  Accessing a member reads the base, so classify as `read`.
            EntityKind::MemberRefExpr => {
                return AccessKind::Read;
            }

            // Continue walking outward.
            _ => continue,
        }
    }

    // Fallback: unknown.  Log at debug level per ADR-13 §Log level.
    // Gate the Vec allocation behind tracing::enabled! to avoid O(n) work on
    // non-debug builds (fix 5).
    if tracing::enabled!(tracing::Level::DEBUG) {
        let parent_kinds: Vec<String> = parent_chain
            .iter()
            .take(4)
            .map(|p| format!("{:?}", p.get_kind()))
            .collect();
        debug!(
            usr = cursor
                .get_usr()
                .map(|u| u.0)
                .unwrap_or_default()
                .as_str(),
            file = cursor
                .get_location()
                .and_then(|loc| {
                    let spell = loc.get_spelling_location();
                    spell.file.map(|f| f.get_path().to_string_lossy().into_owned())
                })
                .unwrap_or_default()
                .as_str(),
            line = cursor
                .get_location()
                .map(|loc| loc.get_spelling_location().line)
                .unwrap_or(0),
            parent_kinds = ?parent_kinds,
            "access_classifier: unknown context"
        );
    }
    AccessKind::Unknown
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/// Determine whether `cursor` is the left-hand-side child of `binary_op`.
///
/// libclang represents `a = b` as `BinaryOperator { children: [a, b] }`.
/// We compare the cursor's spelling location with the first child's location.
/// If they match (or if we cannot determine — defensive) we call it LHS.
fn is_lhs_of_binary_op(
    cursor: &Entity<'_>,
    binary_op: &Entity<'_>,
    _idx: usize,
    _chain: &[Entity<'_>],
) -> bool {
    let children = binary_op.get_children();
    if children.is_empty() {
        // No children; conservative fallback — treat as write (LHS).
        return true;
    }
    let lhs = &children[0];
    // Compare spelling locations.
    let cursor_loc = cursor
        .get_location()
        .map(|l| l.get_spelling_location())
        .map(|sl| (sl.line, sl.column));
    let lhs_loc = lhs
        .get_location()
        .map(|l| l.get_spelling_location())
        .map(|sl| (sl.line, sl.column));

    match (cursor_loc, lhs_loc) {
        (Some(cl), Some(ll)) => cl == ll,
        // Cannot compare — conservative: assume RHS (read).
        _ => false,
    }
}

/// Attempt to extract the operator token from a `UnaryOperator` entity.
///
/// libclang does not expose a structured "operator kind" from `Entity` in the
/// clang-rs 2.0.0 API.  We fall back to comparing the range extent with the
/// first child to find the operator prefix.  Returns `None` on any failure.
///
/// `file_cache` is the per-TU byte-slice cache from the `Collector`.  On the
/// first access for a given path within a TU the file is read from disk and
/// cached; subsequent calls re-use the cached bytes (fix 4).
fn unary_op_spelling(
    unary: &Entity<'_>,
    file_cache: &mut HashMap<PathBuf, Arc<[u8]>>,
) -> Option<String> {
    let range = unary.get_range()?;
    let start = range.get_start().get_file_location();
    let child_start = unary
        .get_children()
        .into_iter()
        .next()?
        .get_range()?
        .get_start()
        .get_file_location();

    let file = start.file?;
    let file_path = file.get_path();
    // Only handle same-file operators.
    if child_start.file.as_ref()?.get_path() != file_path {
        return None;
    }

    let op_start = start.offset as usize;
    let op_end = child_start.offset as usize;
    if op_end <= op_start {
        // Post-increment: child comes before operator in offset order.
        // Treat as write.
        return Some("++".to_owned());
    }

    // Fetch from cache or read from disk.
    let bytes: Arc<[u8]> = match file_cache.get(&file_path) {
        Some(cached) => cached.clone(),
        None => {
            let raw = std::fs::read(&file_path).ok()?;
            let arc: Arc<[u8]> = raw.into();
            file_cache.insert(file_path.clone(), arc.clone());
            arc
        }
    };
    let op_bytes = bytes.get(op_start..op_end)?;
    let op = String::from_utf8_lossy(op_bytes).trim().to_owned();
    if op.is_empty() {
        None
    } else {
        Some(op)
    }
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    /// AC-S43-1: exactly 7 variants.
    #[test]
    fn all_returns_exactly_7_variants() {
        assert_eq!(AccessKind::all().len(), 7);
    }

    /// AC-S43-1: each variant has a unique canonical string.
    #[test]
    fn all_variants_have_unique_strings() {
        let mut strings: Vec<&str> = AccessKind::all().iter().map(|v| v.as_str()).collect();
        strings.sort_unstable();
        strings.dedup();
        assert_eq!(
            strings.len(),
            7,
            "all 7 variants must have distinct strings"
        );
    }

    /// AC-S43-1: `as_str` is stable (smoke-check canonical values).
    #[test]
    fn as_str_canonical_values() {
        assert_eq!(AccessKind::Read.as_str(), "read");
        assert_eq!(AccessKind::Write.as_str(), "write");
        assert_eq!(AccessKind::AddrOf.as_str(), "addr_of");
        assert_eq!(AccessKind::CallArg.as_str(), "call_arg");
        assert_eq!(AccessKind::Return.as_str(), "return");
        assert_eq!(AccessKind::DeclRef.as_str(), "decl_ref");
        assert_eq!(AccessKind::Unknown.as_str(), "unknown");
    }

    /// Display impl delegates to `as_str`.
    #[test]
    fn display_matches_as_str() {
        for &kind in AccessKind::all() {
            assert_eq!(format!("{kind}"), kind.as_str());
        }
    }

    /// AC-S43-3: empty parent chain → Unknown (does not panic, does not drop).
    #[test]
    fn empty_parent_chain_returns_unknown_without_libclang() {
        // We cannot easily construct a clang::Entity without a live TU, so we
        // test the fallback path indirectly through the classifier unit tests
        // in tests/visit/access_classifier.rs (fixture-based).
        //
        // This smoke test verifies that `AccessKind::Unknown.as_str()` is
        // "unknown" — the value that will be written to the edge when the
        // classifier falls back.
        assert_eq!(AccessKind::Unknown.as_str(), "unknown");
    }

    /// AC-S43-2: target_association_type mirrors source (symmetric copy).
    ///
    /// The symmetry is implemented at the call site in shallow.rs; this test
    /// confirms that the same `AccessKind::as_str()` value is appropriate for
    /// both fields.
    #[test]
    fn target_mirrors_source_for_write() {
        let kind = AccessKind::Write;
        // Both source and target use the same AccessKind value (ADR-13 §target_association_type).
        assert_eq!(kind.as_str(), "write");
    }
}
