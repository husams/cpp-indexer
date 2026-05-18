# cpp-indexer Graph Schema Reference

Schema version: **cxg-schema-v5** (M8, ADR-11)

This document is the authoritative table of every promoted property for v5.
"Promoted" means the field has been extracted from `attrs_json` into a native
typed column in Arrow, Neo4j, and IndraDB.  Fields still remaining in
`attrs_json` (e.g. `exception_spec`, `control_flow`, bit-field info) are out
of scope for M8 and are documented in per-kind `attrs_json` sections of the
code comments.

---

## Node promoted properties (10 fields, S40–S43)

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

### Field applicability matrix

| Field | FUNCTION | METHOD | TEMPLATE_DECL | SPECIALIZATION | CLASS | other |
|---|---|---|---|---|---|---|
| `return_type` | yes | yes | — | — | — | — |
| `params` | yes | yes | — | — | — | — |
| `signature` | yes | yes | — | — | — | — |
| `code` | yes | yes | — | — | — | — |
| `code_truncated` | yes | yes | — | — | — | — |
| `template_params` | — | — | yes | — | — | — |
| `template_args` | — | — | — | yes | — | — |
| `is_virtual` | — | yes | — | — | — | — |
| `is_pure_virtual` | — | yes | — | — | — | — |
| `is_static` | yes | yes | — | — | — | — |

`None` / `null` for any cell marked `—`.

---

## Edge promoted properties (2 fields, S43)

| Field | Type | Applicable EdgeKind(s) | Source of truth | Sample Cypher |
|---|---|---|---|---|
| `source_association_type` | `string \| null` | `USES` | `src/visit/access_classifier.rs`, `src/sink/neo4j.rs` | `MATCH ()-[r:EDGE {kind:'USES'}]->() WHERE r.source_association_type = 'read' RETURN count(r)` |
| `target_association_type` | `string \| null` | `USES` | `src/visit/access_classifier.rs`, `src/sink/neo4j.rs` | `MATCH ()-[r:EDGE {kind:'USES'}]->() WHERE r.target_association_type = 'write' RETURN count(r)` |

### `source_association_type` / `target_association_type` value set

Exactly 7 values (closed enum `AccessKind` in `src/visit/access_classifier.rs`):

| Value | Meaning |
|---|---|
| `read` | RHS of assignment, condition, or const-ref argument |
| `write` | LHS of assignment or `++`/`--` operand |
| `addr_of` | Address-of operator (`&x`) or pointer-typed parameter match |
| `call_arg` | Argument where the parameter type is dependent or unresolved |
| `return` | Operand of a `return` statement |
| `decl_ref` | Reference inside a declaration initializer or default value |
| `unknown` | Anything else; logged at `debug` level (see ADR-13) |

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

## Example queries (Q1, Q5)

### Q1 — Find all functions returning a given type (uses `return_type` + `signature`)

```cypher
// Q1: functions/methods returning a specific type, with full signature
MATCH (n:Node)
WHERE n.kind IN ['FUNCTION', 'METHOD']
  AND n.return_type = 'std::vector<int>'
RETURN n.qualified_name, n.signature, n.return_type
ORDER BY n.qualified_name
LIMIT 25
```

This query is accelerated by the `node_kind_return_type_idx` composite index.

### Q5 — Find USES edges by source_association_type (classifier output)

```cypher
// Q5: find all 'write' usages of a symbol, e.g. to find mutation sites
MATCH (src:Node)-[r:EDGE {kind:'USES'}]->(dst:Node {qualified_name:'MyNamespace::counter'})
WHERE r.source_association_type = 'write'
RETURN src.qualified_name, src.kind, r.source_association_type
ORDER BY src.qualified_name
LIMIT 25
```

Combine with `EXTERNAL_REF` edges to track cross-repo mutation:

```cypher
// Q5 extended: cross-repo write usages via EXTERNAL_REF
MATCH (src:Node)-[r:EDGE]->(dst:Node {qualified_name:'MyNamespace::counter'})
WHERE r.kind IN ['USES', 'EXTERNAL_REF']
  AND r.source_association_type = 'write'
RETURN src.qualified_name, src.repo_name, r.kind
ORDER BY src.repo_name, src.qualified_name
LIMIT 25
```

---

## Schema version

The `SCHEMA_VERSION` constant is defined in `src/schema/version.rs`. A
`SchemaVersion` singleton node is written to Neo4j at the start of every Phase
4 run (ADR-9, ADR-11).  Any graph indexed with v4 or earlier must be wiped and
re-indexed against v5 — see the re-index recipe in
`docs/runbooks/staging-recovery.md` §5.

---

## References

- ADR-11: schema version bump policy and no-dual-write rule
- ADR-12: `code` inline cap (32 KiB)
- ADR-13: USES access classifier taxonomy and EXTERNAL_REF mirroring
- ADR-14: Arrow / Neo4j / IndraDB serialization of structured lists
- ADR-15: Neo4j covering indexes and IndraDB property-index parity
- `src/schema/nodes.rs` — `NodeRecord` struct (source of truth for field names and types)
- `src/schema/edges.rs` — `EdgeRecord` struct
- `src/visit/access_classifier.rs` — `AccessKind` enum
- `prompt/graph_database/cpp/schema.txt` — MCP system-prompt schema (must stay in sync)
