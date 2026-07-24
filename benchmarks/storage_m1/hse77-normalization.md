# Storage M1 normalization evidence (HSE-77)

This report is the checked-in result of the standard-library benchmark
`hse77_normalization.py`. It compares the legacy repeated-USR hot rows with the
normalized integer-ID rows, then measures compatibility reads and the proposed
symbol hot/cold split.

Reproduce it with:

```text
PYTHONPATH=. python3 benchmarks/storage_m1/hse77_normalization.py \
  --output benchmarks/storage_m1/hse77-normalization.json
```

Method: 20,000 rows, five repetitions, 500 compatibility rows, SQLite
3.53.3, DELETE journaling, and `synchronous=FULL`. The complete per-object
table/index byte inventory and raw samples are in
`hse77-normalization.json`.

| Measurement | Legacy text rows | Normalized rows |
| --- | ---: | ---: |
| Table/index bytes | 1,884,160 | 1,167,360 |
| Write throughput (median rows/s) | 296,779 | 158,276 |
| Compatibility query (median ms) | — | 0.516 |
| v36-style migration cost | — | 0.106 s / 188,020 rows/s |

The normalized layout removes repeated long identity text from the occurrence
table and retains every unresolved value once in `external_identity`. The
write result includes identity dictionary population, so it is evidence of
the migration/write tradeoff rather than a claim of faster writes. Read-side
compatibility reconstructs the legacy fields through deterministic joins.

The measured symbol split was 1,855,488 bytes and 4.592 ms median for the
unsplit table versus 1,994,752 bytes and 5.726 ms for hot/cold tables. The
decision is therefore to retain symbol attributes in one table: this workload
showed no byte or lookup benefit from paying for the extra join.
