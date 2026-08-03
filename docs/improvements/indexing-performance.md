# Indexing performance improvements

Recommendations derived from a CPU profile of `cidx index` on a single
translation unit. All measurements, raw call graphs and reproduction commands
live in [`docs/prefs/`](../prefs/README.md); this document only states what to
change and why.

## Baseline

`src/query/exec.cpp` into an empty database, `RelWithDebInfo` build,
macOS 26.1 arm64, one thread:

| | |
| --- | ---: |
| Wall | 15.98 s |
| `sqlite_vdbe` | 13.85 s (**87 %**) |
| `clang_front_end` (parse + sema) | 1.45 s (9 %) |
| SQLite `step` calls | 447,203 |
| **SQLite VDBE instructions** | **762,967,701** |
| Symbols emitted | 2,178 |
| Type facts attempted → distinct `type_node` rows | 88,058 → 1,773 |

Indexing this project is not front-end bound. It is bound by the SQL the
extraction passes issue while walking the AST: 82 % of leaf samples are inside
`libsqlite3.dylib` and only 0.2 % inside `cidx` itself.

The dominant frame is `SqliteStorageService::lookup_symbol` at 62.3 % inclusive,
reached from the statement pass through
`CallEdgeEmitter::mint_resolved_target` → `StorageEdgeSink::lookup_symbol_id`.

Recommendations are ordered by measured value. R1 is worth more than everything
below it combined.

---

## R1 — Let the identity lookups use `idx_symbol_identity` (P0)

### Problem

Symbol identity is resolved with

```sql
SELECT <cols> FROM symbol WHERE semantic_universe_id = ? AND identity_key = ?
```

and the index that serves it is **partial**:

```sql
CREATE UNIQUE INDEX idx_symbol_identity
  ON symbol(semantic_universe_id, identity_key) WHERE identity_key <> '';
```
`src/storage/storage_schema.cpp:182-183`

SQLite may only use a partial index when the query's `WHERE` clause provably
implies the index predicate. The query never says `identity_key <> ''`, so the
planner discards it and falls back to `idx_symbol_scope(semantic_universe_id)`,
which matches **every** row in the universe. Every identity resolution walks the
whole symbol table.

Two call sites are affected, both in `src/storage/storage_symbols.cpp`:

| Site | Frequency | Notes |
| --- | --- | --- |
| `lookup_symbol` → `find_by_identity_key`, line 311 | twice per resolution | probes the TU-local key first; for any symbol declared outside the TU that probe **cannot** match, so it always pays a full scan before the second probe starts |
| `mint_symbol_id`'s follow-up `SELECT id`, line 695 | once per symbol mint (8,679 in this TU) | the just-upserted row sorts last in rowid order, so this is a full scan too |

Note that the write paths already name the predicate correctly —
`ON CONFLICT(semantic_universe_id, identity_key) WHERE identity_key <> ''` at
`storage_symbols.cpp:51` and `:651`. Only the reads were left behind.

### Evidence

VDBE instructions per probe, counted exactly with `sqlite3_progress_handler`
(load-independent), `LIMIT 1` to mirror the C++ path, which calls
`SqliteStmt::step()` once:

| Symbol rows `N` | as shipped, hit | as shipped, miss | with predicate, hit | miss |
| ---: | ---: | ---: | ---: | ---: |
| 2,839 | 8,087 | 14,206 | **19.1** | **16.0** |
| 15,015 | 40,844 | 75,086 | **19.1** | **16.0** |

A miss costs exactly `5.00 × N`; a hit averages `≈2.8 × N`; with the predicate
the cost is constant. **The lookup is O(N) in stored symbols today, which makes
indexing a repository quadratic in symbol count. With the predicate it is
O(log N).** At the current repo `index.db` (15,015 symbols) that is a 2,100×
(hit) to 4,700× (miss) reduction in work per probe — and the factor keeps
growing as the index grows.

### Change

Add the predicate to both queries:

```diff
-      auto scoped = db_.prepare(std::string("SELECT ") + kSymbolCols +
-                                " FROM symbol WHERE semantic_universe_id = ?"
-                                " AND identity_key = ?");
+      auto scoped = db_.prepare(std::string("SELECT ") + kSymbolCols +
+                                " FROM symbol WHERE semantic_universe_id = ?"
+                                " AND identity_key = ? AND identity_key <> ''");
```

and the same for the `SELECT id` in `mint_symbol_id` — or delete that statement
outright, see [R4](#r4--return-the-id-from-the-upsert-p1).

### Why this is safe

`symbol_identity_key` (`src/storage/storage_repo.cpp:252-287`) always emits
`<universe-key>\x1f…`, where the universe key falls back to the literal
`"legacy"`. It can never return an empty string, and both databases inspected
contain **0** rows with an empty or NULL `identity_key`. The predicate is
therefore a no-op on results and a pure planner hint.

Add a targeted storage test that asserts the plan, so the regression cannot
come back silently:

```cpp
// EXPLAIN QUERY PLAN for the identity lookup must name idx_symbol_identity.
```

### Alternatives considered

- **`ANALYZE`** — does not help and makes the plan slightly worse. Partial-index
  usability is a *semantic* constraint, not a cost decision. Measured on the
  repo `index.db`: after `ANALYZE` the plan degrades from
  `SEARCH … USING INDEX idx_symbol_scope` to a plain `SCAN symbol`. Still O(N).
- **Drop `WHERE identity_key <> ''` from the index** — changes the uniqueness
  semantics (rows with an empty key would start colliding) and forces matching
  edits to both `ON CONFLICT` clauses. Rejected.
- **Add a second, non-partial `(semantic_universe_id, identity_key)` index** —
  works with no query change (verified: the plan becomes
  `SEARCH … USING COVERING INDEX`), but costs a redundant index on every symbol
  insert and +4.2 % on disk (+2.99 MB on the 71.5 MB repo `index.db`). Keep as
  a fallback only if changing the SQL text is undesirable.

---

## R2 — Cache negative identity lookups (P0)

`StorageEdgeSink::lookup_symbol_id` (`src/ast/storage_edge_sink.cpp:66-92`)
memoizes hits and deliberately **not** misses:

```cpp
  if (!sym) {
    // Do not cache misses: a later mint_symbol() in this translation unit may
    // create the symbol that was absent on this lookup.
    return std::nullopt;
  }
```

The reasoning is sound but the cost is inverted: in a cold database most probes
miss, and a miss is the *most* expensive case — a full scan with no early exit.
Repeated references to the same not-yet-minted callee each pay it again.

**Change.** Keep negative entries, stamped with a mint generation:

```cpp
  std::uint64_t mint_generation_ = 0;                     // bumped in mint_symbol()
  std::unordered_map<std::string,
                     std::pair<std::optional<int64_t>, std::uint64_t>> lookup_cache_;
```

A negative entry is usable only while its stamp equals `mint_generation_`;
`mint_symbol` increments the generation, which invalidates every negative entry
in O(1) while leaving positive entries (which a mint cannot falsify — identities
are stable once written) valid. Cheaper still: on `mint_symbol`, also insert the
positive entry for the identity just created, so the following lookup is a hit
rather than a re-probe.

This is worth doing **even after R1**: an O(log N) seek is still ~16 VDBE
instructions plus a bind and a step, versus a hash lookup.

---

## R3 — Emit a callee's signature once per TU, not once per call site (P1)

### Problem

`CallEdgeEmitter::mint_resolved_target` (`src/ast/call_edge_emitter.cpp:128-152`)
ends with:

```cpp
  StatementDeclarationAdapter declaration_ports(ctx_.ports());
  DeclarationEdgeVisitor signature_visitor(ctx_.context(), declaration_ports,
                                           {}, ctx_.file_id());
  signature_visitor.emit_signature_types_for(callee, dst_id);
```

This runs at **every call site**. `DeclarationEdgeVisitor` owns a `TypeInterner`
(`src/ast/declaration_edge_visitor.hpp:163`) whose memo is keyed on
`QualType` opaque pointers, so constructing a fresh visitor throws that memo
away: the callee's return type and every parameter type are re-interned from
scratch, per call site.

The counters show the effect — 88,058 type facts attempted, **0 duplicates
suppressed**, 1,773 distinct `type_node` rows actually produced. Roughly 98 % of
the work is redundant. Each redundant intern is not cheap: `intern_type_node`
(`src/storage/storage_types.cpp:27-76`) issues up to three statements (decl
lookup, upsert, `SELECT id`) and then `reconcile_type_identity`
(`src/storage/storage.cpp:792-834`) issues five more — **eight statements per
interned type that carries a `decl_usr`**.

Inclusive cost: `emit_signature_types` 1.80 s, `intern_type_node` 1.57 s,
`reconcile_type_identity` 1.16 s.

### Change

Hoist the visitor and memoize per callee for the lifetime of the TU. Add to
`CallEdgeEmitter`:

```cpp
  llvm::DenseSet<const clang::FunctionDecl *> signature_emitted_;
```

and skip the signature block when the canonical declaration has already been
handled. Keep one long-lived `DeclarationEdgeVisitor` (and therefore one
`TypeInterner` memo) for the pass instead of constructing one per call site.

### Correctness note — this also fixes a data defect

Because the visitor is constructed with `target_file = {}` and
`file_id = ctx_.file_id()` (the **caller's** file),
`DeclarationEdgeVisitor::fill_signature_parameter`
(`src/ast/declaration_edge_visitor.cpp:351-355`) writes

```cpp
    record.file_id = file_id_;                    // the call site's file
    record.line    = expansion_loc(param).line;   // the parameter's real line
```

so a callee declared in another file gets an inconsistent `(file_id, line, col)`
triple, and the winner is whichever call site re-emitted it last. Measured on a
two-TU index: **251 of 1,707 parameter rows (14.7 %)** have
`parameter.file_id` ≠ the owner's `decl_file_id`. For example
`json_out::of(const std::string &)` — declared at `src/cli/json_out.hpp:61` —
has its parameter row stamped `file_id → src/query/plan.cpp, line 61`. Line 61 is
correct for the header and meaningless for `plan.cpp`.

Full evidence: [`docs/prefs/raw/parameter-file-id-evidence.txt`](../prefs/raw/parameter-file-id-evidence.txt).

De-duplicating the emission changes which value wins, so fix the stamp at the
same time: resolve `file_id` from the parameter's own `expansion_loc` (as the
declaration pass does via `router_`, `declaration_edge_visitor.cpp:447`,
`:1048`) rather than inheriting the visitor's current file. Landing R3 without
this would freeze an arbitrary winner instead of removing the inconsistency.

Because `emit_signature_types` is a wholesale refresh, emitting it once per
callee leaves the same final rows — the redundant repeats contribute nothing but
statements.

---

## R4 — Return the id from the upsert (P1)

`mint_symbol_id` (`src/storage/storage_symbols.cpp:695-702`) and
`intern_type_node` (`src/storage/storage_types.cpp:67-71`) each follow their
`INSERT … ON CONFLICT DO UPDATE` with a separate `SELECT id`. `add_edge`
(`storage_symbols.cpp:705-720`) already uses `RETURNING id` against the same
SQLite build (3.51.0, `RETURNING` since 3.35).

Appending `RETURNING id` to both upserts removes one statement per symbol mint
and one per type intern — and in `mint_symbol_id` it removes a full-table scan
outright (see [R1](#r1--let-the-identity-lookups-use-idx_symbol_identity-p0)).
`DO UPDATE` always produces a row, so `RETURNING` yields exactly one row on both
the insert and the conflict path; this would not hold for `DO NOTHING`.

---

## R5 — Make `reconcile_type_identity` proportional to distinct types (P2)

`reconcile_type_identity` (`src/storage/storage.cpp:792-834`) fires five UPDATE
statements — `type_node`, `edge_site` ×2, `call_arg` ×2 — for every interned type
carrying a `decl_usr`. All five plans are index-driven; the cost is call volume,
not a missing index. R3 removes most of the volume by itself.

Two further reductions, in order of safety:

1. Skip the call when `intern_type_node` observed no change — an upsert that hit
   an existing row with the same `decl_id` has nothing to reconcile.
2. Defer reconciliation to end-of-TU and run it once over the set of
   `(type_id, decl_usr)` pairs touched, instead of once per intern. This is the
   same shape as `reconcile_pending_symbol_identities`
   (`storage.cpp:664`, 1.80 s inclusive at commit) and should be measured
   against it before choosing.

---

## R6 — Amortize the header passes across TUs (P2)

For the profiled TU the header passes cost `pass.namespaces.headers` 2.27 s +
`pass.declarations.headers` 1.67 s + `pass.symbols.headers` 0.52 s = **28 %** of
the run.

They do not amortize today. Indexing a second TU (`src/query/plan.cpp`) into the
same database reported `new_headers = 0` with 10 headers *already indexed*, and
still spent 1.65 s + 0.65 s + 0.39 s = **2.69 s** in those passes — for a source
file a quarter the size. Raw telemetry:
[`docs/prefs/raw/profile-json-second-tu.json`](../prefs/raw/profile-json-second-tu.json).

Worth investigating before designing a fix: `already_indexed_headers` gates
symbol *emission*, but the namespace-use replay and declaration passes appear to
run over header decls regardless. If a header's facts are already durable for
the same configuration, the pass should be skippable — the existing
`configuration_state` / descriptor machinery in `IndexSession` already tracks
what would invalidate it.

This is the largest remaining item after R1–R4 and the least specified; scope it
with its own measurement rather than from this document.

---

## Non-goals and rejected ideas

- **Front-end reuse / PCH.** `clang_front_end` is 1.45 s of 15.98 s. Even a
  perfect cache recovers at most 9 %. Not the lever.
- **`PRAGMA journal_mode` / `synchronous` changes.** The indexing profile uses
  `journal_mode = DELETE` + `synchronous = FULL` (`src/storage/sqlite.cpp:276`,
  `:279`), but the whole TU runs in one transaction, so this is one fsync at
  commit. `commit` is 1.48 s and dominated by
  `reconcile_pending_symbol_identities`, not by the journal. Changing durability
  settings would trade safety for nothing measurable here.
- **Statement-cache tuning.** `SqliteDb::prepare` already pools compiled
  statements (`sqlite.hpp:152-153`: 256 texts × 4). `sqlite_prepare` totals
  0.065 s across 411,822 prepare calls, i.e. essentially all cache hits. Not a
  lever.
- **Parallel indexing.** Do not reach for it before R1. Parallelism would
  multiply an O(N) scan across threads contending on one SQLite connection.

## Validation

Every item above is measurable with instrumentation that already ships:

```bash
export INDEXER_CACHE=/tmp/profrun
cidx import --db "$(pwd)/build" --name cpp-indexer
cidx index src/query/exec.cpp --profile-json /tmp/after.json
```

Compare against [`docs/prefs/raw/profile-json.json`](../prefs/raw/profile-json.json)
on:

| Metric | Baseline | Direction |
| --- | ---: | --- |
| `counters.sqlite.virtual_machine_steps` | 762,967,701 | must fall sharply after R1 |
| `counters.sqlite.fullscan_steps` | 659,939 | must fall sharply after R1 |
| `counters.sqlite.step_calls` | 447,203 | falls with R3 + R4 |
| `facts_by_family.types.attempted` | 88,058 | falls toward 1,773 with R3 |
| `timings.sqlite_vdbe` | 13.85 s | primary wall-clock signal |
| `timings.body_extraction` | 8.49 s | primary wall-clock signal |

`virtual_machine_steps` and `fullscan_steps` are deterministic — unlike seconds,
they do not move with host load, so use them as the gate and treat wall time as
corroboration. Re-run the `sample` profile afterwards to confirm the hot path
actually moved rather than shifting to the next scan.

Expected effect is stated deliberately as a bound, not a number: R1 removes an
O(N) term from a path holding 62 % of inclusive time, so the ceiling is large,
but what remains — per-statement fixed cost, `step` call volume — is what R2–R4
attack. Measure before claiming a figure.
