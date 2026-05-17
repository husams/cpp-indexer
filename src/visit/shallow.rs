//! Phase 1 — Shallow libclang visitor → Parquet (single-threaded, AC-M1-14..17).
//!
//! `visit_tu` parses one translation unit, walks its AST recursively, and writes
//! every recognised declaration as a [`NodeRecord`] / [`EdgeRecord`] to the
//! provided [`StageWriter`].  No graph-database I/O occurs here (AC-M1-15).
//!
//! ## Error isolation (AC-M1-16)
//! libclang parse errors do NOT abort the run: the TU is marked `partial = true`
//! and a diagnostic is logged, but the visitor continues with whatever AST
//! libclang managed to produce.
//!
//! ## Single-threaded for M1
//! Parallelism via `rayon` is deferred to S17 (AC-M3-1..3 / ADR-7).  This module
//! exposes a plain synchronous `visit_tu` function.

use std::panic::{self, AssertUnwindSafe};
use std::path::Path;

use clang::{Clang, EntityKind, EntityVisitResult, Index};
use tracing::{debug, warn};

use crate::{
    schema::{EdgeKind, EdgeRecord, NodeKind, NodeRecord},
    stage::writer::StageWriter,
    visit::cursor_map::entity_kind_to_node_kind,
    Error, Result,
};

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

/// Options for [`visit_tu`].
pub struct VisitOptions<'a> {
    /// Repository name (written into every node/edge record).
    pub repo_name: &'a str,
    /// Blake3 hash of `(source_bytes || args_string)` for this TU.
    pub tu_hash: [u8; 32],
    /// Path to the source file being visited (used as the `file_path` for the
    /// Module node that represents this TU).
    pub file_path: &'a Path,
    /// Compiler arguments (passed to libclang).
    pub args: &'a [String],
    /// When `true`, entities whose spelling location is inside a system header
    /// (any path under an `-isystem` directory, `/usr/include/`, or
    /// compiler-internal headers) are silently skipped (AC-M2-14).
    /// When `false`, system-header entities are emitted like any other (AC-M2-15).
    pub skip_system_headers: bool,
}

/// Parse one translation unit and write Phase 1 records to `writer`.
///
/// Returns `true` when libclang reported at least one error-severity diagnostic
/// (the TU was parsed `partial`), `false` otherwise.
///
/// # Errors
/// Returns [`Error::Clang`] only when libclang itself fails to produce any AST
/// (e.g. the file does not exist).  Soft parse errors (recoverable diagnostics)
/// are recorded via `partial = true` but do **not** produce an `Err`.
///
/// Returns [`Error::Schema`] / [`Error::Io`] when writing to the Parquet shard
/// fails.
pub fn visit_tu(clang: &Clang, opts: &VisitOptions<'_>, writer: &mut StageWriter) -> Result<bool> {
    let index = Index::new(
        clang, /* exclude_pch */ true, /* diagnostics */ false,
    );

    // Parse the TU; wrap in catch_unwind to isolate libclang panics (AC-M1-16).
    let tu_result = panic::catch_unwind(AssertUnwindSafe(|| {
        index.parser(opts.file_path).arguments(opts.args).parse()
    }));

    let tu = match tu_result {
        Ok(Ok(tu)) => tu,
        Ok(Err(e)) => {
            return Err(Error::Clang(format!(
                "libclang failed to parse {}: {:?}",
                opts.file_path.display(),
                e
            )));
        }
        Err(_panic) => {
            return Err(Error::Clang(format!(
                "libclang panicked while parsing {}",
                opts.file_path.display()
            )));
        }
    };

    // Determine if this TU has error-level diagnostics → partial flag.
    let has_errors = tu.get_diagnostics().iter().any(|d| {
        use clang::diagnostic::Severity;
        matches!(d.get_severity(), Severity::Error | Severity::Fatal)
    });

    if has_errors {
        warn!(
            file = %opts.file_path.display(),
            "libclang parse errors — TU will be written as partial"
        );
    }

    // Collect nodes and edges.
    let mut collector = Collector::new(
        opts.repo_name,
        opts.tu_hash,
        has_errors,
        opts.file_path.to_string_lossy().into_owned(),
        opts.skip_system_headers,
    );

    let root = tu.get_entity();

    // Emit the TU itself as a Module node.
    // The TU root entity does not always have a USR from libclang; we synthesise
    // one from the canonical file path (stable, unique per TU).
    let module_usr = root
        .get_usr()
        .map(|u| u.0)
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| format!("tu:{}", opts.file_path.display()));

    let module_node = NodeRecord {
        usr: module_usr.clone(),
        kind: NodeKind::Module,
        name: opts
            .file_path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default(),
        qualified_name: opts.file_path.to_string_lossy().into_owned(),
        mangled_name: None,
        file_path: opts.file_path.to_string_lossy().into_owned(),
        line: None,
        col: None,
        repo_name: opts.repo_name.to_owned(),
        attrs_json: "{}".to_owned(),
        partial: has_errors,
        phase: 1,
        tu_hash: opts.tu_hash,
    };
    collector.nodes.push(module_node);
    collector.module_usr = Some(module_usr);

    // Walk child declarations.
    root.visit_children(|entity, parent| {
        collector.visit(entity, parent);
        EntityVisitResult::Recurse
    });

    // Write collected records.
    writer.write_nodes(&collector.nodes)?;
    writer.write_edges(&collector.edges)?;

    debug!(
        file = %opts.file_path.display(),
        nodes = collector.nodes.len(),
        edges = collector.edges.len(),
        partial = has_errors,
        "Phase 1 visitor complete"
    );

    Ok(has_errors)
}

// ---------------------------------------------------------------------------
// Internal collector
// ---------------------------------------------------------------------------

struct Collector<'a> {
    repo_name: &'a str,
    tu_hash: [u8; 32],
    partial: bool,
    nodes: Vec<NodeRecord>,
    edges: Vec<EdgeRecord>,
    tu_file_path: String,
    /// USR of the Module node for this TU (used as `src_usr` for CONTAINS edges).
    /// Set after the Module node is pushed in `visit_tu`, before `visit_children`.
    module_usr: Option<String>,
    /// Mirror of `VisitOptions::skip_system_headers`.
    skip_system_headers: bool,
}

impl<'a> Collector<'a> {
    fn new(
        repo_name: &'a str,
        tu_hash: [u8; 32],
        partial: bool,
        tu_file_path: String,
        skip_system_headers: bool,
    ) -> Self {
        Self {
            repo_name,
            tu_hash,
            partial,
            nodes: Vec::new(),
            edges: Vec::new(),
            tu_file_path,
            module_usr: None,
            skip_system_headers,
        }
    }
}

impl<'a> Collector<'a> {
    fn visit(&mut self, entity: clang::Entity<'_>, parent: clang::Entity<'_>) {
        // Only track definitions (skip forward declarations to avoid duplicates).
        // VarDecl at file scope may not have is_definition() = true in all libclang
        // versions, so we include all VarDecl here and rely on USR deduplication.
        let kind = entity.get_kind();
        let node_kind = match entity_kind_to_node_kind(kind) {
            Some(k) if k != NodeKind::Module => k,
            _ => return,
        };

        // Skip non-definitions except for the kinds that don't have definitions.
        if !entity.is_definition() {
            // Fields and global variables: always emit (definitions can be non-existent as decl)
            if !matches!(node_kind, NodeKind::Field | NodeKind::GlobalVariable) {
                return;
            }
        }

        // USR is the primary key; skip entities without one.
        let usr = match entity.get_usr() {
            Some(u) if !u.0.is_empty() => u.0,
            _ => return,
        };

        // System-header filter (AC-M2-14 / AC-M2-15).
        // `entity.is_in_system_header()` returns true for any entity whose
        // spelling location falls inside a header passed via `-isystem`,
        // `/usr/include/`, or a compiler-built-in path.
        if self.skip_system_headers && entity.is_in_system_header() {
            return;
        }

        let name = entity.get_name().unwrap_or_default();
        let display_name = entity.get_display_name().unwrap_or_default();
        let mangled_name = entity.get_mangled_name();

        // Source location.
        let (file_path, line, col) = extract_location(&entity, &self.tu_file_path);

        // Per-kind attrs JSON.
        let attrs_json = build_attrs_json(&entity, node_kind);

        let node = NodeRecord {
            usr: usr.clone(),
            kind: node_kind,
            name,
            qualified_name: display_name,
            mangled_name,
            file_path,
            line,
            col,
            repo_name: self.repo_name.to_owned(),
            attrs_json,
            partial: self.partial,
            phase: 1,
            tu_hash: self.tu_hash,
        };

        self.nodes.push(node);

        // Emit structural edges from parent.
        self.emit_structural_edges(&usr, node_kind, &entity, &parent);
    }

    /// Emit CONTAINS, HAS_METHOD, HAS_FIELD, INHERITS edges.
    fn emit_structural_edges(
        &mut self,
        child_usr: &str,
        child_kind: NodeKind,
        entity: &clang::Entity<'_>,
        parent: &clang::Entity<'_>,
    ) {
        let parent_kind_raw = parent.get_kind();
        let parent_usr = match parent.get_usr() {
            Some(u) if !u.0.is_empty() => u.0,
            _ => {
                // No parent USR: fall back to CONTAINS from the Module.
                if let Some(ref mod_usr) = self.module_usr.clone() {
                    self.push_edge(mod_usr.clone(), child_usr.to_owned(), EdgeKind::Contains);
                }
                return;
            }
        };

        // Select edge kind based on parent/child kinds.
        let edge_kind = match (entity_kind_to_node_kind(parent_kind_raw), child_kind) {
            (Some(NodeKind::Class), NodeKind::Method) => EdgeKind::HasMethod,
            (Some(NodeKind::Class), NodeKind::Field) => EdgeKind::HasField,
            (Some(NodeKind::Class), _) | (Some(NodeKind::Function), _) => EdgeKind::Contains,
            (Some(NodeKind::Module), _) => EdgeKind::Contains,
            _ => EdgeKind::Contains,
        };

        self.push_edge(parent_usr.clone(), child_usr.to_owned(), edge_kind);

        // Emit INHERITS edges for base class specifiers.
        if entity.get_kind() == EntityKind::BaseSpecifier {
            if let Some(base_entity) = entity.get_definition() {
                if let Some(base_usr) = base_entity.get_usr() {
                    if !base_usr.0.is_empty() {
                        self.push_edge(parent_usr, base_usr.0, EdgeKind::Inherits);
                    }
                }
            }
        }

        // If this entity has base specifiers, walk them directly now.
        // (BaseSpecifier nodes are visited via the normal child walk.)
        let _ = entity; // silence unused-variable warning
    }

    fn push_edge(&mut self, src: String, dst: String, kind: EdgeKind) {
        self.edges.push(EdgeRecord {
            src_usr: src,
            dst_usr: Some(dst),
            dst_placeholder: None,
            kind,
            resolved: false, // Phase 3 resolves
            cross_repo_candidate: false,
            repo_name: self.repo_name.to_owned(),
            attrs_json: "{}".to_owned(),
            tu_hash: self.tu_hash,
        });
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Extract `(file_path, line, col)` from an entity's spelling location.
fn extract_location(
    entity: &clang::Entity<'_>,
    fallback_file: &str,
) -> (String, Option<u32>, Option<u32>) {
    if let Some(loc) = entity.get_location() {
        let spell = loc.get_spelling_location();
        let file_path = spell
            .file
            .map(|f| f.get_path().to_string_lossy().into_owned())
            .unwrap_or_else(|| fallback_file.to_owned());
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
        (file_path, line, col)
    } else {
        (fallback_file.to_owned(), None, None)
    }
}

/// Build per-kind `attrs_json` string.
fn build_attrs_json(entity: &clang::Entity<'_>, kind: NodeKind) -> String {
    use std::collections::BTreeMap;

    let mut map: BTreeMap<&str, serde_json::Value> = BTreeMap::new();

    match kind {
        NodeKind::Method => {
            let is_virtual = entity.is_virtual_method();
            let is_pure = entity.is_pure_virtual_method();
            let is_static = entity.is_static_method();
            map.insert("virtual", serde_json::Value::Bool(is_virtual));
            map.insert("pure_virtual", serde_json::Value::Bool(is_pure));
            map.insert("static", serde_json::Value::Bool(is_static));
        }
        NodeKind::Field => {
            let is_bit = entity.get_bit_field_width().is_some();
            map.insert("bit_field", serde_json::Value::Bool(is_bit));
        }
        NodeKind::GlobalVariable => {
            // storage class / linkage — best-effort
            let linkage_str = entity
                .get_linkage()
                .map(|l| format!("{l:?}").to_lowercase())
                .unwrap_or_else(|| "none".to_owned());
            map.insert("linkage", serde_json::Value::String(linkage_str));
        }
        _ => {}
    }

    serde_json::to_string(&map).unwrap_or_else(|_| "{}".to_owned())
}

/// Convenience: visit all TUs in `entries` using a freshly-created `Clang` instance.
///
/// Returns `(nodes_written, errors)` totals.
///
/// # Errors
/// Propagates I/O and schema errors from the [`StageWriter`].  libclang soft
/// parse errors are counted in the returned `errors` value, not propagated.
pub fn visit_all(
    entries: &[crate::bootstrap::TuEntry],
    stage_dir: &Path,
    repo_name: &str,
    skip_system_headers: bool,
) -> Result<(usize, usize)> {
    let clang = Clang::new().map_err(Error::Clang)?;
    let mut total_nodes = 0usize;
    let mut error_count = 0usize;

    for (idx, entry) in entries.iter().enumerate() {
        let worker_id = u32::try_from(idx).unwrap_or(u32::MAX);
        let mut writer = StageWriter::new(stage_dir, worker_id)?;

        let opts = VisitOptions {
            repo_name,
            tu_hash: *entry.hash.as_bytes(),
            file_path: &entry.file,
            args: &entry.args,
            skip_system_headers,
        };

        match visit_tu(&clang, &opts, &mut writer) {
            Ok(had_errors) => {
                if had_errors {
                    error_count += 1;
                }
                // Count nodes from the writer (rough — we track inside Collector instead).
            }
            Err(Error::Clang(msg)) => {
                // libclang hard failure: log and continue (AC-M1-16).
                warn!(file = %entry.file.display(), error = %msg, "skipping TU due to hard libclang failure");
                error_count += 1;
            }
            Err(e) => return Err(e),
        }

        let _shards = writer.finish()?;
        total_nodes += 1; // approximate — placeholder
    }

    Ok((total_nodes, error_count))
}
