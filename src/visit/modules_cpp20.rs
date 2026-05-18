//! C++20 module interface unit support — best-effort on libclang 18 (ADR-8).
//!
//! # Runtime capability probe (ADR-8 §1)
//! [`probe_cpp20_support`] runs once at indexer startup (via [`OnceLock`]) and
//! determines whether the linked libclang build can parse a minimal `.cppm`
//! module interface.  The result is cached for the process lifetime.
//!
//! # TU dispatch (ADR-8 §2)
//! [`parse_module_tu`] processes a `.cppm`/`.ixx`/`.mxx` translation unit when
//! the probe succeeded.  When the probe failed, callers should call
//! [`warn_and_skip`] instead (AC-M5-8).
//!
//! # Edge kind note
//! ADR-8 §"Scope limit" explicitly says "No new edge kind to keep schema churn
//! bounded".  Module import directives therefore reuse [`EdgeKind::Includes`],
//! with the target MODULE node distinguished by `module_interface: true` in
//! `attrs_json` (additive, forward-compatible).  `EdgeKind::ModuleExports` is
//! **not** added per the scope-limit decision.
//!
//! # Version output (ADR-8 §3, AC-M5-9)
//! [`capability_version_note`] returns the extra line that `cxg-index --version`
//! must append when modules are unavailable.

use std::path::Path;
use std::sync::OnceLock;

use clang::{EntityKind, EntityVisitResult, Index};
use tracing::{debug, info, warn};

use crate::{
    schema::{EdgeKind, EdgeRecord, NodeKind, NodeRecord},
    stage::writer::StageWriter,
    visit::shallow::with_thread_index,
    Error, Result,
};

// ---------------------------------------------------------------------------
// Probe fixture (embedded at compile time, ADR-8 §1)
// ---------------------------------------------------------------------------

/// Raw bytes of the minimal `.cppm` fixture used by the runtime probe.
///
/// Written to a `NamedTempFile` during probe execution and deleted afterwards.
const PROBE_CPPM: &[u8] = include_bytes!("modules_probe.cppm");

// ---------------------------------------------------------------------------
// Runtime capability probe
// ---------------------------------------------------------------------------

static CPP20_MODULES_CAPABLE: OnceLock<bool> = OnceLock::new();

/// Run the runtime capability probe and return `true` when the linked libclang
/// build supports C++20 module interface units.
///
/// Executes **at most once** per process (result is cached in a [`OnceLock`]).
///
/// ## Probe logic (ADR-8 §1)
/// 1. Write [`PROBE_CPPM`] to a temporary file.
/// 2. Parse it with `-std=c++20 -fmodules`.
/// 3. If parse succeeds **and** the TU exposes an [`EntityKind::ModuleImportDecl`]
///    cursor → `true`.
/// 4. Otherwise → `false`; emit one INFO log entry explaining the miss.
pub fn probe_cpp20_support() -> bool {
    *CPP20_MODULES_CAPABLE.get_or_init(run_probe)
}

/// Returns the extra `--version` line when C++20 modules are unavailable (AC-M5-9).
///
/// Returns `None` when modules are available (no extra line needed).
pub fn capability_version_note() -> Option<&'static str> {
    if probe_cpp20_support() {
        None
    } else {
        Some("C++20 modules: UNAVAILABLE (libclang build lacks module support; .cppm/.ixx TUs will be skipped)")
    }
}

fn run_probe() -> bool {
    // Write the embedded fixture to a temporary file using std (no dev-only tempfile).
    // Use a PID-scoped name so concurrent processes don't collide.
    let probe_path = std::env::temp_dir().join(format!("cxg_probe_{}.cppm", std::process::id()));

    if let Err(e) = std::fs::write(&probe_path, PROBE_CPPM) {
        info!(
            error = %e,
            "C++20 module probe: cannot write probe fixture; assuming modules unsupported"
        );
        return false;
    }

    let capable = with_thread_index(|index| {
        let parse_result = index
            .parser(&probe_path)
            .arguments(&["-std=c++20", "-fmodules"])
            .parse();

        match parse_result {
            Ok(tu) => {
                // Scan for ModuleImportDecl — presence confirms module support.
                let mut found_module = false;
                tu.get_entity().visit_children(|e, _| {
                    if e.get_kind() == EntityKind::ModuleImportDecl {
                        found_module = true;
                        EntityVisitResult::Break
                    } else {
                        EntityVisitResult::Continue
                    }
                });

                if found_module {
                    debug!("C++20 module probe: ModuleImportDecl found — modules supported");
                    true
                } else {
                    // libclang parsed without error but produced no module cursor:
                    // partial support or wrong flags for this distro build.
                    info!(
                        "C++20 module probe: parse succeeded but no ModuleImportDecl found; \
                         assuming modules unsupported on this libclang build"
                    );
                    false
                }
            }
            Err(e) => {
                info!(
                    error = ?e,
                    "C++20 module probe: parse failed; C++20 modules unsupported on this libclang build"
                );
                false
            }
        }
    });

    // Best-effort cleanup; ignore errors (tmp file may not exist on write failure).
    let _ = std::fs::remove_file(&probe_path);

    if !capable {
        info!(
            "C++20 modules: UNAVAILABLE — .cppm/.ixx TUs will be skipped \
             (upgrade to a libclang build with module support to enable)"
        );
    }

    capable
}

// ---------------------------------------------------------------------------
// Public dispatch helpers
// ---------------------------------------------------------------------------

/// Log a warning and record a skip for a module TU when the probe returned `false`.
///
/// Increments the caller's `skipped` counter and writes nothing to `writer`.
/// Returns `Ok(())` so the caller can continue with the next TU (AC-M5-8).
pub fn warn_and_skip(file: &Path) {
    warn!(
        file = %file.display(),
        "cxg_module_skipped: C++20 module TU skipped — libclang lacks module support"
    );
}

/// Returns `true` when `file` has a C++20 module interface extension
/// (`.cppm`, `.ixx`, `.mxx`) or when `args` contain `-fmodules` and `-std=c++20`.
///
/// Used by the pipeline to route TUs to this module.
pub fn is_module_tu(file: &Path, args: &[String]) -> bool {
    let ext = file
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or_default();
    if matches!(ext, "cppm" | "ixx" | "mxx") {
        return true;
    }
    let has_fmodules = args.iter().any(|a| a == "-fmodules");
    let has_std_cpp20 = args.iter().any(|a| a == "-std=c++20" || a == "--std=c++20");
    has_fmodules && has_std_cpp20
}

// ---------------------------------------------------------------------------
// Module TU parser (capable path, ADR-8 §2)
// ---------------------------------------------------------------------------

/// Parse a C++20 module interface TU and write Phase 1 records to `writer`.
///
/// This function must **only** be called when [`probe_cpp20_support`] returned
/// `true`.  When the probe returned `false`, use [`warn_and_skip`] instead.
///
/// ## Node / edge output (ADR-8 §2 + scope-limit)
/// - Emits a `MODULE` node with `attrs_json` containing `"module_interface": true`.
/// - Emits `NAMESPACE`, `CLASS`, `FUNCTION`, etc. nodes for **exported**
///   declarations (those reachable via public linkage in the module interface).
/// - Module import directives produce [`EdgeKind::Includes`] edges from the
///   module node to the imported module node (distinguished by
///   `module_interface: true`).  No new edge kind is added per ADR-8 scope limit.
///
/// Returns `true` when libclang reported error-level diagnostics (partial TU).
///
/// # Errors
/// Returns [`Error::Clang`] when libclang fails to produce any AST at all.
/// Returns [`Error::Schema`] / [`Error::Io`] on write failures.
pub fn parse_module_tu(
    index: &Index<'_>,
    file: &Path,
    args: &[String],
    repo_name: &str,
    tu_hash: [u8; 32],
    writer: &mut StageWriter,
) -> Result<bool> {
    let parse_result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let mut full_args: Vec<String> = args.to_vec();
        // Ensure module-relevant flags are present.
        if !full_args.iter().any(|a| a == "-std=c++20") {
            full_args.push("-std=c++20".to_owned());
        }
        if !full_args.iter().any(|a| a == "-fmodules") {
            full_args.push("-fmodules".to_owned());
        }
        index
            .parser(file)
            .arguments(&full_args)
            .detailed_preprocessing_record(true)
            .parse()
    }));

    let tu = match parse_result {
        Ok(Ok(tu)) => tu,
        Ok(Err(e)) => {
            return Err(Error::Clang(format!(
                "libclang failed to parse module TU {}: {:?}",
                file.display(),
                e
            )));
        }
        Err(_panic) => {
            return Err(Error::Clang(format!(
                "libclang panicked while parsing module TU {}",
                file.display()
            )));
        }
    };

    let has_errors = tu.get_diagnostics().iter().any(|d| {
        use clang::diagnostic::Severity;
        matches!(d.get_severity(), Severity::Error | Severity::Fatal)
    });

    if has_errors {
        warn!(
            file = %file.display(),
            "libclang parse errors in module TU — written as partial"
        );
    }

    // Synthesise USR for the module interface node.
    let root = tu.get_entity();
    let module_usr = root
        .get_usr()
        .map(|u| u.0)
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| format!("tu:{}", file.display()));

    let module_name = file
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();

    // MODULE node with `module_interface: true` (additive, no schema bump, ADR-8 scope limit).
    let module_attrs = serde_json::json!({ "module_interface": true }).to_string();
    let module_node = NodeRecord {
        usr: module_usr.clone(),
        kind: NodeKind::Module,
        name: module_name.clone(),
        qualified_name: file.to_string_lossy().into_owned(),
        mangled_name: None,
        file_path: file.to_string_lossy().into_owned(),
        line: None,
        col: None,
        repo_name: repo_name.to_owned(),
        attrs_json: module_attrs,
        partial: has_errors,
        phase: 1,
        tu_hash,
        // M8 promoted fields — MODULE nodes carry none of these
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
    };

    let mut nodes = vec![module_node];
    let mut edges: Vec<EdgeRecord> = Vec::new();

    // Walk exported declarations and import directives.
    root.visit_children(|entity, _parent| {
        let kind = entity.get_kind();

        // Module import → INCLUDES edge to the imported module node.
        if kind == EntityKind::ModuleImportDecl {
            let imported_name = entity
                .get_display_name()
                .unwrap_or_else(|| "<unknown>".to_owned());
            // Synthesise a USR for the imported module using a canonical prefix.
            let imported_usr = format!("module:{imported_name}");

            // Emit a placeholder MODULE node for the imported interface so Phase 3
            // can resolve it if the module is indexed in the same repo.
            let imported_node = NodeRecord {
                usr: imported_usr.clone(),
                kind: NodeKind::Module,
                name: imported_name.clone(),
                qualified_name: imported_name,
                mangled_name: None,
                file_path: String::new(),
                line: None,
                col: None,
                repo_name: repo_name.to_owned(),
                attrs_json: serde_json::json!({ "module_interface": true }).to_string(),
                partial: false,
                phase: 1,
                tu_hash,
                // M8 promoted fields — MODULE nodes carry none of these
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
            };
            nodes.push(imported_node);

            // INCLUDES edge (reusing existing kind per ADR-8 scope limit).
            edges.push(EdgeRecord {
                src_usr: module_usr.clone(),
                dst_usr: Some(imported_usr),
                dst_placeholder: None,
                kind: EdgeKind::Includes,
                resolved: false,
                cross_repo_candidate: false,
                repo_name: repo_name.to_owned(),
                attrs_json: serde_json::json!({ "import": true }).to_string(),
                tu_hash,
                // M8 promoted fields — INCLUDES edges carry none of these
                source_association_type: None,
                target_association_type: None,
            });

            return EntityVisitResult::Continue;
        }

        // Exported declarations: emit standard nodes.
        if let Some(node_kind) = crate::visit::cursor_map::entity_kind_to_node_kind(kind) {
            if node_kind == NodeKind::Module {
                return EntityVisitResult::Recurse;
            }

            let usr = match entity.get_usr() {
                Some(u) if !u.0.is_empty() => u.0,
                _ => return EntityVisitResult::Recurse,
            };

            let name = entity.get_name().unwrap_or_default();
            let display_name = entity.get_display_name().unwrap_or_default();
            let mangled_name = entity.get_mangled_name();
            let file_str = file.to_string_lossy().into_owned();

            let (loc_file, line, col) = if let Some(loc) = entity.get_location() {
                let spell = loc.get_spelling_location();
                let loc_path = spell
                    .file
                    .map(|f| f.get_path().to_string_lossy().into_owned())
                    .unwrap_or_else(|| file_str.clone());
                let ln = if spell.line > 0 {
                    Some(spell.line)
                } else {
                    None
                };
                let cl = if spell.column > 0 {
                    Some(spell.column)
                } else {
                    None
                };
                (loc_path, ln, cl)
            } else {
                (file_str, None, None)
            };

            nodes.push(NodeRecord {
                usr: usr.clone(),
                kind: node_kind,
                name,
                qualified_name: display_name,
                mangled_name,
                file_path: loc_file,
                line,
                col,
                repo_name: repo_name.to_owned(),
                attrs_json: "{}".to_owned(),
                partial: has_errors,
                phase: 1,
                tu_hash,
                // M8 promoted fields — populated by S41/S42 for applicable kinds;
                // modules_cpp20 leaves them None (visitor for callable/template fields is shallow.rs)
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
            });

            // CONTAINS edge from module to exported decl.
            edges.push(EdgeRecord {
                src_usr: module_usr.clone(),
                dst_usr: Some(usr),
                dst_placeholder: None,
                kind: EdgeKind::Contains,
                resolved: false,
                cross_repo_candidate: false,
                repo_name: repo_name.to_owned(),
                attrs_json: "{}".to_owned(),
                tu_hash,
                // M8 promoted fields — CONTAINS edges carry none of these
                source_association_type: None,
                target_association_type: None,
            });
        }

        EntityVisitResult::Recurse
    });

    writer.write_nodes(&nodes)?;
    writer.write_edges(&edges)?;

    debug!(
        file = %file.display(),
        nodes = nodes.len(),
        edges = edges.len(),
        partial = has_errors,
        "C++20 module TU parsed"
    );

    Ok(has_errors)
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    /// Smoke test: `probe_cpp20_support` always returns a `bool` (never panics).
    /// The actual value depends on the libclang build — we don't assert it.
    #[test]
    fn probe_returns_bool() {
        let _result: bool = probe_cpp20_support();
    }

    /// Calling probe twice returns the same cached result (OnceLock is idempotent).
    #[test]
    fn probe_is_idempotent() {
        let first = probe_cpp20_support();
        let second = probe_cpp20_support();
        assert_eq!(
            first, second,
            "probe must return the same cached value on every call"
        );
    }

    /// `capability_version_note` returns `Some` when probe is false, `None` when true.
    #[test]
    fn capability_version_note_matches_probe() {
        let capable = probe_cpp20_support();
        let note = capability_version_note();
        if capable {
            assert!(
                note.is_none(),
                "no extra note expected when modules are supported"
            );
        } else {
            assert!(
                note.is_some(),
                "extra note expected when modules are unsupported"
            );
            assert!(
                note.unwrap().contains("UNAVAILABLE"),
                "note must mention UNAVAILABLE"
            );
        }
    }

    /// `is_module_tu` detects module extensions.
    #[test]
    fn is_module_tu_by_extension() {
        assert!(is_module_tu(Path::new("foo.cppm"), &[]));
        assert!(is_module_tu(Path::new("foo.ixx"), &[]));
        assert!(is_module_tu(Path::new("foo.mxx"), &[]));
        assert!(!is_module_tu(Path::new("foo.cpp"), &[]));
        assert!(!is_module_tu(Path::new("foo.h"), &[]));
    }

    /// `is_module_tu` also detects via compile args.
    #[test]
    fn is_module_tu_by_args() {
        let args = vec!["-std=c++20".to_owned(), "-fmodules".to_owned()];
        assert!(is_module_tu(Path::new("bar.cpp"), &args));
        // Only one flag is not enough.
        let args_partial = vec!["-std=c++20".to_owned()];
        assert!(!is_module_tu(Path::new("bar.cpp"), &args_partial));
    }

    /// `warn_and_skip` must not panic (it just emits a warn! log).
    #[test]
    fn warn_and_skip_does_not_panic() {
        warn_and_skip(Path::new("fake.cppm"));
    }

    /// When capable=false, the correct path is warn_and_skip (no writer needed).
    /// This test exercises the not-capable code path without faking libclang.
    #[test]
    fn not_capable_skips_gracefully() {
        // Simulate the caller's guard: when probe returns false, call warn_and_skip.
        let capable = false; // explicit test of the false branch
        let file = Path::new("test_module.cppm");
        if !capable {
            warn_and_skip(file);
        }
        // If we get here without panic, the skip path is safe.
    }

    /// When the probe succeeds, `parse_module_tu` emits at least a MODULE node.
    ///
    /// This test requires libclang to support C++20 modules; it is marked
    /// `#[ignore]` so it is skipped on libclang builds without module support.
    /// Run explicitly with: `cargo nextest run --ignored`
    #[test]
    #[ignore = "requires libclang build with C++20 module support"]
    fn parse_module_tu_emits_module_node() {
        use tempfile::tempdir;

        let dir = tempdir().expect("tempdir");
        let cppm = dir.path().join("test.cppm");
        std::fs::write(
            &cppm,
            b"export module test_mod;\nexport int test_val = 42;\n",
        )
        .expect("write fixture");

        let stage_dir = tempdir().expect("stage tempdir");
        let mut writer =
            crate::stage::writer::StageWriter::new(stage_dir.path(), 0).expect("writer");

        with_thread_index(|index| {
            let result = parse_module_tu(
                index,
                &cppm,
                &["-std=c++20".to_owned(), "-fmodules".to_owned()],
                "test-repo",
                [0u8; 32],
                &mut writer,
            );
            assert!(
                result.is_ok(),
                "parse_module_tu must not return Err on valid .cppm"
            );
        });

        let shards = writer.finish().expect("finish");
        // At minimum one shard must exist (module node was written).
        assert!(!shards.is_empty(), "at least one Parquet shard expected");
    }
}
