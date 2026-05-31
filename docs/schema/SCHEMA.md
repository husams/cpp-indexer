# cpp-indexer Graph Schema Reference

Schema version: **cxg-schema-v7** (v7 full-AST schema, ADR-1)

This document is the authoritative table of every promoted property for v7.
"Promoted" means the field has been extracted from `attrs_json` into a native
typed column in Arrow, Neo4j, and IndraDB.  Fields still remaining in
`attrs_json` (e.g. `exception_spec`, `control_flow`, bit-field info) are out
of scope and are documented in per-kind `attrs_json` sections of the code
comments.

---

## Node promoted properties

### M8 fields (10 node fields, S40–S43)

| Field | Type | Applicable NodeKind(s) | Source of truth | Sample Cypher |
|---|---|---|---|---|
| `return_type` | `string \| null` | `FUNCTION`, `METHOD` | `src/visit/shallow.rs` | `MATCH (n:Node {kind:'METHOD'}) WHERE n.return_type IS NOT NULL RETURN n.name, n.return_type LIMIT 10` |
| `params` | `List<{name:string, type:string}> \| null` | `FUNCTION`, `METHOD` | `src/visit/shallow.rs` | `MATCH (n:Node {kind:'FUNCTION'}) WHERE size(n.params) > 2 RETURN n.qualified_name, n.params LIMIT 5` |
| `signature` | `string \| null` | `FUNCTION`, `METHOD` | `src/visit/shallow.rs` | `MATCH (n:Node) WHERE n.signature CONTAINS 'const' RETURN n.qualified_name, n.signature LIMIT 10` |
| `code` | `string \| null` | `FUNCTION`, `METHOD` | `src/visit/shallow.rs`, `src/schema/limits.rs` | `MATCH (n:Node {kind:'FUNCTION'}) WHERE n.code IS NOT NULL RETURN n.name, n.code_truncated LIMIT 5` |
| `code_truncated` | `bool \| null` | `FUNCTION`, `METHOD` | `src/visit/shallow.rs`, `src/schema/limits.rs` | `MATCH (n:Node) WHERE n.code_truncated = true RETURN n.qualified_name LIMIT 10` |
| `template_params` | `List<{name:string, kind:string, default:string\|null}> \| null` | `TEMPLATE_DECL` | `src/visit/shallow.rs` | `MATCH (n:Node {kind:'TEMPLATE_DECL'}) WHERE n.template_params IS NOT NULL RETURN n.name, n.template_params LIMIT 5` |
| `template_args` | `List<{kind:string, value:string}> \| null` | `SPECIALIZATION` | `src/visit/shallow.rs` | `MATCH (n:Node {kind:'SPECIALIZATION'}) WHERE n.template_args IS NOT NULL RETURN n.name, n.template_args LIMIT 5` |
| `is_virtual` | `bool \| null` | `METHOD` | `src/visit/shallow.rs` | `MATCH (n:Node {kind:'METHOD', is_virtual:true}) RETURN n.qualified_name LIMIT 20` |
| `is_pure_virtual` | `bool \| null` | `METHOD` | `src/visit/shallow.rs` | `MATCH (n:Node {kind:'METHOD', is_pure_virtual:true}) RETURN n.qualified_name LIMIT 20` |
| `is_static` | `bool \| null` | `FUNCTION`, `METHOD` | `src/visit/shallow.rs` | `MATCH (n:Node {kind:'METHOD', is_static:true}) RETURN n.qualified_name LIMIT 20` |

### v7 S1 node fields (Field/GlobalVariable)

| Field | Type | Applicable NodeKind(s) |
|---|---|---|
| `is_const` | `bool \| null` | `FIELD`, `GLOBAL_VARIABLE` |
| `is_constexpr` | `bool \| null` | `FIELD`, `GLOBAL_VARIABLE` |
| `storage_class` | `string \| null` | `FIELD`, `GLOBAL_VARIABLE` |

`storage_class` values: `"auto"` | `"static"` | `"extern"` | `"thread_local"` | `"register"` | `"none"`.

### v7 S2 node fields (Function/Method/Class)

| Field | Type | Applicable NodeKind(s) |
|---|---|---|
| `is_template` | `bool \| null` | `FUNCTION`, `METHOD`, `CLASS`, `TEMPLATE_DECL` |
| `is_noexcept` | `bool \| null` | `FUNCTION`, `METHOD` |
| `is_override` | `bool \| null` | `METHOD` |
| `is_deleted` | `bool \| null` | `FUNCTION`, `METHOD` |
| `is_defaulted` | `bool \| null` | `FUNCTION`, `METHOD` |
| `cv_qualifiers` | `string \| null` | `METHOD` |
| `ref_qualifier` | `string \| null` | `METHOD` |
| `is_final` | `bool \| null` | `CLASS` |
| `is_abstract` | `bool \| null` | `CLASS` |
| `record_kind` | `string \| null` | `CLASS` |
| `type_spelling` | `string \| null` | `TYPE`, `PARAMETER`, `TEMPLATE_ARG` |
| `param_index` | `int \| null` | `PARAMETER`, `TEMPLATE_ARG` |
| `param_kind` | `string \| null` | `PARAMETER`, `TEMPLATE_ARG` |

`cv_qualifiers` values: `"const"` | `"volatile"` | `"const volatile"` | `""` (empty = unqualified).
`ref_qualifier` values: `"&"` (lvalue) | `"&&"` (rvalue) | `""` (empty = none).
`record_kind` values: `"class"` | `"struct"` | `"union"`.
`param_kind` values: `"type"` | `"non_type"` | `"template"` | `"value"`.

### v7 S5 node fields (Enumerator)

| Field | Type | Applicable NodeKind(s) |
|---|---|---|
| `enum_value` | `int \| null` | `ENUMERATOR` |

`enum_value` is the signed i64 constant value from `clang_getEnumConstantDeclValue`.

---

## Edge promoted properties

### M8 edge fields (2 fields, S43)

| Field | Type | Applicable EdgeKind(s) | Source of truth |
|---|---|---|---|
| `source_association_type` | `string \| null` | `USES` | `src/visit/access_classifier.rs`, `src/sink/neo4j.rs` |
| `target_association_type` | `string \| null` | `USES` | `src/visit/access_classifier.rs`, `src/sink/neo4j.rs` |

`source_association_type` / `target_association_type` value set (closed enum `AccessKind`):

| Value | Meaning |
|---|---|
| `read` | RHS of assignment, condition, or const-ref argument |
| `write` | LHS of assignment or `++`/`--` operand |
| `addr_of` | Address-of operator (`&x`) or pointer-typed parameter match |
| `call_arg` | Argument where the parameter type is dependent or unresolved |
| `return` | Operand of a `return` statement |
| `decl_ref` | Reference inside a declaration initializer or default value |
| `unknown` | Anything else; logged at `debug` level (see ADR-13) |

### v7 edge fields

| Field | Type | Applicable EdgeKind(s) |
|---|---|---|
| `access` | `string \| null` | `HAS_METHOD`, `HAS_FIELD`, `INHERITS` |
| `edge_index` | `int \| null` | `HAS_PARAM`, `TEMPLATE_PARAM`, `TEMPLATE_ARG` |
| `inherits_is_virtual` | `bool \| null` | `INHERITS` |

`access` values: `"public"` | `"protected"` | `"private"`. Always emitted (including `"public"`) per ADR-3/OQ-9.

`edge_index` is the 0-based ordering index for ordered-traversal edges (ADR-5). Consumers MUST sort on `edge_index` (or the target node's `param_index`) to obtain parameter/argument order; IndraDB does not guarantee traversal order. This contract applies to `HAS_PARAM`, `TEMPLATE_PARAM`, and `TEMPLATE_ARG` edges.

`inherits_is_virtual` is pinned as `inherits_is_virtual` (not `is_virtual`) to distinguish from the node-level method `is_virtual` property; both names appear as standalone tokens so the schema_drift word-boundary checker finds them independently (design §3.4 / ADR-1).

---

## Field applicability matrix (node)

| Field | FUNCTION | METHOD | CLASS | TEMPLATE\_DECL | FIELD | GLOBAL\_VARIABLE | TYPE | PARAMETER | TEMPLATE\_ARG | ENUMERATOR | other |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `return_type` | yes | yes | — | — | — | — | — | — | — | — | — |
| `params` | yes | yes | — | — | — | — | — | — | — | — | — |
| `signature` | yes | yes | — | — | — | — | — | — | — | — | — |
| `code` | yes | yes | — | — | — | — | — | — | — | — | — |
| `code_truncated` | yes | yes | — | — | — | — | — | — | — | — | — |
| `template_params` | — | — | — | yes | — | — | — | — | — | — | — |
| `template_args` | — | — | — | — | — | — | — | — | — | — | — |
| `is_virtual` | — | yes | — | — | — | — | — | — | — | — | — |
| `is_pure_virtual` | — | yes | — | — | — | — | — | — | — | — | — |
| `is_static` | yes | yes | — | — | — | — | — | — | — | — | — |
| `is_const` | — | — | — | — | yes | yes | — | — | — | — | — |
| `is_constexpr` | — | — | — | — | yes | yes | — | — | — | — | — |
| `storage_class` | — | — | — | — | yes | yes | — | — | — | — | — |
| `is_template` | yes | yes | yes | yes | — | — | — | — | — | — | — |
| `is_noexcept` | yes | yes | — | — | — | — | — | — | — | — | — |
| `is_override` | — | yes | — | — | — | — | — | — | — | — | — |
| `is_deleted` | yes | yes | — | — | — | — | — | — | — | — | — |
| `is_defaulted` | yes | yes | — | — | — | — | — | — | — | — | — |
| `cv_qualifiers` | — | yes | — | — | — | — | — | — | — | — | — |
| `ref_qualifier` | — | yes | — | — | — | — | — | — | — | — | — |
| `is_final` | — | — | yes | — | — | — | — | — | — | — | — |
| `is_abstract` | — | — | yes | — | — | — | — | — | — | — | — |
| `record_kind` | — | — | yes | — | — | — | — | — | — | — | — |
| `type_spelling` | — | — | — | — | — | — | yes | yes | yes | — | — |
| `param_index` | — | — | — | — | — | — | — | yes | yes | — | — |
| `param_kind` | — | — | — | — | — | — | — | yes | yes | — | — |
| `enum_value` | — | — | — | — | — | — | — | — | — | yes | — |

`None` / `null` for any cell marked `—`.

---

## Ordering contract (ADR-5)

Edges `HAS_PARAM`, `TEMPLATE_PARAM`, and `TEMPLATE_ARG` carry an `edge_index`
column (0-based integer).  Consumers that need ordered traversal (e.g. to
reconstruct parameter lists or template argument lists in declaration order)
**MUST sort by `edge_index` on the edge, or by `param_index` on the target
node.**

IndraDB does not guarantee traversal order — it returns edges in arbitrary
order.  Neo4j likewise makes no ordering guarantee without an explicit
`ORDER BY`.

Canonical consumer pattern:

```cypher
// HAS_PARAM in order
MATCH (f:Node {qualified_name:'MyFunc'})-[e:EDGE {kind:'HAS_PARAM'}]->(p:Node)
RETURN p.name, p.type_spelling, e.edge_index
ORDER BY e.edge_index

// TEMPLATE_ARG in order
MATCH (s:Node {kind:'SPECIALIZATION'})-[e:EDGE {kind:'TEMPLATE_ARG'}]->(a:Node)
RETURN a.type_spelling, a.param_kind, e.edge_index
ORDER BY e.edge_index
```

---

## Fidelity section — template argument extraction limits (C5, ADR-7)

### What IS extractable

- **Explicit type arguments** for non-dependent instantiations: `std::vector<int>` →
  one `TemplateArg` node with `type_spelling = "int"`, connected via `OF_TYPE` to the
  `Type` node for `int`.
- **Non-type (integral) arguments**: emitted as `TemplateArg` nodes with
  `param_kind = "non_type"` and `type_spelling` holding the value string.
- **Template template arguments**: `param_kind = "template"`, `type_spelling` holds
  the template name.
- **C++20 Concept constraints** (`CXCursor_ConceptDecl`): the `Concept` node +
  `CONSTRAINED_BY` edge are defined in the schema and round-trip through the
  exporter, but **emission is currently a stub** — see the binding-gap note below.

### What IS NOT extractable (documented fidelity gaps)

- **Dependent template arguments**: when a template argument depends on another
  template parameter (e.g. `Outer<T>::Inner` inside a template), libclang reports
  `CXType_Invalid` or `CXType_Dependent`.  The indexer emits a partial `TemplateArg`
  node with `type_spelling = "<dependent>"` and skips `OF_TYPE`; it does **not** fail
  the TU (C5 / Issue-0001 guard).  The closing summary shows `failed: 0` for that TU.
- **Pseudo-destructor calls** (`T::~T()` in dependent context): libclang does not
  expose a stable USR for pseudo-destructor cursors; no `CALLS` edge is emitted and
  the TU is not failed.
- **Unexpanded parameter packs** (`sizeof...(Args)`, pack expansions in dependent
  contexts): libclang may return `CXType_Unexposed` or `CXType_Invalid`.  The indexer
  skips those positions and emits only the recoverable arguments.
- **Pre-C++20 SFINAE** (`std::enable_if`, `std::void_t`, `requires`-less constraints):
  no `CONSTRAINED_BY` edge is emitted.  SFINAE constraint structure is not modelled
  by this schema (ADR-7).  Concept-based constraints (`requires` clauses, C++20) are
  the supported path.

### Binding-gap limitations (clang-rs 2.0.0 / current platform)

These v7 fields and edges are present in the schema and round-trip correctly, but
their **values are not yet populated** because the `clang-rs 2.0.0` binding does not
expose the required libclang query (or the cursor is not surfaced on the test
platform, Apple Clang 17 / macOS arm64). Consumers must treat them as *unknown*, not
as authoritative `false`/absent:

- **Always-`false` boolean properties** (round-trip correct, but hardwired `false`):
  `is_override`, `is_noexcept`, `is_deleted`, `is_defaulted` (Function/Method),
  `is_constexpr` (Field/GlobalVariable), `is_final` (Class). A query like "is this
  method an override?" will currently return `false` even when it is one.
- **`Concept` node + `CONSTRAINED_BY` edge**: emission is a no-op stub — `clang-rs
  2.0.0` lacks `EntityKind::ConceptDecl`, so no `Concept` nodes are produced and no
  `CONSTRAINED_BY` edges are emitted, even in C++20 mode.
- **`TemplateArg` positional nodes (Q8)**: `ClassTemplatePartialSpecialization`
  cursors are not surfaced on Apple Clang 17, so partial-specialization template-arg
  extraction is **unverified on this platform**; the schema plumbing and
  `TEMPLATE_PARAM` path are covered, but Q8 ("template args of X, ordered by index")
  is not yet demonstrated end-to-end here. May behave differently under libclang 18.
- **`UNDERLYING_TYPE` edge**: emitted for *all* enums, including those relying on the
  compiler-default `int` underlying type — not only enums with an explicit
  `: <type>`. Filter on explicitness downstream if required.

These are tracked as fidelity follow-ups; none fail the TU (Issue-0001 guard holds).

---

## Neo4j covering indexes (S44, ADR-15)

Four indexes added in M8 alongside the two existing v4 indexes:

| Index name | Cypher |
|---|---|
| `node_return_type_idx` | `CREATE INDEX node_return_type_idx IF NOT EXISTS FOR (n:Node) ON (n.return_type)` |
| `node_is_virtual_idx` | `CREATE INDEX node_is_virtual_idx IF NOT EXISTS FOR (n:Node) ON (n.is_virtual)` |
| `node_is_static_idx` | `CREATE INDEX node_is_static_idx IF NOT EXISTS FOR (n:Node) ON (n.is_static)` |
| `node_kind_return_type_idx` | `CREATE INDEX node_kind_return_type_idx IF NOT EXISTS FOR (n:Node) ON (n.kind, n.return_type)` |

Source of truth: `src/sink/neo4j.rs` — constants `CQL_ENSURE_*_INDEX`.

---

## Example queries

### Q1 — Find all functions returning a given type

```cypher
MATCH (n:Node)
WHERE n.kind IN ['FUNCTION', 'METHOD']
  AND n.return_type = 'std::vector<int>'
RETURN n.qualified_name, n.signature, n.return_type
ORDER BY n.qualified_name
LIMIT 25
```

### Q3 — Parameter list for a function (ordered)

```cypher
MATCH (f:Node {qualified_name:'MyFunc'})-[e:EDGE {kind:'HAS_PARAM'}]->(p:Node)
RETURN p.name, p.type_spelling, e.edge_index
ORDER BY e.edge_index
LIMIT 50
```

### Q5 — Find USES edges by source_association_type

```cypher
MATCH (src:Node)-[r:EDGE {kind:'USES'}]->(dst:Node {qualified_name:'MyNamespace::counter'})
WHERE r.source_association_type = 'write'
RETURN src.qualified_name, src.kind, r.source_association_type
ORDER BY src.qualified_name
LIMIT 25
```

### Q6 — Classes inheriting X filtered by access and virtual

```cypher
MATCH (d:Node)-[r:EDGE {kind:'INHERITS'}]->(b:Node {qualified_name:'Base'})
WHERE r.access = 'public' AND r.inherits_is_virtual = false
RETURN d.qualified_name
ORDER BY d.qualified_name
LIMIT 25
```

### Q8 — Template arguments of an instantiation ordered by index

```cypher
MATCH (s:Node {kind:'SPECIALIZATION', name:'MyTemplate'})-[e:EDGE {kind:'TEMPLATE_ARG'}]->(a:Node)
RETURN a.type_spelling, a.param_kind, e.edge_index
ORDER BY e.edge_index
LIMIT 50
```

---

## Schema version

The `SCHEMA_VERSION` constant is defined in `src/schema/version.rs`. A
`SchemaVersion` singleton node is written to Neo4j at the start of every Phase
4 run (ADR-9, ADR-11).  Any graph indexed with v6 or earlier must be wiped and
re-indexed against v7 — see the re-index recipe in
`docs/runbooks/staging-recovery.md` §5.

Read-tolerance: the v7 binary never deserializes v6 Arrow/Parquet data through
`arrow.rs` — the Parquet magic bump (`cxg_parquet_v7`) triggers full-invalidation
of stale v6 staging shards so they are re-parsed.  v6 durable graphs are refused
at write-handshake by `check_schema_version_for_write` until reset (ADR-4).

---

## References

- ADR-1: schema version bump policy (atomic, last commit)
- ADR-2: Type node written-spelling dedup
- ADR-3: access on HasMethod/HasField/Inherits; no REFERENCES/MEMBER_OF
- ADR-4: C2 read-tolerance = version-gate + re-parse, not arrow.rs defaulting
- ADR-5: positional TemplateArg nodes; consumer-side ordering on param_index
- ADR-7: C++20 concepts only; pre-C++20 SFINAE not modelled
- ADR-11: schema version bump policy and no-dual-write rule
- ADR-12: `code` inline cap (32 KiB)
- ADR-13: USES access classifier taxonomy and EXTERNAL_REF mirroring
- ADR-14: Arrow / Neo4j / IndraDB serialization of structured lists
- ADR-15: Neo4j covering indexes and IndraDB property-index parity
- `src/schema/nodes.rs` — `NodeRecord` struct (source of truth for field names and types)
- `src/schema/edges.rs` — `EdgeRecord` struct
- `src/visit/access_classifier.rs` — `AccessKind` enum
- `prompt/graph_database/cpp/schema.txt` — MCP system-prompt schema (must stay in sync)
