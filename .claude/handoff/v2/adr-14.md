# ADR-14: Serialization of structured list fields (params, template_params, template_args) across Arrow / Neo4j / IndraDB

Status: accepted
Date: 2026-05-18
Resolves: AC-S40-2/4, AC-S42-2/3, AC-S44-1, AC-S45-1, requirements (implicit serialization question across stories)

## Context

S40 promotes three list-of-record fields onto `NodeRecord`:

- `params: Option<Vec<Param>>` where `Param = {name: String, type: String}`
- `template_params: Option<Vec<TemplateParam>>` where `TemplateParam = {name: String, kind: String, default: Option<String>}`
- `template_args: Option<Vec<TemplateArg>>` where `TemplateArg = {kind: String, value: String}`

These flow through four serialization boundaries:

1. **In-memory** (`NodeRecord` Rust struct) — typed Rust.
2. **Arrow / Parquet staging** (`src/schema/arrow.rs`) — must round-trip without loss (AC-S40-4).
3. **Neo4j over Bolt** (`src/sink/neo4j.rs`) — Cypher property values.
4. **IndraDB over gRPC** (`src/sink/indradb.rs`) — `indradb::Json` property values per AC-S45-1 ("not folded into a single JSON blob").

AC-S42-2/3 explicitly require structured (not debug-string) representation queryable from Cypher; AC-S45-1 requires "distinct vertex property key" per promoted field. The plain reading of AC-S45-1 is that the prohibition is against one mega-blob containing *all* promoted fields together — per-field `Json` properties (`params`, `template_params`, `template_args`) are distinct keys and satisfy it.

## Decision

### In-memory (Rust)

Define small named structs in `src/schema/nodes.rs`:

```rust
#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct Param { pub name: String, pub type_: String }  // serde rename `type_` -> `type`

#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct TemplateParam { pub name: String, pub kind: String, #[serde(default)] pub default: Option<String> }

#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct TemplateArg { pub kind: String, pub value: String }
```

`NodeRecord` carries `Option<Vec<Param>>` etc. (`None` = field not applicable to this node kind; `Some(vec![])` = applicable but empty).

### Arrow / Parquet (typed columns, not JSON strings)

Use Arrow `List<Struct<...>>`:

```text
params:          List<Struct<name: Utf8, type: Utf8>>             nullable
template_params: List<Struct<name: Utf8, kind: Utf8, default: Utf8 (nullable)>> nullable
template_args:   List<Struct<kind: Utf8, value: Utf8>>            nullable
```

Builders: `ListBuilder<StructBuilder<...>>` from `arrow::array`. Round-trip tests (AC-S40-4) cover `Some(non-empty)`, `Some(empty)`, `None` for every field.

Rationale: typed columns preserve the structure for downstream Parquet consumers and survive the Arrow round-trip cleanly; reverting to JSON strings here would re-introduce the parsing-cost problem PRD §2 was created to solve.

### Neo4j (Bolt parameters)

Bolt natively supports list-of-map values. The Cypher `UNWIND $rows AS row` pattern in `CQL_MERGE_NODES` (`src/sink/neo4j.rs:53`) accepts a parameter where `row.params` is a `List<Map<String, String>>`. Update the Cypher SET clause to add the new properties as native list-of-map values:

```cypher
SET n.params           = row.params,            -- list of {name, type} maps
    n.template_params  = row.template_params,
    n.template_args    = row.template_args,
    n.return_type      = row.return_type,
    n.signature        = row.signature,
    n.code             = row.code,
    n.code_truncated   = row.code_truncated,
    n.is_virtual       = row.is_virtual,
    n.is_pure_virtual  = row.is_pure_virtual,
    n.is_static        = row.is_static
```

Neo4j 5 stores `List<Map>` as the native composite type. Queries can filter with `any(a IN s.template_args WHERE a.kind = 'integral')` (PRD §6 Q4) without JSON parsing. The conversion from Rust `Vec<Param>` to `neo4rs::BoltType::List<BoltType::Map>` is straightforward in the row-builder.

### IndraDB (`indradb::Json` property per top-level field)

Each list field becomes one `indradb::Json(serde_json::Value)` property:

```rust
BulkInsertItem::VertexProperty {
    id: vid,
    name: ident("params")?,
    value: Json::new(serde_json::to_value(&record.params)?),
}
```

Same pattern for `template_params`, `template_args`. Scalar promoted fields (`return_type`, `signature`, `code`, `code_truncated`, `is_virtual`, `is_pure_virtual`, `is_static`) are written as their native IndraDB scalar property kinds.

**AC-S45-1 reading (binding):** "not folded into a single JSON blob" prohibits one mega-blob containing every promoted field. Per-field `Json` properties — `params`, `template_params`, `template_args` as three distinct property keys — satisfy "distinct vertex property key" per AC-S45-1. The alternative (decomposing `Vec<Param>` into N synthetic property keys `params[0].name`, `params[0].type`, …) is rejected as a misreading; it would explode the property count on highly-parameterized templates and is not asked for.

### Edge fields (`source_association_type`, `target_association_type`)

Both are `Option<String>` valued from the closed enum in ADR-13. Trivial across all three serialization layers:
- Arrow: `Utf8` nullable column on `EdgeRecord`.
- Neo4j: scalar Cypher edge property.
- IndraDB: scalar string edge property.

## Alternatives considered

- **JSON string everywhere (`params: Option<String>` holding a JSON blob).** Rejected: defeats AC-S42-2 ("not a debug string") for Cypher queries; forces consumers to use `apoc.convert.fromJsonMap` which PRD §2.1 explicitly named as the failure mode.
- **Side-table per list type (one Arrow file per field).** Rejected: forces a JOIN in every consumer; AC-S40-4 explicitly assumes round-trip within `NodeRecord`.
- **Decomposed property keys on IndraDB (`params[0].name`, `params[0].type`, …).** Rejected: misreading of AC-S45-1; produces unbounded property-key count; not queryable as a unit; not what "distinct property key" means in IndraDB's data model.
- **Flatten `Vec<Param>` into two parallel arrays (`param_names: Vec<String>`, `param_types: Vec<String>`).** Rejected: order-coupling is invariant by convention rather than by type; one struct list is clearer and survives Bolt natively.

## Consequences

Positive:
- Single in-memory shape; three serialization adapters, each idiomatic to its sink.
- Cypher queries like PRD §6 Q4 work natively without JSON parsing.
- AC-S40-4 round-trip is straightforward (every layer preserves typed structure).
- IndraDB `Json` properties remain individually queryable via `get_vertex_properties`; satisfies AC-S45-3.

Negative:
- Three serialization adapters to maintain; mitigated by exhaustive round-trip tests in S40 (Arrow ↔ NodeRecord) and parity tests in S44/S45 (sink ↔ NodeRecord).
- Arrow `ListBuilder<StructBuilder>` is more verbose than a `StringArray`; one-time cost.
- IndraDB consumers must `serde_json::from_value` on the `Json` property to get a typed `Vec<Param>`; documented in `docs/schema/SCHEMA.md` (AC-S46-1).

Follow-ups:
- If a future story needs per-element indexing on IndraDB (e.g., "all nodes where any param.type contains `Slice`"), evaluate decomposed properties then. Not in M8 scope.

## References

- `[[pages/planning/cpp-indexer-structured-attrs-prd]]` §AC-S1.2/1.5/1.6, §AC-S3.1, §6 Q4
- requirements.md AC-S40-2, AC-S40-4, AC-S42-2, AC-S42-3, AC-S44-1, AC-S45-1, AC-S45-3
- `src/schema/arrow.rs:28-44` (existing Field declarations)
- `src/sink/neo4j.rs:53` (`CQL_MERGE_NODES`)
- `src/sink/indradb.rs:62-72` (existing property-name constants)
- Cognee tags: `task:cpp-indexer-m8 role:architect`
