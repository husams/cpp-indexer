//! Phase 2 — Optional deep decoration of Phase 1 nodes (AC-M5-5, AC-M5-6).
//!
//! Phase 2 re-parses each translation unit (TU) to extract annotations that
//! are too expensive to emit during the Phase 1 shallow walk.  For each
//! Function / Method node already in the stage directory it patches
//! `attrs_json` with two new fields:
//!
//! - `"exception_spec"` — the C++ exception specification string
//!   (`"noexcept"`, `"throw()"`, `"throw(...)"`, `"none"`).
//! - `"control_flow"` — a best-effort control-flow summary: `"has_return"`,
//!   `"has_throw"`, `"has_loop"`, or `"basic"` (none of the above detected).
//!
//! ## Short-circuit
//! When `skip_phase2` is `true` the function returns immediately without
//! reading or writing any Parquet shards.  The pipeline gate in
//! `pipeline::run` enforces the same condition, but keeping it here too makes
//! the unit tests straightforward.
//!
//! ## Deduplication
//! Phase 2 writes new Parquet shards named `phase2-nodes-*.parquet` inside
//! the same stage directory.  `pipeline::run` calls `load_nodes_from_stage`
//! which, after this story, prefers `phase=2` records over `phase=1` records
//! for the same USR.  Phase 2 does NOT delete Phase 1 shards.
//!
//! AC covered: AC-M5-5, AC-M5-6.

use std::collections::HashMap;
use std::fs::File;
use std::path::Path;

use clang::{Clang, EntityKind, Index};
use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;
use parquet::arrow::ArrowWriter;
use parquet::file::properties::WriterProperties;
use tracing::{debug, info};

use crate::bootstrap::TuEntry;
use crate::schema::arrow::{node_schema, nodes_to_record_batch, record_batch_to_nodes};
use crate::schema::nodes::{NodeKind, NodeRecord};
use crate::{Error, Result};

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

/// Run Phase 2 decoration over the given translation-unit entries.
///
/// Reads `worker-*/nodes-*.parquet` shards from `stage_dir`, re-parses every
/// TU whose nodes include at least one Function or Method, adds
/// `exception_spec` and `control_flow` fields to the `attrs_json` of those
/// nodes, and writes the decorated records to
/// `<stage_dir>/phase2-nodes-<shard>.parquet`.
///
/// When `skip_phase2` is `true` the function is a no-op and returns
/// `Ok(0)` immediately.
///
/// # Returns
/// Number of nodes decorated (written to phase2 shards).
///
/// # Errors
/// Propagates I/O, Arrow, and Parquet errors.  Per-TU libclang parse failures
/// are logged and skipped rather than propagated.
pub fn run(
    clang: &Clang,
    entries: &[TuEntry],
    stage_dir: &Path,
    skip_phase2: bool,
) -> Result<usize> {
    if skip_phase2 {
        debug!("Phase 2: skip_phase2=true — decoration skipped");
        return Ok(0);
    }

    info!("Phase 2: loading Phase 1 node shards from {:?}", stage_dir);

    // Build a map of USR → NodeRecord for all Function/Method nodes from Phase 1.
    let phase1_nodes = load_function_nodes(stage_dir)?;
    if phase1_nodes.is_empty() {
        info!("Phase 2: no Function/Method nodes to decorate");
        return Ok(0);
    }

    info!(
        "Phase 2: {} Function/Method node(s) to decorate across {} TU(s)",
        phase1_nodes.len(),
        entries.len()
    );

    // Build a TU-file → list-of-USRs index so we only re-parse TUs that have
    // at least one Function/Method node.
    let mut tu_to_usrs: HashMap<String, Vec<String>> = HashMap::new();
    for (usr, node) in &phase1_nodes {
        tu_to_usrs
            .entry(node.file_path.clone())
            .or_default()
            .push(usr.clone());
    }

    // Re-parse each relevant TU and build decoration map.
    let mut decoration: HashMap<String, DecorAttrs> = HashMap::new();
    for entry in entries {
        let file_str = entry.file.to_string_lossy().into_owned();
        if !tu_to_usrs.contains_key(&file_str) {
            continue;
        }

        let index = Index::new(clang, true, false);
        let extra = collect_decoration(&index, entry);
        decoration.extend(extra);
    }

    // Produce decorated NodeRecords.
    let mut decorated: Vec<NodeRecord> = Vec::new();
    for (usr, mut node) in phase1_nodes {
        let attrs = decoration
            .get(&usr)
            .cloned()
            .unwrap_or_else(DecorAttrs::default_basic);
        patch_attrs_json(&mut node, &attrs);
        node.phase = 2;
        decorated.push(node);
    }

    let n = decorated.len();
    write_phase2_shard(stage_dir, &decorated)?;

    info!("Phase 2: complete — {} node(s) decorated", n);
    Ok(n)
}

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

/// Decoration attributes extracted during Phase 2 re-parse.
#[derive(Clone, Debug, Default)]
struct DecorAttrs {
    /// Exception specification: `"noexcept"`, `"throw()"`, `"throw(...)"`,
    /// or `"none"` when no specification is present.
    exception_spec: String,
    /// Control-flow summary: `"has_return"`, `"has_throw"`, `"has_loop"`,
    /// or `"basic"` (none of the above).
    control_flow: String,
}

impl DecorAttrs {
    fn default_basic() -> Self {
        Self {
            exception_spec: "none".to_owned(),
            control_flow: "basic".to_owned(),
        }
    }
}

// ---------------------------------------------------------------------------
// Stage reader — load only Function/Method nodes
// ---------------------------------------------------------------------------

/// Load all `Function` and `Method` nodes from Phase 1 Parquet shards.
///
/// Returns a map of USR → NodeRecord so that decoration writes back to the
/// same record without a secondary lookup.
fn load_function_nodes(stage_dir: &Path) -> Result<HashMap<String, NodeRecord>> {
    let mut map = HashMap::new();

    if !stage_dir.exists() {
        return Ok(map);
    }

    for entry in std::fs::read_dir(stage_dir)? {
        let entry = entry?;
        let path = entry.path();
        let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
        if !path.is_dir() || !name.starts_with("worker-") {
            continue;
        }
        for shard in std::fs::read_dir(&path)? {
            let shard = shard?;
            let sp = shard.path();
            let sn = sp.file_name().and_then(|n| n.to_str()).unwrap_or("");
            if !sn.starts_with("nodes") || !sn.ends_with(".parquet") {
                continue;
            }
            let file = File::open(&sp)?;
            let reader = ParquetRecordBatchReaderBuilder::try_new(file)
                .map_err(|e| Error::Schema(format!("phase2: open shard {}: {e}", sp.display())))?
                .build()
                .map_err(|e| {
                    Error::Schema(format!("phase2: build reader {}: {e}", sp.display()))
                })?;
            for batch_result in reader {
                let batch =
                    batch_result.map_err(|e| Error::Schema(format!("phase2: read batch: {e}")))?;
                for node in record_batch_to_nodes(&batch) {
                    if matches!(node.kind, NodeKind::Function | NodeKind::Method) {
                        map.insert(node.usr.clone(), node);
                    }
                }
            }
        }
    }

    Ok(map)
}

// ---------------------------------------------------------------------------
// TU re-parse for decoration
// ---------------------------------------------------------------------------

/// Re-parse one TU and collect `DecorAttrs` for every Function/Method USR.
fn collect_decoration(index: &Index<'_>, entry: &TuEntry) -> HashMap<String, DecorAttrs> {
    let mut result: HashMap<String, DecorAttrs> = HashMap::new();

    let tu = match index
        .parser(&entry.file)
        .arguments(&entry.args)
        .detailed_preprocessing_record(true)
        .parse()
    {
        Ok(tu) => tu,
        Err(e) => {
            tracing::warn!(
                file = %entry.file.display(),
                error = ?e,
                "Phase 2: libclang parse error — skipping decoration for this TU"
            );
            return result;
        }
    };

    let root = tu.get_entity();
    root.visit_children(|entity, _| {
        let kind = entity.get_kind();
        if !matches!(
            kind,
            EntityKind::FunctionDecl
                | EntityKind::Method
                | EntityKind::Constructor
                | EntityKind::Destructor
                | EntityKind::ConversionFunction
                | EntityKind::FunctionTemplate
        ) {
            return clang::EntityVisitResult::Recurse;
        }
        if !entity.is_definition() {
            return clang::EntityVisitResult::Recurse;
        }
        let usr = match entity.get_usr() {
            Some(u) if !u.0.is_empty() => u.0,
            _ => return clang::EntityVisitResult::Recurse,
        };

        let exception_spec = classify_exception_spec(&entity);
        let control_flow = classify_control_flow(&entity);

        result.insert(
            usr,
            DecorAttrs {
                exception_spec,
                control_flow,
            },
        );

        clang::EntityVisitResult::Recurse
    });

    result
}

// ---------------------------------------------------------------------------
// Classification helpers
// ---------------------------------------------------------------------------

/// Return the exception specification string for a function/method entity.
fn classify_exception_spec(entity: &clang::Entity<'_>) -> String {
    // libclang exposes exception specification type only via the entity type.
    // We approximate from the display name / raw comment where libclang does
    // not surface a direct API.  The clang crate does not expose
    // `clang_getExceptionSpecificationType` at the safe level, so we use the
    // type's display name as a proxy.
    let type_display = entity
        .get_type()
        .map(|t| t.get_display_name())
        .unwrap_or_default();

    if type_display.contains("noexcept") {
        "noexcept".to_owned()
    } else if type_display.contains("throw()") {
        "throw()".to_owned()
    } else if type_display.contains("throw(") {
        "throw(...)".to_owned()
    } else {
        "none".to_owned()
    }
}

/// Classify the control-flow characteristics of a function/method body.
///
/// We walk the entity's direct children looking for well-known statement kinds:
/// `ReturnStmt`, `CXXThrowExpr`, and loop statements.  The first signal found
/// wins, in order: `has_throw` > `has_loop` > `has_return` > `basic`.
fn classify_control_flow(entity: &clang::Entity<'_>) -> String {
    let mut has_return = false;
    let mut has_throw = false;
    let mut has_loop = false;

    entity.visit_children(|child, _| {
        match child.get_kind() {
            EntityKind::ReturnStmt => {
                has_return = true;
            }
            EntityKind::ThrowExpr => {
                has_throw = true;
            }
            EntityKind::ForStmt
            | EntityKind::WhileStmt
            | EntityKind::DoStmt
            | EntityKind::ForRangeStmt => {
                has_loop = true;
            }
            _ => {}
        }
        clang::EntityVisitResult::Recurse
    });

    if has_throw {
        "has_throw".to_owned()
    } else if has_loop {
        "has_loop".to_owned()
    } else if has_return {
        "has_return".to_owned()
    } else {
        "basic".to_owned()
    }
}

// ---------------------------------------------------------------------------
// attrs_json patcher
// ---------------------------------------------------------------------------

/// Merge `decor` into the existing `attrs_json` of `node` in place.
fn patch_attrs_json(node: &mut NodeRecord, decor: &DecorAttrs) {
    let mut map: serde_json::Map<String, serde_json::Value> =
        serde_json::from_str(&node.attrs_json).unwrap_or_default();

    map.insert(
        "exception_spec".to_owned(),
        serde_json::Value::String(decor.exception_spec.clone()),
    );
    map.insert(
        "control_flow".to_owned(),
        serde_json::Value::String(decor.control_flow.clone()),
    );

    node.attrs_json = serde_json::to_string(&map).unwrap_or_else(|_| node.attrs_json.clone());
}

// ---------------------------------------------------------------------------
// Phase 2 shard writer
// ---------------------------------------------------------------------------

/// Write `records` to `<stage_dir>/phase2-nodes-0.parquet`.
fn write_phase2_shard(stage_dir: &Path, records: &[NodeRecord]) -> Result<()> {
    if records.is_empty() {
        return Ok(());
    }

    let shard_path = stage_dir.join("phase2-nodes-0.parquet");
    let file = File::create(&shard_path)?;

    let schema = std::sync::Arc::new(node_schema());
    let props = WriterProperties::builder().build();
    let mut writer = ArrowWriter::try_new(file, schema, Some(props))
        .map_err(|e| Error::Schema(format!("phase2: create parquet writer: {e}")))?;

    let batch = nodes_to_record_batch(records)
        .map_err(|e| Error::Schema(format!("phase2: build record batch: {e}")))?;

    writer
        .write(&batch)
        .map_err(|e| Error::Schema(format!("phase2: write batch: {e}")))?;
    writer
        .close()
        .map_err(|e| Error::Schema(format!("phase2: close writer: {e}")))?;

    debug!(
        "Phase 2: wrote {} node(s) to {:?}",
        records.len(),
        shard_path
    );
    Ok(())
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn skip_phase2_returns_zero_immediately() {
        // When skip_phase2 is true, run() must be a no-op regardless of
        // stage_dir contents (AC-M5-6).
        let clang = crate::visit::shallow::global_clang();
        let dir = tempfile::tempdir().expect("temp dir");
        let result = run(clang, &[], dir.path(), /* skip_phase2 */ true);
        assert_eq!(
            result.unwrap(),
            0,
            "expected 0 decorated nodes when skipped"
        );
    }

    #[test]
    fn empty_stage_dir_produces_zero_decorated() {
        // Phase 2 on an empty stage dir must succeed with 0 decorated nodes.
        let clang = crate::visit::shallow::global_clang();
        let dir = tempfile::tempdir().expect("temp dir");
        let result = run(clang, &[], dir.path(), /* skip_phase2 */ false);
        assert_eq!(result.unwrap(), 0);
    }

    #[test]
    fn patch_attrs_json_merges_fields() {
        let mut node = NodeRecord {
            usr: "c:@F@foo".to_owned(),
            kind: NodeKind::Function,
            name: "foo".to_owned(),
            qualified_name: "foo".to_owned(),
            mangled_name: None,
            file_path: "/tmp/foo.cpp".to_owned(),
            line: Some(1),
            col: Some(1),
            repo_name: "test".to_owned(),
            attrs_json: r#"{"virtual": false}"#.to_owned(),
            partial: false,
            phase: 1,
            tu_hash: [0u8; 32],
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
            symbol_id: 0,
            file_id: 0,
            is_const: None,
            is_constexpr: None,
            storage_class: None,
            // v7 S2 promoted fields — test node carries none
            is_template: None,
            is_noexcept: None,
            is_override: None,
            is_deleted: None,
            is_defaulted: None,
            cv_qualifiers: None,
            ref_qualifier: None,
            is_final: None,
            is_abstract: None,
            record_kind: None,
            type_spelling: None,
            param_index: None,
            param_kind: None,
            enum_value: None,
        };

        let decor = DecorAttrs {
            exception_spec: "noexcept".to_owned(),
            control_flow: "has_return".to_owned(),
        };
        patch_attrs_json(&mut node, &decor);

        let parsed: serde_json::Value = serde_json::from_str(&node.attrs_json).expect("valid JSON");
        assert_eq!(parsed["exception_spec"], "noexcept");
        assert_eq!(parsed["control_flow"], "has_return");
        // Pre-existing field must be preserved.
        assert_eq!(parsed["virtual"], false);
    }

    #[test]
    fn classify_exception_spec_noexcept() {
        // Smoke-test exception spec classification using an in-process parse.
        // We can only test the classifier against a real TU on machines that
        // have libclang — skip gracefully if libclang is unavailable.
        let clang = crate::visit::shallow::global_clang();

        // Write a tiny C++ file to a tempdir.
        let dir = tempfile::tempdir().expect("temp dir");
        let src = dir.path().join("test.cpp");
        std::fs::write(&src, "void foo() noexcept {}\nvoid bar() {}\n").expect("write test src");

        let index = Index::new(clang, true, false);
        let tu = index
            .parser(&src)
            .arguments(&[] as &[String])
            .parse()
            .expect("parse test.cpp");

        let root = tu.get_entity();
        let mut specs: Vec<(String, String)> = Vec::new();
        root.visit_children(|e, _| {
            if matches!(e.get_kind(), EntityKind::FunctionDecl) && e.is_definition() {
                if let Some(u) = e.get_usr() {
                    specs.push((u.0, classify_exception_spec(&e)));
                }
            }
            clang::EntityVisitResult::Continue
        });

        // At least one entry should exist.
        assert!(!specs.is_empty(), "expected at least one function");
        // foo should have noexcept.
        let foo = specs.iter().find(|(u, _)| u.contains("foo"));
        if let Some((_, spec)) = foo {
            assert_eq!(
                spec, "noexcept",
                "foo() noexcept should classify as noexcept"
            );
        }
    }

    #[test]
    fn classify_control_flow_has_return() {
        let clang = crate::visit::shallow::global_clang();

        let dir = tempfile::tempdir().expect("temp dir");
        let src = dir.path().join("cf.cpp");
        std::fs::write(&src, "int foo() { return 42; }\nvoid bar() {}\n").expect("write cf.cpp");

        let index = Index::new(clang, true, false);
        let tu = index
            .parser(&src)
            .arguments(&[] as &[String])
            .parse()
            .expect("parse cf.cpp");

        let root = tu.get_entity();
        let mut flows: Vec<(String, String)> = Vec::new();
        root.visit_children(|e, _| {
            if matches!(e.get_kind(), EntityKind::FunctionDecl) && e.is_definition() {
                if let Some(u) = e.get_usr() {
                    flows.push((u.0, classify_control_flow(&e)));
                }
            }
            clang::EntityVisitResult::Continue
        });

        assert!(!flows.is_empty());
        let foo = flows.iter().find(|(u, _)| u.contains("foo"));
        if let Some((_, cf)) = foo {
            assert_eq!(cf, "has_return");
        }
        let bar = flows.iter().find(|(u, _)| u.contains("bar"));
        if let Some((_, cf)) = bar {
            assert_eq!(cf, "basic");
        }
    }
}
