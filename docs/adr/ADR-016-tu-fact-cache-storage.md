# ADR-016: TU Fact Cache Storage and Authority

Status: Accepted for S-076

## Decision

Use a split design. The existing schema-v40 `ArtifactStore` manifest in
`index.db` records only the optional cache artifact identity, lifecycle, and
current pointer. Each cached translation-unit `FactBatch` and its complete
path-based dependency evidence live together in a content-addressed SQLite
sidecar governed by that manifest. The sidecar uses the
`cidx-tu-fact-cache/v1` contract and the existing crash-safe publication path;
there is no second artifact store and no new core table.

The serialized batch is the canonical S-100 `FactBatch` wire artifact encoded
as hexadecimal text inside the SQLite envelope. This is intentional: the
current `ArtifactStore` validates SQLite envelopes with `PRAGMA
integrity_check` and must never be given arbitrary raw bytes. The adapter also
checks the embedded payload SHA-256 before decoding the batch.

## Authority and currentness

`index.db` remains the only authoritative index. A cache entry is an optional
optimization and cannot make a file, translation unit, configuration, source
revision, or derived transform generation current. Only a failure-atomic
replay or fresh extraction followed by the existing core currentness update
may do that.

A hit requires all of the following: a current manifest, a valid SQLite
envelope, complete/non-truncated/trusted metadata, an exact TU cache identity,
complete dependency evidence, a matching payload digest, a compatible S-100
wire contract, and a successful transactional replay. Any failure is a miss
and forces conservative re-extraction. Missing, stale, deleted, corrupt,
partial, truncated, incompatible, or untrusted optional state therefore cannot
make `index.db` appear current.

## Identity and compatibility

The public `tu-fact-cache/v1` identity is content addressed and excludes
timestamps, sizes, and database row IDs. It includes the canonical main-source
path and content SHA-256; the normalized translation-unit descriptor and
diagnostics policy; Clang, resource, driver, target, and ABI identity;
extractor, pass, catalog, schema, artifact, package, and product versions;
sorted relevant environment-name/value-digest inputs and generated inputs; sorted
transitive dependency paths, kinds, conditional contexts, and content
SHA-256s; and the S-075 front-end reuse identity. Enabled, disabled, and the
explicit `front-end-reuse/v1:none` sentinel are distinct values.

The stable cache slot is workspace + canonical TU source + normalized
configuration. Publishing a new content identity retires the prior artifact in
that slot. Readers compare the expected input fact-set identity before
attachment and classify a mismatch as `stale`.

## On-disk FactBatch representation

Each sidecar contains:

- `tu_fact_cache`: the cache identity, hexadecimal S-100 wire artifact, and
  payload SHA-256;
- `tu_replay_context`: the application-owned, versioned controlled-writer
  routing context and its SHA-256;
- `tu_dependency`: direct path-to-path dependency edges with conditional
  context and resolution state; and
- the standard `ArtifactStore` envelope tying those relations to workspace,
  TU, configuration, catalog, producer, engine, completeness, truncation,
  trust, and retention metadata.

Opened owned, generated, unowned, `.inc`, `.def`, and system/toolchain inputs
are represented by stable path identities even when no core `file` row exists.
The reverse planner traverses these edges per configuration, so traversal does
not stop at an unowned intermediate. System/toolchain compatibility is also
covered by the normalized toolchain and front-end reuse identities.

## Lifecycle, retention, and publication

Publication builds a complete in-memory SQLite database, writes its envelope,
backs it up to a staging file, fsyncs it, validates the acquired directory and
file descriptors, atomically publishes the content-addressed object, and then
switches the core manifest current pointer. Readers see the old complete entry
or the new complete entry, never a partial file or dangling marker.

Retention is one current entry per stable TU/configuration slot plus explicitly
leased or pinned retired entries. Recovery removes only retired, unreferenced
artifacts and orphaned files. A replay lease protects the selected generation;
operator or qualification pins protect evidence that must survive cleanup.
Neither recovery nor normal publication may delete a current, leased, or
pinned artifact.

## Failure, backup, and recovery

Stable decision codes are `hit`, `missing`, `stale`, `corrupt`,
`incompatible`, `partial`, `truncated`, `untrusted`, and `unavailable`.
Incomplete, unresolved, stale, corrupt, or unavailable dependency evidence
returns the full conservative TU/configuration set rather than an empty
"unaffected" result. Replay failures roll back the whole TU and do not update
currentness.

Deleting the sidecar, deleting one object, corrupting the SQLite envelope,
corrupting the embedded payload, or deleting the manifest can only remove the
optimization. The core database remains independently readable. Recovery may
remove stale/orphaned optional state and the next update re-extracts it.

`ArtifactStore::export_plan(true)` is the backup/export inventory for current
cache sidecars. A backup consists of `index.db` plus the returned relative
artifact files when warm-cache preservation is desired. Restoring `index.db`
without optional sidecars is supported and cold; restoring sidecars without
the matching manifest is ignored. Import is normal validated publication, not
filesystem pointer editing.

## Schema and migration impact

No schema version change is required. Schema v40 already provides the
manifest, current/retired state, content hash and byte size, leases, pins,
identity fields, trust/completeness/truncation fields, and crash-safe
content-addressed publication needed by this decision. The new relations are
inside optional SQLite sidecars and are guarded by
`cidx-tu-fact-cache/v1` compatibility checks. Consequently there is no paired
C++/Python schema-pin change, core migration, rollback migration, or
old-database fixture in S-076.

If a future change moves dependency or cache state into core tables, it must be
a separately reviewed schema migration that updates both language pins,
forward migration, rollback/integrity tests, and backup/recovery gates.

## Rejected alternatives

Core-only storage was rejected because cached batches and path-based external
dependency evidence are optional, high-volume, independently replaceable
state. Putting them in core would enlarge backups, couple cleanup to the
authoritative database, and require a paired schema migration without adding
correctness.

A sidecar-only design without the core manifest was rejected because it lacks
an atomic, generation-scoped current pointer and auditable leases/pins. A new
general artifact store was rejected because `ArtifactStore` already provides
the required lifecycle and security checks. Raw S-100 bytes passed directly to
`publish_existing` were rejected because the current validator accepts SQLite
envelopes only.

## Acceptance-criterion mapping

- Storage choice and rejected alternatives: **Decision** and **Rejected
  alternatives**.
- Authoritative currentness and conservative fallback: **Authority and
  currentness** and **Failure, backup, and recovery**.
- Complete semantic identity and PCH/preamble invalidation: **Identity and
  compatibility**.
- Generated/unowned transitive traversal: **On-disk FactBatch representation**.
- Content addressing, atomic publication, retention, leases, and pins:
  **Lifecycle, retention, and publication**.
- Deletion/corruption behavior and independent core readability: **Failure,
  backup, and recovery**.
- Schema/migration obligations: **Schema and migration impact**.
