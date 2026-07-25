# CIDX physical schema guide v1

Status: accepted as the storage architecture contract for schema v40.

This guide is the repository-local companion to the HSE-74 architecture
decision. It describes the physical rules that storage changes must preserve;
it does not turn every acceptance item into a completed scale claim. Measured
claims are linked to result artifacts, and unqualified workloads remain
explicitly deferred.

## Authority and compatibility

- `index.db` is the authoritative local semantic store.
- One core database is the default workspace layout. Core facts are not
  sharded by repository, table family, or analysis merely to make files
  smaller.
- The current repository schema is v40 with the reader window declared by
  [`spec/platform/version.json`](../../spec/platform/version.json). Schema v34
  is the HSE-74 qualification baseline; the authoritative migration floor is
  the value in that version contract, and migrations must remain deterministic.
- Database-local integer IDs are storage keys only. Stable external identity
  is carried by the semantic universe plus USR, type key, or artifact identity.
- A missing, unresolved, stale, or partial fact is not represented as a
  successful empty result.

The runtime and maintenance policy is defined by
[`sqlite-profile-v1.json`](sqlite-profile-v1.json). The benchmark contract is
defined by
[`../benchmarks/storage-m0-hse74-citations.json`](../benchmarks/storage-m0-hse74-citations.json)
and the versioned result schemas below it.

## Physical classification rules

Use the narrowest physical class that preserves the fact's identity, order,
attributes, and lifecycle.

| Class | Use when | CIDX examples | Required properties |
| --- | --- | --- | --- |
| Node | The fact has stable identity and independent references or lifecycle | `symbol`, `type_node`, `file`, `definition`, `entity_node` | Integer local key plus stable external identity where applicable |
| Relation | Both endpoints are independently addressable | `edge`, `type_edge`, `symbol_type`, `entity_edge`, `include_edge`, `possible_call` | Typed endpoint domains, forward/reverse access, explicit applicability |
| Ordered slot | The fact is owner-scoped, ordered, and attribute-rich | `parameter`, `template_param`, `template_arg`, `call_arg` | Owner, position, pack index where applicable, typed references, defaults/provenance |
| Occurrence/evidence | The fact records a source observation rather than a collapsed relation | `decl_site`, `edge_site`, `include_site`, diagnostics, future read/write facts | Source location, occurrence identity, completeness, and unknown handling |
| Catalog | The value set is closed and generated from the semantic contract | generated kind, relation, status, and enum catalogs | Stable catalog version and rejection of unknown IDs |
| Artifact manifest | The product is immutable, rebuildable, optional, or independently retained | sidecars, derived results, proof caches, accelerators | Content hash, producer/input identity, compatibility, completeness, lifecycle |

Typed slots and evidence remain authoritative in their dedicated tables. The
logical QueryPlan/CXQ layer may expose typed views and virtual relations such
as `has_parameter`, `has_template_argument`, `has_argument`, and
`has_evidence`, but it must not copy each row into the generic `edge` table.
Expansion of high-cardinality evidence is explicit, bounded, and reports
truncation or partial coverage.

## Identity and applicability

Every resolvable high-cardinality reference should use a nullable local
integer ID to avoid repeating long USR/type strings. An unresolved external
identity is retained once in a typed dictionary with explicit resolution
state; `NULL` means absent evidence, not unresolved evidence.

Translation-unit configuration identity comes from the canonical descriptor
and semantic digest. A source/header may have multiple configuration
associations. Facts that are not proven invariant across the requested
configuration set remain configuration-scoped or unknown.

Extracted and derived facts carry generation/input/producer/schema/catalog
identity, applicability, completeness, and current/stale state. A new
generation is built and validated before publication; a failed refresh never
publishes partial rows as current.

## Core and sidecar boundaries

The core manifest is the only authority for an attached artifact. A sidecar
must be immutable or atomically replaceable, content-addressed or
generation-stamped, and rebuildable from declared inputs. Readers attach it
read-only only after validating its schema/catalog/producer versions, input
identity, content hash, completeness, and mapping contract.

Sidecars may contain per-TU AST graphs, extension facts, large derived
analysis results, proof/replay artifacts, or optional read-optimized
projections. They must not introduce cross-file foreign-key assumptions or
promise an atomic transaction spanning the core and sidecar. Missing,
corrupt, stale, or incompatible sidecars produce explicit partial/unknown
diagnostics or a deterministic SQLite fallback.

Deleting a sidecar must never delete authoritative core facts. Cleanup must
respect current-manifest references, leases, and pinned replays.

## Qualification gates

The repository currently qualifies the v39 implementation through the
versioned smoke and physical-layout artifacts listed in
[`architecture-v1.json`](architecture-v1.json). The following are deliberately
not claimed until their own result artifacts exist:

- million-node and 10M/50M/100M/500M-relation measurements;
- cpp-indexer self-index qualification;
- banking-corpus qualification;
- an optional accelerator or custom primary store.

A custom primary store can be considered only after a named workload misses a
written SLO after schema, indexes, transactions, statistics, and runtime
tuning have been exhausted, and an immutable derived accelerator cannot meet
the target. The comparison must include migration, recovery, integrity,
backup, inspection, packaging, and compatibility costs.

## Change requirements

Storage changes must update the machine-readable manifest and the relevant
result artifact or test when they change a physical rule. Schema changes must
include deterministic v34-baseline migration evidence, old-database coverage,
and canonical result equivalence. New indexes and projections require a named
workload and before/after measurement.
