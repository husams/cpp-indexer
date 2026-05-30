# ADR-3: SQLite as durable USR/path↔ID authority, with a thread-safe allocator and write-through LRU

Status: accepted

## Context

We need a per-repo USR↔id and path↔id map that is:

- **Durable** across re-index runs so a known USR gets the same integer ID every time (C3,
  S1-SC-02, S7-SC-12). SQLite is the source of truth (C3).
- **Thread-safe** under the parallel libclang visit (`src/pipeline/parallel.rs`), where N rayon
  workers concurrently allocate IDs for the symbols and files they encounter (C4, S2-SC-08/09/10).
- **Fast** — not hitting SQLite on every lookup. A configurable LRU cache fronts SQLite; eviction is
  always safe because SQLite remains authoritative (C5 write-through, S2-SC-03).

Constraints fixed by inputs: get-or-insert semantics (no truncate-and-realloc); both directions
(usr→id and id→usr; path→id and id→path); `cache_size=0` disables the cache entirely; the smallest
functioning LRU is `cache_size=1`. SQLite (`rusqlite`) is the chosen embedded store; it is not yet a
dependency (Cargo.toml has `rocksdb` but no `rusqlite`).

## Decision

### Storage (C3, C6, S1)

- One SQLite DB **per repo**, two tables exactly as the AC specify:
  - `symbols(id INTEGER PRIMARY KEY, usr TEXT NOT NULL UNIQUE)`
  - `files(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE)`
- `id INTEGER PRIMARY KEY` is SQLite's rowid alias → dense monotone allocation starting at 1,
  giving the most compact IDs (supports the ≥30% graph-size goal, S4-SC-06).
- Open with `PRAGMA journal_mode=WAL` and `PRAGMA synchronous=NORMAL` for safe concurrent reads with
  one writer (see allocator below).
- **Default DB path (resolves OQ-1):** `<stage_dir>/cxg-symbols.db`, where `stage_dir` is the
  existing Phase-3 staging directory (`IndexConfig::stage_dir`, default the indexer output dir). One
  map per repo follows naturally because each repo already has its own stage dir. Configurable via
  ADR-3 §config / S3.

### Allocator (C4, S2) — `SymbolAllocator`

- A single `SymbolAllocator` is constructed once per repo and shared as `Arc<SymbolAllocator>`,
  passed into `run_phase1_parallel` next to the existing `Arc`-shared atomic counters, threaded into
  `VisitOptions` so the visit attaches the integer `id` to each `NodeRecord`/`EdgeRecord` it emits.
- **Concurrency model:** a single SQLite write `Connection` behind a `std::sync::Mutex` (one writer;
  WAL allows concurrent readers). `get_or_insert_symbol(&self, usr) -> Result<i64>` runs, inside the
  lock, an `INSERT ... ON CONFLICT(usr) DO NOTHING` followed by `SELECT id WHERE usr=?` — this makes
  concurrent get-or-insert of the same USR allocate **exactly one** row and return the same id to
  both callers (S2-SC-08), and distinct USRs get distinct ids (S2-SC-09). No deadlock: a single lock
  with no nested acquisition (S2-SC-10). This deliberately reuses the project's existing
  single-writer discipline rather than inventing a new lock type; `src/sink/lock.rs` is a *Phase-5
  advisory* lock and does not apply here (it is cross-process DB coordination, not in-process map
  allocation) — recorded so the developer does not mis-wire it (C4 "where applicable").
- **Reverse direction (id→usr, id→path):** `SELECT usr WHERE id=?` / `SELECT path WHERE id=?`,
  cache-fronted, used by the read path (S5) and round-trip tests (S7-SC-13/14).

### Write-through LRU (C5, S2)

- A bidirectional cache fronts each map: usr→id and id→usr (and path→id / id→path). Implement as two
  hand-rolled LRU maps (or one keyed both ways) guarded by a `Mutex`/`RwLock`; **no new crate
  required** for a bounded LRU (a `HashMap` + intrusive recency list, or the existing approach used
  elsewhere in the tree). Keep the cache type private to the allocator module.
- **Write-through:** every successful `get_or_insert` writes SQLite first, *then* populates the
  cache. Therefore an evicted entry is always re-derivable from SQLite — eviction can never lose data
  (S2-SC-03, C5).
- **`cache_size == 0`:** the allocator holds no cache structure at all and every lookup hits SQLite
  directly (S2-SC-04, S7-SC-09). This is a distinct code path (an `Option<Cache>` or a `cap==0`
  guard), not "an LRU of size 0".
- **`cache_size == 1`:** smallest functioning LRU; inserting a second distinct key evicts the first,
  which remains resolvable via SQLite (S2-SC-05).
- Both lookup directions are cached and both are tested (S2-SC-06/07, S7-SC-10/11).

## Alternatives considered

- **(a) RocksDB (already a dependency) instead of SQLite.** Rejected: the AC name SQLite and a
  relational `UNIQUE`-constrained schema explicitly (S1-SC-01); RocksDB has no `UNIQUE` index, so
  get-or-insert atomicity would need a manual merge operator or external locking — more code, weaker
  guarantee, and a contract mismatch with the stated schema.
- **(b) `rusqlite` connection pool (one connection per worker).** Rejected for the *writer*: SQLite
  serialises writes anyway, and multiple write connections invite `SQLITE_BUSY`/retry loops.
  Single-writer-Mutex is simpler and deadlock-free. (Read-only resolution on the daemon side MAY use
  a separate read connection per the WAL model — left to the developer as an optimisation, not
  required by any AC.)
- **(c) A third-party LRU crate (e.g. `lru`).** Acceptable but not mandated; rejected as a *required*
  decision to avoid a new dependency for a small bounded cache. If the developer prefers `lru`, that
  is a fine implementation choice — the ADR fixes the *behaviour* (write-through, bidirectional,
  size-0 disabled, size-1 evicts), not the crate.
- **(d) In-memory-only map, persisted at end of run.** Rejected: a crash mid-run would lose the
  id↔usr mapping and corrupt the partially-written graph (C7); durability must be per-insert.

## Consequences

- Positive: dense small integers; durable ID stability across re-index (C3); deadlock-free parallel
  allocation; eviction provably lossless (write-through).
- Positive: one DB per repo falls out of using the per-repo stage dir → satisfies D1 storage isolation
  (S1-SC-03) with no extra logic.
- Negative: adds `rusqlite` to `Cargo.toml` (bundled SQLite feature, so no system libsqlite needed).
- Negative: the single write-Mutex serialises *allocations* (not visits); allocation is a tiny
  fraction of per-TU work (one indexed insert/select), and the LRU absorbs repeat lookups, so the
  contention is expected to be negligible. If it ever shows up in the `parallel_phase1` bench, batch
  the inserts per worker — noted as a follow-up, not a current requirement.
- Follow-up: the read/query path (S5) and round-trip tests (S7) consume the reverse-direction API;
  the resolver handle must be repo-scoped (ADR-1 consequence).

## References

- requirements.md S1, S2; scenarios S1-SC-01..04, S2-SC-01..10, S7-SC-01..11; constraints C3,C4,C5,C6
- `src/pipeline/parallel.rs` (allocator injection point); `src/sink/lock.rs` (NOT this lock — clarified)
- ADR-1 (per-repo namespace); ADR-2 (schema bump); ADR-4 (migration)
- cognee tags: `task:graph-symbol-ids`, `role:architect`
