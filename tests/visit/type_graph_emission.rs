//! Visitor-level tests for v7 S2 type-graph emission.
//!
//! Tests that visit() emits:
//! - RETURNS edges from Function/Method to Type nodes
//! - HAS_PARAM edges (with edge_index) from Function to Parameter nodes
//! - OF_TYPE edges from Parameter and Field to Type nodes
//! - Type nodes with type_spelling set
//! - Parameter nodes with param_index set
//! - is_template = Some(true) for TemplateDef nodes
//! - POINTS_TO / REFERS_TO for pointer/reference types

use cpp_indexer::schema::{
    arrow::{record_batch_to_edges, record_batch_to_nodes},
    edges::EdgeKind,
    nodes::NodeKind,
};
use cpp_indexer::stage::writer::StageWriter;
use cpp_indexer::visit::shallow::{global_clang, visit_tu, VisitOptions};
use futures::TryStreamExt;
use parquet::arrow::ParquetRecordBatchStreamBuilder;
use tempfile::TempDir;

/// Parse a C++ snippet from a tempfile; return (nodes, edges).
async fn visit_snippet(
    src: &str,
    std_flag: &str,
) -> (
    Vec<cpp_indexer::schema::NodeRecord>,
    Vec<cpp_indexer::schema::EdgeRecord>,
) {
    let dir = TempDir::new().unwrap();
    let stage_dir = dir.path().join("stage");

    let src_file = dir.path().join("test.cpp");
    std::fs::write(&src_file, src).unwrap();

    let clang = global_clang();
    let opts = VisitOptions {
        repo_name: "test-repo",
        tu_hash: [0u8; 32],
        file_path: &src_file,
        args: &["clang++".to_owned(), format!("-std={std_flag}")],
        skip_system_headers: true,
        allocator: None,
    };

    let mut writer = StageWriter::new(&stage_dir, 0).unwrap();
    let _ = visit_tu(clang, &opts, &mut writer).unwrap();
    writer.finish().unwrap();

    let mut nodes = Vec::new();
    let mut edges = Vec::new();

    for entry in walkdir::WalkDir::new(&stage_dir)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| {
            e.file_name()
                .to_str()
                .map(|s| s.ends_with(".parquet"))
                .unwrap_or(false)
        })
    {
        let fname = entry.file_name().to_str().unwrap_or("").to_owned();
        let file = tokio::fs::File::open(entry.path()).await.unwrap();
        let builder = ParquetRecordBatchStreamBuilder::new(file).await.unwrap();
        let mut stream = builder.build().unwrap();

        while let Some(batch) = stream.try_next().await.unwrap() {
            if fname.starts_with("nodes-") {
                nodes.extend(record_batch_to_nodes(&batch));
            } else if fname.starts_with("edges-") {
                edges.extend(record_batch_to_edges(&batch));
            }
        }
    }

    (nodes, edges)
}

/// S2-01: RETURNS edge emitted for a simple function.
#[tokio::test]
async fn returns_edge_emitted() {
    let (nodes, edges) = visit_snippet("int add(int a, int b) { return a + b; }", "c++14").await;

    let fn_node = nodes
        .iter()
        .find(|n| n.name == "add")
        .expect("add function must be present");
    let returns_edge = edges
        .iter()
        .find(|e| e.kind == EdgeKind::Returns && e.src_usr == fn_node.usr);
    assert!(
        returns_edge.is_some(),
        "RETURNS edge from add() must be present"
    );

    // The destination USR must match a Type node with spelling "int".
    let ret_dst_usr = returns_edge.unwrap().dst_usr.as_deref().unwrap();
    let type_node = nodes
        .iter()
        .find(|n| n.kind == NodeKind::Type && n.usr == ret_dst_usr);
    assert!(type_node.is_some(), "Type node for return type must exist");
    assert_eq!(type_node.unwrap().type_spelling, Some("int".to_owned()));
}

/// S2-02: HAS_PARAM edges with correct edge_index emitted.
#[tokio::test]
async fn has_param_edges_with_index() {
    let (nodes, edges) = visit_snippet("void f(int a, double b, float c) {}", "c++14").await;

    let fn_node = nodes
        .iter()
        .find(|n| n.name == "f")
        .expect("f must be present");
    let has_param_edges: Vec<_> = edges
        .iter()
        .filter(|e| e.kind == EdgeKind::HasParam && e.src_usr == fn_node.usr)
        .collect();

    assert_eq!(has_param_edges.len(), 3, "f has 3 parameters");

    // Verify edge_index values are 0, 1, 2.
    let mut indices: Vec<i64> = has_param_edges
        .iter()
        .map(|e| e.edge_index.unwrap())
        .collect();
    indices.sort();
    assert_eq!(indices, vec![0, 1, 2]);

    // Verify Parameter nodes exist with correct param_index.
    let param_nodes: Vec<_> = nodes
        .iter()
        .filter(|n| n.kind == NodeKind::Parameter)
        .collect();
    assert_eq!(param_nodes.len(), 3, "3 Parameter nodes must be emitted");
    let mut param_indices: Vec<i64> = param_nodes.iter().map(|n| n.param_index.unwrap()).collect();
    param_indices.sort();
    assert_eq!(param_indices, vec![0, 1, 2]);
}

/// S2-03: Zero-parameter function emits RETURNS but no HAS_PARAM.
#[tokio::test]
async fn zero_param_function_has_returns_no_hasparam() {
    let (nodes, edges) = visit_snippet("void nothing() {}", "c++14").await;

    let fn_node = nodes
        .iter()
        .find(|n| n.name == "nothing")
        .expect("nothing must be present");
    let has_param_edges: Vec<_> = edges
        .iter()
        .filter(|e| e.kind == EdgeKind::HasParam && e.src_usr == fn_node.usr)
        .collect();
    assert_eq!(
        has_param_edges.len(),
        0,
        "0-param function must have no HAS_PARAM edges"
    );

    let returns_edge = edges
        .iter()
        .find(|e| e.kind == EdgeKind::Returns && e.src_usr == fn_node.usr);
    assert!(
        returns_edge.is_some(),
        "RETURNS edge still present for void function"
    );
}

/// S2-04: OF_TYPE edges from Parameter and Field to Type.
#[tokio::test]
async fn of_type_edges_for_param_and_field() {
    let src = "
struct Foo { int x; };
void bar(int y) {}
";
    let (nodes, edges) = visit_snippet(src, "c++14").await;

    // OF_TYPE from the parameter.
    let of_type_edges: Vec<_> = edges
        .iter()
        .filter(|e| e.kind == EdgeKind::OfType)
        .collect();
    assert!(!of_type_edges.is_empty(), "OF_TYPE edges must be emitted");

    // Parameter's OF_TYPE → Type("int")
    let param_nodes: Vec<_> = nodes
        .iter()
        .filter(|n| n.kind == NodeKind::Parameter)
        .collect();
    assert!(!param_nodes.is_empty(), "Parameter node for y must exist");
    let param_of_type = of_type_edges
        .iter()
        .find(|e| param_nodes.iter().any(|p| p.usr == e.src_usr));
    assert!(
        param_of_type.is_some(),
        "OF_TYPE edge from Parameter must exist"
    );

    // Field's OF_TYPE → Type("int")
    let field_node = nodes
        .iter()
        .find(|n| n.kind == NodeKind::Field && n.name == "x");
    assert!(field_node.is_some(), "Field node x must exist");
    let field_of_type = of_type_edges
        .iter()
        .find(|e| e.src_usr == field_node.unwrap().usr);
    assert!(
        field_of_type.is_some(),
        "OF_TYPE edge from Field x must exist"
    );
}

/// S2-06: Type node dedup — same spelling → one node.
#[tokio::test]
async fn type_node_dedup_same_spelling() {
    let src = "
int f1() { return 1; }
int f2() { return 2; }
";
    let (nodes, _) = visit_snippet(src, "c++14").await;

    let int_type_nodes: Vec<_> = nodes
        .iter()
        .filter(|n| n.kind == NodeKind::Type && n.type_spelling == Some("int".to_owned()))
        .collect();

    assert_eq!(
        int_type_nodes.len(),
        1,
        "same spelling 'int' must deduplicate to one Type node"
    );
}

/// is_template = true for TemplateDef nodes.
#[tokio::test]
async fn is_template_true_for_template_def() {
    let src = "template<class T> T identity(T x) { return x; }";
    let (nodes, _) = visit_snippet(src, "c++14").await;

    let tdef = nodes.iter().find(|n| n.kind == NodeKind::TemplateDef);
    assert!(
        tdef.is_some(),
        "TemplateDef node must be emitted for template<class T> identity"
    );
    assert_eq!(
        tdef.unwrap().is_template,
        Some(true),
        "TemplateDef node must have is_template = Some(true)"
    );
}
