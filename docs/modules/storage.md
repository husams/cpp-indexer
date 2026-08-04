# `src/storage` — the database layer

[← docs index](../README.md) · related: [data model](../data-model.md) · [data flow](../data-flow.md)

The single owner of `index.db`: the schema, all migrations, the write API used
by both indexing engines, and the resolve pass. ~5.5k LOC.

## Files

| File | Role |
|---|---|
| `storage.hpp` / `storage.cpp` | the `Storage` class, schema DDL, migrations, `resolve_pass()` |
| `sqlite.hpp` / `sqlite.cpp` | a thin RAII wrapper over libsqlite3 (`Db`, `Stmt`) |
| `fact_batch_writer.hpp` / `fact_batch_writer.cpp` | the sole production one-TU FactBatch publication path |
| `artifacts.hpp` / `artifacts.cpp` | manifest-governed sidecar publication, validation, read-only attachment, leases, pins, and recovery |
| `records.hpp` | plain-data row structs — no clang types leak here |

## Classes

### `SqliteStorageService` / `Storage` (`storage.hpp`)

`SqliteStorageService` owns the connection and creates/migrates the DB to
`schema_version = 40`. It is the internal SQLite persistence implementation.
`Storage` is now a compatibility façade derived from that service; its public
surface remains for legacy callers while new code composes focused ports and
read adapters.

- **Symbols**: `add_symbol` (upsert keyed on `usr`; also records `decl_site`),
  `mint_symbol_id` (USR-keyed *stub* upsert for a not-yet-indexed target),
  `lookup_symbol`, `lookup_symbol_by_id`, `lookup_symbols_by_name` /
  `…_by_qual_name`, `update_symbol`.
- **Edges**: `add_edge` (upsert on `(src,dst,kind)`, increments `count`),
  `add_edge_site`, `add_call_arg`, `add_template_param`, `add_template_arg`.
- **Definitions (v27)**: `get_or_create_definition`, `add_def_edge`,
  `copy_body_edges_to_def_edge` (snapshots a body's calls/uses so a later
  cross-TU edge rewrite doesn't lose them).
- **Files/components**: `add_file_path`, `get_file`, `is_file_indexed`,
  `mark_file_indexed`, `component_for_path`, `replace_diagnostics`.
- **Re-index cleanup**: `delete_edges_for_file` (excludes `contains`, keyed by
  the source symbol's file), `delete_definitions_for_file`.
- **Resolve**: `resolve_pass()` and its sub-passes (below). Each transform
  persists execution mode, logical rows scanned/inserted/updated/deleted,
  affected keys, and fallback reason. The pipeline summary is derived from
  transforms that actually ran and reports `full`, `incremental`, `mixed`, or
  `reused`; it is never inferred only from the presence of a change set.

### `Transaction` (`storage.hpp:45`)

RAII commit/rollback. `FactBatchWriter` opens the production transaction only
after extraction, planning, and source verification have completed. An
explicit `commit()` surfaces COMMIT failures; any staging, resolution, apply,
publication, cleanup, revalidation, or commit error rolls the whole TU back.

### `FactBatchWriter`

The writer consumes one immutable canonical `FactBatch` plus the frozen owned
file route plan. Its authoritative phase order is plan validation, file-row
resolution, connection-local TEMP staging, natural-key maps, entity and
annotation apply, relation/site/external-identity apply, include and
applicability publication, stale cleanup, source revalidation, currentness,
and commit. Extraction does not mutate persistent storage.

Staging is TEMP-only and does not change the schema. One reusable prepared
statement loads each family; target writes use set-based joins against stable
file/symbol/type/relation/definition maps. The writer report separates prepared,
reused, and eliminated statements; executions and VM steps; prepare, VM, and
commit time; and staged/inserted/updated/ignored/deleted rows per family.
Deterministic failure points cover every apply boundary plus pre-commit and
commit, and TEMP tables are cleared for connection reuse.

The production comparison harness accepts `--baseline-cidx` for paired
pre-writer/candidate trials. S-073 does not change PRAGMAs or recovery policy,
and it retains both binary/NOCASE spelling and qualified-name index pairs.

TEMP staging and observed per-family outcome accounting add a fixed cost on
very small cold translation units. Qualification therefore reports cold and
hot one-file/four-file shapes separately from the 16-file scale shape: the
small cold shapes may regress modestly while hot per-TU publication and scale
must remain within the story's measured gates. This is an explicit throughput
tradeoff for atomic set-based publication and truthful insert/update/ignore
telemetry, not an unmeasured change.

`ast::ExtractedFactPublication` is the stable application-neutral seam used by
the live writer and by future TU-cache serialization/replay. It carries the
canonical batch and frozen route/configuration context without exposing this
module's report types. A cached artifact must also validate the persistent
symbol-identity state described by the
[FactBatch compatibility contract](../fact-batch.md#identity-and-partitioning);
on mismatch the caller re-extracts, and every successful publication still
passes through `FactBatchWriter`.

### `sqlite.hpp`

A minimal wrapper: `Db` (open/exec/prepare), prepared `Stmt` with
`bind`/`reset`/`step`/`col_*`, reusable prepared statements, and
`RETURNING`-based upserts. Requires **SQLite ≥ 3.37** (for `RETURNING` and
`sqlite3_changes64`) — on RHEL 9 this is why the build uses a static SQLite
amalgamation (see [build](../build.md)).

### `records.hpp`

Row structs crossing module boundaries: `Symbol`, `Edge`, `EdgeSite`,
`CallArg`, `TemplateParam`, `TemplateArg`, `File`, `Component`, `Directory`,
`Diagnostic`, `Definition`. Both engines populate these; no libclang/LLVM types
appear here (the [`ast`](ast.md) sinks translate their own records
into these).

### Focused ports and SQLite adapters

New platform code should consume the SQLite-free contracts in
`storage/ports.hpp`: workspace catalog, source, symbol identity, type,
semantic fact, definition, include, schema-read, and unit-of-work ports each
separate read capabilities from writes. `sqlite_adapters.hpp/.cpp` binds those
contracts to the existing `Storage` implementation while the compatibility
facade remains available during the incremental migration. `Storage` exposes
one owned adapter set through typed port accessors, so callers can select the
smallest capability without taking a dependency on the monolithic facade.

The current production migrations are the AST symbol/edge sinks, the
translation-unit unit-of-work boundary, workspace/configuration resolution,
and QueryPlan's `QueryReadPort`/`SqliteQueryReadAdapter` boundary. QueryPlan's
remaining parameterized SQL is kept inside that read adapter; `raw_db()` is not
an indexing or workspace dependency. A repository-wide guard scans C++ sources
and headers against an explicit seven-file persistence/read-adapter allowlist.

Compatibility-facade removal plan:

1. HSE-62: migrate AST extraction, workspace/configuration, and QueryPlan
   consumers to focused ports; retain facade methods only as delegating shims.
2. HSE-63/HSE-67: migrate remaining CLI and graph read surfaces and add fake
   port contract tests.
3. HSE-68: remove the unused facade methods and make adapter construction an
   application-composition concern after downstream callers are port-only.

The plan is intentionally tracked with the HSE work rather than deleting the
facade in this slice; no new platform consumer may add a dependency on it.

Acceptance evidence for this slice is produced by `storage_ports_test` (full
identity/declaration payloads, read/write separation, commit/rollback, and
injected one-TU failure), the v33→v34 and v34 compatibility migration tests,
`cidx db verify`,
and the storage M1 qualification runner. The committed benchmark baselines
remain the regression reference for indexing, resolving, and QueryPlan
latency, statement/transaction counts, and index-size tradeoffs.

## The schema

Full table reference and the ER diagram are on the [data model](../data-model.md)
page. In short: an ownership tree (`repository → component → directory → file`)
feeds Layer-0 (`symbol`, `edge`, `edge_site`, `call_arg`, `template_*`,
`definition`, `def_edge`), which `resolve_pass()` transforms into Layer-1
(`entity_node`, `entity_edge`, `dispatch_calls`, `possible_call`).

## The resolve pass

`cidx resolve` → `resolve_pass()` runs pure-SQL transforms over Layer-0 and
stamps `meta.graph_resolved_at`. The edge-count, multi-definition,
possible-call, and virtual-dispatch passes update only the trusted per-TU
change closure when a published full baseline exists; entity projection keeps
its atomic full-rebuild contract. See the
[data flow](../data-flow.md#the-resolve-pass) diagram; the sub-passes and their
products:

| Pass | Anchor | Produces |
|---|---|---|
| `rollup_edge_counts()` | `2816` | `edge.count = COUNT(edge_site)` for calls/uses |
| `set_multi_def()` | `2957` | `symbol.multi_def = COUNT(definition)` (redefinition indicator) |
| `materialize_possible_calls()` | `2964` | `possible_call`: body → body fan-out for `multi_def > 1` callees |
| `materialize_dispatch_calls()` | `2827` | `edge` kind 18: virtual-dispatch caller edges via a recursive CTE over `overrides` |
| `materialise_entity_edges()` | `4041` | `entity_node` (type classification into design kinds) + `entity_edge` (11 relations: generalizes/implements/specializes/composes/aggregates/associates/creates/uses/destroys/befriends/instantiates/declares) |

`resolve_pass` finally counts remaining **stub** symbols
(`resolved = 0 AND file_id IS NULL AND decl_file_id IS NULL`) and returns that
 count for the `resolve: N still-stub …` line.

## Manifest-governed sidecars (schema v39)

`index.db` remains the authoritative identity store. Immutable or rebuildable
artifacts such as AST graphs, extension facts, proof caches, and accelerators
are published through `ArtifactStore` and recorded in `artifact`; the
`artifact_relation`, `artifact_identity_map`, `artifact_lease`, and
`artifact_pin` tables carry only core metadata and stable identities. No
database-local sidecar integer is used as a portable core identity.

Publication writes and validates a temporary SQLite file, synchronizes it,
renames it into a content-addressed `artifacts/<kind-hash>/<content-hash>.db`
location, then commits the manifest reference. A failed manifest commit leaves
an unreferenced file for `recover()` to remove; this is deliberately not an
atomic cross-file transaction. Current selection is by `logical_id`, separate
from physical location, so rebuilding or relocating a sidecar does not change
the logical result identity.

`attach_current()` requires a current manifest, matching envelope, matching
content hash/size, passing `integrity_check`, complete/non-truncated/
producer- or reader-verified status, the generated numeric catalog contract,
and a deterministic unique attachment identifier. It uses a read-only URI
and `query_only` while attached. Missing, stale, corrupt, incompatible,
partial, truncated, or untrusted artifacts are surfaced as diagnostics rather
than empty complete results. Leases and replay pins protect stale artifacts
from cleanup. Backup/export callers use `export_plan()` to list every current
artifact and report intentionally partial packages.
The single-file versus manifest-plus-sidecar disk, latency, open/attach, and
complexity comparison is owned by the HSE-75 benchmark work; this policy keeps
those layouts interchangeable without changing authoritative graph semantics.
