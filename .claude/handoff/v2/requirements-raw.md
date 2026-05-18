---
name: cpp-indexer Structured Node Attributes Brief
type: planning
status: draft
last_updated: 2026-05-18
sources: 4
tags: [cpp-indexer, schema, codexgraph, graphdb, planning]
---

# cpp-indexer — Structured Node Attributes Brief

## Executive Summary

cpp-indexer already captures the right graph shape for a C++ CODEXGRAPH-style retrieval system, but too much high-value semantic data is either missing or hidden inside opaque `attrs_json` strings. The next schema milestone should promote the most-used function, method, template, and `USES` attributes into native graph properties that Neo4j and IndraDB can index and return directly.

This is not a broad schema expansion. It is a focused queryability pass: make the existing nodes and edges answer agent questions without JSON parsing, full graph scans, or fallback source inspection.

## Problem

The current graph exposes stable identity and location fields (`usr`, `kind`, `name`, `qualified_name`, `file_path`, `line`, `repo_name`) as native properties. However, CODEXGRAPH-style retrieval needs more than symbol identity. Agents need to ask about signatures, return types, template arguments, virtual/static modifiers, source snippets, and whether a dependency is a read, write, address-take, or call argument.

Today those questions are weakly supported:

- Return types, ordered parameters, canonical signatures, and source snippets are not first-class graph properties.
- Template parameters and specialization arguments are incomplete or string-shaped.
- `USES` edges do not distinguish reads from writes or other access modes.
- Existing long-tail metadata in `attrs_json` is not indexable in the graph databases.

The practical result is that a natural-language-to-Cypher agent will generate reasonable queries that either cannot run efficiently or cannot be answered from the graph alone.

## High-Level Requirements

1. **Promote callable signature data.** `FUNCTION` and `METHOD` nodes should expose native `return_type`, ordered `params`, and canonical `signature` properties.
2. **Expose source context safely.** `FUNCTION` and `METHOD` nodes should optionally carry bounded `code` snippets, capped at 32 KiB per node with an explicit truncation marker.
3. **Structure template metadata.** `TEMPLATE_DECL` nodes should expose structured `template_params`; `SPECIALIZATION` nodes should expose structured `template_args`.
4. **Classify `USES` semantics.** `USES` edges should expose `source_association_type` and `target_association_type`, at minimum covering `read`, `write`, `addr_of`, `call_arg`, `return`, `decl_ref`, and `unknown`.
5. **Keep promoted fields native in both sinks.** Neo4j and IndraDB writes must store the promoted fields as graph properties, not only inside `attrs_json`.
6. **Add indexes for hot query paths.** Neo4j should index common filters such as `return_type`, virtual/static flags, and useful composites such as `(kind, return_type)`. IndraDB should preserve the same properties for its query surface.
7. **Bump schema version and require re-index.** This is a schema-breaking enhancement. `SCHEMA_VERSION` should move forward, old graphs should be rejected by schema handshake, and users should re-index rather than migrate in place.

## Out Of Scope

- New node kinds or edge kinds.
- NL-to-Cypher translator implementation.
- Agent loop implementation.
- Automatic migration of old graphs.
- Demangling ABI names.
- Capturing call-site argument expressions for `CALLS` edges.
- Moving to Neo4j multi-label nodes such as `:Node:METHOD`.

## Success Criteria

The milestone is successful when these questions are answerable as single graph queries with no JSON parsing and no whole-graph scan:

1. What is the return type and signature of function `X`?
2. What are the ordered parameter names and types of method `C::f`?
3. Which virtual non-pure methods return `Status`?
4. Which specializations of template `T` use a given template argument?
5. Which methods write to field `mutex_`, rather than merely referencing it?

For Neo4j, the canonical explain plans should use index-backed operators for the indexed predicates. For both sinks, final job counters and metrics should continue reporting stored node/edge counts, not attempted rows.

## Design Constraints

- Preserve `attrs_json` for rare and experimental attributes; only promote fields with clear agent-query value.
- Keep the schema prompt and examples aligned with the actual sink properties.
- Bound graph growth from source snippets; prefer missing/truncated code over unbounded storage.
- Degrade gracefully when libclang cannot classify an access mode; emit `unknown` rather than dropping the edge.
- Treat old graph data as disposable for this milestone; re-index is simpler and safer than migration.

## Sequencing

Track this as **M8 — Structured Attributes**, with seven implementation stories:

1. `S40` — schema version bump, `NodeRecord` / `EdgeRecord` fields, Arrow round trips.
2. `S41` — callable extraction: return type, params, signature, bounded code.
3. `S42` — template parameter and specialization argument extraction.
4. `S43` — `USES` access classifier.
5. `S44` — Neo4j native property writes and indexes.
6. `S45` — IndraDB native property writes.
7. `S46` — schema docs, prompt/example refresh, wiki/code page cross-links.

Detailed acceptance criteria and worked Cypher examples live in [[pages/planning/cpp-indexer-structured-attrs-prd]].

## Risks

- **Storage growth:** `code` snippets can dominate graph size. The 32 KiB cap is mandatory, and large-repo measurements should decide whether code belongs on nodes long-term.
- **Classifier ambiguity:** C++ access classification has hard cases (`*p = x`, references, overloaded operators). The first pass should classify obvious cases and mark the rest `unknown`.
- **Sink parity:** Neo4j supports explicit indexes; IndraDB has a different property-query model. The requirement is property parity, not identical query planning.
- **Prompt/schema drift:** If `schema.txt`, examples, and sink writes disagree, agents will generate invalid queries. Drift checks should be part of the implementation.

## Sources

- [[pages/planning/cpp-indexer-structured-attrs-prd]]
- [[pages/code/cpp-indexer]]
- [[pages/research/codexgraph]]
- `/Users/husam/workspace/cpp-indexer/prompt/graph_database/cpp/schema.txt`
