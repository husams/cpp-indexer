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
        FixedSizeBinaryBuilder, Int64Array, Int64Builder, Int8Array, ListArray, ListBuilder,
        StringArray, StringDictionaryBuilder, StructArray, UInt32Array, UInt32Builder, UInt8Array,
        UInt8Builder,
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
/// graph-symbol-ids (Story 3, v6): two new non-nullable Int64 columns at end.
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
        // graph-symbol-ids columns (Story 3, v6):
        Field::new("symbol_id", DataType::Int64, false),
        Field::new("file_id", DataType::Int64, false),
        // v7 S1 columns:
        Field::new("is_const", DataType::Boolean, true),
        Field::new("is_constexpr", DataType::Boolean, true),
        Field::new("storage_class", DataType::Utf8, true),
        // v7 S2 columns:
        Field::new("is_template", DataType::Boolean, true),
        Field::new("is_noexcept", DataType::Boolean, true),
        Field::new("is_override", DataType::Boolean, true),
        Field::new("is_deleted", DataType::Boolean, true),
        Field::new("is_defaulted", DataType::Boolean, true),
        Field::new("cv_qualifiers", DataType::Utf8, true),
        Field::new("ref_qualifier", DataType::Utf8, true),
        Field::new("is_final", DataType::Boolean, true),
        Field::new("is_abstract", DataType::Boolean, true),
        Field::new("record_kind", DataType::Utf8, true),
        Field::new("type_spelling", DataType::Utf8, true),
        Field::new("param_index", DataType::Int64, true),
        Field::new("param_kind", DataType::Utf8, true),
        // v7 S5 columns:
        Field::new("enum_value", DataType::Int64, true),
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

    // graph-symbol-ids columns (Story 3, v6)
    let mut symbol_id_builder = Int64Builder::new();
    for r in records {
        symbol_id_builder.append_value(r.symbol_id);
    }
    let symbol_id: ArrayRef = Arc::new(symbol_id_builder.finish());

    let mut file_id_builder = Int64Builder::new();
    for r in records {
        file_id_builder.append_value(r.file_id);
    }
    let file_id: ArrayRef = Arc::new(file_id_builder.finish());

    // v7 S1 node columns ──────────────────────────────────────────────────────

    let mut is_const_builder = BooleanBuilder::new();
    for r in records {
        match r.is_const {
            Some(v) => is_const_builder.append_value(v),
            None => is_const_builder.append_null(),
        }
    }
    let is_const: ArrayRef = Arc::new(is_const_builder.finish());

    let mut is_constexpr_builder = BooleanBuilder::new();
    for r in records {
        match r.is_constexpr {
            Some(v) => is_constexpr_builder.append_value(v),
            None => is_constexpr_builder.append_null(),
        }
    }
    let is_constexpr: ArrayRef = Arc::new(is_constexpr_builder.finish());

    let storage_class: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.storage_class.as_deref())
            .collect::<Vec<_>>(),
    ));

    // v7 S2 node columns ──────────────────────────────────────────────────────

    let mut is_template_builder = BooleanBuilder::new();
    for r in records {
        match r.is_template {
            Some(v) => is_template_builder.append_value(v),
            None => is_template_builder.append_null(),
        }
    }
    let is_template: ArrayRef = Arc::new(is_template_builder.finish());

    let mut is_noexcept_builder = BooleanBuilder::new();
    for r in records {
        match r.is_noexcept {
            Some(v) => is_noexcept_builder.append_value(v),
            None => is_noexcept_builder.append_null(),
        }
    }
    let is_noexcept: ArrayRef = Arc::new(is_noexcept_builder.finish());

    let mut is_override_builder = BooleanBuilder::new();
    for r in records {
        match r.is_override {
            Some(v) => is_override_builder.append_value(v),
            None => is_override_builder.append_null(),
        }
    }
    let is_override: ArrayRef = Arc::new(is_override_builder.finish());

    let mut is_deleted_builder = BooleanBuilder::new();
    for r in records {
        match r.is_deleted {
            Some(v) => is_deleted_builder.append_value(v),
            None => is_deleted_builder.append_null(),
        }
    }
    let is_deleted: ArrayRef = Arc::new(is_deleted_builder.finish());

    let mut is_defaulted_builder = BooleanBuilder::new();
    for r in records {
        match r.is_defaulted {
            Some(v) => is_defaulted_builder.append_value(v),
            None => is_defaulted_builder.append_null(),
        }
    }
    let is_defaulted: ArrayRef = Arc::new(is_defaulted_builder.finish());

    let cv_qualifiers: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.cv_qualifiers.as_deref())
            .collect::<Vec<_>>(),
    ));

    let ref_qualifier: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.ref_qualifier.as_deref())
            .collect::<Vec<_>>(),
    ));

    let mut is_final_builder = BooleanBuilder::new();
    for r in records {
        match r.is_final {
            Some(v) => is_final_builder.append_value(v),
            None => is_final_builder.append_null(),
        }
    }
    let is_final: ArrayRef = Arc::new(is_final_builder.finish());

    let mut is_abstract_builder = BooleanBuilder::new();
    for r in records {
        match r.is_abstract {
            Some(v) => is_abstract_builder.append_value(v),
            None => is_abstract_builder.append_null(),
        }
    }
    let is_abstract: ArrayRef = Arc::new(is_abstract_builder.finish());

    let record_kind: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.record_kind.as_deref())
            .collect::<Vec<_>>(),
    ));

    let type_spelling: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.type_spelling.as_deref())
            .collect::<Vec<_>>(),
    ));

    let mut param_index_builder = Int64Builder::new();
    for r in records {
        match r.param_index {
            Some(v) => param_index_builder.append_value(v),
            None => param_index_builder.append_null(),
        }
    }
    let param_index: ArrayRef = Arc::new(param_index_builder.finish());

    let param_kind: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.param_kind.as_deref())
            .collect::<Vec<_>>(),
    ));

    // v7 S5 node column
    let mut enum_value_builder = Int64Builder::new();
    for r in records {
        match r.enum_value {
            Some(v) => enum_value_builder.append_value(v),
            None => enum_value_builder.append_null(),
        }
    }
    let enum_value: ArrayRef = Arc::new(enum_value_builder.finish());

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
            // graph-symbol-ids columns:
            symbol_id,
            file_id,
            // v7 S1 columns:
            is_const,
            is_constexpr,
            storage_class,
            // v7 S2 columns:
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
            type_spelling,
            param_index,
            param_kind,
            // v7 S5 columns:
            enum_value,
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

    // graph-symbol-ids columns (23-24)
    let symbol_id_col = batch
        .column(23)
        .as_any()
        .downcast_ref::<Int64Array>()
        .expect("symbol_id column must be Int64Array");

    let file_id_col = batch
        .column(24)
        .as_any()
        .downcast_ref::<Int64Array>()
        .expect("file_id column must be Int64Array");

    // v7 S1 columns (25-27)
    let is_const_col = batch
        .column(25)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("is_const column must be BooleanArray");

    let is_constexpr_col = batch
        .column(26)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("is_constexpr column must be BooleanArray");

    let storage_class_col = batch
        .column(27)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("storage_class column must be StringArray");

    // v7 S2 columns (28-40)
    let is_template_col = batch
        .column(28)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("is_template column must be BooleanArray");

    let is_noexcept_col = batch
        .column(29)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("is_noexcept column must be BooleanArray");

    let is_override_col = batch
        .column(30)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("is_override column must be BooleanArray");

    let is_deleted_col = batch
        .column(31)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("is_deleted column must be BooleanArray");

    let is_defaulted_col = batch
        .column(32)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("is_defaulted column must be BooleanArray");

    let cv_qualifiers_col = batch
        .column(33)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("cv_qualifiers column must be StringArray");

    let ref_qualifier_col = batch
        .column(34)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("ref_qualifier column must be StringArray");

    let is_final_col = batch
        .column(35)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("is_final column must be BooleanArray");

    let is_abstract_col = batch
        .column(36)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("is_abstract column must be BooleanArray");

    let record_kind_col = batch
        .column(37)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("record_kind column must be StringArray");

    let type_spelling_col = batch
        .column(38)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("type_spelling column must be StringArray");

    let param_index_col = batch
        .column(39)
        .as_any()
        .downcast_ref::<Int64Array>()
        .expect("param_index column must be Int64Array");

    let param_kind_col = batch
        .column(40)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("param_kind column must be StringArray");

    // v7 S5 column (41)
    let enum_value_col = batch
        .column(41)
        .as_any()
        .downcast_ref::<Int64Array>()
        .expect("enum_value column must be Int64Array");

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
                // graph-symbol-ids columns (Story 3, v6):
                symbol_id: symbol_id_col.value(i),
                file_id: file_id_col.value(i),
                // v7 S1 columns:
                is_const: if is_const_col.is_null(i) {
                    None
                } else {
                    Some(is_const_col.value(i))
                },
                is_constexpr: if is_constexpr_col.is_null(i) {
                    None
                } else {
                    Some(is_constexpr_col.value(i))
                },
                storage_class: if storage_class_col.is_null(i) {
                    None
                } else {
                    Some(storage_class_col.value(i).to_owned())
                },
                // v7 S2 columns:
                is_template: if is_template_col.is_null(i) {
                    None
                } else {
                    Some(is_template_col.value(i))
                },
                is_noexcept: if is_noexcept_col.is_null(i) {
                    None
                } else {
                    Some(is_noexcept_col.value(i))
                },
                is_override: if is_override_col.is_null(i) {
                    None
                } else {
                    Some(is_override_col.value(i))
                },
                is_deleted: if is_deleted_col.is_null(i) {
                    None
                } else {
                    Some(is_deleted_col.value(i))
                },
                is_defaulted: if is_defaulted_col.is_null(i) {
                    None
                } else {
                    Some(is_defaulted_col.value(i))
                },
                cv_qualifiers: if cv_qualifiers_col.is_null(i) {
                    None
                } else {
                    Some(cv_qualifiers_col.value(i).to_owned())
                },
                ref_qualifier: if ref_qualifier_col.is_null(i) {
                    None
                } else {
                    Some(ref_qualifier_col.value(i).to_owned())
                },
                is_final: if is_final_col.is_null(i) {
                    None
                } else {
                    Some(is_final_col.value(i))
                },
                is_abstract: if is_abstract_col.is_null(i) {
                    None
                } else {
                    Some(is_abstract_col.value(i))
                },
                record_kind: if record_kind_col.is_null(i) {
                    None
                } else {
                    Some(record_kind_col.value(i).to_owned())
                },
                type_spelling: if type_spelling_col.is_null(i) {
                    None
                } else {
                    Some(type_spelling_col.value(i).to_owned())
                },
                param_index: if param_index_col.is_null(i) {
                    None
                } else {
                    Some(param_index_col.value(i))
                },
                param_kind: if param_kind_col.is_null(i) {
                    None
                } else {
                    Some(param_kind_col.value(i).to_owned())
                },
                // v7 S5 columns:
                enum_value: if enum_value_col.is_null(i) {
                    None
                } else {
                    Some(enum_value_col.value(i))
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
/// graph-symbol-ids (Story 3, v6): three new columns appended: src_id (Int64, non-null),
/// dst_id (Int64, nullable), dst_repo_name (Utf8, non-null).
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
        // graph-symbol-ids columns (Story 3, v6):
        Field::new("src_id", DataType::Int64, false),
        Field::new("dst_id", DataType::Int64, true),
        Field::new("dst_repo_name", DataType::Utf8, false),
        // v7 S1 columns:
        Field::new("access", DataType::Utf8, true),
        // v7 S2 columns:
        Field::new("edge_index", DataType::Int64, true),
        // v7 S4 columns:
        Field::new("inherits_is_virtual", DataType::Boolean, true),
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

    // graph-symbol-ids columns (Story 3, v6)
    let mut src_id_builder = Int64Builder::new();
    for r in records {
        src_id_builder.append_value(r.src_id);
    }
    let src_id: ArrayRef = Arc::new(src_id_builder.finish());

    let mut dst_id_builder = Int64Builder::new();
    for r in records {
        match r.dst_id {
            Some(v) => dst_id_builder.append_value(v),
            None => dst_id_builder.append_null(),
        }
    }
    let dst_id: ArrayRef = Arc::new(dst_id_builder.finish());

    let dst_repo_name: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.dst_repo_name.as_str())
            .collect::<Vec<_>>(),
    ));

    // v7 S1 edge column
    let access: ArrayRef = Arc::new(StringArray::from(
        records
            .iter()
            .map(|r| r.access.as_deref())
            .collect::<Vec<_>>(),
    ));

    // v7 S2 edge column
    let mut edge_index_builder = Int64Builder::new();
    for r in records {
        match r.edge_index {
            Some(v) => edge_index_builder.append_value(v),
            None => edge_index_builder.append_null(),
        }
    }
    let edge_index: ArrayRef = Arc::new(edge_index_builder.finish());

    // v7 S4 edge column
    let mut inherits_is_virtual_builder = BooleanBuilder::new();
    for r in records {
        match r.inherits_is_virtual {
            Some(v) => inherits_is_virtual_builder.append_value(v),
            None => inherits_is_virtual_builder.append_null(),
        }
    }
    let inherits_is_virtual: ArrayRef = Arc::new(inherits_is_virtual_builder.finish());

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
            // graph-symbol-ids columns:
            src_id,
            dst_id,
            dst_repo_name,
            // v7 S1 columns:
            access,
            // v7 S2 columns:
            edge_index,
            // v7 S4 columns:
            inherits_is_virtual,
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

    // graph-symbol-ids columns (11-13)
    let src_id_col = batch
        .column(11)
        .as_any()
        .downcast_ref::<Int64Array>()
        .expect("src_id column must be Int64Array");

    let dst_id_col = batch
        .column(12)
        .as_any()
        .downcast_ref::<Int64Array>()
        .expect("dst_id column must be Int64Array");

    let dst_repo_name_col = batch
        .column(13)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("dst_repo_name column must be StringArray");

    // v7 S1 edge column (14)
    let access_col = batch
        .column(14)
        .as_any()
        .downcast_ref::<StringArray>()
        .expect("access column must be StringArray");

    // v7 S2 edge column (15)
    let edge_index_col = batch
        .column(15)
        .as_any()
        .downcast_ref::<Int64Array>()
        .expect("edge_index column must be Int64Array");

    // v7 S4 edge column (16)
    let inherits_is_virtual_col = batch
        .column(16)
        .as_any()
        .downcast_ref::<BooleanArray>()
        .expect("inherits_is_virtual column must be BooleanArray");

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
                // graph-symbol-ids columns (Story 3, v6):
                src_id: src_id_col.value(i),
                dst_id: if dst_id_col.is_null(i) {
                    None
                } else {
                    Some(dst_id_col.value(i))
                },
                dst_repo_name: dst_repo_name_col.value(i).to_owned(),
                // v7 S1 columns:
                access: if access_col.is_null(i) {
                    None
                } else {
                    Some(access_col.value(i).to_owned())
                },
                // v7 S2 columns:
                edge_index: if edge_index_col.is_null(i) {
                    None
                } else {
                    Some(edge_index_col.value(i))
                },
                // v7 S4 columns:
                inherits_is_virtual: if inherits_is_virtual_col.is_null(i) {
                    None
                } else {
                    Some(inherits_is_virtual_col.value(i))
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
            // v6 integer ID fields
            symbol_id: 0,
            file_id: 0,
            // v7 S1 fields — None by default
            is_const: None,
            is_constexpr: None,
            storage_class: None,
            // v7 S2 fields — None by default
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
            // v7 S5 fields — None by default
            enum_value: None,
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
            // v6 integer ID fields
            src_id: i as i64 + 1,
            dst_id: if i.is_multiple_of(2) {
                Some(i as i64 + 2)
            } else {
                None
            },
            dst_repo_name: "my-repo".to_owned(),
            // v7 S1 fields — None by default
            access: None,
            // v7 S2 fields — None by default
            edge_index: None,
            // v7 S4 fields — None by default
            inherits_is_virtual: None,
        }
    }

    /// For each `NodeKind` variant: build a sample record, write to `RecordBatch`, read back,
    /// assert equality.
    #[test]
    fn node_round_trip_all_variants() {
        for &kind in NodeKind::all() {
            let original = vec![sample_node(kind, &format!("{kind}"))];
            let batch = nodes_to_record_batch(&original).expect("serialisation must succeed");

            // Verify schema shape — 13 base + 10 M8 + 2 v6 + 3 v7-S1 + 13 v7-S2 + 1 v7-S5 = 42
            assert_eq!(batch.num_columns(), 42);
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

            // 9 base + 2 M8 + 3 v6 + 1 v7-S1 + 1 v7-S2 + 1 v7-S4 = 17
            assert_eq!(batch.num_columns(), 17);
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
        // v6 fields round-trip
        assert_eq!(recovered[0].symbol_id, 0);
        assert_eq!(recovered[0].file_id, 0);
        // v7 S1 nullable fields
        assert_eq!(recovered[0].is_const, None);
        assert_eq!(recovered[0].is_constexpr, None);
        assert_eq!(recovered[0].storage_class, None);
        // v7 S2 nullable fields
        assert_eq!(recovered[0].is_template, None);
        assert_eq!(recovered[0].is_noexcept, None);
        assert_eq!(recovered[0].is_override, None);
        assert_eq!(recovered[0].is_deleted, None);
        assert_eq!(recovered[0].is_defaulted, None);
        assert_eq!(recovered[0].cv_qualifiers, None);
        assert_eq!(recovered[0].ref_qualifier, None);
        assert_eq!(recovered[0].is_final, None);
        assert_eq!(recovered[0].is_abstract, None);
        assert_eq!(recovered[0].record_kind, None);
        assert_eq!(recovered[0].type_spelling, None);
        assert_eq!(recovered[0].param_index, None);
        assert_eq!(recovered[0].param_kind, None);
        // v7 S5 nullable field
        assert_eq!(recovered[0].enum_value, None);
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
                // graph-symbol-ids columns (Story 3, v6):
                "symbol_id",
                "file_id",
                // v7 S1 columns:
                "is_const",
                "is_constexpr",
                "storage_class",
                // v7 S2 columns:
                "is_template",
                "is_noexcept",
                "is_override",
                "is_deleted",
                "is_defaulted",
                "cv_qualifiers",
                "ref_qualifier",
                "is_final",
                "is_abstract",
                "record_kind",
                "type_spelling",
                "param_index",
                "param_kind",
                // v7 S5 columns:
                "enum_value",
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
            "symbol_id",
            "file_id",
        ];
        for name in not_null {
            let field = schema.field_with_name(name).expect("field must exist");
            assert!(!field.is_nullable(), "field {name} must be not-null");
        }
        // Nullable columns (base + M8 + v7 S1 + v7 S2)
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
            "is_const",
            "is_constexpr",
            "storage_class",
            "is_template",
            "is_noexcept",
            "is_override",
            "is_deleted",
            "is_defaulted",
            "cv_qualifiers",
            "ref_qualifier",
            "is_final",
            "is_abstract",
            "record_kind",
            "type_spelling",
            "param_index",
            "param_kind",
            // v7 S5:
            "enum_value",
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
                // graph-symbol-ids columns (Story 3, v6):
                "src_id",
                "dst_id",
                "dst_repo_name",
                // v7 S1 columns:
                "access",
                // v7 S2 columns:
                "edge_index",
                // v7 S4 columns:
                "inherits_is_virtual",
            ]
        );
        // M8 edge columns are nullable
        for name in ["source_association_type", "target_association_type"] {
            let field = schema.field_with_name(name).expect("field must exist");
            assert!(field.is_nullable(), "field {name} must be nullable");
        }
        // v6 columns: src_id and dst_repo_name non-null; dst_id nullable
        assert!(
            !schema.field_with_name("src_id").unwrap().is_nullable(),
            "src_id must be non-nullable"
        );
        assert!(
            schema.field_with_name("dst_id").unwrap().is_nullable(),
            "dst_id must be nullable"
        );
        assert!(
            !schema
                .field_with_name("dst_repo_name")
                .unwrap()
                .is_nullable(),
            "dst_repo_name must be non-nullable"
        );
        // v7 S1: access is nullable
        assert!(
            schema.field_with_name("access").unwrap().is_nullable(),
            "access must be nullable"
        );
        // v7 S2: edge_index is nullable
        assert!(
            schema.field_with_name("edge_index").unwrap().is_nullable(),
            "edge_index must be nullable"
        );
        // v7 S4: inherits_is_virtual is nullable
        assert!(
            schema
                .field_with_name("inherits_is_virtual")
                .unwrap()
                .is_nullable(),
            "inherits_is_virtual must be nullable"
        );
    }

    // ── v7 S1 round-trip tests ────────────────────────────────────────────────

    /// v7 S1: `is_const`, `is_constexpr`, `storage_class` round-trip with `Some` values.
    #[test]
    fn v7_s1_node_scalar_fields_some_round_trip() {
        let mut r = sample_node(NodeKind::Field, "v7s1_field");
        r.is_const = Some(true);
        r.is_constexpr = Some(false);
        r.storage_class = Some("static".to_owned());
        let batch = nodes_to_record_batch(&[r.clone()]).expect("batch");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(
            recovered[0].is_const,
            Some(true),
            "is_const Some(true) round-trip failed"
        );
        assert_eq!(
            recovered[0].is_constexpr,
            Some(false),
            "is_constexpr Some(false) round-trip failed"
        );
        assert_eq!(
            recovered[0].storage_class,
            Some("static".to_owned()),
            "storage_class round-trip failed"
        );
    }

    /// v7 S1: node new fields round-trip as `None` for non-Field/GV kinds.
    #[test]
    fn v7_s1_node_fields_none_for_other_kinds() {
        for &kind in &[NodeKind::Function, NodeKind::Method, NodeKind::Class] {
            let r = sample_node(kind, "v7s1_none");
            let batch = nodes_to_record_batch(std::slice::from_ref(&r)).expect("batch");
            let recovered = record_batch_to_nodes(&batch);
            assert_eq!(
                recovered[0].is_const, None,
                "{kind:?}: is_const must be None"
            );
            assert_eq!(
                recovered[0].is_constexpr, None,
                "{kind:?}: is_constexpr must be None"
            );
            assert_eq!(
                recovered[0].storage_class, None,
                "{kind:?}: storage_class must be None"
            );
        }
    }

    /// v7 S1: edge `access` round-trips with `Some` and `None`.
    #[test]
    fn v7_s1_edge_access_round_trip() {
        // Some — simulates HAS_FIELD edge with access set
        let mut e = sample_edge(EdgeKind::HasField, 0);
        e.access = Some("protected".to_owned());
        let batch = edges_to_record_batch(&[e.clone()]).expect("batch");
        let recovered = record_batch_to_edges(&batch);
        assert_eq!(
            recovered[0].access,
            Some("protected".to_owned()),
            "access Some round-trip failed"
        );

        // None — simulates CALLS edge with no access
        let e_none = sample_edge(EdgeKind::Calls, 1);
        let batch2 = edges_to_record_batch(&[e_none]).expect("batch");
        let recovered2 = record_batch_to_edges(&batch2);
        assert_eq!(recovered2[0].access, None, "access None round-trip failed");
    }

    /// v7 S1: `access` field not in `attrs_json` (promoted column, not double-written).
    #[test]
    fn v7_s1_access_not_in_attrs_json() {
        let e = sample_edge(EdgeKind::HasMethod, 0);
        assert!(
            !e.attrs_json.contains("access"),
            "promoted field 'access' must NOT appear in attrs_json"
        );
    }

    // ── v7 S2 round-trip tests ────────────────────────────────────────────────

    /// v7 S2: Function node extended props round-trip with `Some` values.
    #[test]
    fn v7_s2_function_extended_props_round_trip() {
        let mut r = sample_node(NodeKind::Function, "v7s2_fn");
        r.is_template = Some(true);
        r.is_noexcept = Some(false);
        r.is_override = Some(false);
        r.is_deleted = Some(false);
        r.is_defaulted = Some(false);
        r.cv_qualifiers = Some("const".to_owned());
        r.ref_qualifier = Some("&".to_owned());
        let batch = nodes_to_record_batch(&[r.clone()]).expect("batch");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(recovered[0].is_template, Some(true));
        assert_eq!(recovered[0].is_noexcept, Some(false));
        assert_eq!(recovered[0].cv_qualifiers, Some("const".to_owned()));
        assert_eq!(recovered[0].ref_qualifier, Some("&".to_owned()));
    }

    /// v7 S2: Class node extended props round-trip with `Some` values.
    #[test]
    fn v7_s2_class_extended_props_round_trip() {
        let mut r = sample_node(NodeKind::Class, "v7s2_class");
        r.is_template = Some(false);
        r.is_final = Some(true);
        r.is_abstract = Some(true);
        r.record_kind = Some("struct".to_owned());
        let batch = nodes_to_record_batch(&[r.clone()]).expect("batch");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(recovered[0].is_final, Some(true));
        assert_eq!(recovered[0].is_abstract, Some(true));
        assert_eq!(recovered[0].record_kind, Some("struct".to_owned()));
    }

    /// v7 S2: Type node props round-trip with `Some` values.
    #[test]
    fn v7_s2_type_node_props_round_trip() {
        let mut r = sample_node(NodeKind::Type, "v7s2_type");
        r.type_spelling = Some("const std::string &".to_owned());
        let batch = nodes_to_record_batch(&[r.clone()]).expect("batch");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(
            recovered[0].type_spelling,
            Some("const std::string &".to_owned())
        );
    }

    /// v7 S2: Parameter node props round-trip with `Some` values.
    #[test]
    fn v7_s2_parameter_node_props_round_trip() {
        let mut r = sample_node(NodeKind::Parameter, "v7s2_param");
        r.type_spelling = Some("int".to_owned());
        r.param_index = Some(0);
        r.param_kind = Some("value".to_owned());
        let batch = nodes_to_record_batch(&[r.clone()]).expect("batch");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(recovered[0].type_spelling, Some("int".to_owned()));
        assert_eq!(recovered[0].param_index, Some(0));
        assert_eq!(recovered[0].param_kind, Some("value".to_owned()));
    }

    /// v7 S2: `edge_index` round-trips with `Some` and `None`.
    #[test]
    fn v7_s2_edge_index_round_trip() {
        // Some — simulates HAS_PARAM edge with index
        let mut e = sample_edge(EdgeKind::HasParam, 0);
        e.edge_index = Some(2);
        let batch = edges_to_record_batch(&[e.clone()]).expect("batch");
        let recovered = record_batch_to_edges(&batch);
        assert_eq!(
            recovered[0].edge_index,
            Some(2),
            "edge_index Some(2) round-trip failed"
        );

        // None — simulates CALLS edge with no index
        let e_none = sample_edge(EdgeKind::Calls, 1);
        let batch2 = edges_to_record_batch(&[e_none]).expect("batch");
        let recovered2 = record_batch_to_edges(&batch2);
        assert_eq!(
            recovered2[0].edge_index, None,
            "edge_index None round-trip failed"
        );
    }

    /// v7 S2: v7 S2 node fields are None for non-Type/Parameter/Function/Class kinds.
    #[test]
    fn v7_s2_node_fields_none_for_unrelated_kinds() {
        for &kind in &[NodeKind::Field, NodeKind::GlobalVariable, NodeKind::Enum] {
            let r = sample_node(kind, "v7s2_none");
            let batch = nodes_to_record_batch(std::slice::from_ref(&r)).expect("batch");
            let recovered = record_batch_to_nodes(&batch);
            assert_eq!(
                recovered[0].is_template, None,
                "{kind:?}: is_template must be None"
            );
            assert_eq!(
                recovered[0].type_spelling, None,
                "{kind:?}: type_spelling must be None"
            );
            assert_eq!(
                recovered[0].param_index, None,
                "{kind:?}: param_index must be None"
            );
        }
    }

    // ── v7 S4 round-trip tests ────────────────────────────────────────────────

    /// v7 S4: `inherits_is_virtual` round-trips with `Some(true)`, `Some(false)`, and `None`.
    ///
    /// Covers S4-01 (non-virtual → false) and S4-02 (virtual → true) scenarios.
    #[test]
    fn v7_s4_inherits_is_virtual_round_trip() {
        // Some(true) — protected virtual inheritance (S4-02)
        let mut e_virtual = sample_edge(EdgeKind::Inherits, 0);
        e_virtual.access = Some("protected".to_owned());
        e_virtual.inherits_is_virtual = Some(true);
        let batch = edges_to_record_batch(&[e_virtual.clone()]).expect("batch");
        let recovered = record_batch_to_edges(&batch);
        assert_eq!(
            recovered[0].inherits_is_virtual,
            Some(true),
            "inherits_is_virtual Some(true) round-trip failed"
        );
        assert_eq!(
            recovered[0].access,
            Some("protected".to_owned()),
            "access round-trip alongside inherits_is_virtual failed"
        );

        // Some(false) — public non-virtual inheritance (S4-01)
        let mut e_nonvirtual = sample_edge(EdgeKind::Inherits, 2);
        e_nonvirtual.access = Some("public".to_owned());
        e_nonvirtual.inherits_is_virtual = Some(false);
        let batch2 = edges_to_record_batch(&[e_nonvirtual.clone()]).expect("batch");
        let recovered2 = record_batch_to_edges(&batch2);
        assert_eq!(
            recovered2[0].inherits_is_virtual,
            Some(false),
            "inherits_is_virtual Some(false) round-trip failed"
        );

        // None — non-INHERITS edge
        let e_none = sample_edge(EdgeKind::Calls, 1);
        let batch3 = edges_to_record_batch(&[e_none]).expect("batch");
        let recovered3 = record_batch_to_edges(&batch3);
        assert_eq!(
            recovered3[0].inherits_is_virtual, None,
            "inherits_is_virtual None round-trip failed"
        );
    }

    /// v7 S4: `inherits_is_virtual` field not in `attrs_json` (promoted column).
    #[test]
    fn v7_s4_inherits_is_virtual_not_in_attrs_json() {
        let e = sample_edge(EdgeKind::Inherits, 0);
        assert!(
            !e.attrs_json.contains("inherits_is_virtual"),
            "promoted field 'inherits_is_virtual' must NOT appear in attrs_json"
        );
    }

    // ── v7 S5 round-trip tests ────────────────────────────────────────────────

    /// v7 S5: `enum_value` round-trips with `Some(i64)` values including negative.
    #[test]
    fn v7_s5_enumerator_node_props_round_trip() {
        // Positive value
        let mut r = sample_node(NodeKind::Enumerator, "v7s5_enum_pos");
        r.enum_value = Some(42);
        let batch = nodes_to_record_batch(&[r.clone()]).expect("batch");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(
            recovered[0].enum_value,
            Some(42),
            "enum_value Some(42) round-trip failed"
        );

        // Negative value (e.g. -1 for sentinel)
        let mut r_neg = sample_node(NodeKind::Enumerator, "v7s5_enum_neg");
        r_neg.enum_value = Some(-1);
        let batch2 = nodes_to_record_batch(&[r_neg.clone()]).expect("batch");
        let recovered2 = record_batch_to_nodes(&batch2);
        assert_eq!(
            recovered2[0].enum_value,
            Some(-1),
            "enum_value Some(-1) round-trip failed"
        );

        // None — non-Enumerator kind
        let r_none = sample_node(NodeKind::Enum, "v7s5_none");
        let batch3 = nodes_to_record_batch(&[r_none]).expect("batch");
        let recovered3 = record_batch_to_nodes(&batch3);
        assert_eq!(
            recovered3[0].enum_value, None,
            "enum_value must be None for non-Enumerator kinds"
        );
    }

    /// v7 S5: `enum_value` not in `attrs_json` (promoted column, not double-written).
    #[test]
    fn v7_s5_enum_value_not_in_attrs_json() {
        let r = sample_node(NodeKind::Enumerator, "v7s5_no_double_write");
        assert!(
            !r.attrs_json.contains("enum_value"),
            "promoted field 'enum_value' must NOT appear in attrs_json"
        );
    }

    /// v7 S5: Enumerator NodeKind round-trips through Arrow.
    #[test]
    fn v7_s5_enumerator_node_kind_round_trip() {
        let r = sample_node(NodeKind::Enumerator, "v7s5_kind");
        let batch = nodes_to_record_batch(std::slice::from_ref(&r)).expect("batch");
        let recovered = record_batch_to_nodes(&batch);
        assert_eq!(recovered[0].kind, NodeKind::Enumerator);
    }
}
