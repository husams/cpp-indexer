# Immutable FactBatch contract

[← docs index](README.md) · related: [AST engine](modules/ast.md) ·
[data flow](data-flow.md)

`ast::FactBatch` is the immutable, database-independent output boundary for one
serial translation-unit extraction. `FactBatchRecorder` is the mutable builder;
`snapshot()` and `canonical_batch()` publish read-only shared state, so later
builder operations cannot alter an already published batch.

## Identity and partitioning

Every fact belongs to one `FactPartitionKey`:

```text
portable file = (component.path, directory.path, file.name)
configuration = (semantic universe, translation unit,
                 normalized configuration digest, identity source,
                 reconstructable IncludeConfig content)
```

Configuration content carries the optional driver, working directory,
ordered arguments, language mode, and resource directory. The normalized
configuration is a portable digest of that content. Database configuration row
IDs are deliberately ignored by the batch recorder and never enter a partition
key.

`SymbolNaturalKey` follows storage linkage semantics: externally linked
symbols coalesce by semantic universe and USR, while internal and no-linkage
symbols additionally include translation-unit, identity-source, and optional
local-anchor identity. Type and relation natural keys refer to the same
partitioned identity vocabulary.
Emitter ports continue to exchange `int64_t` values for source compatibility,
but these are collision-checked transient handles backed by natural-key
dictionaries. They are never SQLite `file.id`, `symbol.id`, `edge.id`, or
`definition.id`. The writer resolves them to database-local IDs through a
per-transaction apply map.

The published `file_keys()` dictionary maps every transient file handle used
by file-bearing facts to its portable partition key. Replay resolves these
handles before applying symbols or later families; no transient file handle is
passed to storage.

The initial replay key is exactly `(component.path, directory.path,
file.name)`. `LegacyApplyOrderKey` additionally retains first-seen and conflict
ordinals where the current first-writer or last-writer behavior is observable.
Paths inferred outside the active component retain an empty component rather
than being falsely rebased beneath it. Symbol groups are ordered by their first
portable file key, while conflicting records within one natural-symbol group
retain emission order.
Canonicalization sorts and deduplicates exact records inside each partition;
it retains conflicting payloads, repeated declarations, duplicate-USR groups,
semantic-universe distinctions, and their apply metadata.

## Typed fact coverage

The immutable record set explicitly covers:

- symbols and declaration sites;
- relations, edge sites, and call arguments;
- template parameters and arguments;
- types, type edges, parameters, and symbol types;
- definitions and definition edges;
- include directives and macro uses;
- diagnostics and extraction evidence;
- presentation intents;
- declarative lifecycle-cleanup intents; and
- applicability ownership.

Macro uses retain both a typed portable definition-file identity when the
definition is component-owned and the original raw definition path for
diagnostics and foreign definitions. Include directive enum values intentionally
match the persisted `1..5` representation. Lifecycle and applicability
generation keys are opaque content-derived tokens owned by S-099; they are not
storage-local generation counters.

Lifecycle and applicability entries are declarations only. Extraction cannot
execute cleanup or mutate authoritative storage.

## Builder complexity contract

This table is authoritative and mirrors the comment beside
`FactBatchRecorder` in `src/ast/fact_batch.hpp`.

| Operation | Index or bucket | Bound |
|---|---|---|
| symbol emit / exact source lookup | natural key and `(source, USR)` | amortised O(1) |
| source-less symbol lookup | `(universe, TU, USR)` → transient handles | amortised O(1); succeeds only for one unambiguous result |
| qualified/unqualified type candidate lookup | name → first-seen candidates | amortised O(1) + output |
| qualified-name and kind lookup | `(qualified name, kind)` → handles | amortised O(1) + output |
| display-name update | symbol handle → record positions | O(records for that symbol) |
| edge add/aggregation | relation natural key → record position | amortised O(1) |
| parameter replacement | owner handle → parameter bucket | O(new owner parameters) |
| body-edge count/copy | source handle → body-edge positions | O(requested source bucket) |
| type/definition interning | natural key → transient handle | amortised O(1) |
| final canonicalization | partition/family sort, dedup, materialization | O(n log n) + output |

No per-emission operation scans a growing whole-batch vector. The isolated
`fact_batch_complexity_test` asserts touched-record counts. The
`benchmarks/indexing/fact_batch_scaling.py` gate runs at 1,000, 2,000, 4,000,
and 8,000 symbols with at least five trials, reports emission separately from
canonicalization, fits a quadratic component, and rejects it above the recorded
tolerance. Reports retain per-size medians; the fitted gate uses per-size
minima so a one-sided scheduling outlier cannot dominate the four-point fit.
Each trial also compares a canonical-output fingerprint across repeated builds
of the same mixed-partition workload.

## Extraction and replay failure boundaries

`extract_serial_fact_batch` takes an immutable `SerialFactRoute`, binds the
registered pass ports to one recorder, and returns either one finalized batch
or a typed failure. A parse, budget, pass, or pre-publication failure exposes no
partial batch and has no storage port to mutate.

`application::replay_fact_batch` applies partitions in the legacy file order
inside one `FactBatchReplayPort` translation-unit transaction. It resolves
natural handles through transient file/symbol/relation/definition maps. Symbol
conflicts are applied in recorded legacy emission order even though the
published record vectors are canonical. Any failure before apply, mid-family,
before commit, or from commit rolls the whole TU back.

## Ownership boundaries

- S-099 owns owned-header planning, file-row creation, lifecycle execution,
  and applicability publication.
- S-100 owns serialization, digest/version identity, transfer, and spill.
- S-073 owns prepared, set-based SQL persistence.
- S-074 owns parallel scheduling and deterministic reorder buffers.
- S-101 owns atomic clean-rebuild replacement.

This contract adds no schema or Python-indexer change.
