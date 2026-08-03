# `src/storage` — the database layer

[← docs index](../README.md) · related: [data model](../data-model.md) · [data flow](../data-flow.md)

The single owner of `index.db`: the schema, all migrations, the write API used
by both indexing engines, and the resolve pass. ~5.5k LOC.

## Files

| File | Role |
|---|---|
| `storage.hpp` / `storage.cpp` | the `Storage` class, schema DDL, migrations, `resolve_pass()` |
| `sqlite.hpp` / `sqlite.cpp` | a thin RAII wrapper over libsqlite3 (`Db`, `Stmt`) |
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

RAII commit/rollback. The LibTooling translation-unit pipeline obtains a
`UnitOfWork` from the focused port factory and wraps symbol, edge, definition,
include, and header publication in one transaction. An explicit `commit()`
surfaces COMMIT failures instead of swallowing them; destruction after an
injected adapter/transform failure rolls the whole TU back.

### `sqlite.hpp`

A minimal wrapper: `Db` (open/exec/prepare), prepared `Stmt` with
`bind`/`step`/`col_*`, `RETURNING`-based upserts. Requires **SQLite ≥ 3.35**
(for `RETURNING`) — on RHEL 9 this is why the build uses a static SQLite
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
