//! Arrow round-trip integration tests for M8 (S40) promoted fields (AC-S40-4).
//!
//! Three coverage cases per new field:
//! - `Some(non-empty)` — field is populated.
//! - `Some(empty)` — field is present but empty (e.g. zero-length params list).
//! - `None` — field is absent.
//!
//! Tests for the structured list fields (`params`, `template_params`, `template_args`)
//! additionally verify that `Some([])` (empty list) is preserved as `Some([])` and not
//! collapsed to `None` (AC-S40-4 empty-list distinction).

use cpp_indexer::schema::arrow::{
    edges_to_record_batch, nodes_to_record_batch, record_batch_to_edges, record_batch_to_nodes,
};
use cpp_indexer::schema::edges::{EdgeKind, EdgeRecord};
use cpp_indexer::schema::nodes::{NodeKind, NodeRecord, Param, TemplateArg, TemplateParam};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn base_node(suffix: &str) -> NodeRecord {
    NodeRecord {
        usr: format!("c:@F@fn_{suffix}"),
        kind: NodeKind::Function,
        name: format!("fn_{suffix}"),
        qualified_name: format!("ns::fn_{suffix}"),
        file_path: format!("/repo/{suffix}.cpp"),
        line: Some(1),
        col: Some(1),
        repo_name: "test-repo".to_owned(),
        ..Default::default()
    }
}

fn base_edge(suffix: &str, kind: EdgeKind) -> EdgeRecord {
    EdgeRecord {
        src_usr: format!("src_{suffix}"),
        dst_usr: Some(format!("dst_{suffix}")),
        kind,
        resolved: true,
        repo_name: "test-repo".to_owned(),
        dst_repo_name: "test-repo".to_owned(),
        ..Default::default()
    }
}

fn roundtrip_nodes(records: Vec<NodeRecord>) -> Vec<NodeRecord> {
    let batch = nodes_to_record_batch(&records).expect("serialize must succeed");
    record_batch_to_nodes(&batch)
}

fn roundtrip_edges(records: Vec<EdgeRecord>) -> Vec<EdgeRecord> {
    let batch = edges_to_record_batch(&records).expect("serialize must succeed");
    record_batch_to_edges(&batch)
}

// ---------------------------------------------------------------------------
// return_type (AC-S40-4)
// ---------------------------------------------------------------------------

#[test]
fn return_type_some_nonempty() {
    let mut r = base_node("rt_some");
    r.return_type = Some("int".to_owned());
    let got = roundtrip_nodes(vec![r.clone()]);
    assert_eq!(got[0].return_type, Some("int".to_owned()));
}

#[test]
fn return_type_some_empty() {
    let mut r = base_node("rt_empty");
    r.return_type = Some(String::new());
    let got = roundtrip_nodes(vec![r.clone()]);
    assert_eq!(got[0].return_type, Some(String::new()));
}

#[test]
fn return_type_none() {
    let r = base_node("rt_none");
    let got = roundtrip_nodes(vec![r]);
    assert_eq!(got[0].return_type, None);
}

// ---------------------------------------------------------------------------
// params (AC-S40-4) — List<Struct>
// ---------------------------------------------------------------------------

#[test]
fn params_some_nonempty() {
    let mut r = base_node("params_some");
    r.params = Some(vec![
        Param {
            name: "x".to_owned(),
            type_: "int".to_owned(),
        },
        Param {
            name: "s".to_owned(),
            type_: "const std::string &".to_owned(),
        },
    ]);
    let got = roundtrip_nodes(vec![r.clone()]);
    assert_eq!(got[0].params, r.params);
}

#[test]
fn params_some_empty_list_is_not_none() {
    let mut r = base_node("params_empty");
    r.params = Some(vec![]);
    let got = roundtrip_nodes(vec![r]);
    assert_eq!(
        got[0].params,
        Some(vec![]),
        "empty params list must round-trip as Some([]), not None"
    );
}

#[test]
fn params_none() {
    let r = base_node("params_none");
    let got = roundtrip_nodes(vec![r]);
    assert_eq!(got[0].params, None);
}

// ---------------------------------------------------------------------------
// signature (AC-S40-4)
// ---------------------------------------------------------------------------

#[test]
fn signature_some_nonempty() {
    let mut r = base_node("sig_some");
    r.signature = Some("void(int) const".to_owned());
    let got = roundtrip_nodes(vec![r.clone()]);
    assert_eq!(got[0].signature, r.signature);
}

#[test]
fn signature_some_empty() {
    let mut r = base_node("sig_empty");
    r.signature = Some(String::new());
    let got = roundtrip_nodes(vec![r]);
    assert_eq!(got[0].signature, Some(String::new()));
}

#[test]
fn signature_none() {
    let got = roundtrip_nodes(vec![base_node("sig_none")]);
    assert_eq!(got[0].signature, None);
}

// ---------------------------------------------------------------------------
// code / code_truncated (AC-S40-4)
// ---------------------------------------------------------------------------

#[test]
fn code_some_nonempty() {
    let mut r = base_node("code_some");
    r.code = Some("int f() { return 1; }".to_owned());
    r.code_truncated = Some(false);
    let got = roundtrip_nodes(vec![r.clone()]);
    assert_eq!(got[0].code, r.code);
    assert_eq!(got[0].code_truncated, Some(false));
}

#[test]
fn code_truncated_true() {
    let mut r = base_node("code_truncated");
    r.code = None;
    r.code_truncated = Some(true);
    let got = roundtrip_nodes(vec![r]);
    assert_eq!(got[0].code, None);
    assert_eq!(got[0].code_truncated, Some(true));
}

#[test]
fn code_none() {
    let got = roundtrip_nodes(vec![base_node("code_none")]);
    assert_eq!(got[0].code, None);
    assert_eq!(got[0].code_truncated, None);
}

// ---------------------------------------------------------------------------
// template_params (AC-S40-4) — List<Struct>
// ---------------------------------------------------------------------------

#[test]
fn template_params_some_nonempty() {
    let mut r = base_node("tp_some");
    r.template_params = Some(vec![
        TemplateParam {
            name: "T".to_owned(),
            kind: "type".to_owned(),
            default: Some("int".to_owned()),
        },
        TemplateParam {
            name: "N".to_owned(),
            kind: "non_type".to_owned(),
            default: None,
        },
    ]);
    let got = roundtrip_nodes(vec![r.clone()]);
    assert_eq!(got[0].template_params, r.template_params);
}

#[test]
fn template_params_some_empty_list_is_not_none() {
    let mut r = base_node("tp_empty");
    r.template_params = Some(vec![]);
    let got = roundtrip_nodes(vec![r]);
    assert_eq!(got[0].template_params, Some(vec![]));
}

#[test]
fn template_params_none() {
    let got = roundtrip_nodes(vec![base_node("tp_none")]);
    assert_eq!(got[0].template_params, None);
}

/// `default: None` in TemplateParam round-trips as `None`, not as an empty string.
#[test]
fn template_param_default_none_preserved() {
    let mut r = base_node("tp_default_none");
    r.template_params = Some(vec![TemplateParam {
        name: "T".to_owned(),
        kind: "type".to_owned(),
        default: None,
    }]);
    let got = roundtrip_nodes(vec![r]);
    let tp = &got[0].template_params.as_ref().unwrap()[0];
    assert_eq!(
        tp.default, None,
        "TemplateParam.default must round-trip as None"
    );
}

// ---------------------------------------------------------------------------
// template_args (AC-S40-4) — List<Struct>
// ---------------------------------------------------------------------------

#[test]
fn template_args_some_nonempty() {
    let mut r = base_node("ta_some");
    r.template_args = Some(vec![
        TemplateArg {
            kind: "type".to_owned(),
            value: "int".to_owned(),
        },
        TemplateArg {
            kind: "integral".to_owned(),
            value: "42".to_owned(),
        },
    ]);
    let got = roundtrip_nodes(vec![r.clone()]);
    assert_eq!(got[0].template_args, r.template_args);
}

#[test]
fn template_args_some_empty_list_is_not_none() {
    let mut r = base_node("ta_empty");
    r.template_args = Some(vec![]);
    let got = roundtrip_nodes(vec![r]);
    assert_eq!(got[0].template_args, Some(vec![]));
}

#[test]
fn template_args_none() {
    let got = roundtrip_nodes(vec![base_node("ta_none")]);
    assert_eq!(got[0].template_args, None);
}

// ---------------------------------------------------------------------------
// is_virtual / is_pure_virtual / is_static (AC-S40-4)
// ---------------------------------------------------------------------------

#[test]
fn bool_fields_some_true() {
    let mut r = base_node("bool_true");
    r.is_virtual = Some(true);
    r.is_pure_virtual = Some(true);
    r.is_static = Some(true);
    let got = roundtrip_nodes(vec![r]);
    assert_eq!(got[0].is_virtual, Some(true));
    assert_eq!(got[0].is_pure_virtual, Some(true));
    assert_eq!(got[0].is_static, Some(true));
}

#[test]
fn bool_fields_some_false() {
    let mut r = base_node("bool_false");
    r.is_virtual = Some(false);
    r.is_pure_virtual = Some(false);
    r.is_static = Some(false);
    let got = roundtrip_nodes(vec![r]);
    assert_eq!(got[0].is_virtual, Some(false));
    assert_eq!(got[0].is_pure_virtual, Some(false));
    assert_eq!(got[0].is_static, Some(false));
}

#[test]
fn bool_fields_none() {
    let got = roundtrip_nodes(vec![base_node("bool_none")]);
    assert_eq!(got[0].is_virtual, None);
    assert_eq!(got[0].is_pure_virtual, None);
    assert_eq!(got[0].is_static, None);
}

// ---------------------------------------------------------------------------
// Edge: source_association_type / target_association_type (AC-S40-3 / AC-S40-4)
// ---------------------------------------------------------------------------

#[test]
fn edge_assoc_type_some_nonempty() {
    let mut e = base_edge("assoc_some", EdgeKind::Uses);
    e.source_association_type = Some("read".to_owned());
    e.target_association_type = Some("write".to_owned());
    let got = roundtrip_edges(vec![e]);
    assert_eq!(got[0].source_association_type, Some("read".to_owned()));
    assert_eq!(got[0].target_association_type, Some("write".to_owned()));
}

#[test]
fn edge_assoc_type_some_empty() {
    let mut e = base_edge("assoc_empty", EdgeKind::Uses);
    e.source_association_type = Some(String::new());
    e.target_association_type = Some(String::new());
    let got = roundtrip_edges(vec![e]);
    assert_eq!(got[0].source_association_type, Some(String::new()));
    assert_eq!(got[0].target_association_type, Some(String::new()));
}

#[test]
fn edge_assoc_type_none() {
    let e = base_edge("assoc_none", EdgeKind::Calls);
    let got = roundtrip_edges(vec![e]);
    assert_eq!(got[0].source_association_type, None);
    assert_eq!(got[0].target_association_type, None);
}

// ---------------------------------------------------------------------------
// AC-S40-6 structural check: promoted fields must not appear in attrs_json
// ---------------------------------------------------------------------------

#[test]
fn promoted_fields_absent_from_attrs_json() {
    let mut r = base_node("no_double_write");
    r.return_type = Some("void".to_owned());
    r.is_virtual = Some(true);
    r.is_static = Some(false);
    r.params = Some(vec![Param {
        name: "n".to_owned(),
        type_: "int".to_owned(),
    }]);

    let promoted = [
        "return_type",
        "params",
        "signature",
        "code",
        "code_truncated",
        "template_params",
        "template_args",
        "is_virtual",
        "is_pure_virtual",
        "is_static",
    ];
    for key in promoted {
        assert!(
            !r.attrs_json.contains(key),
            "promoted field '{key}' must NOT appear in attrs_json (AC-S40-6)"
        );
    }
}
