# Translation-unit FactBatch cache

The TU cache is an optional performance layer. `index.db` remains authoritative
and readable without it. Cache loss, deletion, version skew, or corruption
always causes fresh extraction; it never marks the core index current.

## Where it runs

`cidx::application::TuFactCacheIndexer` (`src/application/tu_fact_cache_service.cpp`)
is the single decision point, and both product surfaces route through it: the
`cidx index` command and the application index service. It wraps
`ast::run_index_one`, so a miss is an ordinary extraction and a hit never
reaches the Clang front end at all.

Per translation unit it resolves the configuration through the indexing
session (no parse), reads the slot's recorded dependency evidence, re-reads
the current content of every recorded non-system dependency, and rebuilds the
identity. Only an exact identity match asks for a payload. A hit decodes the
S-100 wire artifact and the versioned replay context, rebuilds the publication
plan, and republishes through the same `FactBatchWriter` the extraction path
uses, under a content-equality source check.

`CIDX_TU_FACT_CACHE=0` disables the layer for a run; `CIDX_TU_FACT_CACHE_ROOT`
overrides the artifact root.

## What invalidates an entry

Editing the main source, any owned header, or any unowned/generated
intermediate (`.inc`, `.def`, a header outside every component root) changes
that file's recorded content digest and therefore the identity. Changing the
normalized configuration, the toolchain identity, the schema/catalog/product
versions, a relevant environment variable, a generated input, or the front-end
reuse identity — including enabling versus explicitly disabling reuse — also
changes it.

Two inputs are deliberately not content-hashed per translation unit: system
and toolchain headers, which the normalized toolchain and front-end reuse
identities cover (their *set* still participates, so adding or removing a
system include invalidates), and unresolved include directives, which cannot
be revalidated at all — a translation unit with one is never published, and an
entry that records one is never a hit. An include that starts resolving to a
different file because a *new* file appeared earlier in the search path with
no tracked file changing is the residual gap this leaves; it is bounded by the
configuration identity covering the search path itself.

## Location and identity

Cache objects use the existing artifact root (by default
`.cidx-artifacts/`) and the `cidx-tu-fact-cache/v1` SQLite sidecar contract.
The stable slot is workspace + canonical source path + normalized
configuration. The content identity covers source and transitive dependency
SHA-256s, the normalized TU/diagnostics configuration, toolchain and ABI,
generated/environment inputs, extractor/pass/catalog/schema/product versions,
and the front-end reuse identity, including the explicit `none` sentinel.

Timestamps and database row IDs are not validity inputs. Conditional edges and
opened owned, generated, unowned, `.inc`, `.def`, and system/toolchain paths are
recorded even when no core `file` row exists.

## Decisions and diagnostics

Machine-readable profiles publish `tu_fact_cache.*` counters for `hit`,
`missing`, `stale`, `corrupt`, `incompatible`, `partial`, `truncated`,
`untrusted`, and `unavailable` decisions, plus parser calls avoided, replay
work, and cache recovery. `tu_dependency.*` counters report affected and proven
unaffected configurations, visited nodes/edges, and conservative fallbacks.

A hit is usable only after the manifest, SQLite envelope, payload hash,
compatibility metadata, dependency evidence, versioned replay-context hash, and
S-100 wire artifact all validate. Replay is transactional. A replay failure
rolls back the TU and falls back to extraction without updating currentness.

## Retention, leases, and cleanup

There is one current object per TU/configuration slot. Replaced objects are
eligible for `ArtifactStore::recover()` unless leased or pinned. A replay lease
protects the selected generation; qualification or operator pins protect an
artifact that must survive cleanup. Recovery never deletes current, leased, or
pinned objects.

## Deletion and corruption recovery

Deleting `.cidx-artifacts/`, deleting one object, or corrupting a sidecar is
safe: the next update reports a miss/corruption and re-extracts. Do not edit
manifest pointers or sidecar envelopes manually. If cleanup is required, stop
active indexing, remove only optional cache objects, and run a normal update.
The core database remains independently queryable throughout.

## Backup, export, and restore

`ArtifactStore::export_plan(true)` lists current optional objects for a warm
backup. A cold backup needs only `index.db`; omitting cache objects is supported.
For a warm restore, restore `index.db` and the matching exported relative
artifact paths together. Sidecars without matching manifests are ignored and
later recovery removes them. Imported cache data must go through validated
publication rather than direct file replacement.

The normative authority, compatibility, publication, and recovery decision is
[ADR-016](adr/ADR-016-tu-fact-cache-storage.md).
