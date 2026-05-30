//! Fixture-based integration tests for S43 USES edge access-mode classification.
//!
//! AC covered:
//!   AC-S43-1 — closed 7-value enum; `AccessKind::all()` returns exactly 7.
//!   AC-S43-2 — target_association_type mirrors source (symmetric copy).
//!   AC-S43-3 — Unknown does not drop the edge (edge still emitted).
//!   AC-S43-4 — association type fields appear as native edge properties.
//!   AC-S43-6 — unit tests cover read / write / call_arg / unknown modes.
//!
//! The `CPP_INDEXER_LIVE_NEO4J` gated AC-S43-5 (leveldb corpus) is covered by
//! `tests/integration/neo4j_indexes.rs` (S44).

use std::path::PathBuf;

use arrow::array::{Array, StringArray};
use arrow::compute::cast;
use arrow::datatypes::DataType;
use cpp_indexer::schema::edges::{EdgeKind, EdgeRecord};
use cpp_indexer::stage::writer::StageWriter;
use cpp_indexer::visit::access_classifier::AccessKind;
use cpp_indexer::visit::shallow::{global_clang, visit_tu, VisitOptions};
use futures::TryStreamExt;
use parquet::arrow::ParquetRecordBatchStreamBuilder;
use tempfile::TempDir;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn access_fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/access_classifier")
}

/// Visit `access.cpp` and return all edge records.
async fn visit_access_fixture() -> Vec<EdgeRecord> {
    let dir = TempDir::new().unwrap();
    let stage_dir = dir.path().join("stage");

    let clang = global_clang();
    let fixture = access_fixture_dir();
    let src = fixture.join("access.cpp");

    let opts = VisitOptions {
        repo_name: "test-repo",
        tu_hash: [0u8; 32],
        file_path: &src,
        args: &[
            "clang++".to_owned(),
            "-std=c++14".to_owned(),
            format!("-I{}", fixture.display()),
        ],
        skip_system_headers: true,
        allocator: None,
    };

    let mut writer = StageWriter::new(&stage_dir, 0).unwrap();
    let _ = visit_tu(clang, &opts, &mut writer).unwrap();
    writer.finish().unwrap();

    collect_edge_records(&stage_dir).await
}

/// Read all edge records from Parquet shards under `stage_dir`.
async fn collect_edge_records(stage_dir: &std::path::Path) -> Vec<EdgeRecord> {
    let mut records = Vec::new();

    for entry in walkdir::WalkDir::new(stage_dir)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| {
            e.file_name()
                .to_str()
                .map(|s| s.starts_with("edges-") && s.ends_with(".parquet"))
                .unwrap_or(false)
        })
    {
        let file = tokio::fs::File::open(entry.path()).await.unwrap();
        let builder = ParquetRecordBatchStreamBuilder::new(file).await.unwrap();
        let mut stream = builder.build().unwrap();

        while let Some(batch) = stream.try_next().await.unwrap() {
            let nrows = batch.num_rows();

            // kind column (Dictionary → Utf8)
            let kind_col = {
                let raw = batch.column_by_name("kind").unwrap();
                let utf8 = cast(raw, &DataType::Utf8).unwrap();
                utf8.as_any()
                    .downcast_ref::<StringArray>()
                    .unwrap()
                    .iter()
                    .map(|v| v.unwrap_or("").to_owned())
                    .collect::<Vec<_>>()
            };

            let src_usr_col = batch
                .column_by_name("src_usr")
                .unwrap()
                .as_any()
                .downcast_ref::<StringArray>()
                .unwrap()
                .iter()
                .map(|v| v.unwrap_or("").to_owned())
                .collect::<Vec<_>>();

            let dst_usr_col: Vec<Option<String>> = batch
                .column_by_name("dst_usr")
                .unwrap()
                .as_any()
                .downcast_ref::<StringArray>()
                .unwrap()
                .iter()
                .map(|v| v.map(|s| s.to_owned()))
                .collect();

            let src_assoc_col: Vec<Option<String>> = batch
                .column_by_name("source_association_type")
                .unwrap()
                .as_any()
                .downcast_ref::<StringArray>()
                .unwrap()
                .iter()
                .map(|v| v.map(|s| s.to_owned()))
                .collect();

            let dst_assoc_col: Vec<Option<String>> = batch
                .column_by_name("target_association_type")
                .unwrap()
                .as_any()
                .downcast_ref::<StringArray>()
                .unwrap()
                .iter()
                .map(|v| v.map(|s| s.to_owned()))
                .collect();

            for i in 0..nrows {
                let kind = EdgeKind::try_from_arrow_str(&kind_col[i]).unwrap_or(EdgeKind::Contains);
                records.push(EdgeRecord {
                    src_usr: src_usr_col[i].clone(),
                    dst_usr: dst_usr_col[i].clone(),
                    dst_placeholder: None,
                    kind,
                    resolved: false,
                    cross_repo_candidate: false,
                    repo_name: "test-repo".to_owned(),
                    attrs_json: "{}".to_owned(),
                    tu_hash: [0u8; 32],
                    source_association_type: src_assoc_col[i].clone(),
                    target_association_type: dst_assoc_col[i].clone(),
                    src_id: 0,
                    dst_id: None,
                    dst_repo_name: "test-repo".to_owned(),
                });
            }
        }
    }

    records
}

// ---------------------------------------------------------------------------
// AC-S43-1: exactly 7 variants in the closed enum
// ---------------------------------------------------------------------------

/// AC-S43-1: AccessKind::all() returns exactly 7 variants.
#[test]
fn access_kind_has_exactly_7_variants() {
    assert_eq!(AccessKind::all().len(), 7);
}

/// AC-S43-1: each canonical string is distinct (no duplicates).
#[test]
fn access_kind_strings_are_unique() {
    let mut strs: Vec<&str> = AccessKind::all().iter().map(|v| v.as_str()).collect();
    strs.sort_unstable();
    strs.dedup();
    assert_eq!(strs.len(), 7);
}

// ---------------------------------------------------------------------------
// AC-S43-4: association fields appear on USES edges (native property, not attrs_json)
// ---------------------------------------------------------------------------

/// AC-S43-4: USES edges carry source_association_type (Some, not None).
#[tokio::test]
async fn uses_edges_have_association_type_set() {
    let edges = visit_access_fixture().await;
    let uses_edges: Vec<&EdgeRecord> = edges.iter().filter(|e| e.kind == EdgeKind::Uses).collect();
    assert!(
        !uses_edges.is_empty(),
        "access.cpp must produce at least one USES edge"
    );
    for e in &uses_edges {
        assert!(
            e.source_association_type.is_some(),
            "USES edge src_usr={} dst_usr={:?} must have source_association_type set",
            e.src_usr,
            e.dst_usr
        );
    }
}

/// AC-S43-4 (src_usr quality): USES edges are sourced from an enclosing function/method USR,
/// not the TU module node.  Verifies the semantic-parent walk in emit_uses_edge.
#[tokio::test]
async fn uses_edges_src_usr_is_enclosing_function() {
    let edges = visit_access_fixture().await;
    let uses_edges: Vec<&EdgeRecord> = edges.iter().filter(|e| e.kind == EdgeKind::Uses).collect();
    assert!(
        !uses_edges.is_empty(),
        "access.cpp must produce at least one USES edge"
    );
    for e in &uses_edges {
        assert!(
            !e.src_usr.starts_with("tu:"),
            "USES edge src_usr must be an enclosing function USR, not the TU node; \
             got src_usr={} dst_usr={:?}",
            e.src_usr,
            e.dst_usr
        );
    }
}

/// AC-S43-2: target_association_type mirrors source_association_type on every USES edge.
#[tokio::test]
async fn target_mirrors_source_association_type() {
    let edges = visit_access_fixture().await;
    for e in edges.iter().filter(|e| e.kind == EdgeKind::Uses) {
        assert_eq!(
            e.source_association_type, e.target_association_type,
            "target must mirror source for USES edge src={} dst={:?}",
            e.src_usr, e.dst_usr
        );
    }
}

// ---------------------------------------------------------------------------
// AC-S43-6: classifier unit tests (fixture-based)
// ---------------------------------------------------------------------------

/// AC-S43-6: plain variable read → "read".
///
/// `read_member` uses a Counter object as the base of a member access (`c.value`);
/// the DeclRefExpr for the base object (`c`) has parent `MemberRefExpr` and is
/// classified as `read` (reading the object to access its member).
#[tokio::test]
async fn plain_read_produces_read_classification() {
    let edges = visit_access_fixture().await;
    let uses: Vec<&EdgeRecord> = edges
        .iter()
        .filter(|e| {
            e.kind == EdgeKind::Uses && e.source_association_type.as_deref() == Some("read")
        })
        .collect();
    assert!(
        !uses.is_empty(),
        "expected at least one USES edge with source_association_type='read' \
         from read_member fixture (base-object-of-member-access pattern)"
    );
}

/// AC-S43-6: assignment to field → "write".
///
/// `field_write` assigns `v` to `c.value` — MemberRefExpr on LHS of BinaryOperator.
#[tokio::test]
async fn field_assignment_produces_write_classification() {
    let edges = visit_access_fixture().await;
    let uses: Vec<&EdgeRecord> = edges
        .iter()
        .filter(|e| {
            e.kind == EdgeKind::Uses && e.source_association_type.as_deref() == Some("write")
        })
        .collect();
    assert!(
        !uses.is_empty(),
        "expected at least one USES edge with source_association_type='write' \
         from field_write fixture"
    );
}

/// AC-S43-6: variable passed as function argument → "call_arg".
///
/// `arg_pass(int x)` calls `take_ref(x)` — DeclRefExpr inside CallExpr.
#[tokio::test]
async fn function_arg_produces_call_arg_classification() {
    let edges = visit_access_fixture().await;
    let uses: Vec<&EdgeRecord> = edges
        .iter()
        .filter(|e| {
            e.kind == EdgeKind::Uses && e.source_association_type.as_deref() == Some("call_arg")
        })
        .collect();
    assert!(
        !uses.is_empty(),
        "expected at least one USES edge with source_association_type='call_arg' \
         from arg_pass fixture"
    );
}

/// AC-S43-6: overloaded operator → "unknown"; edge is NOT dropped (AC-S43-3).
#[tokio::test]
async fn overloaded_operator_produces_unknown_and_edge_not_dropped() {
    let edges = visit_access_fixture().await;
    // There must be at least one "unknown" edge (overloaded_op fixture).
    let unknown_uses: Vec<&EdgeRecord> = edges
        .iter()
        .filter(|e| {
            e.kind == EdgeKind::Uses && e.source_association_type.as_deref() == Some("unknown")
        })
        .collect();
    assert!(
        !unknown_uses.is_empty(),
        "expected at least one USES edge with source_association_type='unknown' \
         from overloaded_op fixture — edge must NOT be dropped (AC-S43-3)"
    );
}

/// AC-S43-1: no USES edge carries a value outside the 7 allowed strings.
#[tokio::test]
async fn no_uses_edge_carries_out_of_band_classification() {
    let valid: std::collections::HashSet<&str> =
        AccessKind::all().iter().map(|v| v.as_str()).collect();

    let edges = visit_access_fixture().await;
    for e in edges.iter().filter(|e| e.kind == EdgeKind::Uses) {
        if let Some(ref sat) = e.source_association_type {
            assert!(
                valid.contains(sat.as_str()),
                "USES edge has out-of-band source_association_type='{sat}'"
            );
        }
        if let Some(ref tat) = e.target_association_type {
            assert!(
                valid.contains(tat.as_str()),
                "USES edge has out-of-band target_association_type='{tat}'"
            );
        }
    }
}
