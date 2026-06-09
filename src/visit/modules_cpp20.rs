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

use std::sync::Arc;

use crate::{
    resolve::symbol_map::SymbolAllocator,
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
        warn!("C++20 modules: UNAVAILABLE — .cppm/.ixx TUs skipped (need libclang 18+ with module support)");
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
/// (`.cppm`, `.ixx`, `.mxx`) OR when `args` contain any PCM-related flag:
/// `-fmodules`, `-fmodule-file=…`, or `-fprebuilt-module-path=…`.
///
/// Detection is flag-presence only — no `-std=c++20` co-requirement (ADR-2).
/// Used by the pipeline to route TUs to this module.
pub fn is_module_tu(file: &Path, args: &[String]) -> bool {
    let ext = file
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or_default();
    if matches!(ext, "cppm" | "ixx" | "mxx") {
        return true;
    }
    args.iter().any(|a| {
        a == "-fmodules"
            || a.starts_with("-fmodule-file=")
            || a.starts_with("-fprebuilt-module-path")
    })
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
/// Check whether an explicit `-fmodule-file=[name=]path` argument references a
/// `.pcm` file that does not exist on disk.  Returns the missing path if so.
///
/// Parses both Clang forms:
/// - `name=path` (path is the substring after the second `=`)
/// - bare `path` (whole remainder, no `=` after the prefix)
fn check_explicit_pcm_files(args: &[String]) -> Option<std::path::PathBuf> {
    for arg in args {
        if let Some(rest) = arg.strip_prefix("-fmodule-file=") {
            // rest is either "Name=/path/to/Foo.pcm" or "/path/to/Foo.pcm"
            let pcm_path = if let Some((_name, path)) = rest.split_once('=') {
                std::path::Path::new(path)
            } else {
                std::path::Path::new(rest)
            };
            if !pcm_path.exists() {
                return Some(pcm_path.to_path_buf());
            }
        }
    }
    None
}

pub fn parse_module_tu(
    index: &Index<'_>,
    file: &Path,
    args: &[String],
    repo_name: &str,
    tu_hash: [u8; 32],
    writer: &mut StageWriter,
    allocator: Option<Arc<SymbolAllocator>>,
) -> Result<bool> {
    // S3-AC1: pre-parse stat gate — detect missing explicit .pcm files before any write.
    if let Some(missing) = check_explicit_pcm_files(args) {
        tracing::error!(
            pcm = %missing.display(),
            tu = %file.display(),
            "PCM load failure: explicit .pcm file is missing — no graph nodes emitted for this TU"
        );
        return Err(Error::Clang(format!(
            "missing PCM file '{}' referenced by TU '{}'",
            missing.display(),
            file.display()
        )));
    }

    let parse_result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let mut full_args: Vec<String> = args.to_vec();
        // For module-interface extensions (.cppm/.ixx/.mxx) force c++20 semantics (ADR-1 §3).
        // For PCM-consuming .cpp TUs, preserve the TU's original -std to avoid semantics change.
        let ext = file
            .extension()
            .and_then(|e| e.to_str())
            .unwrap_or_default();
        let is_interface_ext = matches!(ext, "cppm" | "ixx" | "mxx");
        if is_interface_ext && !full_args.iter().any(|a| a == "-std=c++20") {
            full_args.push("-std=c++20".to_owned());
        }
        if is_interface_ext && !full_args.iter().any(|a| a == "-fmodules") {
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

    // S3-AC2: post-parse Fatal gate — Fatal diagnostics indicate corrupt/invalid .pcm.
    // Suppress all writes and return Err so the TU counts as tu_error (ADR-3 §B).
    let has_fatal = tu.get_diagnostics().iter().any(|d| {
        use clang::diagnostic::Severity;
        matches!(d.get_severity(), Severity::Fatal)
    });
    if has_fatal {
        tracing::error!(
            tu = %file.display(),
            "PCM load failure: Fatal diagnostic from libclang — corrupt or invalid .pcm; \
             no graph nodes emitted for this TU"
        );
        return Err(Error::Clang(format!(
            "Fatal libclang diagnostic while parsing module TU '{}'",
            file.display()
        )));
    }

    let has_errors = tu.get_diagnostics().iter().any(|d| {
        use clang::diagnostic::Severity;
        matches!(d.get_severity(), Severity::Error)
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
        file_path: file.to_string_lossy().into_owned(),
        repo_name: repo_name.to_owned(),
        attrs_json: module_attrs,
        partial: has_errors,
        tu_hash,
        ..Default::default()
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
                repo_name: repo_name.to_owned(),
                attrs_json: serde_json::json!({ "module_interface": true }).to_string(),
                tu_hash,
                ..Default::default()
            };
            nodes.push(imported_node);

            // INCLUDES edge (reusing existing kind per ADR-8 scope limit).
            edges.push(EdgeRecord {
                src_usr: module_usr.clone(),
                dst_usr: Some(imported_usr),
                kind: EdgeKind::Includes,
                repo_name: repo_name.to_owned(),
                attrs_json: serde_json::json!({ "import": true }).to_string(),
                tu_hash,
                dst_repo_name: repo_name.to_owned(),
                ..Default::default()
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
                partial: has_errors,
                tu_hash,
                ..Default::default()
            });

            // CONTAINS edge from module to exported decl.
            edges.push(EdgeRecord {
                src_usr: module_usr.clone(),
                dst_usr: Some(usr),
                kind: EdgeKind::Contains,
                repo_name: repo_name.to_owned(),
                tu_hash,
                dst_repo_name: repo_name.to_owned(),
                ..Default::default()
            });
        }

        EntityVisitResult::Recurse
    });

    // graph-symbol-ids post-processing pass (Story 3, v6): populate integer IDs.
    if let Some(ref alloc) = allocator {
        for node in &mut nodes {
            node.symbol_id = alloc.get_or_insert_symbol(&node.usr).unwrap_or(0);
            node.file_id = alloc.get_or_insert_file(&node.file_path).unwrap_or(0);
        }
        for edge in &mut edges {
            edge.src_id = alloc.get_or_insert_symbol(&edge.src_usr).unwrap_or(0);
            edge.dst_id = if let Some(ref dst) = edge.dst_usr {
                alloc.get_or_insert_symbol(dst).ok()
            } else {
                None
            };
        }
    }

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

    /// `is_module_tu` detects via compile args — no `-std=c++20` co-requirement (ADR-2).
    #[test]
    fn is_module_tu_by_args() {
        // -fmodules alone → true (S1 implied; ADR-2)
        let args_fmodules = vec!["-fmodules".to_owned()];
        assert!(is_module_tu(Path::new("bar.cpp"), &args_fmodules));

        // -fmodule-file= alone → true (S1-AC2)
        let args_modfile = vec!["-fmodule-file=Foo=/build/Foo.pcm".to_owned()];
        assert!(is_module_tu(Path::new("bar.cpp"), &args_modfile));

        // -fprebuilt-module-path= alone → true (S1-AC1)
        let args_prebuilt = vec!["-fprebuilt-module-path=/build/modules".to_owned()];
        assert!(is_module_tu(Path::new("bar.cpp"), &args_prebuilt));

        // Both PCM flags present → still true, not double-counted (edge case)
        let args_both = vec![
            "-fmodule-file=Foo=/build/Foo.pcm".to_owned(),
            "-fprebuilt-module-path=/build/modules".to_owned(),
        ];
        assert!(is_module_tu(Path::new("bar.cpp"), &args_both));

        // No PCM flags → false (S1-AC3)
        let args_no_pcm: Vec<String> = vec!["-std=c++17".to_owned()];
        assert!(!is_module_tu(Path::new("bar.cpp"), &args_no_pcm));

        // No flags at all → false
        assert!(!is_module_tu(Path::new("bar.cpp"), &[]));
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

    /// S3-AC1: missing explicit .pcm file → Err, no write (hermetic, no libclang needed).
    #[test]
    fn missing_pcm_stat_check_returns_err() {
        let missing =
            check_explicit_pcm_files(&["-fmodule-file=Foo=/nonexistent/path/Foo.pcm".to_owned()]);
        assert!(
            missing.is_some(),
            "check_explicit_pcm_files must detect a missing .pcm"
        );
        assert!(
            missing.unwrap().ends_with("Foo.pcm"),
            "returned path must be the .pcm file"
        );
    }

    /// S3: present .pcm file → no error from stat check.
    #[test]
    fn present_pcm_stat_check_returns_none() {
        // Use a file that is guaranteed to exist.
        let present = check_explicit_pcm_files(&[format!(
            "-fmodule-file=Foo={}",
            std::env::current_exe()
                .expect("current exe")
                .to_str()
                .expect("utf8")
        )]);
        assert!(
            present.is_none(),
            "check_explicit_pcm_files must not flag a present file"
        );
    }

    /// S3: bare form (no name= prefix) is also stat-checked.
    #[test]
    fn missing_pcm_bare_form_detected() {
        let missing =
            check_explicit_pcm_files(&["-fmodule-file=/nonexistent/path/Bar.pcm".to_owned()]);
        assert!(missing.is_some(), "bare form must also be detected");
    }

    /// Empirical test: what severity does libclang report for a corrupt .pcm?
    /// Used to validate the S3-AC2 Fatal-severity assumption (ADR-3 §A).
    /// This test prints findings and always passes; it is `#[ignore]`d in CI.
    #[test]
    #[ignore = "diagnostic-severity probe: run manually on libclang 18+ to validate ADR-3 Fatal assumption"]
    fn corrupt_pcm_fatal_severity_probe() {
        use clang::diagnostic::Severity;

        let corrupt_pcm = std::env::temp_dir().join("corrupt_severity_probe.pcm");
        std::fs::write(&corrupt_pcm, b"garbage not valid PCM content at all").unwrap();
        let consumer = std::env::temp_dir().join("severity_consumer.cpp");
        std::fs::write(&consumer, b"import Foo;\nint main() { return 0; }\n").unwrap();

        with_thread_index(|index| {
            let args = vec![
                "-std=c++20".to_string(),
                "-fmodules".to_string(),
                format!("-fmodule-file=Foo={}", corrupt_pcm.display()),
            ];
            match index.parser(&consumer).arguments(&args).parse() {
                Ok(tu) => {
                    let diags = tu.get_diagnostics();
                    eprintln!("corrupt-pcm diagnostic count: {}", diags.len());
                    let mut has_fatal = false;
                    let mut has_error = false;
                    for d in &diags {
                        let sev = d.get_severity();
                        eprintln!("  severity={sev:?} msg={}", d.get_text());
                        match sev {
                            Severity::Fatal => has_fatal = true,
                            Severity::Error => has_error = true,
                            _ => {}
                        }
                    }
                    eprintln!(
                        "RESULT: has_fatal={has_fatal} has_error={has_error} -- \
                         S3-AC2 Fatal gate is {} for corrupt .pcm",
                        if has_fatal {
                            "SUFFICIENT"
                        } else {
                            "NOT sufficient (Error only)"
                        }
                    );
                }
                Err(e) => {
                    eprintln!("Parse API failed (libclang returned error directly): {e:?}");
                }
            }
        });
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
                None, // no allocator in test
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
