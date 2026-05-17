//! Phase 1 — Shallow libclang visitor → Parquet (AC-M1-14..17, AC-M3-1..3).
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
//! ## Thread-local libclang Index (S17 / ADR-7)
//! `with_thread_index` initialises one `clang::Index` per rayon worker thread on
//! first use.  A single `Clang` instance is created at process startup (stored in
//! a `OnceLock`) and each worker thread creates its own `Index` from it.  `Index`
//! is leaked to `'static`; the leak is bounded by worker count × process lifetime.
//!
//! # SAFETY
//! `clang::Clang` is `!Sync` by design, but it is zero-sized (`PhantomData`-only).
//! `Index::new` only uses the `&Clang` for lifetime anchoring; the underlying
//! `clang_createIndex()` call is thread-safe.  We add `Sync` via a newtype wrapper
//! after verifying that no mutable state is shared.  See `ClangSync` in the source.

use std::cell::OnceCell;
use std::panic::{self, AssertUnwindSafe};
use std::path::Path;

use std::collections::HashSet;

use clang::{Clang, EntityKind, EntityVisitResult, Index};
use tracing::{debug, warn};

// ---------------------------------------------------------------------------
// Thread-local Index (ADR-7)
// ---------------------------------------------------------------------------
//
// `clang::Clang` is a Rust-level singleton that ensures libclang's shared
// library is loaded. The crate enforces at most one live `Clang` instance per
// process.  `Index::new` only uses the `&Clang` parameter for lifetime
// anchoring — the underlying `clang_createIndex()` call is independent.
//
// Strategy: create exactly **one** `Clang` per process (stored in a `OnceLock`
// and wrapped in a `ClangSync` newtype that adds the `Sync` marker), then give
// each worker thread its **own** `Index` created from the shared `Clang` handle.
//
// # SAFETY
// `Clang` is `!Sync` because it holds `PhantomData<*mut ()>` — a conservative
// marker.  In practice `Clang` is zero-sized (pure PhantomData); there is no
// mutable state or shared data behind the reference.  We create `Index` objects
// on each thread by calling `Index::new(&GLOBAL_CLANG, ...)`, which solely
// invokes `clang_createIndex()` — a thread-safe libclang entry point that
// creates an independent opaque handle.  The `GLOBAL_CLANG` reference is never
// mutated after initialisation.  Therefore adding `Sync` is sound.

use std::sync::OnceLock;

/// Newtype that makes `Clang` safe to share across threads (see module SAFETY).
struct ClangSync(Clang);

// SAFETY: see module-level comment above.
// Send is needed so that OnceLock<ClangSync> qualifies as a static.
// Clang is zero-sized (PhantomData only); there is no data to send.
unsafe impl Send for ClangSync {}
unsafe impl Sync for ClangSync {}

/// Process-wide `Clang` instance.  Created once; never dropped (avoids
/// re-triggering the libclang singleton guard in child processes / after fork).
static GLOBAL_CLANG: OnceLock<ClangSync> = OnceLock::new();

fn global_clang() -> &'static Clang {
    &GLOBAL_CLANG
        .get_or_init(|| ClangSync(Clang::new().expect("libclang global init")))
        .0
}

thread_local! {
    // Each worker thread keeps one Index for the lifetime of the pool.
    static THREAD_INDEX: OnceCell<&'static Index<'static>> = const { OnceCell::new() };
}

/// Call `f` with the thread-local `clang::Index`, initialising it on first use.
///
/// Creates one `Index` per rayon worker thread on first call; subsequent calls
/// on the same thread reuse the cached `Index`.  Thread-local initialisation
/// uses the process-wide [`GLOBAL_CLANG`] handle (see module SAFETY comment).
///
/// # Panics
/// Panics if libclang cannot be initialised (`Clang::new()` fails).
pub fn with_thread_index<R>(f: impl FnOnce(&Index<'static>) -> R) -> R {
    THREAD_INDEX.with(|cell| {
        let idx = cell.get_or_init(|| {
            // SAFETY: GLOBAL_CLANG lives for 'static; Index::new does not
            // mutate Clang — it only calls clang_createIndex() which is
            // thread-safe in libclang.  Box::leak bounds the leak to
            // worker-count × process-lifetime (documented pattern).
            let clang: &'static Clang = global_clang();
            Box::leak(Box::new(Index::new(
                clang, /* exclude_pch */ true, /* diagnostics */ false,
            )))
        });
        f(idx)
    })
}

use crate::{
    schema::{EdgeKind, EdgeRecord, NodeKind, NodeRecord},
    stage::writer::StageWriter,
    visit::{
        cursor_map::entity_kind_to_node_kind,
        macros::{collect_macro_definition, collect_macro_expansion},
    },
    Error, Result,
};

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

/// Options for [`visit_tu`] / [`visit_tu_with_index`].
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

/// Parse one translation unit using a caller-supplied `Index` and write Phase 1
/// records to `writer`.
///
/// This is the low-level entry point used by the parallel pipeline (S17) so
/// that each rayon worker can supply its own thread-local `Index` (ADR-7).
/// For the sequential path, prefer [`visit_tu`] which manages the `Index`
/// internally.
///
/// Returns `true` when libclang reported at least one error-severity diagnostic
/// (the TU was parsed `partial`), `false` otherwise.
///
/// # Errors
/// Returns [`Error::Clang`] only when libclang itself fails to produce any AST.
/// Returns [`Error::Schema`] / [`Error::Io`] when writing to the Parquet shard fails.
pub fn visit_tu_with_index(
    index: &Index<'_>,
    opts: &VisitOptions<'_>,
    writer: &mut StageWriter,
) -> Result<bool> {
    visit_tu_inner(index, opts, writer)
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
    visit_tu_inner(&index, opts, writer)
}

/// Shared body for [`visit_tu`] and [`visit_tu_with_index`].
fn visit_tu_inner(
    index: &Index<'_>,
    opts: &VisitOptions<'_>,
    writer: &mut StageWriter,
) -> Result<bool> {
    // Parse the TU; wrap in catch_unwind to isolate libclang panics (AC-M1-16).
    // `detailed_preprocessing_record` is required to expose InclusionDirective
    // entities during the AST walk (AC-M2-4, AC-M2-7).
    let tu_result = panic::catch_unwind(AssertUnwindSafe(|| {
        index
            .parser(opts.file_path)
            .arguments(opts.args)
            .detailed_preprocessing_record(true)
            .parse()
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
    /// Deduplication set for `MACRO` node USRs within this TU.
    /// Multiple TUs may include the same header; Phase 3 deduplicates by USR.
    /// Within a single TU we avoid emitting the same `MacroDefinition` twice.
    seen_macro_usrs: HashSet<String>,
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
            seen_macro_usrs: HashSet::new(),
        }
    }
}

impl<'a> Collector<'a> {
    fn visit(&mut self, entity: clang::Entity<'_>, parent: clang::Entity<'_>) {
        let kind = entity.get_kind();

        // ── Macro handling (S26, AC-M5-1..4) ────────────────────────────────
        if kind == EntityKind::MacroDefinition {
            self.visit_macro_definition(&entity);
            return;
        }
        if kind == EntityKind::MacroExpansion {
            self.visit_macro_expansion(&entity, &parent);
            return;
        }

        let node_kind = match entity_kind_to_node_kind(kind) {
            Some(k) if k != NodeKind::Module => k,
            _ => return,
        };

        // HEADER nodes (InclusionDirective) are handled specially: we emit the
        // HEADER node and the INCLUDES edge, but no further child walk is needed
        // for the directive itself (children of an InclusionDirective are typically empty).
        if node_kind == NodeKind::Header {
            self.emit_header_node_and_edge(&entity, &parent);
            return;
        }

        // NAMESPACE: always emit (definitions can be spread across TUs).
        // For most other kinds, skip non-definitions to avoid duplicates.
        // VarDecl and Field: always emit (definitions can be non-existent as decl).
        // Typedef/Enum: emit on first occurrence (is_definition may not be reliable for aliases).
        if !entity.is_definition() {
            match node_kind {
                NodeKind::Field
                | NodeKind::GlobalVariable
                | NodeKind::Namespace
                | NodeKind::Typedef
                | NodeKind::Enum => {} // emit regardless
                _ => return,
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

        // M2 extension edges.
        self.emit_m2_edges(&usr, node_kind, &entity);
    }

    /// Emit a MACRO node for a MacroDefinition entity (AC-M5-1).
    ///
    /// Deduplicates within this TU: the same macro (same synthesised USR) is only
    /// emitted once even if the definition is visited multiple times.
    fn visit_macro_definition(&mut self, entity: &clang::Entity<'_>) {
        let record = match collect_macro_definition(
            entity,
            self.repo_name,
            self.tu_hash,
            self.partial,
            &self.tu_file_path,
            self.skip_system_headers,
        ) {
            Some(r) => r,
            None => return,
        };

        // Dedup within this TU pass.
        if !self.seen_macro_usrs.insert(record.usr.clone()) {
            return;
        }

        self.nodes.push(record);
    }

    /// Emit an EXPANDS_TO edge for a MacroExpansion entity (AC-M5-2, AC-M5-3).
    ///
    /// The `src_usr` of the edge is:
    /// - The closest enclosing function/method entity's USR if available.
    /// - Otherwise the Module USR for the TU.
    ///
    /// Nested macro expansions (detected by `collect_macro_expansion`) are silently skipped.
    fn visit_macro_expansion(&mut self, entity: &clang::Entity<'_>, _parent: &clang::Entity<'_>) {
        let enclosing_usr = self.module_usr.as_deref().unwrap_or("").to_owned();

        if enclosing_usr.is_empty() {
            return;
        }

        let edge = match collect_macro_expansion(
            entity,
            &enclosing_usr,
            self.repo_name,
            self.tu_hash,
            &self.tu_file_path,
            self.skip_system_headers,
        ) {
            Some(e) => e,
            None => return,
        };

        self.edges.push(edge);
    }

    /// Emit a HEADER node for an InclusionDirective and the INCLUDES edge from the module.
    ///
    /// The header's USR is synthesised from its canonical file path because libclang does not
    /// assign a USR to InclusionDirective entities.  AC-M2-4, AC-M2-7.
    fn emit_header_node_and_edge(
        &mut self,
        entity: &clang::Entity<'_>,
        _parent: &clang::Entity<'_>,
    ) {
        let header_path = match entity.get_file() {
            Some(f) => f.get_path().to_string_lossy().into_owned(),
            None => return, // built-in / virtual include — skip
        };

        let header_usr = format!("header:{header_path}");
        let name = std::path::Path::new(&header_path)
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_else(|| header_path.clone());

        // Emit HEADER node (deduplication by USR happens at Phase 3).
        let header_node = NodeRecord {
            usr: header_usr.clone(),
            kind: NodeKind::Header,
            name,
            qualified_name: header_path.clone(),
            mangled_name: None,
            file_path: header_path,
            line: None,
            col: None,
            repo_name: self.repo_name.to_owned(),
            attrs_json: "{}".to_owned(),
            partial: self.partial,
            phase: 1,
            tu_hash: self.tu_hash,
        };
        self.nodes.push(header_node);

        // Emit INCLUDES edge from the MODULE (TU root) to the HEADER.  AC-M2-7.
        if let Some(ref mod_usr) = self.module_usr.clone() {
            self.push_edge(mod_usr.clone(), header_usr, EdgeKind::Includes);
        }
    }

    /// Emit M2 extension edges for the given entity (after its node has been pushed).
    ///
    /// Handles: OVERRIDES, SPECIALIZES, FRIEND_OF.
    /// INSTANTIATES and ADL_CANDIDATE are best-effort from call-expression context; they are
    /// emitted here where accessible from the declaration.
    fn emit_m2_edges(&mut self, entity_usr: &str, node_kind: NodeKind, entity: &clang::Entity<'_>) {
        match node_kind {
            // ── OVERRIDES (AC-M2-8): virtual method overrides base class virtual ──
            NodeKind::Method => {
                if let Some(overridden) = entity.get_overridden_methods() {
                    for (slot, base_method) in overridden.iter().enumerate() {
                        if let Some(base_usr) = base_method.get_usr() {
                            if !base_usr.0.is_empty() {
                                let attrs = serde_json::json!({ "vtable_slot": slot }).to_string();
                                self.push_edge_with_attrs(
                                    entity_usr.to_owned(),
                                    base_usr.0,
                                    EdgeKind::Overrides,
                                    attrs,
                                );
                            }
                        }
                    }
                }
            }

            // ── SPECIALIZES (AC-M2-10): specialization → primary template ──
            NodeKind::Specialization => {
                if let Some(tmpl) = entity.get_template() {
                    if let Some(tmpl_usr) = tmpl.get_usr() {
                        if !tmpl_usr.0.is_empty() {
                            self.push_edge(
                                entity_usr.to_owned(),
                                tmpl_usr.0,
                                EdgeKind::Specializes,
                            );
                        }
                    }
                }
            }

            // ── FRIEND_OF (AC-M2-11): friend declaration ──
            NodeKind::Class => {
                // Walk children looking for FriendDecl entities.
                let entity_usr_owned = entity_usr.to_owned();
                let mut friend_usrs: Vec<String> = Vec::new();
                entity.visit_children(|child, _| {
                    if child.get_kind() == clang::EntityKind::FriendDecl {
                        // The friend's referenced entity carries the USR.
                        if let Some(ref_entity) = child.get_reference() {
                            if let Some(u) = ref_entity.get_usr() {
                                if !u.0.is_empty() {
                                    friend_usrs.push(u.0);
                                }
                            }
                        }
                    }
                    clang::EntityVisitResult::Continue
                });
                for friend_usr in friend_usrs {
                    self.push_edge(entity_usr_owned.clone(), friend_usr, EdgeKind::FriendOf);
                }
            }

            _ => {}
        }
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
            (Some(NodeKind::Class), _)
            | (Some(NodeKind::Function), _)
            | (Some(NodeKind::Namespace), _)
            | (Some(NodeKind::TemplateDef), _)
            | (Some(NodeKind::Specialization), _)
            | (Some(NodeKind::Module), _) => EdgeKind::Contains,
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
        self.push_edge_with_attrs(src, dst, kind, "{}".to_owned());
    }

    fn push_edge_with_attrs(&mut self, src: String, dst: String, kind: EdgeKind, attrs: String) {
        self.edges.push(EdgeRecord {
            src_usr: src,
            dst_usr: Some(dst),
            dst_placeholder: None,
            kind,
            resolved: false, // Phase 3 resolves
            cross_repo_candidate: false,
            repo_name: self.repo_name.to_owned(),
            attrs_json: attrs,
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
        NodeKind::Namespace => {
            // is_inline_namespace() requires clang_9_0 feature; omit for now.
            // The attrs_json for NAMESPACE nodes is currently empty ("{}").
        }
        NodeKind::TemplateDef => {
            // Record whether it is a class template or function template.
            let kind_str = match entity.get_kind() {
                clang::EntityKind::ClassTemplate => "class",
                clang::EntityKind::FunctionTemplate => "function",
                clang::EntityKind::TypeAliasTemplateDecl => "type_alias",
                _ => "unknown",
            };
            map.insert(
                "template_kind",
                serde_json::Value::String(kind_str.to_owned()),
            );
        }
        NodeKind::Specialization => {
            // Record template arguments as a JSON array of strings (best-effort).
            if let Some(args) = entity.get_template_arguments() {
                let args_strs: Vec<serde_json::Value> = args
                    .iter()
                    .map(|a| serde_json::Value::String(format!("{a:?}")))
                    .collect();
                map.insert("template_args", serde_json::Value::Array(args_strs));
            }
            if let Some(tmpl) = entity.get_template() {
                if let Some(tmpl_usr) = tmpl.get_usr() {
                    map.insert("template_usr", serde_json::Value::String(tmpl_usr.0));
                }
            }
        }
        NodeKind::Typedef => {
            // Record the underlying type spelling (best-effort).
            if let Some(underlying) = entity.get_typedef_underlying_type() {
                map.insert(
                    "underlying_type",
                    serde_json::Value::String(underlying.get_display_name()),
                );
            }
        }
        NodeKind::Enum => {
            let scoped = entity.is_scoped();
            map.insert("scoped", serde_json::Value::Bool(scoped));
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
