# S-074 serial/parallel qualification

Produced by `benchmarks/indexing/parallel_matrix.py`, which extends the existing
`benchmarks/indexing/run.py` corpus generator and the `scripts/dump_layer0.sh`
projection rather than introducing a second harness. Each arm gets its own
corpus and its own index, so no arm can observe another's edits.

Host: Apple Silicon (Darwin 25.1.0), 3 paired trials per arm, `index rebuild`
over a generated corpus with two shared headers.

```
python3 benchmarks/indexing/parallel_matrix.py --cidx build/cidx \
    --count 1000 --trials 3 --output parallel-1000.json
```

## Canonical parity

Every arm is compared against `--jobs 1` on three axes, and all three hold at
24, 64 and 1000 translation units for `--jobs 2`, `--jobs 4` and the automatic
policy:

| Axis | What is compared | Result |
| --- | --- | --- |
| Layer-0 projection | `scripts/dump_layer0.sh`: file, symbol, decl_site, edge, edge_site, call_arg, template_arg — surrogate keys resolved, rows ordered | identical |
| Whole-schema row counts | every user table, including the ones the Layer-0 dump omits: definition, def_edge, type_node, type_edge, parameter, symbol_type, template_param, diagnostic, fact_applicability, file_config, translation_unit_config, semantic_universe, include_* | identical |
| Database soundness | `PRAGMA integrity_check`, `PRAGMA foreign_key_check` | `ok`, 0 violations |

The 24-unit comparison also ran against the pre-change binary on `main`
(`24fb651`): 1624 canonical rows, identical. That figure includes the 46 rows —
every `uses` edge from a unit into the header-owned `shared` namespace — that
are lost if cross-translation-unit identity is resolved from a pre-run snapshot
instead of at publication.

Deeper per-row projections of the tables the Layer-0 dump omits (definition and
def_edge in particular) are compared in `tests/parallel_index_database_test.cpp`,
which also indexes with worker completion forced into the exact reverse of the
dispatch order, and covers delete/re-emit, repeated declarations and the same
symbol declared by every unit.

## Shared-header amortisation

Aggregate owned-header counters, identical on every arm and repeatable across
all three trials:

| Corpus | indexed | already |
| --- | --- | --- |
| 64 | 2 | 63 |
| 1000 | **2** | **999** |

The 1,000-unit `2 indexed, 999 already` split is the serial figure the story
names. Concurrency does not turn shared-header work into per-unit work.

## Wall time, speedup and efficiency

| Corpus | Arm | Median | Per TU | Speedup | Efficiency |
| --- | --- | --- | --- | --- | --- |
| 64 | `--jobs 1` | 1.403 s | 21.92 ms | 1.00x | — |
| 64 | `--jobs 2` | 0.738 s | 11.53 ms | 1.90x | 0.95 |
| 64 | `--jobs 4` | 0.738 s | 11.53 ms | 1.90x | 0.48 |
| 64 | automatic | 0.763 s | 11.92 ms | 1.84x | — |
| 1000 | `--jobs 1` | 43.74 s | 43.74 ms | 1.00x | — |
| 1000 | `--jobs 2` | 31.40 s | 31.40 ms | 1.39x | 0.70 |
| 1000 | `--jobs 4` | 30.95 s | 30.95 ms | 1.41x | 0.35 |
| 1000 | automatic | 31.09 s | 31.09 ms | 1.41x | — |

Every paired automatic trial beat its `--jobs 1` partner on both corpora, so the
material-speedup bar (median automatic >= 1.20x `--jobs 1`, all three paired
trials faster) is **met** — 1.41x on the 1,000-unit corpus, 1.84x at 64.

## Incremental path

| Corpus | Arm | No-op run | One changed source |
| --- | --- | --- | --- |
| 1000 | `--jobs 1` | 0.53 s | 0.73 s |
| 1000 | `--jobs 2` | 0.51 s | 0.54 s |
| 1000 | `--jobs 4` | 0.47 s | 0.53 s |
| 1000 | automatic | 0.49 s | 0.56 s |

Neither warm path regresses. A single changed source plans one worker, so it
takes the serial path — the same computation, with the translation-unit fact
cache and without paying for an extraction snapshot.

**Known limitation, disclosed rather than folded into the parity claim:** a
multi-worker run does not populate the optional translation-unit FactBatch
cache (2000 `tu-fact-cache` artifacts after a serial 1,000-unit rebuild, 1 after
a parallel one — that 1 being the single-file incremental run above, which
planned one worker). The cache decision lives in the serial `TuFactCacheIndexer`
wrapper that the scheduler bypasses. It is an accelerator, not a fact: the
published facts are identical either way, and the measurements above show the
parallel arm is faster than the serial arm both cold and warm despite not having
it.

## Why the speedup saturates, and where the cold time actually goes

Measured cold profile of a 1,000-unit serial index (`--profile-json`), largest
terms:

| Term | Seconds |
| --- | --- |
| `tu_fact_cache.extraction_rebuild` (whole per-TU extraction + publication) | 19.51 |
| `sqlite_vdbe` | 10.95 |
| `fact_batch_writer.virtual_machine` | 9.46 |
| `clang_tool_inclusive` | 4.69 |
| `clang_front_end` | 3.68 |
| `fact_batch_writer.prepare` | 2.35 |
| `commit` / `fact_batch_writer.commit` | 1.34 |

This is the profile the story asked to be cited, and it is not the retired
pre-v0.39 Amdahl ceiling: cold time is indexing-dominated, but **within**
indexing the controlled writer's SQLite work is a larger term than the Clang
parse. Parallel extraction shrinks only the parse side; publication is serial by
construction — one controlled writer, legacy apply order. That is why efficiency
past two workers is poor and why the speedup lands near 1.4x rather than near
the worker count. Adding workers past the point where extraction stops being the
bottleneck buys nothing.

## Scaling position: the superlinear term is NOT removed

Per-translation-unit cost against corpus size, 64 -> 1000 units (15.6x):

| Arm | 64 TU | 1000 TU | Growth | Implied exponent |
| --- | --- | --- | --- | --- |
| `--jobs 1` | 21.92 ms | 43.74 ms | 2.00x | ~N^1.25 |
| automatic | 11.92 ms | 31.09 ms | 2.61x | ~N^1.35 |

Two things follow, and neither is favourable:

1. **The residual superlinear per-unit term survives parallel extraction.**
   Serial per-unit cost still doubles over a 15.6x corpus. Parallelism
   distributes that work across cores; it does not remove the term.
2. **The parallel arm's per-unit cost grows faster than the serial arm's**, so
   the speedup decays with corpus size — 1.84x at 64 units, 1.41x at 1000.
   Extrapolating the reported speedup from a small corpus to a production one
   would overstate it.

`cidx index --jobs N` therefore does **not** close the root-cause requirement in
S-068 (HSE-103). The superlinear term must still be characterised and fixed on
its own; this story's remit is bounded parallel extraction, and it is reported
here explicitly rather than hidden behind a headline speedup.

## Scope of the corpus

These measurements are on the generated corpus this harness produces, not on the
`cpp-indexer` checkout itself: a self-index is far too slow to sit on the
critical path of this change, and the project's standing instruction keeps
re-indexing off the exit gates. The generated corpus is the one the
parity/repeatability gate already uses, it carries the shared-header fan-in the
amortisation contract is defined against, and it is where the 1,000-unit
`2 indexed, 999 already` figure in the story's acceptance criteria comes from.
