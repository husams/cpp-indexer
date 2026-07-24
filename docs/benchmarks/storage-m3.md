# Storage M3: optional graph accelerator qualification

HSE-81 keeps SQLite as the authoritative local store and turns optional graph
acceleration into a measured, disposable projection decision. The checked-in
contract is `benchmarks/storage_m3/evaluation-v1.json`; its gate is:

```text
PYTHONPATH=. python3 -m benchmarks.storage_m3.gate \
  --report benchmarks/storage_m3/evaluation-v1.json \
  --root .
```

The report retains the HSE-75 v34 SQLite-only baseline and a deliberately
unsuitable mutable-global-graph control. It also names the qualified candidate
shapes without pretending that a synthetic smoke workload proves their value:
memory-mapped CSR adjacency, compact ID/degree/offset tables, columnar scan
artifacts, and bounded reachability indexes. An embedded alternative engine is
not a mandatory service and cannot be evaluated as a primary store until the
gate is reopened.

Every candidate descriptor binds source fact sets, producer/version, build
options, generation-scoped identity mapping, invalidation inputs, publication
and rebuild behavior, QueryPlan/result semantics, and local fallback behavior.
The projection contract requires explicit completeness, truncation, evidence,
ordering, and stale/missing fallback. SQLite remains readable if a sidecar is
deleted, stale, corrupt, or absent.

The cost model is end-to-end: build and update time, hot and total query
latency, duplicate disk, memory mapping, open/attach overhead, cleanup and
retention, portability and packaging, crash recovery, and maintenance. Hot
traversal latency alone is not a decision criterion.

The current recommendation is `do_nothing`: the retained HSE-75 v34 baseline
has no failed SLO on its measured smoke workload. Reopening the custom-primary
store gate requires the same named workload to reproduce an exact failed SLO
after HSE-77/HSE-78 optimization, a separately measured derived accelerator
that cannot address it, full compatibility/operational evidence, and an ADR
plus explicit user decision. The existing HSE-75 gate remains the validator for
the required schema/tuning and derived-accelerator alternatives.
