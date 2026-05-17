/// Arrow schemas and `NodeRecord`/`EdgeRecord` ↔ `RecordBatch` round-trip (AC-M1-4, ADR-3).
///
/// Column layout follows ADR-3 exactly. The `kind` column uses a `Dictionary<Int8, Utf8>` for
/// compact storage. `tu_hash` is `FixedSizeBinary(32)`.
use std::sync::Arc;

use arrow::{
    array::{
        Array, ArrayRef, BooleanArray, FixedSizeBinaryArray, FixedSizeBinaryBuilder, Int8Array,
        StringArray, StringDictionaryBuilder, UInt32Array, UInt32Builder, UInt8Array, UInt8Builder,
    },
    datatypes::{DataType, Field, Int8Type, Schema},
    record_batch::RecordBatch,
};

use super::{
    edges::{EdgeKind, EdgeRecord},
    nodes::{NodeKind, NodeRecord},
};

// ---------------------------------------------------------------------------
// Node schema
// ---------------------------------------------------------------------------

/// Returns the Arrow `Schema` for `nodes.parquet` (ADR-3).
pub fn node_schema() -> Schema {
    Schema::new(vec![
        Field::new("usr", DataType::Utf8, false),
        Field::new(
            "kind",
            DataType::Dictionary(Box::new(DataType::Int8), Box::new(DataType::Utf8)),
            false,
        ),
        Field::new("name", DataType::Utf8, false),
        Field::new("qualified_name", DataType::Utf8, false),
        Field::new("mangled_name", DataType::Utf8, true),
        Field::new("file_path", DataType::Utf8, false),
        Field::new("line", DataType::UInt32, true),
        Field::new("col", DataType::UInt32, true),
        Field::new("repo_name", DataType::Utf8, false),
        Field::new("attrs_json", DataType::Utf8, false),
        Field::new("partial", DataType::Boolean, false),
        Field::new("phase", DataType::UInt8, false),
        Field::new("tu_hash", DataType::FixedSizeBinary(32), false),
    ])
}

/// Serialise a slice of `NodeRecord`s into an Arrow `RecordBatch`.
///
/// # Errors
/// Returns an `arrow::error::ArrowError` if any array construction fails.
pub fn nodes_to_record_batch(
    records: &[NodeRecord],
) -> Result<RecordBatch, arrow::error::ArrowError> {
    let schema = Arc::new(node_schema());

    let usr: ArrayRef = Arc::new(StringArray::from(
        records.iter().map(|r| r.usr.as_str()).collect::<Vec<_>>(),
    ));

    let mut kind_builder: StringDictionaryBuilder<Int8Type> = StringDictionaryBuilder::new();
    for r in records {
        kind_builder.append_value(r.kind.as_str());
    }
    let kind: ArrayRef = Arc::new(kind_builder.finish());

    let name: ArrayRef = Arc::new(StringArray::from(
        records.iter().map(|r| r.name.as_str()).collect::<Vec<_>>(),
    ));

    let qualified_name: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.qualified_name.as_str())
            .collect::<Vec<_>>(),
    ));

    let mangled_name: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.mangled_name.as_deref())
            .collect::<Vec<_>>(),
    ));

    let file_path: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.file_path.as_str())
            .collect::<Vec<_>>(),
    ));

    let mut line_builder = UInt32Builder::new();
    for r in records {
        match r.line {
            Some(v) => line_builder.append_value(v),
            None => line_builder.append_null(),
        }
    }
    let line: ArrayRef = Arc::new(line_builder.finish());

    let mut col_builder = UInt32Builder::new();
    for r in records {
        match r.col {
            Some(v) => col_builder.append_value(v),
            None => col_builder.append_null(),
        }
    }
    let col: ArrayRef = Arc::new(col_builder.finish());

    let repo_name: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.repo_name.as_str())
            .collect::<Vec<_>>(),
    ));

    let attrs_json: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.attrs_json.as_str())
            .collect::<Vec<_>>(),
    ));

    let partial: ArrayRef = Arc::new(BooleanArray::from(
        records.iter().map(|r| r.partial).collect::<Vec<_>>(),
    ));

    let mut phase_builder = UInt8Builder::new();
    for r in records {
        phase_builder.append_value(r.phase);
    }
    let phase: ArrayRef = Arc::new(phase_builder.finish());

    let mut tu_hash_builder = FixedSizeBinaryBuilder::with_capacity(records.len(), 32);
    for r in records {
        tu_hash_builder.append_value(r.tu_hash)?;
    }
    let tu_hash: ArrayRef = Arc::new(tu_hash_builder.finish());

    RecordBatch::try_new(
        schema,
        vec![
            usr,
            kind,
            name,
            qualified_name,
            mangled_name,
            file_path,
            line,
            col,
            repo_name,
            attrs_json,
            partial,
            phase,
            tu_hash,
        ],
    )
}

/// Deserialise a `RecordBatch` (previously produced by `nodes_to_record_batch`) back into
/// `NodeRecord`s.
///
/// Column ordering must match `node_schema()`.
pub fn record_batch_to_nodes(batch: &RecordBatch) -> Vec<NodeRecord> {
    let n = batch.num_rows();
    if n == 0 {
        return Vec::new();
    }

    let usr_col = batch
        .column(0)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("usr column must be StringArray");

    let kind_col = batch
        .column(1)
        .as_any()
        .downcast_ref::<arrow::array::DictionaryArray<Int8Type>>()
        .expect("kind column must be DictionaryArray<Int8Type>");
    let kind_values = kind_col
        .values()
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("kind dictionary values must be StringArray");

    let name_col = batch
        .column(2)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("name column must be StringArray");

    let qualified_name_col = batch
        .column(3)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("qualified_name column must be StringArray");

    let mangled_name_col = batch
        .column(4)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("mangled_name column must be StringArray");

    let file_path_col = batch
        .column(5)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("file_path column must be StringArray");

    let line_col = batch
        .column(6)
        .as_any()
        .downcast_ref::<UInt32Array>()
        .expect("line column must be UInt32Array");

    let col_col = batch
        .column(7)
        .as_any()
        .downcast_ref::<UInt32Array>()
        .expect("col column must be UInt32Array");

    let repo_name_col = batch
        .column(8)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("repo_name column must be StringArray");

    let attrs_json_col = batch
        .column(9)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("attrs_json column must be StringArray");

    let partial_col = batch
        .column(10)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("partial column must be BooleanArray");

    let phase_col = batch
        .column(11)
        .as_any()
        .downcast_ref::<UInt8Array>()
        .expect("phase column must be UInt8Array");

    let tu_hash_col = batch
        .column(12)
        .as_any()
        .downcast_ref::<FixedSizeBinaryArray>()
        .expect("tu_hash column must be FixedSizeBinaryArray");

    (0..n)
        .map(|i| {
            // Dictionary lookup: keys are Int8 indices into the values array.
            let key = kind_col
                .keys()
                .as_any()
                .downcast_ref::<Int8Array>()
                .expect("kind keys must be Int8Array")
                .value(i);
            let kind_str = kind_values.value(key as usize);
            let kind = NodeKind::try_from_arrow_str(kind_str)
                .unwrap_or_else(|| panic!("unknown NodeKind string: {kind_str}"));

            let tu_hash_bytes = tu_hash_col.value(i);
            let mut tu_hash = [0u8; 32];
            tu_hash.copy_from_slice(tu_hash_bytes);

            NodeRecord {
                usr: usr_col.value(i).to_owned(),
                kind,
                name: name_col.value(i).to_owned(),
                qualified_name: qualified_name_col.value(i).to_owned(),
                mangled_name: if mangled_name_col.is_null(i) {
                    None
                } else {
                    Some(mangled_name_col.value(i).to_owned())
                },
                file_path: file_path_col.value(i).to_owned(),
                line: if line_col.is_null(i) {
                    None
                } else {
                    Some(line_col.value(i))
                },
                col: if col_col.is_null(i) {
                    None
                } else {
                    Some(col_col.value(i))
                },
                repo_name: repo_name_col.value(i).to_owned(),
                attrs_json: attrs_json_col.value(i).to_owned(),
                partial: partial_col.value(i),
                phase: phase_col.value(i),
                tu_hash,
            }
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Edge schema
// ---------------------------------------------------------------------------

/// Returns the Arrow `Schema` for `edges.parquet` (ADR-3).
pub fn edge_schema() -> Schema {
    Schema::new(vec![
        Field::new("src_usr", DataType::Utf8, false),
        Field::new("dst_usr", DataType::Utf8, true),
        Field::new("dst_placeholder", DataType::Utf8, true),
        Field::new(
            "kind",
            DataType::Dictionary(Box::new(DataType::Int8), Box::new(DataType::Utf8)),
            false,
        ),
        Field::new("resolved", DataType::Boolean, false),
        Field::new("cross_repo_candidate", DataType::Boolean, false),
        Field::new("repo_name", DataType::Utf8, false),
        Field::new("attrs_json", DataType::Utf8, false),
        Field::new("tu_hash", DataType::FixedSizeBinary(32), false),
    ])
}

/// Serialise a slice of `EdgeRecord`s into an Arrow `RecordBatch`.
pub fn edges_to_record_batch(
    records: &[EdgeRecord],
) -> Result<RecordBatch, arrow::error::ArrowError> {
    let schema = Arc::new(edge_schema());

    let src_usr: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.src_usr.as_str())
            .collect::<Vec<_>>(),
    ));

    let dst_usr: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.dst_usr.as_deref())
            .collect::<Vec<_>>(),
    ));

    let dst_placeholder: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.dst_placeholder.as_deref())
            .collect::<Vec<_>>(),
    ));

    let mut kind_builder: StringDictionaryBuilder<Int8Type> = StringDictionaryBuilder::new();
    for r in records {
        kind_builder.append_value(r.kind.as_str());
    }
    let kind: ArrayRef = Arc::new(kind_builder.finish());

    let resolved: ArrayRef = Arc::new(BooleanArray::from(
        records.iter().map(|r| r.resolved).collect::<Vec<_>>(),
    ));

    let cross_repo_candidate: ArrayRef = Arc::new(BooleanArray::from(
        records
            .iter()
            .map(|r| r.cross_repo_candidate)
            .collect::<Vec<_>>(),
    ));

    let repo_name: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.repo_name.as_str())
            .collect::<Vec<_>>(),
    ));

    let attrs_json: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.attrs_json.as_str())
            .collect::<Vec<_>>(),
    ));

    let mut tu_hash_builder = FixedSizeBinaryBuilder::with_capacity(records.len(), 32);
    for r in records {
        tu_hash_builder.append_value(r.tu_hash)?;
    }
    let tu_hash: ArrayRef = Arc::new(tu_hash_builder.finish());

    RecordBatch::try_new(
        schema,
        vec![
            src_usr,
            dst_usr,
            dst_placeholder,
            kind,
            resolved,
            cross_repo_candidate,
            repo_name,
            attrs_json,
            tu_hash,
        ],
    )
}

/// Deserialise a `RecordBatch` back into `EdgeRecord`s.
pub fn record_batch_to_edges(batch: &RecordBatch) -> Vec<EdgeRecord> {
    let n = batch.num_rows();
    if n == 0 {
        return Vec::new();
    }

    let src_usr_col = batch
        .column(0)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("src_usr column must be StringArray");

    let dst_usr_col = batch
        .column(1)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("dst_usr column must be StringArray");

    let dst_placeholder_col = batch
        .column(2)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("dst_placeholder column must be StringArray");

    let kind_col = batch
        .column(3)
        .as_any()
        .downcast_ref::<arrow::array::DictionaryArray<Int8Type>>()
        .expect("kind column must be DictionaryArray<Int8Type>");
    let kind_values = kind_col
        .values()
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("kind dictionary values must be StringArray");

    let resolved_col = batch
        .column(4)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("resolved column must be BooleanArray");

    let cross_repo_col = batch
        .column(5)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("cross_repo_candidate column must be BooleanArray");

    let repo_name_col = batch
        .column(6)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("repo_name column must be StringArray");

    let attrs_json_col = batch
        .column(7)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("attrs_json column must be StringArray");

    let tu_hash_col = batch
        .column(8)
        .as_any()
        .downcast_ref::<FixedSizeBinaryArray>()
        .expect("tu_hash column must be FixedSizeBinaryArray");

    (0..n)
        .map(|i| {
            let key = kind_col
                .keys()
                .as_any()
                .downcast_ref::<Int8Array>()
                .expect("kind keys must be Int8Array")
                .value(i);
            let kind_str = kind_values.value(key as usize);
            let kind = EdgeKind::try_from_arrow_str(kind_str)
                .unwrap_or_else(|| panic!("unknown EdgeKind string: {kind_str}"));

            let tu_hash_bytes = tu_hash_col.value(i);
            let mut tu_hash = [0u8; 32];
            tu_hash.copy_from_slice(tu_hash_bytes);

            EdgeRecord {
                src_usr: src_usr_col.value(i).to_owned(),
                dst_usr: if dst_usr_col.is_null(i) {
                    None
                } else {
                    Some(dst_usr_col.value(i).to_owned())
                },
                dst_placeholder: if dst_placeholder_col.is_null(i) {
                    None
                } else {
                    Some(dst_placeholder_col.value(i).to_owned())
                },
                kind,
                resolved: resolved_col.value(i),
                cross_repo_candidate: cross_repo_col.value(i),
                repo_name: repo_name_col.value(i).to_owned(),
                attrs_json: attrs_json_col.value(i).to_owned(),
                tu_hash,
            }
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Tests (AC-M1-4)
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_tu_hash(seed: u8) -> [u8; 32] {
        [seed; 32]
    }

    fn sample_node(kind: NodeKind, suffix: &str) -> NodeRecord {
        NodeRecord {
            usr: format!("c:@F@fn_{suffix}"),
            kind,
            name: format!("fn_{suffix}"),
            qualified_name: format!("ns::fn_{suffix}"),
            mangled_name: if kind == NodeKind::Function {
                Some(format!("_ZN2ns5fn_{suffix}Ev"))
            } else {
                None
            },
            file_path: format!("/repo/src/{suffix}.cpp"),
            line: Some(10),
            col: Some(1),
            repo_name: "my-repo".to_owned(),
            attrs_json: "{}".to_owned(),
            partial: false,
            phase: 1,
            tu_hash: sample_tu_hash(42),
        }
    }

    fn sample_edge(kind: EdgeKind, i: usize) -> EdgeRecord {
        EdgeRecord {
            src_usr: format!("c:@F@src_{i}"),
            dst_usr: if i.is_multiple_of(2) {
                Some(format!("c:@F@dst_{i}"))
            } else {
                None
            },
            dst_placeholder: if !i.is_multiple_of(2) {
                Some(format!("placeholder_{i}"))
            } else {
                None
            },
            kind,
            resolved: i.is_multiple_of(2),
            cross_repo_candidate: i.is_multiple_of(3),
            repo_name: "my-repo".to_owned(),
            attrs_json: "{}".to_owned(),
            tu_hash: sample_tu_hash(u8::try_from(i % 256).unwrap_or(0)),
        }
    }

    /// For each `NodeKind` variant: build a sample record, write to `RecordBatch`, read back,
    /// assert equality.
    #[test]
    fn node_round_trip_all_variants() {
        for &kind in NodeKind::all() {
            let original = vec![sample_node(kind, &format!("{kind}"))];
            let batch = nodes_to_record_batch(&original).expect("serialisation must succeed");

            // Verify schema shape
            assert_eq!(batch.num_columns(), 13);
            assert_eq!(batch.num_rows(), 1);

            let recovered = record_batch_to_nodes(&batch);
            assert_eq!(recovered, original, "NodeKind::{kind:?} round-trip failed");
        }
    }

    /// For each `EdgeKind` variant: build a sample record, write to `RecordBatch`, read back,
    /// assert equality.
    #[test]
    fn edge_round_trip_all_variants() {
        for (i, &kind) in EdgeKind::all().iter().enumerate() {
            let original = vec![sample_edge(kind, i)];
            let batch = edges_to_record_batch(&original).expect("serialisation must succeed");

            assert_eq!(batch.num_columns(), 9);
            assert_eq!(batch.num_rows(), 1);

            let recovered = record_batch_to_edges(&batch);
            assert_eq!(recovered, original, "EdgeKind::{kind:?} round-trip failed");
        }
    }

    /// A batch with multiple rows (mixed kinds) round-trips correctly.
    #[test]
    fn node_batch_multiple_rows() {
        let original: Vec<NodeRecord> = NodeKind::all()
            .iter()
            .enumerate()
            .map(|(i, &k)| {
                let mut r = sample_node(k, &i.to_string());
                r.line = if i.is_multiple_of(2) {
                    Some(i as u32 + 1)
                } else {
                    None
                };
                r.col = if i.is_multiple_of(3) {
                    Some(i as u32 + 1)
                } else {
                    None
                };
                r
            })
            .collect();

        let batch = nodes_to_record_batch(&original).expect("batch creation");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(recovered, original);
    }

    #[test]
    fn edge_batch_multiple_rows() {
        let original: Vec<EdgeRecord> = EdgeKind::all()
            .iter()
            .enumerate()
            .map(|(i, &k)| sample_edge(k, i))
            .collect();

        let batch = edges_to_record_batch(&original).expect("batch creation");
        let recovered = record_batch_to_edges(&batch);
        assert_eq!(recovered, original);
    }

    /// Empty batches must not panic and return empty vecs.
    #[test]
    fn empty_node_batch_round_trip() {
        let batch = nodes_to_record_batch(&[]).expect("empty batch");
        assert_eq!(batch.num_rows(), 0);
        assert!(record_batch_to_nodes(&batch).is_empty());
    }

    #[test]
    fn empty_edge_batch_round_trip() {
        let batch = edges_to_record_batch(&[]).expect("empty batch");
        assert_eq!(batch.num_rows(), 0);
        assert!(record_batch_to_edges(&batch).is_empty());
    }

    /// `tu_hash` field survives round-trip exactly.
    #[test]
    fn tu_hash_round_trip() {
        let mut r = sample_node(NodeKind::Function, "hash_test");
        r.tu_hash = {
            let mut h = [0u8; 32];
            for (i, b) in h.iter_mut().enumerate() {
                *b = i as u8;
            }
            h
        };
        let batch = nodes_to_record_batch(&[r.clone()]).expect("batch");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(recovered[0].tu_hash, r.tu_hash);
    }

    /// Nullable columns (mangled_name, line, col) round-trip correctly when None.
    #[test]
    fn nullable_fields_none_round_trip() {
        let mut r = sample_node(NodeKind::Class, "nullable");
        r.mangled_name = None;
        r.line = None;
        r.col = None;
        let batch = nodes_to_record_batch(&[r.clone()]).expect("batch");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(recovered[0].mangled_name, None);
        assert_eq!(recovered[0].line, None);
        assert_eq!(recovered[0].col, None);
    }

    /// Node schema field names and nullability match ADR-3.
    #[test]
    fn node_schema_field_names() {
        let schema = node_schema();
        let names: Vec<&str> = schema.fields().iter().map(|f| f.name().as_str()).collect();
        assert_eq!(
            names,
            &[
                "usr",
                "kind",
                "name",
                "qualified_name",
                "mangled_name",
                "file_path",
                "line",
                "col",
                "repo_name",
                "attrs_json",
                "partial",
                "phase",
                "tu_hash",
            ]
        );
        // Not-null columns
        let not_null = [
            "usr",
            "kind",
            "name",
            "qualified_name",
            "file_path",
            "repo_name",
            "attrs_json",
            "partial",
            "phase",
            "tu_hash",
        ];
        for name in not_null {
            let field = schema.field_with_name(name).expect("field must exist");
            assert!(!field.is_nullable(), "field {name} must be not-null");
        }
        // Nullable columns
        let nullable = ["mangled_name", "line", "col"];
        for name in nullable {
            let field = schema.field_with_name(name).expect("field must exist");
            assert!(field.is_nullable(), "field {name} must be nullable");
        }
    }

    /// Edge schema field names and nullability match ADR-3.
    #[test]
    fn edge_schema_field_names() {
        let schema = edge_schema();
        let names: Vec<&str> = schema.fields().iter().map(|f| f.name().as_str()).collect();
        assert_eq!(
            names,
            &[
                "src_usr",
                "dst_usr",
                "dst_placeholder",
                "kind",
                "resolved",
                "cross_repo_candidate",
                "repo_name",
                "attrs_json",
                "tu_hash",
            ]
        );
    }
}
