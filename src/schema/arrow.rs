/// Arrow schemas and `NodeRecord`/`EdgeRecord` ↔ `RecordBatch` round-trip (AC-M1-4, ADR-3).
///
/// Column layout follows ADR-3 exactly. The `kind` column uses a `Dictionary<Int8, Utf8>` for
/// compact storage. `tu_hash` is `FixedSizeBinary(32)`.
///
/// M8 (S40): ten new node columns and two new edge columns added per design.md §3.4.
/// `params`, `template_params`, `template_args` use `List<Struct>` Arrow type (ADR-14).
use std::sync::Arc;

use arrow::{
    array::{
        Array, ArrayRef, BooleanArray, BooleanBuilder, FixedSizeBinaryArray,
        FixedSizeBinaryBuilder, Int8Array, ListArray, ListBuilder, StringArray,
        StringDictionaryBuilder, StructArray, UInt32Array, UInt32Builder, UInt8Array, UInt8Builder,
    },
    datatypes::{DataType, Field, Fields, Int8Type, Schema},
    record_batch::RecordBatch,
};

use super::{
    edges::{EdgeKind, EdgeRecord},
    nodes::{NodeKind, NodeRecord, Param, TemplateArg, TemplateParam},
};

// ---------------------------------------------------------------------------
// Node schema
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Schema helpers for List<Struct> fields (ADR-14)
// ---------------------------------------------------------------------------

/// Arrow struct fields for a `Param` record: `{name: Utf8, type: Utf8}`.
fn param_struct_fields() -> Fields {
    Fields::from(vec![
        Field::new("name", DataType::Utf8, false),
        Field::new("type", DataType::Utf8, false),
    ])
}

/// Arrow struct fields for a `TemplateParam` record: `{name: Utf8, kind: Utf8, default: Utf8?}`.
fn template_param_struct_fields() -> Fields {
    Fields::from(vec![
        Field::new("name", DataType::Utf8, false),
        Field::new("kind", DataType::Utf8, false),
        Field::new("default", DataType::Utf8, true),
    ])
}

/// Arrow struct fields for a `TemplateArg` record: `{kind: Utf8, value: Utf8}`.
fn template_arg_struct_fields() -> Fields {
    Fields::from(vec![
        Field::new("kind", DataType::Utf8, false),
        Field::new("value", DataType::Utf8, false),
    ])
}

/// Returns the Arrow `Schema` for `nodes.parquet` (ADR-3).
///
/// M8 (S40): ten new nullable columns appended after `tu_hash` per design.md §3.4.
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
        // M8 columns:
        Field::new("return_type", DataType::Utf8, true),
        Field::new(
            "params",
            DataType::List(Arc::new(Field::new(
                "item",
                DataType::Struct(param_struct_fields()),
                false,
            ))),
            true,
        ),
        Field::new("signature", DataType::Utf8, true),
        Field::new("code", DataType::Utf8, true),
        Field::new("code_truncated", DataType::Boolean, true),
        Field::new(
            "template_params",
            DataType::List(Arc::new(Field::new(
                "item",
                DataType::Struct(template_param_struct_fields()),
                false,
            ))),
            true,
        ),
        Field::new(
            "template_args",
            DataType::List(Arc::new(Field::new(
                "item",
                DataType::Struct(template_arg_struct_fields()),
                false,
            ))),
            true,
        ),
        Field::new("is_virtual", DataType::Boolean, true),
        Field::new("is_pure_virtual", DataType::Boolean, true),
        Field::new("is_static", DataType::Boolean, true),
    ])
}

// ---------------------------------------------------------------------------
// List<Struct> builder helpers (ADR-14)
// ---------------------------------------------------------------------------

/// Build a nullable `List<Struct<name,type>>` Arrow array from a slice of `Option<Vec<Param>>`.
///
/// The outer list is null when `Option` is `None`; an empty `Vec` produces a non-null empty list.
fn build_params_array(records: &[&Option<Vec<Param>>]) -> ArrayRef {
    let fields = param_struct_fields();
    let item_field = Arc::new(Field::new("item", DataType::Struct(fields.clone()), false));
    let struct_builder = arrow::array::StructBuilder::new(
        fields,
        vec![
            Box::new(arrow::array::StringBuilder::new()),
            Box::new(arrow::array::StringBuilder::new()),
        ],
    );
    let mut list_builder = ListBuilder::new(struct_builder).with_field(item_field);

    for opt in records {
        match opt {
            None => list_builder.append_null(),
            Some(params) => {
                let sb = list_builder.values();
                for p in params {
                    sb.field_builder::<arrow::array::StringBuilder>(0)
                        .unwrap()
                        .append_value(&p.name);
                    sb.field_builder::<arrow::array::StringBuilder>(1)
                        .unwrap()
                        .append_value(&p.type_);
                    sb.append(true);
                }
                list_builder.append(true);
            }
        }
    }
    Arc::new(list_builder.finish())
}

/// Build a nullable `List<Struct<name,kind,default>>` array from `Option<Vec<TemplateParam>>`.
fn build_template_params_array(records: &[&Option<Vec<TemplateParam>>]) -> ArrayRef {
    let fields = template_param_struct_fields();
    let item_field = Arc::new(Field::new("item", DataType::Struct(fields.clone()), false));
    let struct_builder = arrow::array::StructBuilder::new(
        fields,
        vec![
            Box::new(arrow::array::StringBuilder::new()),
            Box::new(arrow::array::StringBuilder::new()),
            Box::new(arrow::array::StringBuilder::new()),
        ],
    );
    let mut list_builder = ListBuilder::new(struct_builder).with_field(item_field);

    for opt in records {
        match opt {
            None => list_builder.append_null(),
            Some(tps) => {
                let sb = list_builder.values();
                for tp in tps {
                    sb.field_builder::<arrow::array::StringBuilder>(0)
                        .unwrap()
                        .append_value(&tp.name);
                    sb.field_builder::<arrow::array::StringBuilder>(1)
                        .unwrap()
                        .append_value(&tp.kind);
                    sb.field_builder::<arrow::array::StringBuilder>(2)
                        .unwrap()
                        .append_option(tp.default.as_deref());
                    sb.append(true);
                }
                list_builder.append(true);
            }
        }
    }
    Arc::new(list_builder.finish())
}

/// Build a nullable `List<Struct<kind,value>>` array from `Option<Vec<TemplateArg>>`.
fn build_template_args_array(records: &[&Option<Vec<TemplateArg>>]) -> ArrayRef {
    let fields = template_arg_struct_fields();
    let item_field = Arc::new(Field::new("item", DataType::Struct(fields.clone()), false));
    let struct_builder = arrow::array::StructBuilder::new(
        fields,
        vec![
            Box::new(arrow::array::StringBuilder::new()),
            Box::new(arrow::array::StringBuilder::new()),
        ],
    );
    let mut list_builder = ListBuilder::new(struct_builder).with_field(item_field);

    for opt in records {
        match opt {
            None => list_builder.append_null(),
            Some(args) => {
                let sb = list_builder.values();
                for a in args {
                    sb.field_builder::<arrow::array::StringBuilder>(0)
                        .unwrap()
                        .append_value(&a.kind);
                    sb.field_builder::<arrow::array::StringBuilder>(1)
                        .unwrap()
                        .append_value(&a.value);
                    sb.append(true);
                }
                list_builder.append(true);
            }
        }
    }
    Arc::new(list_builder.finish())
}

// ---------------------------------------------------------------------------
// List<Struct> reader helpers (ADR-14)
// ---------------------------------------------------------------------------

/// Extract `Vec<Param>` from a single row of a `ListArray` over `StructArray`.
/// Returns `None` when the list is null at row `i`.
fn read_params_at(col: &ListArray, i: usize) -> Option<Vec<Param>> {
    if col.is_null(i) {
        return None;
    }
    let offsets = col.offsets();
    let start = offsets[i] as usize;
    let end = offsets[i + 1] as usize;
    let values = col
        .values()
        .as_any()
        .downcast_ref::<StructArray>()
        .expect("params values must be StructArray");
    let names = values
        .column(0)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("params[0] must be StringArray");
    let types = values
        .column(1)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("params[1] must be StringArray");
    let result: Vec<Param> = (start..end)
        .map(|j| Param {
            name: names.value(j).to_owned(),
            type_: types.value(j).to_owned(),
        })
        .collect();
    Some(result)
}

/// Extract `Vec<TemplateParam>` from a single row of a `ListArray` over `StructArray`.
fn read_template_params_at(col: &ListArray, i: usize) -> Option<Vec<TemplateParam>> {
    if col.is_null(i) {
        return None;
    }
    let offsets = col.offsets();
    let start = offsets[i] as usize;
    let end = offsets[i + 1] as usize;
    let values = col
        .values()
        .as_any()
        .downcast_ref::<StructArray>()
        .expect("template_params values must be StructArray");
    let names = values
        .column(0)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("template_params[0] must be StringArray");
    let kinds = values
        .column(1)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("template_params[1] must be StringArray");
    let defaults = values
        .column(2)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("template_params[2] must be StringArray");
    let result: Vec<TemplateParam> = (start..end)
        .map(|j| TemplateParam {
            name: names.value(j).to_owned(),
            kind: kinds.value(j).to_owned(),
            default: if defaults.is_null(j) {
                None
            } else {
                Some(defaults.value(j).to_owned())
            },
        })
        .collect();
    Some(result)
}

/// Extract `Vec<TemplateArg>` from a single row of a `ListArray` over `StructArray`.
fn read_template_args_at(col: &ListArray, i: usize) -> Option<Vec<TemplateArg>> {
    if col.is_null(i) {
        return None;
    }
    let offsets = col.offsets();
    let start = offsets[i] as usize;
    let end = offsets[i + 1] as usize;
    let values = col
        .values()
        .as_any()
        .downcast_ref::<StructArray>()
        .expect("template_args values must be StructArray");
    let kinds = values
        .column(0)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("template_args[0] must be StringArray");
    let vals = values
        .column(1)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("template_args[1] must be StringArray");
    let result: Vec<TemplateArg> = (start..end)
        .map(|j| TemplateArg {
            kind: kinds.value(j).to_owned(),
            value: vals.value(j).to_owned(),
        })
        .collect();
    Some(result)
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

    // M8 columns ──────────────────────────────────────────────────────────────

    let return_type: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.return_type.as_deref())
            .collect::<Vec<_>>(),
    ));

    let params_col = build_params_array(&records.iter().map(|r| &r.params).collect::<Vec<_>>());

    let signature: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.signature.as_deref())
            .collect::<Vec<_>>(),
    ));

    let code: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.code.as_deref())
            .collect::<Vec<_>>(),
    ));

    let mut code_truncated_builder = BooleanBuilder::new();
    for r in records {
        match r.code_truncated {
            Some(v) => code_truncated_builder.append_value(v),
            None => code_truncated_builder.append_null(),
        }
    }
    let code_truncated: ArrayRef = Arc::new(code_truncated_builder.finish());

    let template_params_col = build_template_params_array(
        &records
            .iter()
            .map(|r| &r.template_params)
            .collect::<Vec<_>>(),
    );

    let template_args_col =
        build_template_args_array(&records.iter().map(|r| &r.template_args).collect::<Vec<_>>());

    let mut is_virtual_builder = BooleanBuilder::new();
    for r in records {
        match r.is_virtual {
            Some(v) => is_virtual_builder.append_value(v),
            None => is_virtual_builder.append_null(),
        }
    }
    let is_virtual: ArrayRef = Arc::new(is_virtual_builder.finish());

    let mut is_pure_virtual_builder = BooleanBuilder::new();
    for r in records {
        match r.is_pure_virtual {
            Some(v) => is_pure_virtual_builder.append_value(v),
            None => is_pure_virtual_builder.append_null(),
        }
    }
    let is_pure_virtual: ArrayRef = Arc::new(is_pure_virtual_builder.finish());

    let mut is_static_builder = BooleanBuilder::new();
    for r in records {
        match r.is_static {
            Some(v) => is_static_builder.append_value(v),
            None => is_static_builder.append_null(),
        }
    }
    let is_static: ArrayRef = Arc::new(is_static_builder.finish());

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
            // M8 columns:
            return_type,
            params_col,
            signature,
            code,
            code_truncated,
            template_params_col,
            template_args_col,
            is_virtual,
            is_pure_virtual,
            is_static,
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

    // M8 columns (columns 13-22)
    let return_type_col = batch
        .column(13)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("return_type column must be StringArray");

    let params_col = batch
        .column(14)
        .as_any()
        .downcast_ref::<ListArray>()
        .expect("params column must be ListArray");

    let signature_col = batch
        .column(15)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("signature column must be StringArray");

    let code_col = batch
        .column(16)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("code column must be StringArray");

    let code_truncated_col = batch
        .column(17)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("code_truncated column must be BooleanArray");

    let template_params_col = batch
        .column(18)
        .as_any()
        .downcast_ref::<ListArray>()
        .expect("template_params column must be ListArray");

    let template_args_col = batch
        .column(19)
        .as_any()
        .downcast_ref::<ListArray>()
        .expect("template_args column must be ListArray");

    let is_virtual_col = batch
        .column(20)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("is_virtual column must be BooleanArray");

    let is_pure_virtual_col = batch
        .column(21)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("is_pure_virtual column must be BooleanArray");

    let is_static_col = batch
        .column(22)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("is_static column must be BooleanArray");

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
                // M8 columns:
                return_type: if return_type_col.is_null(i) {
                    None
                } else {
                    Some(return_type_col.value(i).to_owned())
                },
                params: read_params_at(params_col, i),
                signature: if signature_col.is_null(i) {
                    None
                } else {
                    Some(signature_col.value(i).to_owned())
                },
                code: if code_col.is_null(i) {
                    None
                } else {
                    Some(code_col.value(i).to_owned())
                },
                code_truncated: if code_truncated_col.is_null(i) {
                    None
                } else {
                    Some(code_truncated_col.value(i))
                },
                template_params: read_template_params_at(template_params_col, i),
                template_args: read_template_args_at(template_args_col, i),
                is_virtual: if is_virtual_col.is_null(i) {
                    None
                } else {
                    Some(is_virtual_col.value(i))
                },
                is_pure_virtual: if is_pure_virtual_col.is_null(i) {
                    None
                } else {
                    Some(is_pure_virtual_col.value(i))
                },
                is_static: if is_static_col.is_null(i) {
                    None
                } else {
                    Some(is_static_col.value(i))
                },
            }
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Edge schema
// ---------------------------------------------------------------------------

/// Returns the Arrow `Schema` for `edges.parquet` (ADR-3).
///
/// M8 (S40): two new nullable columns appended per design.md §3.4.
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
        // M8 columns:
        Field::new("source_association_type", DataType::Utf8, true),
        Field::new("target_association_type", DataType::Utf8, true),
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

    // M8 edge columns
    let source_association_type: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.source_association_type.as_deref())
            .collect::<Vec<_>>(),
    ));

    let target_association_type: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.target_association_type.as_deref())
            .collect::<Vec<_>>(),
    ));

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
            // M8 columns:
            source_association_type,
            target_association_type,
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

    // M8 edge columns (9-10)
    let source_assoc_col = batch
        .column(9)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("source_association_type column must be StringArray");

    let target_assoc_col = batch
        .column(10)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("target_association_type column must be StringArray");

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
                // M8 columns:
                source_association_type: if source_assoc_col.is_null(i) {
                    None
                } else {
                    Some(source_assoc_col.value(i).to_owned())
                },
                target_association_type: if target_assoc_col.is_null(i) {
                    None
                } else {
                    Some(target_assoc_col.value(i).to_owned())
                },
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
            // M8 fields — None by default; specific tests set these
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
            // M8 fields — None by default
            source_association_type: None,
            target_association_type: None,
        }
    }

    /// For each `NodeKind` variant: build a sample record, write to `RecordBatch`, read back,
    /// assert equality.
    #[test]
    fn node_round_trip_all_variants() {
        for &kind in NodeKind::all() {
            let original = vec![sample_node(kind, &format!("{kind}"))];
            let batch = nodes_to_record_batch(&original).expect("serialisation must succeed");

            // Verify schema shape — 13 base columns + 10 M8 node columns = 23
            assert_eq!(batch.num_columns(), 23);
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

            // 9 base columns + 2 M8 edge columns = 11
            assert_eq!(batch.num_columns(), 11);
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
    /// Extended for M8: all new node columns round-trip as None.
    #[test]
    fn nullable_fields_none_round_trip() {
        let mut r = sample_node(NodeKind::Class, "nullable");
        r.mangled_name = None;
        r.line = None;
        r.col = None;
        // M8 fields all None (the default from sample_node)
        let batch = nodes_to_record_batch(&[r.clone()]).expect("batch");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(recovered[0].mangled_name, None);
        assert_eq!(recovered[0].line, None);
        assert_eq!(recovered[0].col, None);
        // M8 nullable fields
        assert_eq!(recovered[0].return_type, None);
        assert_eq!(recovered[0].params, None);
        assert_eq!(recovered[0].signature, None);
        assert_eq!(recovered[0].code, None);
        assert_eq!(recovered[0].code_truncated, None);
        assert_eq!(recovered[0].template_params, None);
        assert_eq!(recovered[0].template_args, None);
        assert_eq!(recovered[0].is_virtual, None);
        assert_eq!(recovered[0].is_pure_virtual, None);
        assert_eq!(recovered[0].is_static, None);
    }

    /// M8: simple scalar node fields round-trip with `Some(non-empty)` values.
    #[test]
    fn m8_node_scalar_fields_some_round_trip() {
        use super::super::nodes::{Param, TemplateArg, TemplateParam};

        let mut r = sample_node(NodeKind::Function, "m8_scalar");
        r.return_type = Some("int".to_owned());
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
        r.signature = Some("int(int, const std::string &)".to_owned());
        r.code = Some("int foo(int x) { return x; }".to_owned());
        r.code_truncated = Some(false);
        r.is_static = Some(true);
        r.is_virtual = Some(false);
        r.is_pure_virtual = Some(false);
        r.template_params = Some(vec![TemplateParam {
            name: "T".to_owned(),
            kind: "type".to_owned(),
            default: Some("int".to_owned()),
        }]);
        r.template_args = Some(vec![TemplateArg {
            kind: "type".to_owned(),
            value: "double".to_owned(),
        }]);

        let batch = nodes_to_record_batch(&[r.clone()]).expect("batch");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(recovered[0], r, "M8 node scalar fields round-trip failed");
    }

    /// M8: `params` = `Some(empty vec)` is distinct from `None` and round-trips correctly.
    #[test]
    fn m8_node_params_some_empty_round_trip() {
        let mut r = sample_node(NodeKind::Function, "m8_empty_params");
        r.params = Some(vec![]);
        r.template_params = Some(vec![]);
        r.template_args = Some(vec![]);
        let batch = nodes_to_record_batch(&[r.clone()]).expect("batch");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(
            recovered[0].params,
            Some(vec![]),
            "empty params list must be Some([]), not None"
        );
        assert_eq!(recovered[0].template_params, Some(vec![]));
        assert_eq!(recovered[0].template_args, Some(vec![]));
    }

    /// M8: TemplateParam with `default: None` round-trips correctly.
    #[test]
    fn m8_template_param_default_none_round_trip() {
        use super::super::nodes::TemplateParam;
        let mut r = sample_node(NodeKind::TemplateDef, "m8_tmpl");
        r.template_params = Some(vec![TemplateParam {
            name: "N".to_owned(),
            kind: "non_type".to_owned(),
            default: None,
        }]);
        let batch = nodes_to_record_batch(&[r.clone()]).expect("batch");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(recovered[0], r);
    }

    /// M8: edge association type fields round-trip with `Some` and `None`.
    #[test]
    fn m8_edge_association_type_round_trip() {
        let mut e = sample_edge(EdgeKind::Uses, 0);
        e.source_association_type = Some("read".to_owned());
        e.target_association_type = Some("write".to_owned());
        let batch = edges_to_record_batch(&[e.clone()]).expect("batch");
        let recovered = record_batch_to_edges(&batch);
        assert_eq!(
            recovered[0].source_association_type,
            Some("read".to_owned())
        );
        assert_eq!(
            recovered[0].target_association_type,
            Some("write".to_owned())
        );

        // None case
        let e_none = sample_edge(EdgeKind::Calls, 1);
        let batch2 = edges_to_record_batch(&[e_none]).expect("batch");
        let recovered2 = record_batch_to_edges(&batch2);
        assert_eq!(recovered2[0].source_association_type, None);
        assert_eq!(recovered2[0].target_association_type, None);
    }

    /// M8: attrs_json does NOT contain promoted field names (AC-S40-6 structural check).
    #[test]
    fn m8_promoted_fields_not_in_attrs_json() {
        use super::super::nodes::Param;
        let mut r = sample_node(NodeKind::Function, "m8_no_double_write");
        r.return_type = Some("void".to_owned());
        r.params = Some(vec![Param {
            name: "x".to_owned(),
            type_: "int".to_owned(),
        }]);
        r.is_static = Some(false);
        // attrs_json must not contain promoted field keys
        let promoted_keys = [
            "return_type",
            "is_virtual",
            "is_pure_virtual",
            "is_static",
            "template_args",
            "template_params",
        ];
        for key in promoted_keys {
            assert!(
                !r.attrs_json.contains(key),
                "promoted field '{key}' must NOT appear in attrs_json"
            );
        }
    }

    /// Node schema field names and nullability match ADR-3 + M8 additions.
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
                // M8 columns:
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
        // Nullable columns (base + M8)
        let nullable = [
            "mangled_name",
            "line",
            "col",
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
        for name in nullable {
            let field = schema.field_with_name(name).expect("field must exist");
            assert!(field.is_nullable(), "field {name} must be nullable");
        }
    }

    /// Edge schema field names and nullability match ADR-3 + M8 additions.
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
                // M8 columns:
                "source_association_type",
                "target_association_type",
            ]
        );
        // M8 edge columns are nullable
        for name in ["source_association_type", "target_association_type"] {
            let field = schema.field_with_name(name).expect("field must exist");
            assert!(field.is_nullable(), "field {name} must be nullable");
        }
    }
}
