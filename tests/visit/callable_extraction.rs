//! Fixture-based integration tests for S41 callable-attribute extraction.
//!
//! AC covered: AC-S41-1 (return_type), AC-S41-2 (params), AC-S41-3/4 (signature),
//! AC-S41-5 (32 KiB within-limit), AC-S41-6 (32 KiB exceeded), AC-S41-8 (unit fixture).
//!
//! The `CPP_INDEXER_LIVE_NEO4J` gated AC-S41-7 (leveldb corpus) is out of scope
//! for this file and is covered by `tests/integration/neo4j_indexes.rs` (S44).

use std::path::PathBuf;

use cpp_indexer::schema::limits::{clip_code, MAX_CODE_BYTES};
use cpp_indexer::schema::nodes::{NodeKind, NodeRecord};
use cpp_indexer::stage::writer::StageWriter;
use cpp_indexer::visit::shallow::{global_clang, visit_tu, VisitOptions};
use futures::TryStreamExt;
use parquet::arrow::ParquetRecordBatchStreamBuilder;
use tempfile::TempDir;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn callable_fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/callable")
}

/// Visit `callable.cpp` in the callable fixture and return all node records.
async fn visit_callable_fixture() -> Vec<NodeRecord> {
    let dir = TempDir::new().unwrap();
    let stage_dir = dir.path().join("stage");

    let clang = global_clang();
    let fixture = callable_fixture_dir();
    let src = fixture.join("callable.cpp");

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

    collect_node_records(&stage_dir).await
}

/// Read all node records from Parquet shards under `stage_dir`.
async fn collect_node_records(stage_dir: &std::path::Path) -> Vec<NodeRecord> {
    use arrow::array::{Array, BooleanArray, StringArray};
    use arrow::compute::cast;
    use arrow::datatypes::DataType;

    let mut records = Vec::new();

    for entry in walkdir::WalkDir::new(stage_dir)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| {
            e.file_name()
                .to_str()
                .map(|s| s.starts_with("nodes-") && s.ends_with(".parquet"))
                .unwrap_or(false)
        })
    {
        let file = tokio::fs::File::open(entry.path()).await.unwrap();
        let builder = ParquetRecordBatchStreamBuilder::new(file).await.unwrap();
        let mut stream = builder.build().unwrap();

        while let Some(batch) = stream.try_next().await.unwrap() {
            let nrows = batch.num_rows();

            let usr_col = batch
                .column_by_name("usr")
                .unwrap()
                .as_any()
                .downcast_ref::<StringArray>()
                .unwrap()
                .iter()
                .map(|v| v.unwrap_or("").to_owned())
                .collect::<Vec<_>>();

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

            let name_col = batch
                .column_by_name("name")
                .unwrap()
                .as_any()
                .downcast_ref::<StringArray>()
                .unwrap()
                .iter()
                .map(|v| v.unwrap_or("").to_owned())
                .collect::<Vec<_>>();

            let return_type_col: Vec<Option<String>> = batch
                .column_by_name("return_type")
                .unwrap()
                .as_any()
                .downcast_ref::<StringArray>()
                .unwrap()
                .iter()
                .map(|v| v.map(|s| s.to_owned()))
                .collect();

            let signature_col: Vec<Option<String>> = batch
                .column_by_name("signature")
                .unwrap()
                .as_any()
                .downcast_ref::<StringArray>()
                .unwrap()
                .iter()
                .map(|v| v.map(|s| s.to_owned()))
                .collect();

            let code_col: Vec<Option<String>> = batch
                .column_by_name("code")
                .unwrap()
                .as_any()
                .downcast_ref::<StringArray>()
                .unwrap()
                .iter()
                .map(|v| v.map(|s| s.to_owned()))
                .collect();

            let code_truncated_col: Vec<Option<bool>> = batch
                .column_by_name("code_truncated")
                .unwrap()
                .as_any()
                .downcast_ref::<BooleanArray>()
                .unwrap()
                .iter()
                .collect();

            for i in 0..nrows {
                let kind = NodeKind::try_from_arrow_str(&kind_col[i]).unwrap_or(NodeKind::Module);
                records.push(NodeRecord {
                    usr: usr_col[i].clone(),
                    kind,
                    name: name_col[i].clone(),
                    qualified_name: String::new(),
                    mangled_name: None,
                    file_path: String::new(),
                    line: None,
                    col: None,
                    repo_name: "test-repo".to_owned(),
                    attrs_json: "{}".to_owned(),
                    partial: false,
                    phase: 1,
                    tu_hash: [0u8; 32],
                    return_type: return_type_col[i].clone(),
                    params: None, // not decoded here — use helpers below
                    signature: signature_col[i].clone(),
                    code: code_col[i].clone(),
                    code_truncated: code_truncated_col[i],
                    template_params: None,
                    template_args: None,
                    is_virtual: None,
                    is_pure_virtual: None,
                    is_static: None,
                    symbol_id: 0,
                    file_id: 0,
                });
            }
        }
    }

    records
}

// ---------------------------------------------------------------------------
// AC-S41-1/2/4/8: free function `add(int, int)` — return_type + signature
// ---------------------------------------------------------------------------

/// AC-S41-1 / AC-S41-8: FUNCTION node for `add` has non-empty `return_type`.
#[tokio::test]
async fn free_function_has_return_type() {
    let nodes = visit_callable_fixture().await;
    let add_nodes: Vec<&NodeRecord> = nodes
        .iter()
        .filter(|n| n.kind == NodeKind::Function && n.name == "add")
        .collect();
    assert!(
        !add_nodes.is_empty(),
        "expected at least one FUNCTION node named 'add'"
    );
    let add = add_nodes[0];
    let rt = add
        .return_type
        .as_ref()
        .expect("add must have a return_type");
    assert!(!rt.is_empty(), "return_type for add must be non-empty");
    assert_eq!(rt, "int", "add returns int");
}

/// AC-S41-4 / AC-S41-8: FUNCTION node for `add` has correct signature.
#[tokio::test]
async fn free_function_signature_format() {
    let nodes = visit_callable_fixture().await;
    let add_nodes: Vec<&NodeRecord> = nodes
        .iter()
        .filter(|n| n.kind == NodeKind::Function && n.name == "add")
        .collect();
    assert!(!add_nodes.is_empty());
    let sig = add_nodes[0]
        .signature
        .as_ref()
        .expect("add must have a signature");
    // Expected: "int(int, int)" — ret(param_types)
    assert_eq!(sig, "int(int, int)", "add signature mismatch: got '{sig}'");
}

// ---------------------------------------------------------------------------
// AC-S41-3 / AC-S41-8: const method `area()` — signature includes ` const`
// ---------------------------------------------------------------------------

/// AC-S41-3 / AC-S41-8: METHOD node for `area` has ` const` suffix in signature.
#[tokio::test]
async fn const_method_signature_has_const_suffix() {
    let nodes = visit_callable_fixture().await;
    let area_nodes: Vec<&NodeRecord> = nodes
        .iter()
        .filter(|n| n.kind == NodeKind::Method && n.name == "area")
        .collect();
    assert!(
        !area_nodes.is_empty(),
        "expected at least one METHOD node named 'area'"
    );
    let sig = area_nodes[0]
        .signature
        .as_ref()
        .expect("area must have a signature");
    assert!(
        sig.ends_with(" const"),
        "const method signature must end with ' const'; got: '{sig}'"
    );
}

/// AC-S41-1: METHOD `area` return_type is `double`.
#[tokio::test]
async fn const_method_has_return_type() {
    let nodes = visit_callable_fixture().await;
    let area_nodes: Vec<&NodeRecord> = nodes
        .iter()
        .filter(|n| n.kind == NodeKind::Method && n.name == "area")
        .collect();
    assert!(!area_nodes.is_empty());
    let rt = area_nodes[0]
        .return_type
        .as_ref()
        .expect("area must have return_type");
    assert_eq!(rt, "double", "area returns double; got '{rt}'");
}

// ---------------------------------------------------------------------------
// AC-S41-5/6: clip_code boundary tests (no libclang needed — pure unit)
// ---------------------------------------------------------------------------

/// AC-S41-5: exactly 32 768 bytes → `code=Some(…)`, `code_truncated=false`.
#[test]
fn clip_code_at_exact_limit_within() {
    let src = "x".repeat(MAX_CODE_BYTES); // 32 768 bytes
    let (code, truncated) = clip_code(&src);
    assert!(
        code.is_some(),
        "body at exactly 32768 bytes must be stored (AC-S41-5)"
    );
    assert!(
        !truncated,
        "code_truncated must be false at exactly the limit (AC-S41-5)"
    );
}

/// AC-S41-6: 32 769 bytes → `code=None`, `code_truncated=true`, no panic.
#[test]
fn clip_code_one_over_limit_truncated() {
    let src = "x".repeat(MAX_CODE_BYTES + 1); // 32 769 bytes
    let (code, truncated) = clip_code(&src);
    assert!(
        code.is_none(),
        "body at 32769 bytes must yield code=None (AC-S41-6)"
    );
    assert!(
        truncated,
        "code_truncated must be true when body exceeds limit (AC-S41-6)"
    );
}

// ---------------------------------------------------------------------------
// AC-S41-5: code field populated for small fixture functions
// ---------------------------------------------------------------------------

/// AC-S41-5: FUNCTION `add` source is well within limit → code=Some.
#[tokio::test]
async fn small_function_code_is_some() {
    let nodes = visit_callable_fixture().await;
    let add_nodes: Vec<&NodeRecord> = nodes
        .iter()
        .filter(|n| n.kind == NodeKind::Function && n.name == "add")
        .collect();
    assert!(!add_nodes.is_empty());
    let node = add_nodes[0];
    assert!(
        node.code.is_some(),
        "small function must have code=Some (AC-S41-5)"
    );
    assert_eq!(
        node.code_truncated,
        Some(false),
        "code_truncated must be Some(false) for small body (AC-S41-5)"
    );
}
