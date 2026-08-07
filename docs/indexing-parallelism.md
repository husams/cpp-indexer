# Parallel translation-unit extraction

Operating guide for `cidx index --jobs` and the bounded-extraction budgets.

> **Status: available.** `--jobs N` runs bounded parallel extraction, and
> omitting the option selects the automatic policy. The one case that is still
> refused is an index with no on-disk path (an in-memory database), because
> each worker opens its own handle to the database being indexed.

## Options

| Option | Meaning | Default |
| --- | --- | --- |
| `--jobs N` | Extraction workers. | omitted = automatic policy |
| `--max-queue-bytes N` | Byte budget for extracted-but-unpublished payload. | derived from the memory budget |
| `--max-queue-items N` | Translation-unit budget for the same buffer. | `2 x workers`, at least 2 |
| `--memory-budget-bytes N` | Resident-memory ceiling used to derive the worker count. | 60% of host memory |

All four take a **positive integer**. Zero, negatives, non-numeric text,
trailing garbage, a leading `+`, decimals and empty values are rejected during
parsing with `<option> must be a positive integer` and exit code 2. The same
predicate and the same message are used by `cidx analyze --jobs` and by both of
the project's parser implementations, so the commands cannot drift.

## Automatic worker selection

Omitting `--jobs` selects the documented automatic policy rather than a silent
`1`. It picks the smallest of:

1. **pending translation units** — a three-file run never starts eight workers;
2. **logical cores**;
3. **memory budget / reserved bytes per worker** — 768 MiB is reserved per
   in-flight extraction, dominated by one live `ASTContext` and its source
   buffers.

The result is never zero, and the chosen bound is reported as
`parallel.workers` with a `selection_reason` of `automatic (work-bound)`,
`automatic (core-bound)` or `automatic (memory-bound)`.

The policy deliberately does **not** default to the logical-core count. On a
16-core machine with 4 GiB free, one worker per core would need 12 GiB of AST
memory alone.

## Ordering guarantees

Two orderings matter, and they are not the same thing.

* **Dispatch order** is the legacy apply key
  `(component.path, directory.path, file.name)` — the order `list_files()`
  already returns.
* **Publication order** is the same key. Completed batches wait in a bounded
  reorder buffer until their turn. **Worker completion order never becomes
  persistence order**, which is what keeps `--jobs N` output equal to
  `--jobs 1` output.

Only the scheduler thread writes, and it writes through the single controlled
`FactBatchWriter`. Workers never mutate the index.

## Shared-header amortisation

Serial indexing amortises a shared header *by side effect*: the first
translation unit to reach it indexes it, and every later one reads the committed
row and reports it "already". On a 1,000-unit corpus sharing two headers that is
the familiar `2 indexed, 999 already`.

Concurrent extraction would destroy that, because every in-flight worker sees
the same pre-publication state and would extract the header once per worker.
That shard-style duplication is explicitly **not** an acceptable fallback.

Instead the scheduler owns the assignment. Each worker asks once, after its
parse, and the request blocks until every lower-ranked translation unit has
asked. Ownership is therefore granted in legacy apply order and is independent
of completion order, so a header shared by K translation units is extracted
exactly once and the indexed/already split matches the serial run.

The claim key is `(header path, normalized configuration hash)` — the same
predicate the serial engine applies, since a header only counts as current when
a `file_config` row registers it under *this* unit's normalized configuration.
Two units built under different configurations both legitimately index the same
header.

> The key is the configuration **hash**, not the configuration id. An id is a
> transient negative value derived from that hash until the writer mints the
> row; keying on it would change identity mid-run and hand an owned header to a
> second translation unit.

## Failure handling

| Failure | Behaviour |
| --- | --- |
| Source changed under the parse | **Retried**, up to 2 times. The re-extraction re-captures the snapshot, and the oracle re-grants that rank its own headers so a retry cannot lose them to itself. Exhausting the budget reports a failure. |
| Parse failure | Reported, not retried. |
| Worker crash / thrown exception | Caught, reported as a failed translation unit. The ordered gate is released so successors are not stalled. |
| Writer or commit failure | The writer's transaction rolls back; the unit is reported failed and its file stays pending. Never a partially committed unit. The owned-header grants it held are **revoked**, so those headers become claimable again — leaving them granted would deny them to a later unit that would then report them "already" while no row was ever written. |
| User interruption / cancellation | Dispatch and publication stop. Every dispatched-but-unpublished rank releases its gate. |

Deterministic failures are deliberately not retried: a retry burns a full parse
to reach the same answer and hides the defect.

## Telemetry

With `--profile-json PATH` the run publishes:

* `parallel.workers`, `parallel.max_queue_items`, `parallel.max_queue_bytes`,
  `parallel.memory_budget_bytes`, `parallel.reserved_bytes_per_worker`
* `parallel.items_dispatched`, `parallel.items_published`
* `parallel.peak_reorder_items`, `parallel.peak_reorder_bytes`,
  `parallel.peak_reserved_bytes`, `parallel.peak_rss_bytes`
* `parallel.header_claim_candidates`, `.granted`,
  `.denied_already_indexed`, `.denied_in_flight_owner`, `.regranted_on_retry`,
  `.revoked_after_publish_failure`
* `parallel.publication_throughput_units_per_second`
* `parallel.source_change_retries`, `parallel.retry_exhausted`
* timings: `parallel.wall`, `.worker_active`, `.worker_idle`,
  `.backpressure`, `.publish_wait`, `.header_claim_gate_wait`,
  `.header_claim_gate_wait_max`

`worker_active` against `wall x workers` gives utilisation; a large
`backpressure` means the byte or item budget is the bottleneck; a large
`header_claim_gate_wait` means one slow low-ranked unit is holding the ordered
gate.

### What the topology changes about the rest of the telemetry

The scheduler owns its own controlled writer and its own dispatch order, so it
publishes both of their measurements itself:

* `fact_batch_writer.*` — statements prepared/reused/executed, virtual-machine
  steps, row outcomes, and the prepare/virtual-machine/commit timings. These
  are **identical** to the serial run's for the same corpus, because the same
  writer applies the same batches in the same order;
* each translation unit's `start_position` is its **dispatch rank** in legacy
  apply order. Serially that is also "how many units have been recorded so
  far", but under concurrency every worker starts before any has recorded, so
  the recorded-count reading would give every unit position 0 and the per-unit
  cost-versus-corpus-position analysis would have no abscissa at all.

Two counter families are **not** published by a multi-worker run, because the
mechanisms behind them do not run in it:

| Family | Why |
| --- | --- |
| `tu_fact_cache.*` | The cache decision lives in the serial `TuFactCacheIndexer` wrapper, which the scheduler bypasses. See [tu-fact-cache.md](tu-fact-cache.md). |
| `tu_dependency.*` | Same wrapper: the dependency-invalidation walk runs inside it. |

Their absence is a property of the mode, not a telemetry defect, and it is not
the same thing as the cache reporting zero. `benchmarks/indexing/production.py`
records the measured topology in `index_topology` and, for a parallel run,
lists the withheld counters with that reason in `unavailable_counters`. Pass
`--index-jobs 1` to that harness when the cache and dependency taxonomies are
what you need to measure.

## What a worker may read

A worker must not read the authoritative database. Not because the reads are
wrong — file rows, component ownership and configuration identity are all
run-invariant — but because they cannot coexist with publication. cidx ships
rollback journaling with FULL synchronous durability on purpose
(`storage/sqlite.cpp`: `PRAGMA journal_mode = DELETE`; WAL is explicitly gated
behind measured atomicity evidence). Under rollback journaling the writer needs
an EXCLUSIVE lock, which it cannot take while any connection holds SHARED, so a
worker that reads while the scheduler commits **fails the publication** rather
than merely slowing it.

Two mechanisms remove every such read:

**One immutable extraction snapshot per run.** Before any worker starts, the
run copies the database through the SQLite backup API and every worker opens
that copy READ-ONLY. It does not change for the duration of the run, so a
worker's view of the database is independent of how far publication has
progressed. Read-only is enforced by the open mode, not asserted: a regression
that reintroduces a write from extraction surfaces as `SQLITE_READONLY`.

**Cross-unit symbol identity is resolved at publication, not during the parse.**
This is the one read that is genuinely not hoistable. It is keyed on a USR that
only the parse discovers, and it deliberately resolves against symbols published
by *other* translation units in the same run — so answering it during extraction
makes identity depend on wall-clock timing, and answering it from a pre-run
snapshot silently drops facts (on a 24-unit corpus, 46 canonical rows: every
`uses` edge from a unit into the header-owned `shared` namespace).

Extraction therefore records the *question* — `(usr, identity_source,
semantic universe, translation unit)` against a batch handle — as a
`PendingSymbolReference`, and the controlled writer answers it at publication
against the live database, using the same two identity-key probes the in-parse
lookup used. Because the writer applies batches serially in the legacy apply
order, cross-unit identity becomes a function of that order rather than of parse
timing. That is strictly stronger than the behaviour it replaces.

A reference that still does not resolve at publication is not an error: it is
the deferred form of the serial lookup returning nothing, and the facts that
depend on it are dropped exactly as the serial visitor never emitted them. The
"relation endpoint did not resolve" invariant still holds for every endpoint
this extraction minted itself.

Worker-produced artifacts carry their pending references (FactBatch artifact
wire version 3), so a transferred batch is self-describing.

## Troubleshooting

**"--jobs must be a positive integer"** — the value was 0, negative,
non-numeric, had trailing characters, or was empty.

**"--jobs greater than 1 needs an index stored on disk"** — the selected index
has no file to snapshot (an in-memory database). Use `--jobs 1`.

**A run that plans one worker takes the serial path.** `--jobs 1`, a
single-translation-unit run, and an automatic plan bounded to one worker all
compute the same thing, and the serial path does it with the translation-unit
fact cache and without paying for an extraction snapshot. `--jobs 4` on a
one-unit run is a one-worker plan, and is reported as such.

**Low utilisation with high `publish_wait`** — one slow low-ranked translation
unit is holding the reorder buffer. Expected on corpora with one very large
unit; it costs latency, not correctness.

**High `backpressure`** — raise `--max-queue-bytes` or `--max-queue-items`, or
raise `--memory-budget-bytes` so the derived budgets grow with it.

**Memory pressure** — lower `--memory-budget-bytes`; the worker count and both
queue budgets are derived from it.
