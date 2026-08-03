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

| Domain | Natural identity |
|---|---|
| file | `(component.path, directory.path, file.name)` |
| configuration | semantic universe, translation unit, normalized configuration digest, identity source, and reconstructable configuration content |
| partition | portable file plus configuration identity |
| symbol | partition/linkage scope, USR, and optional local anchor |
| type | partition plus `type_key` |
| relation | source symbol identity, destination symbol identity, kind, base access, and virtual flag |
| definition | owning partition, symbol identity, source extent, and optional initializer text |
| other typed families | owning partition plus the family's complete typed payload; canonicalization removes only exact duplicates |

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
The serialized artifact preserves that within-group sequence and validates its
renumbered first-seen/conflict ordinals for internal consistency. Because the
legacy sequence originates in extraction order, a decoder cannot independently
derive it from the remaining record fields; the content digest protects the
sequence in transit but is not an authenticity claim about its producer.

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

Here `k` is one selected keyed bucket, `r` is returned or copied output, and
`n` is the whole batch. Hash-table bounds are amortised; ordered maps are
`O(log n)`.

| Operation | Bound |
|---|---|
| `set_partition` | `O(log n)` |
| `set_completeness` | `O(1)` |
| `emit(SymbolRecord)`, `mint_symbol` | `O(log n)` |
| `emit(EvidenceRecord/DeclarationSiteRecord)` | amortised `O(1)` |
| `emit(IncludeDirectiveRecord/MacroUseRecord)` | amortised `O(1)` |
| `emit(DiagnosticFactRecord/LifecycleCleanupIntent)` | amortised `O(1)` |
| `emit(ApplicabilityOwnershipRecord/PresentationIntent)` | amortised `O(1)` |
| `lookup_symbol_id` | amortised `O(1)` |
| `file_id_for_path` | amortised `O(1)` |
| `type_arg_candidates`, `symbol_ids_by_qual_name_kind` | `O(r)` |
| `add_edge`, `ensure_edge` | `O(log n)` |
| `add_edge_site`, `add_call_arg` | `O(log n)` |
| `add_template_param`, `add_template_arg` | amortised `O(1)` |
| `intern_type_node` | `O(log n)` |
| `add_type_edge`, `add_symbol_type`, `add_def_edge` | amortised `O(1)` |
| `replace_parameters` | `O(log n + input)` |
| `get_or_create_definition` | `O(log n)` |
| `body_edge_count` | amortised `O(1)` |
| `copy_body_edges_to_def_edge` | `O(k)` |
| `set_current_file_id` | `O(log n)` |
| `set_identity_translation_unit_config_id` | `O(log n)` |
| `set_identity_translation_unit_file_id` | `O(log n)` |
| `delete_edges_for_file`, `delete_definitions_for_file` | `O(log n)` |
| `lookup_display_name` | `O(log n)` |
| `update_display_name` | `O(log n + k)` |
| `snapshot`, `batch`, `canonical_batch` | `O(n log n)` |
| artifact canonical-layout validation | `O(n)` plus stable-key materialization |
| `counters` | `O(1)` |

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

`application::replay_fact_batch` applies partitions in the exact legacy file
order `(component.path, directory.path, file.name)`
inside one `FactBatchReplayPort` translation-unit transaction. It resolves
natural handles through transient file/symbol/relation/definition maps. Symbol
conflicts are applied in recorded legacy emission order even though the
published record vectors are canonical. Any failure before apply, mid-family,
before commit, or from commit rolls the whole TU back.

The correctness replay oracle applies symbols, relations, definitions, and
diagnostics for main-only and preplanned multi-file routes. Include and
applicability publication, generation lifecycle, and derived resolve inputs
remain deliberate `apply_other` boundaries: S-099 owns their controlled
planning/lifecycle, and S-073 owns their set-based storage publication.

## Ownership boundaries

- S-099 owns owned-header planning, file-row creation, lifecycle execution,
  and applicability publication.
- S-100 owns serialization, digest/version identity, transfer, and spill.
- S-073 owns prepared, set-based SQL persistence.
- S-074 owns parallel scheduling and deterministic reorder buffers.
- S-101 owns atomic clean-rebuild replacement.

This contract adds no schema or Python-indexer change.

The durable S-100 representation is frozen separately in the
[FactBatch artifact v1 wire contract](fact-batch-artifact-v1.md).
