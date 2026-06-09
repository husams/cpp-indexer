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
//! ## Thread-local libclang Index (S17 / ADR-7, Issue 0002 Bug 1d)
//! `with_thread_index` initialises one `clang::Index` per rayon worker thread on
//! first use.  A single `Clang` instance is created at process startup (stored in
//! a `OnceLock`) and each worker thread creates its own `Index` from it.
//!
//! The per-thread `Index` is **owned** in a `thread_local!` `RefCell` (not
//! `Box::leak`'d) so it can be recycled.  libclang `Index` objects accumulate
//! internal state across thousands of parses; [`with_thread_index_recycle`]
//! tracks a per-thread parse counter and drops + recreates the `Index` every N
//! parses to bound that accumulation (the residual RSS creep in Issue 0002).
//! Recreation happens only **between** parses — never while a `TranslationUnit`
//! borrowed from the `Index` is alive — because each parse is fully consumed
//! inside the `with_thread_index*` closure.
//!
//! # SAFETY
//! `clang::Clang` is `!Sync` by design, but it is zero-sized (`PhantomData`-only).
//! `Index::new` only uses the `&Clang` for lifetime anchoring; the underlying
//! `clang_createIndex()` call is thread-safe.  We add `Sync` via a newtype wrapper
//! after verifying that no mutable state is shared.  See `ClangSync` in the source.

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::panic::{self, AssertUnwindSafe};
use std::path::{Path, PathBuf};

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

#[doc(hidden)]
pub fn global_clang() -> &'static Clang {
    &GLOBAL_CLANG
        .get_or_init(|| ClangSync(Clang::new().expect("libclang global init")))
        .0
}

/// Per-thread libclang `Index` plus a parse counter for recycling (Bug 1d).
struct ThreadIndex {
    /// The owned `Index`.  Owned (not `Box::leak`'d) so it can be dropped and
    /// recreated to bound libclang's per-`Index` state accumulation.
    index: Box<Index<'static>>,
    /// Number of parses serviced by `index` since it was (re)created.
    parses: u32,
}

thread_local! {
    // Each worker thread owns one Index, recreated every N parses.
    static THREAD_INDEX: RefCell<Option<ThreadIndex>> = const { RefCell::new(None) };
}

/// Whether the per-thread `Index` should be recycled before a parse.
///
/// `interval == 0` disables recycling.  Otherwise recycle once the count of
/// parses serviced by the current `Index` has reached `interval`.  Extracted as
/// a pure function so the recycle cadence is unit-testable without libclang.
#[inline]
fn should_recycle(parses: u32, interval: u32) -> bool {
    interval != 0 && parses >= interval
}

/// Create a fresh per-thread `Index` from the process-wide [`GLOBAL_CLANG`].
fn new_thread_index() -> Box<Index<'static>> {
    // SAFETY: GLOBAL_CLANG lives for 'static; Index::new does not mutate Clang
    // — it only calls clang_createIndex(), a thread-safe libclang entry point
    // that returns an independent opaque handle (see module SAFETY comment).
    let clang: &'static Clang = global_clang();
    Box::new(Index::new(
        clang, /* exclude_pch */ true, /* diagnostics */ false,
    ))
}

/// Call `f` with the thread-local `clang::Index`, initialising it on first use.
///
/// Equivalent to [`with_thread_index_recycle`] with recycling disabled — the
/// `Index` is created once per worker thread and reused.  Use this for cheap
/// one-off probes (e.g. capability detection) where recycling is irrelevant.
///
/// # Panics
/// Panics if libclang cannot be initialised (`Clang::new()` fails).
pub fn with_thread_index<R>(f: impl FnOnce(&Index<'static>) -> R) -> R {
    with_thread_index_recycle(0, f)
}

/// Call `f` with the thread-local `clang::Index`, recycling it every
/// `recycle_interval` parses (Issue 0002 Bug 1d).
///
/// On entry, if the per-thread parse counter has reached `recycle_interval`
/// (and the interval is non-zero), the existing `Index` is **dropped and
/// recreated** before `f` runs, bounding libclang's per-`Index` state
/// accumulation.  `recycle_interval == 0` disables recycling (reuse forever).
///
/// # Safety contract
/// Recreation happens only here, *between* parses, while no `TranslationUnit`
/// borrowed from the `Index` is alive: each parse is fully consumed inside `f`
/// and no `Index` reference escapes the closure.  Recreating the `Index` while
/// a borrowed `TranslationUnit` were alive would be UB — the closure boundary
/// is what makes this sound.
///
/// # Panics
/// Panics if libclang cannot be initialised (`Clang::new()` fails).
pub fn with_thread_index_recycle<R>(
    recycle_interval: u32,
    f: impl FnOnce(&Index<'static>) -> R,
) -> R {
    THREAD_INDEX.with(|cell| {
        {
            let mut slot = cell.borrow_mut();
            match slot.as_mut() {
                None => {
                    *slot = Some(ThreadIndex {
                        index: new_thread_index(),
                        parses: 0,
                    });
                }
                Some(ti) => {
                    if should_recycle(ti.parses, recycle_interval) {
                        // Drop the old Index (no live TranslationUnit borrow —
                        // see the safety contract above) and recreate it.
                        ti.index = new_thread_index();
                        ti.parses = 0;
                    }
                }
            }
        }

        // Borrow immutably for the duration of the parse closure.  The closure
        // takes `&Index<'static>`; the `'static` is the Index *type* parameter
        // (anchored to GLOBAL_CLANG), not the reference lifetime, so a
        // short-lived borrow satisfies it.
        let result = {
            let slot = cell.borrow();
            let ti = slot
                .as_ref()
                .expect("thread Index initialised immediately above");
            f(&ti.index)
        };

        // Count this parse after `f` returns (so the borrow has ended).
        if let Some(ti) = cell.borrow_mut().as_mut() {
            ti.parses = ti.parses.saturating_add(1);
        }

        result
    })
}

use std::sync::Arc;

use crate::{
    resolve::symbol_map::SymbolAllocator,
    schema::{
        clip_code, EdgeKind, EdgeRecord, NodeKind, NodeRecord, Param, TemplateArg, TemplateParam,
    },
    stage::writer::StageWriter,
    visit::{
        access_classifier::{classify_use, AccessKind},
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
    /// Optional `SymbolAllocator` for per-repo integer ID allocation (graph-symbol-ids, Story 3).
    ///
    /// When `Some`, `visit_tu_inner` will call `get_or_insert_symbol`/`get_or_insert_file`
    /// for every emitted `NodeRecord` and `EdgeRecord` and populate `symbol_id`, `file_id`,
    /// `src_id`, `dst_id` accordingly.
    ///
    /// When `None`, the integer ID fields remain zero (staging only; sinks must not write
    /// integer ID fields in this mode). This is the default for tests that do not set up
    /// a SQLite database.
    pub allocator: Option<Arc<SymbolAllocator>>,
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
        file_path: opts.file_path.to_string_lossy().into_owned(),
        repo_name: opts.repo_name.to_owned(),
        partial: has_errors,
        tu_hash: opts.tu_hash,
        ..Default::default()
    };
    collector.seen_node_usrs.insert(module_usr.clone());
    collector.nodes.push(module_node);
    collector.module_usr = Some(module_usr);

    // Walk child declarations.
    root.visit_children(|entity, parent| {
        collector.visit(entity, parent);
        EntityVisitResult::Recurse
    });

    // graph-symbol-ids post-processing pass (Story 3, v6):
    // If an allocator is provided, populate integer ID fields on every node and edge.
    if let Some(ref alloc) = opts.allocator {
        for node in &mut collector.nodes {
            node.symbol_id = alloc.get_or_insert_symbol(&node.usr).unwrap_or(0);
            node.file_id = alloc.get_or_insert_file(&node.file_path).unwrap_or(0);
        }
        for edge in &mut collector.edges {
            edge.src_id = alloc.get_or_insert_symbol(&edge.src_usr).unwrap_or(0);
            edge.dst_id = if let Some(ref dst) = edge.dst_usr {
                alloc.get_or_insert_symbol(dst).ok()
            } else {
                None
            };
            // dst_repo_name is already set to repo_name for intra-repo edges at construction.
        }
    }

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

/// Stable spelling location used as a map key for implicit-wrapper context tracking.
/// Keyed by (line, column) within a single TU (file path is implicit — one Collector per TU).
type SpellLoc = (u32, u32);

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
    /// Deduplication set for ALL pushed node USRs within this TU (O(1) lookup).
    ///
    /// Covers all synthetic node kinds (Type, Parameter, TemplateArg) and also
    /// every regular node pushed via `self.nodes.push(...)` — inserted in the
    /// same call that pushes the node.  This replaces the four `O(n²)` linear
    /// scans in `get_or_create_type_node`, `emit_type_graph_edges`,
    /// `emit_template_param_edges`, and `emit_template_arg_nodes`.
    ///
    /// Why a single universe: synthetic USR prefixes (`type:`, `param:`,
    /// `tparam:`, `targ:`) are disjoint from libclang USRs (which start with
    /// `c:`) so collision is impossible.  Using one set is therefore equivalent
    /// to the old `self.nodes.iter().any(|n| n.usr == usr)` scans.
    seen_node_usrs: HashSet<String>,
    /// Per-TU file-content cache for code-snippet extraction (Fix-3).
    ///
    /// Populated on first access per file path; entries live only as long as
    /// this Collector (= one TU parse), so memory is bounded per TU.
    file_cache: HashMap<PathBuf, std::sync::Arc<[u8]>>,
    /// Spelling locations of `UnexposedExpr` nodes whose visitor-callback `parent` is
    /// `ReturnStmt`.  clang-rs exposes `ImplicitCastExpr` as `UnexposedExpr` and its
    /// `get_lexical_parent()` returns `None`, so we capture context at visit time and
    /// look it up when the nested `DeclRefExpr` is classified (QD-3 fix, S43).
    return_wrappers: HashSet<SpellLoc>,
    /// Spelling locations of `UnexposedExpr` nodes whose visitor-callback `parent` is
    /// `VarDecl`, `FieldDecl`, or `ParmDecl` (initializer context → `decl_ref`).
    decl_init_wrappers: HashSet<SpellLoc>,
    /// `(src_usr, dst_usr)` pairs already emitted as `CALLS` edges from a `CallExpr`.
    /// Pre-order DFS visits the `CallExpr` before its callee `DeclRefExpr`, so the
    /// pair is recorded first; `emit_uses_edge` consults this set to suppress the
    /// redundant `USES` edge that the callee reference would otherwise produce.
    call_pairs: HashSet<(String, String)>,
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
            seen_node_usrs: HashSet::new(),
            file_cache: HashMap::new(),
            return_wrappers: HashSet::new(),
            decl_init_wrappers: HashSet::new(),
            call_pairs: HashSet::new(),
        }
    }
}

impl<'a> Collector<'a> {
    fn visit(&mut self, entity: clang::Entity<'_>, parent: clang::Entity<'_>) {
        let kind = entity.get_kind();

        // ── Implicit-wrapper context capture (QD-3 fix, S43) ─────────────────
        // clang-rs exposes ImplicitCastExpr as UnexposedExpr.  Its
        // `get_lexical_parent()` returns None, so the parent chain built later
        // in `emit_uses_edge` cannot reach `ReturnStmt` or `VarDecl`.  We
        // capture the visitor-supplied `parent` here (pre-order DFS means
        // the wrapper is always visited before its DeclRefExpr child) and
        // record the wrapper's spelling location so `emit_uses_edge` can look
        // it up when processing the nested DeclRefExpr.
        if kind == EntityKind::UnexposedExpr {
            if let Some(loc) = entity.get_location() {
                let sl = loc.get_spelling_location();
                let key = (sl.line, sl.column);
                match parent.get_kind() {
                    EntityKind::ReturnStmt => {
                        self.return_wrappers.insert(key);
                    }
                    EntityKind::VarDecl | EntityKind::FieldDecl | EntityKind::ParmDecl => {
                        self.decl_init_wrappers.insert(key);
                    }
                    _ => {}
                }
            }
            // Do NOT return — let Recurse descend into the children.
        }

        // ── Macro handling (S26, AC-M5-1..4) ────────────────────────────────
        if kind == EntityKind::MacroDefinition {
            self.visit_macro_definition(&entity);
            return;
        }
        if kind == EntityKind::MacroExpansion {
            self.visit_macro_expansion(&entity, &parent);
            return;
        }

        // ── USES edge emission (S43, AC-S43-1..4) ────────────────────────────
        // DeclRefExpr / MemberRefExpr / TypeRef represent a reference to another
        // entity at a use site.  Emit a USES edge from the enclosing node (parent
        // USR) to the referenced entity's USR, annotated with the access classifier.
        if matches!(
            kind,
            EntityKind::DeclRefExpr | EntityKind::MemberRefExpr | EntityKind::TypeRef
        ) {
            self.emit_uses_edge(&entity, &parent);
            return;
        }

        // ── INHERITS edge (M2): a CXXBaseSpecifier names a base class ────────
        // `entity_kind_to_node_kind(BaseSpecifier)` is `None`, so base specifiers
        // never reach the node-emission path below; INHERITS is emitted here.
        if kind == EntityKind::BaseSpecifier {
            self.emit_inherits_edge(&entity, &parent);
            return;
        }

        // ── CALLS edge: the callee of a call expression ──────────────────────
        if kind == EntityKind::CallExpr {
            self.emit_calls_edge(&entity, &parent);
            return;
        }

        // ── INSTANTIATES edge: a template referenced at a concrete use site ──
        if kind == EntityKind::TemplateRef {
            self.emit_instantiates_edge(&entity, &parent);
            return;
        }

        // ── v7 S5: USES_NAMESPACE (using-directive) / USES_DECLARATION (using-decl) ──
        // These entity kinds are not tracked as nodes (no USR-keyed node needed),
        // so they are intercepted before the `entity_kind_to_node_kind` filter and
        // returned early after emitting the appropriate edge.
        if kind == EntityKind::UsingDirective {
            self.emit_uses_namespace_edge(&entity, &parent);
            return;
        }
        if kind == EntityKind::UsingDeclaration {
            self.emit_uses_declaration_edge(&entity, &parent);
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
        // v7 S5 (OQ-7): anonymous Enum nodes get a synthetic USR derived from their
        // source location (`enum:<file>:<line>:<col>`) so they are not silently dropped.
        let usr = match entity.get_usr() {
            Some(u) if !u.0.is_empty() => u.0,
            _ => {
                // Synthesize USR for anonymous enums only; other kinds are dropped.
                if node_kind == NodeKind::Enum {
                    let (file_path_loc, line_loc, col_loc) =
                        extract_location(&entity, &self.tu_file_path);
                    let line_val = line_loc.unwrap_or(0);
                    let col_val = col_loc.unwrap_or(0);
                    format!("enum:{file_path_loc}:{line_val}:{col_val}")
                } else {
                    return;
                }
            }
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

        // S41: extract callable attributes for FUNCTION / METHOD nodes.
        let (return_type, params, signature, code, code_truncated) =
            if matches!(node_kind, NodeKind::Function | NodeKind::Method) {
                extract_callable_attrs(&entity, node_kind, &mut self.file_cache)
            } else {
                (None, None, None, None, None)
            };

        // S42: extract template attributes for TEMPLATE_DECL / SPECIALIZATION nodes.
        let (template_params, template_args) = match node_kind {
            NodeKind::TemplateDef => (Some(extract_template_params(&entity)), None),
            NodeKind::Specialization => (None, Some(extract_template_args(&entity))),
            _ => (None, None),
        };

        // S43 (AC-S40-6): promote is_virtual / is_pure_virtual / is_static to native
        // NodeRecord fields.  These were previously written to attrs_json; they are
        // now native-only per ADR-11 §3 (no dual-write).
        let (is_virtual, is_pure_virtual, is_static) = match node_kind {
            NodeKind::Method => (
                Some(entity.is_virtual_method()),
                Some(entity.is_pure_virtual_method()),
                Some(entity.is_static_method()),
            ),
            NodeKind::Function => (None, None, Some(entity.is_static_method())),
            _ => (None, None, None),
        };

        // v7 S1: extract is_const / is_constexpr / storage_class for Field and GlobalVariable.
        // - is_const: type-level const qualifier via clang_isConstQualifiedType.
        // - is_constexpr: not directly exposed by libclang 18 — always emitted as Some(false);
        //   tracked as a follow-up limitation in implementation-notes.md.
        // - storage_class: mapped from clang StorageClass enum.
        // - is_static on Field/GlobalVariable is also mapped from StorageClass.
        let (is_const, is_constexpr, storage_class) =
            if matches!(node_kind, NodeKind::Field | NodeKind::GlobalVariable) {
                let const_qualified = entity
                    .get_type()
                    .map(|t| t.is_const_qualified())
                    .unwrap_or(false);
                let sc_str = entity
                    .get_storage_class()
                    .map(|sc| match sc {
                        clang::StorageClass::None => "none",
                        clang::StorageClass::Auto => "auto",
                        clang::StorageClass::Static => "static",
                        clang::StorageClass::Extern => "extern",
                        clang::StorageClass::Register => "register",
                        _ => "none",
                    })
                    .unwrap_or("none");
                (
                    Some(const_qualified),
                    Some(false), // libclang 18 does not expose is_constexpr for variables
                    Some(sc_str.to_owned()),
                )
            } else {
                (None, None, None)
            };

        // v7 S2: extract extended Function/Method props.
        // NOTE: several fields are not yet exposed by clang-rs 2.0.0 and are
        // always emitted as Some(false) until the binding is available.
        // Tracked as follow-ups in implementation-notes.md.
        //
        // NOTE on is_template: ClassTemplate/FunctionTemplate map to NodeKind::TemplateDef
        // (not Class/Function/Method), so the is_template field on those TemplateDef nodes
        // is set in the TemplateDef branch below.  Function/Method nodes that are
        // non-template specialisations have is_template derived from get_template().
        let (
            is_template_node,
            is_noexcept,
            is_override,
            is_deleted,
            is_defaulted,
            cv_qualifiers,
            ref_qualifier,
        ) = if matches!(node_kind, NodeKind::Function | NodeKind::Method) {
            // A Function/Method can be a specialisation of a function template.
            // get_template() returns Some when this entity is a specialisation.
            let tmpl = entity.get_template().is_some();

            // cv_qualifiers: derived from is_const_method (volatile not exposed)
            let cv = if node_kind == NodeKind::Method && entity.is_const_method() {
                "const".to_owned()
            } else {
                String::new()
            };
            // ref_qualifier: from the type's ref qualifier
            let rq = entity
                .get_type()
                .and_then(|t| t.get_ref_qualifier())
                .map(|rq| match rq {
                    clang::RefQualifier::LValue => "&",
                    clang::RefQualifier::RValue => "&&",
                })
                .unwrap_or("")
                .to_owned();

            (
                Some(tmpl),
                Some(false), // is_noexcept: not exposed by clang-rs 2.0.0
                Some(false), // is_override: not exposed by clang-rs 2.0.0
                Some(false), // is_deleted: not exposed by clang-rs 2.0.0
                Some(false), // is_defaulted: requires clang_3_9 feature (not enabled)
                Some(cv),
                Some(rq),
            )
        } else {
            (None, None, None, None, None, None, None)
        };

        // v7 S2: extract Class props.  Note: ClassDecl/StructDecl → NodeKind::Class.
        // ClassTemplate → NodeKind::TemplateDef (handled below); it never enters this branch.
        let (is_template_class, is_final, is_abstract, record_kind) =
            if matches!(node_kind, NodeKind::Class | NodeKind::Specialization) {
                // A Class node that is a concrete specialisation of a class template
                // has get_template() → Some.  Pure ClassDecl has None.
                let tmpl = entity.get_template().is_some();
                // is_abstract: requires clang_6_0 feature (enabled in Cargo.toml)
                let abstract_val = entity.is_abstract_record();
                // record_kind: derived from entity kind
                let rk = match entity.get_kind() {
                    clang::EntityKind::StructDecl => "struct",
                    clang::EntityKind::UnionDecl => "union",
                    _ => "class",
                };
                (
                    Some(tmpl),
                    Some(false), // is_final: not exposed by clang-rs 2.0.0
                    Some(abstract_val),
                    Some(rk.to_owned()),
                )
            } else {
                (None, None, None, None)
            };

        // TemplateDef nodes (ClassTemplate, FunctionTemplate, TypeAliasTemplateDecl)
        // ARE templates by definition.
        let is_template_def = if node_kind == NodeKind::TemplateDef {
            Some(true)
        } else {
            None
        };

        // Merge is_template from all paths (one column shared).
        let is_template = is_template_node.or(is_template_class).or(is_template_def);

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
            tu_hash: self.tu_hash,
            // S41: callable fields populated above
            return_type,
            params,
            signature,
            code,
            code_truncated,
            // S42: template fields populated above
            template_params,
            template_args,
            // S43: promoted native fields
            is_virtual,
            is_pure_virtual,
            is_static,
            // v7 S1: field/gv props populated above
            is_const,
            is_constexpr,
            storage_class,
            // v7 S2: extended fn/class props
            is_template,
            is_noexcept,
            is_override,
            is_deleted,
            is_defaulted,
            cv_qualifiers,
            ref_qualifier,
            is_final,
            is_abstract,
            record_kind,
            ..Default::default()
        };

        // Register USR before emitters (O(1) dedup for any downstream get-or-create).
        self.seen_node_usrs.insert(usr.clone());

        // Emit structural edges from parent.
        self.emit_structural_edges(&usr, node_kind, &entity, &parent);

        // M2 extension edges.
        self.emit_m2_edges(&usr, node_kind, &entity);

        // v7 S2: emit RETURNS + HAS_PARAM + OF_TYPE type-graph edges for fn/method.
        self.emit_type_graph_edges(&usr, node_kind, &entity);

        // Push the node by move — no clone needed (emit_type_graph_edges no longer
        // takes a &NodeRecord parameter).
        self.nodes.push(node);

        // v7 S2: emit OF_TYPE for Field nodes (design §3.5: "(Parameter|Field)-[:OF_TYPE]->(Type)").
        if node_kind == NodeKind::Field {
            if let Some(field_type) = entity.get_type() {
                let spelling = field_type.get_display_name();
                if !spelling.is_empty() {
                    let type_usr = self.get_or_create_type_node(&spelling);
                    self.push_edge_full(
                        usr.clone(),
                        type_usr,
                        EdgeKind::OfType,
                        "{}".to_owned(),
                        None,
                        None,
                        None,
                    );
                }
            }
        }

        // v7 S3: emit TEMPLATE_PARAM edges for TemplateDef nodes.
        if node_kind == NodeKind::TemplateDef {
            self.emit_template_param_edges(&usr, &entity);
        }

        // v7 S3: emit TEMPLATE_ARG positional nodes + edges for Specialization nodes (ADR-5).
        if node_kind == NodeKind::Specialization {
            self.emit_template_arg_nodes(&usr, &entity);
        }

        // v7 S5: emit Enumerator nodes + ENUMERATOR_OF edges + UNDERLYING_TYPE for Enum nodes.
        if node_kind == NodeKind::Enum {
            self.emit_enum_enumerators(&usr, &entity);
        }

        // v7 S5: emit ALIAS_OF edge for Typedef nodes (one link per typedef in the chain).
        if node_kind == NodeKind::Typedef {
            self.emit_typedef_alias_of(&usr, &entity);
        }
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

        self.seen_node_usrs.insert(record.usr.clone());
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

    /// Emit a USES edge for a DeclRefExpr / MemberRefExpr / TypeRef entity (S43).
    ///
    /// The edge goes from the enclosing entity's USR (resolved via `parent`, then
    /// falling back to the module USR) to the referenced entity's USR.  The access
    /// classifier assigns `source_association_type`; `target_association_type` is a
    /// symmetric copy of the same value per ADR-13.
    ///
    /// Skipped when:
    /// - The parent has no USR (not a tracked declaration).
    /// - The referenced entity has no USR.
    /// - The reference points into a system header (when `skip_system_headers` is set).
    fn emit_uses_edge(&mut self, entity: &clang::Entity<'_>, parent: &clang::Entity<'_>) {
        // Resolve the source USR: walk up via get_semantic_parent() until we find
        // an entity with a non-empty USR that maps to a tracked declaration kind
        // (Function, Method, Class, etc.).  Expression-context parents (BinaryOperator,
        // CallExpr, CompoundStmt, …) have no USR and must be skipped.
        let src_usr = {
            let mut candidate: Option<clang::Entity<'_>> = Some(*parent);
            let mut found: Option<String> = None;
            while let Some(cur) = candidate {
                if let Some(u) = cur.get_usr() {
                    if !u.0.is_empty() && entity_kind_to_node_kind(cur.get_kind()).is_some() {
                        found = Some(u.0);
                        break;
                    }
                }
                candidate = cur.get_semantic_parent();
            }
            match found {
                Some(u) => u,
                None => match &self.module_usr {
                    Some(m) => m.clone(),
                    None => return,
                },
            }
        };

        // The referenced entity carries the destination USR.
        let ref_entity = match entity.get_reference() {
            Some(r) => r,
            None => return,
        };

        let dst_usr = match ref_entity.get_usr() {
            Some(u) if !u.0.is_empty() => u.0,
            _ => return,
        };

        // Skip self-references (entity referencing its own declaration).
        if src_usr == dst_usr {
            return;
        }

        // Suppress the redundant USES that a callee `DeclRefExpr` produces: the
        // `CALLS` edge emitted from the enclosing `CallExpr` (visited first in
        // pre-order DFS) already captures this `(caller, callee)` pair.
        if self
            .call_pairs
            .contains(&(src_usr.clone(), dst_usr.clone()))
        {
            return;
        }

        // System-header filter: skip if the referenced entity is in a system header.
        if self.skip_system_headers && ref_entity.is_in_system_header() {
            return;
        }

        // Build a parent chain by walking get_lexical_parent() up to 4 levels.
        // This lets the classifier see through ImplicitCastExpr and other
        // transparent wrappers to find the true semantic context.
        let mut parent_chain: Vec<clang::Entity<'_>> = Vec::with_capacity(4);
        parent_chain.push(*parent);
        {
            let mut cur = *parent;
            for _ in 0..3 {
                match cur.get_lexical_parent() {
                    Some(p) => {
                        parent_chain.push(p);
                        cur = p;
                    }
                    None => break,
                }
            }
        }
        // QD-3 fix (S43): when `parent` is UnexposedExpr (clang-rs wraps
        // ImplicitCastExpr as UnexposedExpr), the parent chain built above
        // cannot reach ReturnStmt or VarDecl because get_lexical_parent()
        // returns None on expression nodes.  We check whether the wrapper was
        // recorded at visit time (pre-order DFS guarantees the wrapper is
        // captured before the DeclRefExpr is processed here).
        let kind = if parent.get_kind() == EntityKind::UnexposedExpr {
            if let Some(loc) = parent.get_location() {
                let sl = loc.get_spelling_location();
                let key = (sl.line, sl.column);
                if self.return_wrappers.contains(&key) {
                    AccessKind::Return
                } else if self.decl_init_wrappers.contains(&key) {
                    AccessKind::DeclRef
                } else {
                    classify_use(entity, &parent_chain, &mut self.file_cache)
                }
            } else {
                classify_use(entity, &parent_chain, &mut self.file_cache)
            }
        } else {
            classify_use(entity, &parent_chain, &mut self.file_cache)
        };
        let assoc = kind.as_str().to_owned();

        // target mirrors source (ADR-13 §target_association_type, AC-S43-2).
        self.edges.push(EdgeRecord {
            src_usr,
            dst_usr: Some(dst_usr),
            kind: EdgeKind::Uses,
            repo_name: self.repo_name.to_owned(),
            tu_hash: self.tu_hash,
            source_association_type: Some(assoc.clone()),
            target_association_type: Some(assoc),
            dst_repo_name: self.repo_name.to_owned(),
            ..Default::default()
        });
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

        // Emit HEADER node (Phase-3 deduplicates across TUs; seen_node_usrs tracks
        // within-TU for O(1) correctness of the synthetic-USR universe).
        self.seen_node_usrs.insert(header_usr.clone());
        let header_node = NodeRecord {
            usr: header_usr.clone(),
            kind: NodeKind::Header,
            name,
            qualified_name: header_path.clone(),
            file_path: header_path,
            repo_name: self.repo_name.to_owned(),
            partial: self.partial,
            tu_hash: self.tu_hash,
            ..Default::default()
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
        // ── SPECIALIZES (AC-M2-10): a specialization → its primary template ──
        // Partial class specializations map to `NodeKind::Specialization`; full
        // explicit specializations map to `NodeKind::Class` (class spec) or
        // `NodeKind::Function` (function spec).  `get_template()` is `Some` only
        // for specializations/instantiations, so ordinary classes/functions are
        // unaffected; instantiations are never emitted as nodes (see `visit`).
        if matches!(
            node_kind,
            NodeKind::Specialization | NodeKind::Class | NodeKind::Function
        ) {
            if let Some(tmpl) = entity.get_template() {
                if let Some(tmpl_usr) = tmpl.get_usr() {
                    if !tmpl_usr.0.is_empty() && tmpl_usr.0 != entity_usr {
                        self.push_edge(entity_usr.to_owned(), tmpl_usr.0, EdgeKind::Specializes);
                    }
                }
            }
        }

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

    /// Walk `start` and its semantic ancestors until an entity with a non-empty
    /// USR that maps to a tracked [`NodeKind`] is found.  Returns `(usr, kind)`,
    /// or `None` (callers decide whether to fall back to the module USR).
    fn resolve_enclosing(start: clang::Entity<'_>) -> Option<(String, EntityKind)> {
        let mut cur = Some(start);
        while let Some(e) = cur {
            if let Some(u) = e.get_usr() {
                if !u.0.is_empty() && entity_kind_to_node_kind(e.get_kind()).is_some() {
                    return Some((u.0, e.get_kind()));
                }
            }
            // Reference cursors (e.g. `BaseSpecifier`) report no semantic parent;
            // fall back to the lexical parent to reach the enclosing declaration.
            cur = e.get_semantic_parent().or_else(|| e.get_lexical_parent());
        }
        None
    }

    /// Emit an INHERITS edge from the derived class (the base specifier's parent)
    /// to the base class.  A base that is a template instantiation (`Box<double>`)
    /// is resolved to its primary template so the edge lands on an existing
    /// `TEMPLATE_DECL` node rather than dangling.
    fn emit_inherits_edge(&mut self, entity: &clang::Entity<'_>, parent: &clang::Entity<'_>) {
        let derived_usr = match parent.get_usr() {
            Some(u) if !u.0.is_empty() => u.0,
            _ => return,
        };
        let base = match entity
            .get_definition()
            .or_else(|| entity.get_type().and_then(|t| t.get_declaration()))
        {
            Some(b) => b,
            None => return,
        };
        // A specialization/instantiation base resolves to its primary template;
        // a plain base resolves to itself.
        let base_template = base.get_template();
        let target = base_template.unwrap_or(base);
        if self.skip_system_headers && target.is_in_system_header() {
            return;
        }
        let target_usr = match target.get_usr() {
            Some(u) if !u.0.is_empty() && u.0 != derived_usr => u.0,
            _ => return,
        };
        // v7 S1 (ADR-3/OQ-9): always emit `access` on INHERITS edges (default "public").
        let access = entity
            .get_accessibility()
            .map(|a| match a {
                clang::Accessibility::Public => "public",
                clang::Accessibility::Protected => "protected",
                clang::Accessibility::Private => "private",
            })
            .unwrap_or("public");

        // v7 S4: emit `inherits_is_virtual` on INHERITS edges (ADR-3/design §3.4).
        // Pinned name `inherits_is_virtual` (not `is_virtual`) to avoid schema_drift collision
        // with the method-level `is_virtual` node property (design §3.4 / ADR-1).
        let inherits_is_virtual = entity.is_virtual_base();

        self.push_edge_full(
            derived_usr.clone(),
            target_usr.clone(),
            EdgeKind::Inherits,
            "{}".to_owned(),
            Some(access.to_owned()),
            None, // INHERITS edges do not carry edge_index
            Some(inherits_is_virtual),
        );
        // When the base is a template instantiation (`Box<double>`), the derived
        // class also instantiates the primary template.  A `BaseSpecifier` reports
        // no semantic/lexical parent, so the `TemplateRef` handler cannot reach the
        // enclosing class — emit the INSTANTIATES edge here, where it is known.
        if base_template.is_some() {
            self.push_edge(derived_usr, target_usr, EdgeKind::Instantiates);
        }
    }

    /// Emit a CALLS edge from the enclosing function/method to the callee of a
    /// `CallExpr`.  Records the `(caller, callee)` pair so `emit_uses_edge`
    /// suppresses the redundant callee `USES` edge.
    fn emit_calls_edge(&mut self, entity: &clang::Entity<'_>, parent: &clang::Entity<'_>) {
        let callee = match entity.get_reference() {
            Some(c) => c,
            // A type-dependent call inside an uninstantiated template (e.g.
            // `call<T>(a)`) has no resolved callee — libclang's two-phase lookup
            // leaves the callee cursor referent-less, so no CALLS edge is possible.
            None => return,
        };
        let callee_usr = match callee.get_usr() {
            Some(u) if !u.0.is_empty() => u.0,
            _ => return,
        };
        if self.skip_system_headers && callee.is_in_system_header() {
            return;
        }
        let src_usr = match Self::resolve_enclosing(*parent) {
            Some((u, _)) => u,
            None => match &self.module_usr {
                Some(m) => m.clone(),
                None => return,
            },
        };
        if src_usr == callee_usr {
            return;
        }
        self.call_pairs
            .insert((src_usr.clone(), callee_usr.clone()));
        self.push_edge(src_usr, callee_usr, EdgeKind::Calls);
    }

    /// Emit an INSTANTIATES edge from a concrete use site to the template named
    /// by a `TemplateRef` (e.g. `Box<double>` as a base, `id<int>(...)`).  Refs
    /// inside a template/specialization *declaration* (the `Box` in the header of
    /// `Box<T*>`) are skipped — that relationship is SPECIALIZES, not an instance.
    fn emit_instantiates_edge(&mut self, entity: &clang::Entity<'_>, parent: &clang::Entity<'_>) {
        let tmpl = match entity.get_reference() {
            Some(t) => t,
            None => return,
        };
        let tmpl_usr = match tmpl.get_usr() {
            Some(u) if !u.0.is_empty() => u.0,
            _ => return,
        };
        if self.skip_system_headers && tmpl.is_in_system_header() {
            return;
        }
        let (src_usr, src_kind) = match Self::resolve_enclosing(*parent) {
            Some(pair) => pair,
            None => return,
        };
        // Skip references that occur within a template/specialization header
        // (declaration context) rather than at a concrete instantiation site.
        if matches!(
            src_kind,
            EntityKind::ClassTemplate
                | EntityKind::FunctionTemplate
                | EntityKind::ClassTemplatePartialSpecialization
                | EntityKind::TypeAliasTemplateDecl
        ) {
            return;
        }
        if src_usr == tmpl_usr {
            return;
        }
        self.push_edge(src_usr, tmpl_usr, EdgeKind::Instantiates);
    }

    /// Emit CONTAINS, HAS_METHOD, HAS_FIELD edges. (INHERITS is emitted
    /// separately from `visit` on the `BaseSpecifier` cursor.)
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

        // v7 S1 (ADR-3/OQ-9): always emit `access` on HAS_METHOD and HAS_FIELD edges,
        // including "public".  CONTAINS and other edges get `None`.
        let access = if matches!(edge_kind, EdgeKind::HasMethod | EdgeKind::HasField) {
            let acc_str = entity
                .get_accessibility()
                .map(|a| match a {
                    clang::Accessibility::Public => "public",
                    clang::Accessibility::Protected => "protected",
                    clang::Accessibility::Private => "private",
                })
                .unwrap_or("public"); // default per design §3.4
            Some(acc_str.to_owned())
        } else {
            None
        };

        self.push_edge_full(
            parent_usr,
            child_usr.to_owned(),
            edge_kind,
            "{}".to_owned(),
            access,
            None, // HAS_METHOD/HAS_FIELD/CONTAINS do not carry edge_index
            None, // HAS_METHOD/HAS_FIELD/CONTAINS do not carry inherits_is_virtual
        );
    }

    fn push_edge(&mut self, src: String, dst: String, kind: EdgeKind) {
        self.push_edge_with_attrs(src, dst, kind, "{}".to_owned());
    }

    fn push_edge_with_attrs(&mut self, src: String, dst: String, kind: EdgeKind, attrs: String) {
        self.push_edge_full(src, dst, kind, attrs, None, None, None);
    }

    /// Core edge builder; called by all emit helpers.
    ///
    /// `access` is `Some("public"|"protected"|"private")` for HAS_METHOD, HAS_FIELD, and
    /// INHERITS edges; `None` for all other edge kinds (ADR-3/OQ-9).
    ///
    /// `edge_index` is `Some(i)` for HAS_PARAM edges (ADR-5); `None` for all other kinds.
    ///
    /// `inherits_is_virtual` is `Some(bool)` for INHERITS edges only (v7 S4); `None` for all others.
    #[allow(clippy::too_many_arguments)] // 8 params: private internal builder, not public API
    fn push_edge_full(
        &mut self,
        src: String,
        dst: String,
        kind: EdgeKind,
        attrs: String,
        access: Option<String>,
        edge_index: Option<i64>,
        inherits_is_virtual: Option<bool>,
    ) {
        self.edges.push(EdgeRecord {
            src_usr: src,
            dst_usr: Some(dst),
            kind,
            repo_name: self.repo_name.to_owned(),
            attrs_json: attrs,
            tu_hash: self.tu_hash,
            dst_repo_name: self.repo_name.to_owned(),
            access,
            edge_index,
            inherits_is_virtual,
            ..Default::default()
        });
    }

    // ── v7 S2: Type node get-or-create + type-graph edge emission ────────────

    /// Get-or-create a `Type` node for the given written spelling (ADR-2).
    ///
    /// The synthetic USR `type:<spelling>` uniquely identifies the type globally
    /// within a run.  If a node with that USR was already pushed in this TU,
    /// this is a no-op; dedup across TUs happens at the symbol allocator level.
    ///
    /// Returns the synthetic USR so the caller can push edges to it.
    fn get_or_create_type_node(&mut self, spelling: &str) -> String {
        let usr = format!("type:{spelling}");
        // Avoid re-pushing the same Type node within one TU.  O(1) via
        // seen_node_usrs; Phase 3 deduplicates across TUs.
        if self.seen_node_usrs.insert(usr.clone()) {
            let node = NodeRecord {
                usr: usr.clone(),
                kind: NodeKind::Type,
                name: spelling.to_owned(),
                qualified_name: spelling.to_owned(),
                file_path: self.tu_file_path.clone(),
                repo_name: self.repo_name.to_owned(),
                partial: self.partial,
                tu_hash: self.tu_hash,
                type_spelling: Some(spelling.to_owned()),
                ..Default::default()
            };
            self.nodes.push(node);

            // Recurse one declarator level: pointer → POINTS_TO, reference → REFERS_TO.
            // We check the SPELLING to detect `*` or `&` declarators (written form, ADR-2).
            // This avoids chasing canonical types which would collapse cv-qualified chains.
            if spelling.ends_with('*') {
                // Strip trailing `*` (and possible spaces) to get the pointee spelling.
                let pointee = spelling.trim_end_matches('*').trim_end();
                if !pointee.is_empty() && pointee != spelling {
                    let pointee_usr = self.get_or_create_type_node(pointee);
                    self.push_edge(usr.clone(), pointee_usr, EdgeKind::PointsTo);
                }
            } else if spelling.ends_with('&') {
                // Strip trailing `&` (handles both `&` and `&&` via the written spelling).
                let referent = spelling.trim_end_matches('&').trim_end();
                if !referent.is_empty() && referent != spelling {
                    let referent_usr = self.get_or_create_type_node(referent);
                    self.push_edge(usr.clone(), referent_usr, EdgeKind::RefersTo);
                }
            }
        }
        usr
    }

    /// Emit RETURNS, HAS_PARAM, and OF_TYPE edges for Function/Method nodes (S2).
    ///
    /// Called after the Function/Method node has been pushed.
    /// Graceful degradation: failures on individual params skip that param but
    /// do not fail the TU (C5 / Issue-0001 guard).
    fn emit_type_graph_edges(
        &mut self,
        fn_usr: &str,
        node_kind: NodeKind,
        entity: &clang::Entity<'_>,
    ) {
        if !matches!(node_kind, NodeKind::Function | NodeKind::Method) {
            return;
        }

        // ── RETURNS edge ─────────────────────────────────────────────────────
        if let Some(ret_type) = entity.get_result_type() {
            let spelling = ret_type.get_display_name();
            let type_usr = self.get_or_create_type_node(&spelling);
            self.push_edge_full(
                fn_usr.to_owned(),
                type_usr,
                EdgeKind::Returns,
                "{}".to_owned(),
                None,
                None,
                None,
            );
        }

        // ── HAS_PARAM edges (one per parameter) ──────────────────────────────
        let args = entity.get_arguments().unwrap_or_default();
        for (idx, arg) in args.iter().enumerate() {
            let param_index = idx as i64;
            let param_name = arg.get_name().unwrap_or_default();
            let param_type_spelling = arg
                .get_type()
                .map(|t| t.get_display_name())
                .unwrap_or_default();

            // Synthetic USR for the Parameter node.
            let param_usr = format!("param:{fn_usr}:{idx}");

            // Emit Parameter node if not already present (O(1) via seen_node_usrs).
            if self.seen_node_usrs.insert(param_usr.clone()) {
                let param_node = NodeRecord {
                    usr: param_usr.clone(),
                    kind: NodeKind::Parameter,
                    name: param_name,
                    qualified_name: format!("{fn_usr}::{param_index}"),
                    file_path: self.tu_file_path.clone(),
                    repo_name: self.repo_name.to_owned(),
                    partial: self.partial,
                    tu_hash: self.tu_hash,
                    type_spelling: Some(param_type_spelling.clone()),
                    param_index: Some(param_index),
                    param_kind: Some("value".to_owned()),
                    ..Default::default()
                };
                self.nodes.push(param_node);
            }

            // HAS_PARAM edge with edge_index = param_index (ADR-5).
            self.push_edge_full(
                fn_usr.to_owned(),
                param_usr.clone(),
                EdgeKind::HasParam,
                "{}".to_owned(),
                None,
                Some(param_index),
                None,
            );

            // OF_TYPE edge from Parameter to Type node.
            if !param_type_spelling.is_empty() {
                let type_usr = self.get_or_create_type_node(&param_type_spelling);
                self.push_edge_full(
                    param_usr,
                    type_usr,
                    EdgeKind::OfType,
                    "{}".to_owned(),
                    None,
                    None,
                    None,
                );
            }
        }
    }

    // ── v7 S3: Template-param edge emission ──────────────────────────────────

    /// Emit TEMPLATE_PARAM edges from a `TemplateDef` node to its `Parameter` nodes (S3).
    ///
    /// Walks the immediate children of the template entity and emits one
    /// `NodeKind::Parameter` node (synthetic USR `tparam:<template-usr>:<index>`) and
    /// one `TEMPLATE_PARAM` edge per template parameter.
    ///
    /// Graceful degradation: any per-parameter failure logs and skips that position
    /// without failing the TU (C5 / Issue-0001 guard).
    fn emit_template_param_edges(&mut self, tmpl_usr: &str, entity: &clang::Entity<'_>) {
        use clang::EntityKind as EK;

        let mut param_index: i64 = 0;
        for child in entity.get_children() {
            let param_kind_str = match child.get_kind() {
                EK::TemplateTypeParameter => "type",
                EK::NonTypeTemplateParameter => "non_type",
                EK::TemplateTemplateParameter => "template",
                _ => continue,
            };

            let param_name = child.get_name().unwrap_or_default();
            let tparam_usr = format!("tparam:{tmpl_usr}:{param_index}");

            // Emit Parameter node if not already present (O(1) via seen_node_usrs).
            if self.seen_node_usrs.insert(tparam_usr.clone()) {
                let tparam_node = NodeRecord {
                    usr: tparam_usr.clone(),
                    kind: NodeKind::Parameter,
                    name: param_name,
                    qualified_name: format!("{tmpl_usr}::{param_index}"),
                    file_path: self.tu_file_path.clone(),
                    repo_name: self.repo_name.to_owned(),
                    partial: self.partial,
                    tu_hash: self.tu_hash,
                    param_index: Some(param_index),
                    param_kind: Some(param_kind_str.to_owned()),
                    ..Default::default()
                };
                self.nodes.push(tparam_node);
            }

            // TEMPLATE_PARAM edge with edge_index = param_index (ADR-5).
            self.push_edge_full(
                tmpl_usr.to_owned(),
                tparam_usr,
                EdgeKind::TemplateParam,
                "{}".to_owned(),
                None,
                Some(param_index),
                None,
            );

            param_index += 1;
        }
    }

    // ── v7 S3: Template-arg positional node emission (ADR-5) ─────────────────

    /// Emit positional `TemplateArg` nodes and `TEMPLATE_ARG` edges for a
    /// `Specialization` node (ADR-5).
    ///
    /// Each template argument becomes a distinct `NodeKind::TemplateArg` positional
    /// node with synthetic USR `targ:<specialization-usr>:<index>`.  For type-kind
    /// arguments the node additionally emits an `OF_TYPE` edge to the deduped `Type`
    /// node (ADR-2).  Non-type/expression arguments carry their value in
    /// `type_spelling` and emit no `OF_TYPE`.
    ///
    /// Ordering is via `param_index` on the positional node (ADR-5); consumers must
    /// sort on `param_index` for ordered traversal (IndraDB returns unordered).
    ///
    /// Graceful degradation: `Null` (unresolved/dependent) args are skipped; the TU
    /// is never marked as a total failure (C5 / Issue-0001 guard).
    fn emit_template_arg_nodes(&mut self, spec_usr: &str, entity: &clang::Entity<'_>) {
        use clang::TemplateArgument;

        let args = match entity.get_template_arguments() {
            Some(a) => a,
            None => return, // no args exposed (e.g. implicit instantiation)
        };

        for (idx, arg) in args.iter().enumerate() {
            let (param_kind_str, spelling, is_type_arg) = match arg {
                TemplateArgument::Type(ty) => {
                    let sp = ty.get_display_name();
                    ("type", sp, true)
                }
                TemplateArgument::Integral(signed, _unsigned) => {
                    ("non_type", signed.to_string(), false)
                }
                TemplateArgument::Template | TemplateArgument::TemplateExpansion => {
                    ("template", String::new(), false)
                }
                TemplateArgument::Expression => ("expression", String::new(), false),
                TemplateArgument::Declaration => ("non_type", String::new(), false),
                TemplateArgument::Nullptr => ("non_type", String::new(), false),
                TemplateArgument::Pack => ("expression", String::new(), false),
                // Null = unresolved / dependent — skip this position gracefully (C5).
                TemplateArgument::Null => {
                    debug!(
                        spec_usr = %spec_usr,
                        idx = idx,
                        "S3: skipping unresolvable template arg (Null/dependent)"
                    );
                    continue;
                }
            };

            let targ_usr = format!("targ:{spec_usr}:{idx}");
            let param_index = idx as i64;

            // Emit TemplateArg positional node if not already present (O(1) via seen_node_usrs).
            if self.seen_node_usrs.insert(targ_usr.clone()) {
                let targ_node = NodeRecord {
                    usr: targ_usr.clone(),
                    kind: NodeKind::TemplateArg,
                    name: spelling.clone(),
                    qualified_name: targ_usr.clone(),
                    file_path: self.tu_file_path.clone(),
                    repo_name: self.repo_name.to_owned(),
                    partial: self.partial,
                    tu_hash: self.tu_hash,
                    type_spelling: if spelling.is_empty() {
                        None
                    } else {
                        Some(spelling.clone())
                    },
                    param_index: Some(param_index),
                    param_kind: Some(param_kind_str.to_owned()),
                    ..Default::default()
                };
                self.nodes.push(targ_node);
            }

            // TEMPLATE_ARG edge from Specialization to positional TemplateArg node.
            self.push_edge_full(
                spec_usr.to_owned(),
                targ_usr.clone(),
                EdgeKind::TemplateArg,
                "{}".to_owned(),
                None,
                Some(param_index),
                None,
            );

            // For type-kind args: additionally emit OF_TYPE → deduped Type node (ADR-5).
            if is_type_arg && !spelling.is_empty() {
                let type_usr = self.get_or_create_type_node(&spelling);
                self.push_edge_full(
                    targ_usr,
                    type_usr,
                    EdgeKind::OfType,
                    "{}".to_owned(),
                    None,
                    None,
                    None,
                );
            }
        }
    }

    // ── v7 S3: Concept node emission (ADR-7) ─────────────────────────────────

    /// Emit a `Concept` node for a `CXCursor_ConceptDecl` entity (ADR-7, C++20 only).
    ///
    /// NOTE: clang-rs 2.0.0 does not expose `EntityKind::ConceptDecl`.  This method
    /// is a placeholder stub that never emits, following the established pattern for
    /// libclang-binding gaps (see `is_final`, `is_deleted`, etc. in nodes.rs).
    /// A follow-up is required once clang-rs exposes the `ConceptDecl` entity kind.
    ///
    /// Pre-C++20 SFINAE constraints are intentionally NOT modelled (ADR-7 §skip+document).
    #[allow(dead_code)]
    fn emit_concept_node(&mut self, _entity: &clang::Entity<'_>, _constrained_usr: &str) {
        // clang-rs 2.0.0 does not expose EntityKind::ConceptDecl — stub only.
        // Follow-up: add ConceptDecl handling once the binding is available.
    }

    // ── v7 S5: Enumerator + enum edge emission ───────────────────────────────

    /// Emit `Enumerator` nodes + `ENUMERATOR_OF` edges for all enumeration constants
    /// in an `Enum` node, and an `UNDERLYING_TYPE` edge when an explicit underlying
    /// type is present (v7 S5 / scenarios S5-01, S5-02).
    ///
    /// Anonymous enums are assigned a synthetic USR `enum:<file>:<line>:<col>` in the
    /// outer node path (OQ-7); here `enum_usr` is whatever USR was used for the Enum.
    ///
    /// Graceful degradation: any per-enumerator extraction failure is logged + skipped;
    /// the parent Enum and other enumerators are unaffected (C5 / Issue-0001 guard).
    fn emit_enum_enumerators(&mut self, enum_usr: &str, entity: &clang::Entity<'_>) {
        // UNDERLYING_TYPE: emit if an explicit underlying type is present (S5-02).
        // `get_enum_underlying_type()` returns `Some` only when the enum has an explicit
        // underlying type annotation (e.g. `enum class Status : uint8_t`).
        if let Some(underlying_type) = entity.get_enum_underlying_type() {
            let spelling = underlying_type.get_display_name();
            if !spelling.is_empty() {
                let type_usr = self.get_or_create_type_node(&spelling);
                self.push_edge_full(
                    enum_usr.to_owned(),
                    type_usr,
                    EdgeKind::UnderlyingType,
                    "{}".to_owned(),
                    None,
                    None,
                    None,
                );
            }
        }

        // Walk immediate children looking for EnumConstantDecl entities.
        let mut enumerator_data: Vec<(String, String, Option<i64>)> = Vec::new();
        entity.visit_children(|child, _| {
            if child.get_kind() != clang::EntityKind::EnumConstantDecl {
                return clang::EntityVisitResult::Continue;
            }

            // Real USR from libclang (enumerators always have USRs).
            let child_usr = match child.get_usr() {
                Some(u) if !u.0.is_empty() => u.0,
                _ => return clang::EntityVisitResult::Continue,
            };

            if self.skip_system_headers && child.is_in_system_header() {
                return clang::EntityVisitResult::Continue;
            }

            let name = child.get_name().unwrap_or_default();
            // Signed integer value from clang_getEnumConstantDeclValue.
            let enum_value = child.get_enum_constant_value().map(|(signed, _)| signed);

            enumerator_data.push((child_usr, name, enum_value));
            clang::EntityVisitResult::Continue
        });

        for (child_usr, name, enum_value) in enumerator_data {
            let node = NodeRecord {
                usr: child_usr.clone(),
                kind: NodeKind::Enumerator,
                name: name.clone(),
                qualified_name: name,
                file_path: self.tu_file_path.clone(),
                repo_name: self.repo_name.to_owned(),
                partial: self.partial,
                tu_hash: self.tu_hash,
                enum_value,
                ..Default::default()
            };
            self.seen_node_usrs.insert(child_usr.clone());
            self.nodes.push(node);

            // ENUMERATOR_OF: Enumerator → Enum
            self.push_edge_full(
                child_usr,
                enum_usr.to_owned(),
                EdgeKind::EnumeratorOf,
                "{}".to_owned(),
                None,
                None,
                None,
            );
        }
    }

    /// Emit an `ALIAS_OF` edge from a `Typedef` node to its written underlying type
    /// (v7 S5 / scenarios S5-04, EC-03).
    ///
    /// Uses the WRITTEN spelling (not canonical) so that a chain like
    /// `typedef int Base; typedef Base Mid; typedef Mid Top;`
    /// produces three distinct ALIAS_OF edges (Top→Mid, Mid→Base, Base→int) rather
    /// than collapsing to the canonical type (int).  This relies on ADR-2.
    fn emit_typedef_alias_of(&mut self, typedef_usr: &str, entity: &clang::Entity<'_>) {
        if let Some(underlying) = entity.get_typedef_underlying_type() {
            let spelling = underlying.get_display_name();
            if !spelling.is_empty() {
                let type_usr = self.get_or_create_type_node(&spelling);
                self.push_edge_full(
                    typedef_usr.to_owned(),
                    type_usr,
                    EdgeKind::AliasOf,
                    "{}".to_owned(),
                    None,
                    None,
                    None,
                );
            }
        }
    }

    /// Emit a `USES_NAMESPACE` edge for a `using namespace X;` directive (v7 S5 / S5-05, EC-04).
    ///
    /// Source scope: resolved via `resolve_enclosing`, falling back to the module USR.
    /// Target: the referenced Namespace node's USR (from `get_reference()`).
    fn emit_uses_namespace_edge(&mut self, entity: &clang::Entity<'_>, parent: &clang::Entity<'_>) {
        // Resolve the enclosing scope (function, class, namespace, or module).
        let src_usr = Self::resolve_enclosing(*parent)
            .map(|(u, _)| u)
            .or_else(|| self.module_usr.clone());
        let src_usr = match src_usr {
            Some(u) if !u.is_empty() => u,
            _ => return,
        };

        // The referenced namespace.
        let target = match entity.get_reference() {
            Some(r) => r,
            None => return,
        };
        // libclang does not populate the USR on the reference cursor for
        // UsingDirective; fall back to the referenced definition's USR (QD-1).
        let dst_usr = match target.get_usr() {
            Some(u) if !u.0.is_empty() => u.0,
            _ => match target.get_definition().and_then(|d| d.get_usr()) {
                Some(u) if !u.0.is_empty() => u.0,
                _ => return,
            },
        };
        if self.skip_system_headers && target.is_in_system_header() {
            return;
        }

        self.push_edge_full(
            src_usr,
            dst_usr,
            EdgeKind::UsesNamespace,
            "{}".to_owned(),
            None,
            None,
            None,
        );
    }

    /// Emit a `USES_DECLARATION` edge for a `using X::y;` declaration (v7 S5 / S5-06, EC-04).
    ///
    /// For `using Base::method` in a derived class, this is DUAL-EMITTED: the
    /// existing OVERRIDES/HAS_METHOD path is NOT replaced (OQ-8/additive).
    ///
    /// Source scope: resolved via `resolve_enclosing`, falling back to the module USR.
    /// Target: the referenced entity's USR (from `get_reference()`).
    fn emit_uses_declaration_edge(
        &mut self,
        entity: &clang::Entity<'_>,
        parent: &clang::Entity<'_>,
    ) {
        // Resolve the enclosing scope.
        let src_usr = Self::resolve_enclosing(*parent)
            .map(|(u, _)| u)
            .or_else(|| self.module_usr.clone());
        let src_usr = match src_usr {
            Some(u) if !u.is_empty() => u,
            _ => return,
        };

        // The referenced declaration.
        let target = match entity.get_reference() {
            Some(r) => r,
            None => return,
        };

        // libclang resolves `using ns::name;` to an OverloadedDeclRef cursor whose
        // own USR is empty (QD-2). The actual targets live in its overload set —
        // this also covers the single-decl (variable) case, which is likewise
        // surfaced as a one-element OverloadedDeclRef. Fall back to the direct USR
        // / definition USR for any non-overloaded reference.
        let targets: Vec<clang::Entity<'_>> = match target.get_usr() {
            Some(u) if !u.0.is_empty() => vec![target],
            _ => match target.get_overloaded_declarations() {
                Some(decls) if !decls.is_empty() => decls,
                _ => match target.get_definition() {
                    Some(d) => vec![d],
                    None => return,
                },
            },
        };

        for decl in targets {
            let dst_usr = match decl.get_usr() {
                Some(u) if !u.0.is_empty() => u.0,
                _ => continue,
            };
            if self.skip_system_headers && decl.is_in_system_header() {
                continue;
            }
            self.push_edge_full(
                src_usr.clone(),
                dst_usr,
                EdgeKind::UsesDeclaration,
                "{}".to_owned(),
                None,
                None,
                None,
            );
        }
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

/// Callable attribute bundle returned by [`extract_callable_attrs`].
type CallableAttrs = (
    Option<String>,     // return_type
    Option<Vec<Param>>, // params
    Option<String>,     // signature
    Option<String>,     // code
    Option<bool>,       // code_truncated
);

/// Extract callable attributes from a FUNCTION or METHOD entity (S41, AC-S41-1..6).
///
/// Returns `(return_type, params, signature, code, code_truncated)`.
///
/// - `return_type`: display name of the return type via `entity.get_result_type()`.
/// - `params`: ordered list of `{name, type}` pairs via `entity.get_arguments()`.
/// - `signature`: `"return_type(param_type, …)"` with ` const` suffix for METHOD when applicable.
/// - `code` / `code_truncated`: verbatim source range read from disk; capped at 32 KiB
///   per ADR-12.  On any I/O failure the code fields fall back to `None` (non-fatal).
fn extract_callable_attrs(
    entity: &clang::Entity<'_>,
    node_kind: NodeKind,
    file_cache: &mut HashMap<PathBuf, std::sync::Arc<[u8]>>,
) -> CallableAttrs {
    // ── return_type ─────────────────────────────────────────────────────────
    let return_type: Option<String> = entity.get_result_type().map(|t| t.get_display_name());

    // ── params ───────────────────────────────────────────────────────────────
    let params: Option<Vec<Param>> = {
        let args = entity.get_arguments().unwrap_or_default();
        let list: Vec<Param> = args
            .iter()
            .map(|arg| Param {
                name: arg.get_name().unwrap_or_default(),
                type_: arg
                    .get_type()
                    .map(|t| t.get_display_name())
                    .unwrap_or_default(),
            })
            .collect();
        Some(list)
    };

    // ── signature ────────────────────────────────────────────────────────────
    let signature: Option<String> = {
        let ret = return_type.as_deref().unwrap_or("void");
        let csv = params
            .as_ref()
            .map(|ps| {
                ps.iter()
                    .map(|p| p.type_.as_str())
                    .collect::<Vec<_>>()
                    .join(", ")
            })
            .unwrap_or_default();
        let mut sig = format!("{ret}({csv})");
        // Note: clang-rs 2.0.0 does not expose is_volatile_method(); the volatile
        // qualifier on methods is omitted from the signature (deferred to M9).
        if node_kind == NodeKind::Method && entity.is_const_method() {
            sig.push_str(" const");
        }
        Some(sig)
    };

    // ── code / code_truncated ────────────────────────────────────────────────
    let (code, code_truncated) = extract_code_snippet(entity, file_cache);

    (return_type, params, signature, code, code_truncated)
}

/// Read the verbatim source bytes for `entity`'s range and apply the 32 KiB cap
/// (ADR-12).
///
/// `file_cache` is a per-TU map from path → arc'd byte slice.  On the first call
/// for a given path the file is read from disk and stored; subsequent calls for
/// the same path re-use the cached bytes.  The cache is owned by the `Collector`
/// and drops with it, so memory is bounded per TU.
///
/// Returns `(Some(snippet), Some(false))` when the body fits, `(None, Some(true))`
/// when oversize, and `(None, None)` on any I/O error (non-fatal: code is best-effort).
fn extract_code_snippet(
    entity: &clang::Entity<'_>,
    file_cache: &mut HashMap<PathBuf, std::sync::Arc<[u8]>>,
) -> (Option<String>, Option<bool>) {
    let range = match entity.get_range() {
        Some(r) => r,
        None => return (None, None),
    };

    let start_loc = range.get_start().get_file_location();
    let end_loc = range.get_end().get_file_location();

    // Both ends must be in the same file.
    let file = match start_loc.file {
        Some(ref f) => f.get_path(),
        None => return (None, None),
    };
    if end_loc.file.as_ref().map(|f| f.get_path()) != Some(file.clone()) {
        return (None, None);
    }

    let start_offset = start_loc.offset as usize;
    let end_offset = end_loc.offset as usize;

    if end_offset < start_offset {
        return (None, None);
    }

    // Fetch from cache or read from disk (first access per path within this TU).
    let bytes: std::sync::Arc<[u8]> = match file_cache.get(&file) {
        Some(cached) => cached.clone(),
        None => {
            let raw = match std::fs::read(&file) {
                Ok(b) => b,
                Err(_) => return (None, None),
            };
            let arc: std::sync::Arc<[u8]> = raw.into();
            file_cache.insert(file.clone(), arc.clone());
            arc
        }
    };

    let snippet_bytes = match bytes.get(start_offset..end_offset) {
        Some(s) => s,
        None => return (None, None),
    };

    // Convert to UTF-8 (best-effort — replace invalid sequences).
    let snippet = String::from_utf8_lossy(snippet_bytes).into_owned();

    let (code, truncated) = clip_code(&snippet);
    (code, Some(truncated))
}

/// Build per-kind `attrs_json` string.
fn build_attrs_json(entity: &clang::Entity<'_>, kind: NodeKind) -> String {
    use std::collections::BTreeMap;

    let mut map: BTreeMap<&str, serde_json::Value> = BTreeMap::new();

    match kind {
        NodeKind::Method => {
            // S43 (AC-S40-6): is_virtual / is_pure_virtual / is_static are promoted
            // to native NodeRecord fields; MUST NOT be written into attrs_json (ADR-11 §3).
            // attrs_json for METHOD nodes is now empty ("{}") unless future per-method
            // attributes are added here.
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
            // S42 (AC-S40-6): template_args is now a promoted structured field on NodeRecord;
            // it MUST NOT be written into attrs_json. Only non-promoted ancillary data lives here.
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

/// Extract template parameters from a TEMPLATE_DECL entity (S42, AC-S42-1).
///
/// Walks the immediate children of the entity and collects:
/// - `TemplateTypeParameter` → kind `"type"`
/// - `NonTypeTemplateParameter` → kind `"non_type"`
/// - `TemplateTemplateParameter` → kind `"template"`
///
/// For each parameter the name and an optional default are extracted.  The
/// default is inferred from the first child of the parameter that has a
/// displayable name (covers simple type defaults like `= int`).
fn extract_template_params(entity: &clang::Entity<'_>) -> Vec<TemplateParam> {
    use clang::EntityKind as EK;

    let mut params = Vec::new();

    for child in entity.get_children() {
        let kind_str = match child.get_kind() {
            EK::TemplateTypeParameter => "type",
            EK::NonTypeTemplateParameter => "non_type",
            EK::TemplateTemplateParameter => "template",
            _ => continue,
        };

        let name = child.get_name().unwrap_or_default();

        // Best-effort default: look for the first child whose display name is
        // non-empty.  Covers `typename T = int` (TypeRef child) and simple
        // integral defaults.  Complex expression defaults are left as `None`.
        let default = child
            .get_children()
            .into_iter()
            .filter_map(|c| c.get_display_name())
            .find(|s| !s.is_empty());

        params.push(TemplateParam {
            name,
            kind: kind_str.to_owned(),
            default,
        });
    }

    params
}

/// Extract template arguments from a SPECIALIZATION entity (S42, AC-S42-2).
///
/// Uses `entity.get_template_arguments()` (requires `clang_3_6` feature, enabled
/// in `Cargo.toml` for M8).  Each `TemplateArgument` variant is classified as:
/// - `Type`        → kind `"type"`,  value = display name of the type
/// - `Integral`    → kind `"integral"`, value = decimal string of the signed value
/// - `Template` / `TemplateExpansion` → kind `"template"`, value = display name if available
/// - `Declaration` → kind `"declaration"`, value = empty string
/// - `Nullptr`     → kind `"nullptr"`, value = empty string
/// - `Expression`  → kind `"expression"`, value = empty string
/// - `Pack`        → kind `"pack"`, value = empty string
/// - `Null`        → skipped (unresolved / not-yet-deduced argument)
///
/// Returns an empty `Vec` when `get_template_arguments()` returns `None` or the
/// list is empty (e.g. implicit instantiations where libclang does not expose
/// argument detail).
fn extract_template_args(entity: &clang::Entity<'_>) -> Vec<TemplateArg> {
    use clang::TemplateArgument;

    let args = match entity.get_template_arguments() {
        Some(a) => a,
        None => return Vec::new(),
    };

    args.into_iter()
        .filter_map(|arg| match arg {
            TemplateArgument::Type(ty) => Some(TemplateArg {
                kind: "type".to_owned(),
                value: ty.get_display_name(),
            }),
            TemplateArgument::Integral(signed, _unsigned) => Some(TemplateArg {
                kind: "integral".to_owned(),
                value: signed.to_string(),
            }),
            TemplateArgument::Template | TemplateArgument::TemplateExpansion => Some(TemplateArg {
                kind: "template".to_owned(),
                value: String::new(),
            }),
            TemplateArgument::Declaration => Some(TemplateArg {
                kind: "declaration".to_owned(),
                value: String::new(),
            }),
            TemplateArgument::Nullptr => Some(TemplateArg {
                kind: "nullptr".to_owned(),
                value: String::new(),
            }),
            TemplateArgument::Expression => Some(TemplateArg {
                kind: "expression".to_owned(),
                value: String::new(),
            }),
            TemplateArgument::Pack => Some(TemplateArg {
                kind: "pack".to_owned(),
                value: String::new(),
            }),
            // Null = unresolved / not-yet-deduced — skip entirely
            TemplateArgument::Null => None,
        })
        .collect()
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
    let clang = global_clang();
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
            allocator: None,
        };

        match visit_tu(clang, &opts, &mut writer) {
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

// ---------------------------------------------------------------------------
// Tests (Issue 0002 Bug 1d — Index recycle cadence)
// ---------------------------------------------------------------------------

#[cfg(test)]
mod recycle_tests {
    use super::should_recycle;

    #[test]
    fn recycle_disabled_when_interval_zero() {
        // interval 0 means "never recycle", regardless of parse count.
        assert!(!should_recycle(0, 0));
        assert!(!should_recycle(1_000_000, 0));
    }

    #[test]
    fn recycle_fires_exactly_at_interval() {
        let interval = 256;
        // Below the interval: keep the Index.
        for parses in [0u32, 1, 100, 255] {
            assert!(
                !should_recycle(parses, interval),
                "must not recycle before reaching interval (parses={parses})"
            );
        }
        // At and beyond the interval: recycle.
        assert!(
            should_recycle(256, interval),
            "must recycle at the interval"
        );
        assert!(
            should_recycle(257, interval),
            "must recycle past the interval"
        );
    }

    #[test]
    fn recycle_every_n_in_a_simulated_run() {
        // Simulate the per-thread counter: increment after each parse, reset to
        // 0 when a recycle fires.  Assert a recreate fires exactly every N.
        let interval = 4u32;
        let mut parses = 0u32;
        let mut recreate_at = Vec::new();
        for tu in 0..20u32 {
            if should_recycle(parses, interval) {
                recreate_at.push(tu);
                parses = 0;
            }
            // (parse happens here)
            parses += 1;
        }
        // With interval 4, recreate fires before TUs 4, 8, 12, 16 (the counter
        // reaches 4 after 4 parses, triggering on the next entry).
        assert_eq!(recreate_at, vec![4, 8, 12, 16]);
    }
}
