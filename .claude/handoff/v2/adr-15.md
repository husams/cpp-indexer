# ADR-15: Neo4j index set and IndraDB property-index parity (limitation acknowledged)

Status: accepted
Date: 2026-05-18
Resolves: requirements OQ-5 (IndraDB index hints), AC-S44-3/4, AC-S45-*, PRD §AC-S4.3, §3 G4/G5

## Context

S44 requires covering Neo4j indexes for the hot promoted properties so that the canonical agent queries (PRD §6 Q1–Q5) plan as `NodeIndexSeek` (AC-S44-4). S45 must achieve property parity (every key present in both sinks for the same fixture) but IndraDB v5's index control is limited: only vertex/edge identifier indexes and per-property *existence* indexes are exposed via the v5 API; range or composite indexes are not configurable until v6 (PRD §8).

The dispatch raised OQ-5: attempt index-equivalent behavior on IndraDB now, or document as known limitation for v6. The IndraDB schema model already constrains queries to property-equality lookups; range queries are not supported regardless of indexing. The only knob v5 exposes is `IndexedPropertyQuery` — register a property name to make subsequent `get_vertex_properties(IndexedPropertyQuery(name, value))` lookups indexed.

## Decision

### Neo4j index set (S44)

Create with `IF NOT EXISTS` at sink initialization (`src/sink/neo4j.rs` ensure-indexes path, alongside the existing `CQL_ENSURE_NODE_USR_INDEX` / `CQL_ENSURE_NODE_REPO_INDEX`):

```cypher
CREATE INDEX node_return_type_idx       IF NOT EXISTS FOR (n:Node) ON (n.return_type);
CREATE INDEX node_is_virtual_idx        IF NOT EXISTS FOR (n:Node) ON (n.is_virtual);
CREATE INDEX node_is_static_idx         IF NOT EXISTS FOR (n:Node) ON (n.is_static);
CREATE INDEX node_kind_return_type_idx  IF NOT EXISTS FOR (n:Node) ON (n.kind, n.return_type);
```

`is_pure_virtual` is intentionally *not* indexed standalone. It is almost always queried as a refinement of `is_virtual` (PRD §6 Q2: `is_virtual:true, is_pure_virtual:false`); the planner can use `node_is_virtual_idx` and filter in-memory, which is cheap on the much smaller virtual subset.

Edge properties `source_association_type` / `target_association_type` are not indexed in M8. Neo4j relationship-property indexes exist but the canonical Q5 query (PRD §6) starts from a method MATCH and traverses to a known target, so the edge filter is applied within a tight neighborhood. Defer until a query actually scans edges by class.

### Index existence test (AC-S44-3, AC-S44-4)

Add `tests/integration/neo4j_indexes.rs` (gated like the existing Neo4j tests). Two cases:
1. After a fresh write, `SHOW INDEXES` lists all four indexes by name.
2. `EXPLAIN MATCH (m:Node {kind:'METHOD', is_virtual:true}) WHERE m.return_type = 'leveldb::Status' RETURN m.qualified_name` plan contains `NodeIndexSeek` and contains neither `AllNodesScan` nor `NodeByLabelScan` (string match on the plan output).

### IndraDB property-index parity (S45)

Call `client.index_property(Identifier::new(name)?)` for each property key that has a Neo4j index counterpart, at sink initialization, after the existing schema-version vertex setup:

```rust
for name in &["return_type", "is_virtual", "is_static", "kind"] {
    client.index_property(ident(name)?).await.map_err(wrap)?;
}
```

This gives IndraDB the same *existence* tracking Neo4j gets from `CREATE INDEX`. Composite (`kind, return_type`) is **not available in v5**; documented as a known limitation. The `node_kind_return_type_idx` Neo4j composite has no IndraDB peer.

### Documentation of the limitation (S46)

`docs/schema/SCHEMA.md` and the wiki page each carry a "Sink parity" subsection that lists:
- Property keys present in both sinks (full M8 promoted set).
- Neo4j-only capability: composite index `(kind, return_type)`; relationship-property indexes (not used in M8 but possible).
- IndraDB v5 limitation: no composite, no range index. Revisit in v6.

## Alternatives considered

- **Index every promoted property on Neo4j (10 indexes).** Rejected: index storage cost scales with `O(nodes × indexes)`; the hot set (return_type, is_virtual, is_static, kind+return_type) covers PRD §6 Q1–Q3; the rest are queried in conjunction with `usr`/`qualified_name` which are already indexed.
- **Skip IndraDB indexing entirely (defer to v6).** Rejected: leaves AC-S45 parity reading thin; `index_property` is one line per key and matches Neo4j semantics for equality lookups.
- **Add Neo4j edge-property indexes for `source_association_type`.** Rejected: PRD §6 Q5 starts from a method anchor, so the edge filter operates on a small neighborhood; cost of the index outweighs benefit. Add later if a global "find all writes" query emerges.
- **Promote `is_pure_virtual` to a standalone index.** Rejected: redundancy with `is_virtual` filter; pure-virtual rate is <5% of virtual on typical C++ corpora — in-memory filter is faster than a second index seek.

## Consequences

Positive:
- AC-S44-4 satisfied (NodeIndexSeek) for Q1/Q2.
- AC-S45 parity satisfied for equality-lookup capability.
- Index set bounded (4 new Neo4j indexes, 4 IndraDB property registrations); storage cost predictable.

Negative:
- IndraDB has no composite-index analog; the composite Neo4j index is asymmetric. Documented, not hidden.
- `is_pure_virtual` queries that do not also constrain `is_virtual` will not be index-seeked. Acceptable given query patterns.

Follow-ups:
- IndraDB v6 upgrade (when released): revisit composite + range indexes; expected to be a follow-up story, not an M8 blocker.
- Track Neo4j EXPLAIN regressions in CI when query patterns change.

## References

- `[[pages/planning/cpp-indexer-structured-attrs-prd]]` §AC-S4.3, §AC-S5, §8, §3 G4/G5
- requirements.md AC-S44-3, AC-S44-4, AC-S45-3, OQ-5
- `src/sink/neo4j.rs:43-48` (existing index constants pattern)
- `src/sink/indradb.rs` (`client.index_property` usage in lock/schema-version setup)
- Cognee tags: `task:cpp-indexer-m8 role:architect`
