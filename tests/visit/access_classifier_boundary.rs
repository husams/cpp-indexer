//! Boundary tests for S43 access-classifier — QA-added (M8 qa-engineer pass).
//!
//! The primary `access_classifier.rs` covers: read, write, call_arg, unknown.
//! These tests extend end-to-end coverage to the remaining 3 variants:
//!   addr_of, return, decl_ref
//!
//! AC covered:
//!   AC-S43-1 — closed 7-value enum; no out-of-band value escapes the sink.
//!   AC-S43-6 — classifier unit tests cover all 7 access modes (this file
//!               adds the 3 not covered by access_classifier.rs).
//!
//! Design: parse `tests/fixtures/access_classifier/access_extended.cpp`
//! (QA-added fixture), collect USES edges, assert at least one edge per mode.

use std::path::PathBuf;

use arrow::array::StringArray;
use arrow::compute::cast;
use arrow::datatypes::DataType;
use cpp_indexer::schema::edges::{EdgeKind, EdgeRecord};
use cpp_indexer::stage::writer::StageWriter;
use cpp_indexer::visit::shallow::{global_clang, visit_tu, VisitOptions};
use futures::TryStreamExt;
use parquet::arrow::ParquetRecordBatchStreamBuilder;
use tempfile::TempDir;

// ---------------------------------------------------------------------------
// Helpers (mirrors access_classifier.rs pattern)
// ---------------------------------------------------------------------------

fn access_fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/access_classifier")
}

async fn visit_extended_fixture() -> Vec<EdgeRecord> {
    let dir = TempDir::new().unwrap();
    let stage_dir = dir.path().join("stage");

    let clang = global_clang();
    let fixture = access_fixture_dir();
    let src = fixture.join("access_extended.cpp");

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
// AC-S43-6 boundary: addr_of
// ---------------------------------------------------------------------------

/// AC-S43-6: address-of operator (&x) → source_association_type = "addr_of".
///
/// `addr_of_var` in access_extended.cpp wraps `global_val` in a UnaryOperator(&).
/// The classifier should match the UnaryOperator parent and return AddrOf.
#[tokio::test]
async fn addr_of_operator_produces_addr_of_classification() {
    let edges = visit_extended_fixture().await;
    let addr_of_edges: Vec<&EdgeRecord> = edges
        .iter()
        .filter(|e| {
            e.kind == EdgeKind::Uses && e.source_association_type.as_deref() == Some("addr_of")
        })
        .collect();

    assert!(
        !addr_of_edges.is_empty(),
        "expected at least one USES edge with source_association_type='addr_of' \
         from addr_of_var fixture (&global_val); got all USES classifications: {:?}",
        edges
            .iter()
            .filter(|e| e.kind == EdgeKind::Uses)
            .map(|e| e.source_association_type.as_deref())
            .collect::<Vec<_>>()
    );
}

// ---------------------------------------------------------------------------
// AC-S43-6 boundary: return
// ---------------------------------------------------------------------------

/// AC-S43-6: operand of return statement → source_association_type = "return".
///
/// `return_var` in access_extended.cpp has `return global_val;`.
/// The classifier should match the ReturnStmt parent and return Return.
#[tokio::test]
async fn return_operand_produces_return_classification() {
    let edges = visit_extended_fixture().await;
    let return_edges: Vec<&EdgeRecord> = edges
        .iter()
        .filter(|e| {
            e.kind == EdgeKind::Uses && e.source_association_type.as_deref() == Some("return")
        })
        .collect();

    assert!(
        !return_edges.is_empty(),
        "expected at least one USES edge with source_association_type='return' \
         from return_var fixture (return global_val); got all USES classifications: {:?}",
        edges
            .iter()
            .filter(|e| e.kind == EdgeKind::Uses)
            .map(|e| e.source_association_type.as_deref())
            .collect::<Vec<_>>()
    );
}

// ---------------------------------------------------------------------------
// AC-S43-6 boundary: decl_ref
// ---------------------------------------------------------------------------

/// AC-S43-6: VarDecl initializer → source_association_type = "decl_ref".
///
/// `decl_ref_var` in access_extended.cpp has `int x = global_val;`.
/// The classifier should match the VarDecl parent and return DeclRef.
#[tokio::test]
async fn decl_initializer_produces_decl_ref_classification() {
    let edges = visit_extended_fixture().await;
    let decl_ref_edges: Vec<&EdgeRecord> = edges
        .iter()
        .filter(|e| {
            e.kind == EdgeKind::Uses && e.source_association_type.as_deref() == Some("decl_ref")
        })
        .collect();

    assert!(
        !decl_ref_edges.is_empty(),
        "expected at least one USES edge with source_association_type='decl_ref' \
         from decl_ref_var fixture (int x = global_val); got all USES classifications: {:?}",
        edges
            .iter()
            .filter(|e| e.kind == EdgeKind::Uses)
            .map(|e| e.source_association_type.as_deref())
            .collect::<Vec<_>>()
    );
}

// ---------------------------------------------------------------------------
// AC-S43-1: no out-of-band values escape (extended fixture)
// ---------------------------------------------------------------------------

/// AC-S43-1: all USES edges in the extended fixture carry only valid
/// classification strings.  Complements the same check in access_classifier.rs.
#[tokio::test]
async fn extended_fixture_no_out_of_band_classification() {
    use cpp_indexer::visit::access_classifier::AccessKind;
    let valid: std::collections::HashSet<&str> =
        AccessKind::all().iter().map(|v| v.as_str()).collect();

    let edges = visit_extended_fixture().await;
    for e in edges.iter().filter(|e| e.kind == EdgeKind::Uses) {
        if let Some(ref sat) = e.source_association_type {
            assert!(
                valid.contains(sat.as_str()),
                "extended fixture: USES edge has out-of-band \
                 source_association_type='{sat}'"
            );
        }
        if let Some(ref tat) = e.target_association_type {
            assert!(
                valid.contains(tat.as_str()),
                "extended fixture: USES edge has out-of-band \
                 target_association_type='{tat}'"
            );
        }
    }
}
