# Clean rebuild and atomic publication

`cidx index rebuild --clean` rebuilds the whole index into a private candidate
database, verifies that candidate in full, and publishes it over the database in
service with a single atomic rename.

It is **opt-in**. Nothing about the default `cidx index`, `cidx index rebuild`,
`--no-graph` or `--defer-transforms` behaviour changes when the flag is absent.

## The guarantee

The database currently in service is never edited. It is opened **read-only** to
capture the rebuild inputs and is then left alone until it is replaced, in one
`rename(2)`, by a candidate that passed every check.

A failed, cancelled, interrupted, or verification-refused rebuild therefore
leaves the pre-existing database **intact, readable, and byte-for-byte
unchanged**, and removes the candidate. Because `rename(2)` is atomic, a
concurrent reader observes either the whole previous database or the whole
published one — never a partial file.

## Sequence

| # | Phase | Effect on the serving database |
|---|-------|-------------------------------|
| 1 | Capture inputs under a read-only handle; record its `sha256` | read only |
| 2 | Create the candidate beside the target | untouched |
| 3 | Replay the input catalog into the candidate | untouched |
| 4 | Index the candidate through the ordinary index lifecycle | untouched |
| 5 | Verify the candidate | untouched |
| 6 | Re-check the recorded `sha256`, `fsync`, then `rename` | replaced |

The candidate is always a sibling of the target (`.cidx-rebuild-<pid>-<name>`),
so publication is a same-filesystem rename rather than a copy.

Step 6 re-reads the serving database's digest **before** anything replaces it. If
another process wrote to the index while the rebuild was running, publication is
refused and that process's database survives.

## What "verified" means

The candidate is published only when all of the following hold:

- `PRAGMA integrity_check` returns `ok`;
- `PRAGMA foreign_key_check` returns no rows;
- `meta.schema_version` equals the schema version of the running binary;
- the candidate's **catalog identity** equals the one captured from the serving
  database — semantic universes, repositories, clones, components (including
  clone-relative paths and versions), registered files with their compile
  options and driver, and labels;
- no registered file is left pending;
- the index pass itself completed (a rebuild that reported failures, unknown
  files, a truncated budget, or cancellation is never published).

The report also carries a **canonical semantic digest**: a `sha256` over an
ordered, database-local-id-free projection of `file`, `symbol`, `edge`,
`definition`, `diagnostic` and `include_edge`. Two independently built databases
holding the same semantic content produce the same digest, so it can be compared
across a rebuild.

## Output

`--json` adds a `clean_rebuild` object beside the ordinary `index` result:

```json
{
  "index": { "indexed": 1, "failed": 0, "...": "..." },
  "clean_rebuild": {
    "published": true,
    "candidate": "/…/cache/.cidx-rebuild-76986-index.db",
    "schema_version": 40,
    "integrity_ok": true,
    "foreign_keys_ok": true,
    "catalog_ok": true,
    "complete": true,
    "files_pending": 0,
    "catalog_digest": "sha256:…",
    "semantic_digest": "sha256:…",
    "detail": ""
  }
}
```

On refusal `published` is `false`, `detail` names the failed check, and the exit
code is non-zero.

## Scope rules

A clean rebuild is a whole-index publication contract, so it refuses partial
scopes rather than publishing an incomplete database:

- `--source COMPONENT` is rejected;
- file arguments are rejected;
- `--clean` outside `index rebuild` is a usage error (exit 2).

## Source preconditions

Two properties of the database in service are checked read-only, before anything
is built, and a failure is an early refusal naming the remedy:

- **schema identity** — the serving database must already be at the current
  schema version. Input capture reads it through the ordinary read-only storage
  handle, which by design does not migrate, and that handle refuses an
  out-of-date database outright rather than misreading its catalog. Run
  `cidx migrate` first.
- **whole-file containment** — the database must be wholly contained in its main
  file, because publication replaces that file and nothing else. A **WAL**
  database is refused: its committed content also lives in the `-wal` sidecar,
  so the rename would publish a main file against a sidecar describing a
  different database. Any existing `-wal`, `-shm` or `-journal` sidecar is
  refused for the same reason, including one left behind by a crashed writer.
  The other rollback modes (`DELETE`, `TRUNCATE`, `PERSIST`) are accepted:
  their committed database *is* the main file.

The containment check is re-established at the rename itself, not only at
capture, so a sidecar that appears while the rebuild runs also refuses.

## Failure injection

`CIDX_CLEAN_REBUILD_FAIL_AT` aborts the rebuild at a named point. It exists for
qualification and is not part of the supported CLI surface.

Two kinds of point exist and they are not interchangeable. **Boundary** points
abort *between* phases; **in-phase** points abort *inside* one, which is the
only way to exercise a partially applied translation unit, a half-written unit
of work, or an interrupted rename. In execution order:

| Value | Kind | Aborts |
|---|---|---|
| `after-inputs-captured` | boundary | after the input catalog is captured |
| `after-candidate-created` | boundary | after the candidate database exists |
| `after-inputs-replayed` | boundary | after the catalog is replayed into it |
| `during-extraction` | **in-phase** | inside the translation-unit pipeline, while that unit's facts are being handed to the storage adapter |
| `during-writer-commit` | **in-phase** | inside the controlled writer, as the unit of work commits with its facts staged |
| `after-candidate-indexed` | boundary | after the index pass completes |
| `after-verification` | boundary | after the candidate passes every check |
| `before-publication` | boundary | after the pre-rename gate, before publication |
| `during-publication` | **in-phase** | delivers SIGINT *inside* the publication window — after the candidate and directory are on stable storage, before `rename(2)` |
| `after-rename` | **in-phase** | delivers SIGINT immediately after `rename(2)` returns |

The two in-phase extraction points are delivered by the serial translation-unit
pipeline's injector, which the bounded parallel scheduler does not carry;
combining them with `--jobs > 1` is refused rather than silently ignored.

An unset or unrecognised value means "no injection", so a stray environment
variable can never silently skip publication.

## Ownership

This path owns the temporary database, its verification, and the atomic file
replacement. Per-translation-unit writer rollback is **not** reimplemented here —
it stays owned by the controlled set-based writer, and the candidate is indexed
through that same writer.
