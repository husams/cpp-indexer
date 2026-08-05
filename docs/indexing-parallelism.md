# Parallel translation-unit extraction

Operating guide for `cidx index --jobs` and the bounded-extraction budgets.

> **Status: the mechanism ships, the mode does not.** `--jobs N` greater than 1
> is currently refused with a diagnostic. See
> [Why `--jobs N > 1` is refused](#why---jobs-n--1-is-refused) for the exact
> blocker and what closing it requires. Everything else on this page describes
> behaviour that is implemented and tested.

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
* `parallel.source_change_retries`, `parallel.retry_exhausted`
* timings: `parallel.wall`, `.worker_active`, `.worker_idle`,
  `.backpressure`, `.publish_wait`, `.header_claim_gate_wait`,
  `.header_claim_gate_wait_max`

`worker_active` against `wall x workers` gives utilisation; a large
`backpressure` means the byte or item budget is the bottleneck; a large
`header_claim_gate_wait` means one slow low-ranked unit is holding the ordered
gate.

## Why `--jobs N > 1` is refused

Extraction still **reads the authoritative database throughout each parse** —
owned-header file rows, per-file configuration applicability, component
ownership, portable identities, and cross-translation-unit external symbol
identities.

cidx ships rollback journaling with FULL synchronous durability on purpose
(`storage/sqlite.cpp`: `PRAGMA journal_mode = DELETE`; WAL is explicitly gated
behind measured atomicity evidence). Under rollback journaling a writer needs an
EXCLUSIVE lock, which it cannot take while any connection holds SHARED. With
several workers reading continuously, the window in which no reader holds SHARED
shrinks toward zero, so the controlled writer is **starved** rather than merely
slowed. Reproduced on a 24-unit corpus: every publication failed at
`INSERT INTO translation_unit_config`.

Most of those reads are hoistable into an immutable per-run input, but one is
not. `set_persistent_symbol_lookup` resolves a symbol's *external identity*
against the live symbol table, mid-parse, keyed on a USR that is only discovered
by parsing, and it deliberately sees rows published by **other translation units
in the same run**. Ablating it on the 24-unit corpus loses 46 canonical rows —
every `uses` edge from a unit into the header-owned `shared` namespace.

That makes it order-dependent by construction: a unit extracted concurrently
with its predecessor would see a symbol table missing that predecessor's
symbols, and the answer would depend on wall-clock timing rather than on legacy
order. Making it deterministic again would require each unit's *parse* to wait
for its predecessor's *publication*, which is full serialisation.

Closing it therefore means resolving cross-unit identity **after** extraction
instead of during it, which is a change to the identity model rather than to the
scheduler.

## Troubleshooting

**"--jobs must be a positive integer"** — the value was 0, negative,
non-numeric, had trailing characters, or was empty.

**Low utilisation with high `publish_wait`** — one slow low-ranked translation
unit is holding the reorder buffer. Expected on corpora with one very large
unit; it costs latency, not correctness.

**High `backpressure`** — raise `--max-queue-bytes` or `--max-queue-items`, or
raise `--memory-budget-bytes` so the derived budgets grow with it.

**Memory pressure** — lower `--memory-budget-bytes`; the worker count and both
queue budgets are derived from it.
