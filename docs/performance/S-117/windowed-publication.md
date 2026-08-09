# S-117 ordered windowed publication

The baseline binary is the accepted S-116 parent commit `7c509c4`; the
candidate is the S-117 working branch. Both were Release builds made with the
same compiler and measured on the same arm64 macOS host. Every run used a fresh
disposable database. Absolute paths in the JSON captures are normalized.

## Five-TU corpus

The corpus is `src/query/exec.cpp`, `src/query/plan.cpp`,
`src/query/cxq.cpp`, `src/cli/commands.cpp`, and
`src/ast/index_engine.cpp`, with `--jobs 5`.

| Metric | S-116 parent | S-117 candidate | Result |
| --- | ---: | ---: | --- |
| `parallel.wall` | 15.8078s | 14.7421s | 6.7% lower |
| `parallel.publish_wait` | 5.7859s | 5.6226s | **fails** S-117 limit of 2.28s |
| `parallel.header_claim_gate_wait` | 2.4901s | 2.3461s | **fails** S-117 limit of 0.59s |
| `fact_batch_writer.virtual_machine` | 9.5805s | 8.7251s | 8.9% lower |
| `fact_batch_writer.commit` | 0.1200s | 0.0461s | 61.6% lower |
| writer transactions | 5 | 2 | 60% fewer |
| temporary-table checks | 75 | 30 | 60% fewer |
| publication windows / peak size | not applicable | 2 / 4 TUs | bounded by configured limits |

The requested threshold comparison is against the older S-111 reference
(`publish_wait=4.56s`, header gate wait `1.18s`). The candidate is 123% and
199% of those references respectively, rather than at most 50%.

The measurements expose a mismatch between the requested outcome and the
existing telemetry. `parallel.publish_wait` measures publisher time waiting for
the next ranked extraction result. On this corpus it is dominated by the first
ranked TU's extraction latency, not time spent serializing SQLite publication.
Windowing reduces writer transactions and wall time, but cannot halve that
pre-publication wait. Likewise, `header_claim_gate_wait` measures time workers
wait for legacy rank sequencing, not time holding the claim mutex.

## Synthetic 1,000-TU corpus

The repository's `benchmarks/indexing/run.py` generator produced the standard
two-shared-header corpus. Both binaries indexed the identical generated corpus
with the automatic ten-worker plan.

| Metric | S-116 parent | S-117 candidate | Result |
| --- | ---: | ---: | --- |
| `parallel.wall` | 7.8029s | 6.5310s | 16.3% lower |
| `parallel.publish_wait` | 0.1259s | 0.1296s | effectively unchanged |
| `fact_batch_writer.virtual_machine` | 4.4061s | 3.9058s | 11.4% lower |
| `fact_batch_writer.commit` | 0.9662s | 0.2871s | 70.3% lower |
| writer transactions | 1,000 | 51 | 94.9% fewer |
| temporary-table checks | 15,000 | 765 | 94.9% fewer |
| publication windows / peak size | not applicable | 51 / 20 TUs | bounded by configured limits |
| peak window bytes | not applicable | 942,080 bytes | below 2,576,980,377-byte bound |

The candidate reports 1,000 window items, 47,142,400 total window bytes, 51
successful window commits, zero window rollbacks, and zero fallback replays in
the success-path measurement. Failure-path rollback/replay is covered by the
FactBatchWriter contract tests rather than benchmark injection.

## Reproduction

For the five-TU run, import the checkout's Release build compilation database
into a fresh `INDEXER_CACHE`, then run:

```sh
cidx index src/query/exec.cpp src/query/plan.cpp src/query/cxq.cpp \
  src/cli/commands.cpp src/ast/index_engine.cpp --jobs 5 \
  --profile-json <capture.json>
```

For the larger run, generate 1,000 units with
`benchmarks/indexing/run.py::generate_corpus`, import its
`compile_commands.json` into a fresh cache, and run `cidx index
--profile-json <capture.json>`.

Raw captures:

- `before-five-tu.json`
- `after-five-tu.json`
- `before-1000-tu.json`
- `after-1000-tu.json`
