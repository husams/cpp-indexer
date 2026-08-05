# S-074 serial/parallel qualification

Produced by `benchmarks/indexing/parallel_matrix.py`, which extends the existing
`benchmarks/indexing/run.py` corpus generator and the `scripts/dump_layer0.sh`
projection rather than introducing a second harness.

Host: Apple Silicon (Darwin 25.1.0), 3 paired trials per arm, `index rebuild`
over a generated corpus with two shared headers.

```
python3 benchmarks/indexing/parallel_matrix.py --cidx build/cidx \
    --count 1000 --trials 3 --output parallel-1000.json
```

## Canonical parity

Every arm's normalized, ordered Layer-0 projection is **identical** to
`--jobs 1`, at 24, 64 and 1000 translation units:

| Corpus | `--jobs 1` | `--jobs 2` | `--jobs 4` | automatic |
| --- | --- | --- | --- | --- |
| 24 | baseline | identical | identical | identical |
| 64 | baseline | identical | identical | identical |
| 1000 | baseline | identical | identical | identical |

The 24-unit comparison also ran against the pre-change binary on `main`
(`24fb651`): 1624 canonical rows, identical. That figure includes the 46 rows —
every `uses` edge from a unit into the header-owned `shared` namespace — that
are lost if cross-translation-unit identity is resolved from a pre-run snapshot
instead of at publication.

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
| 64 | `--jobs 1` | 1.407 s | 21.99 ms | 1.00x | — |
| 64 | `--jobs 2` | 0.785 s | 12.26 ms | 1.79x | 0.90 |
| 64 | `--jobs 4` | 0.792 s | 12.37 ms | 1.78x | 0.44 |
| 64 | automatic | 0.786 s | 12.28 ms | 1.79x | — |
| 1000 | `--jobs 1` | 39.677 s | 39.68 ms | 1.00x | — |
| 1000 | `--jobs 2` | 28.893 s | 28.89 ms | 1.37x | 0.69 |
| 1000 | `--jobs 4` | 28.434 s | 28.43 ms | 1.40x | 0.35 |
| 1000 | automatic | 28.966 s | 28.97 ms | 1.37x | — |

Every paired automatic trial beat its `--jobs 1` partner on both corpora, so the
material-speedup bar (median automatic >= 1.20x `--jobs 1`, all three paired
trials faster) is **met** at 1.37x on the 1,000-unit corpus and 1.79x at 64.

Efficiency past two workers is poor and gets worse with scale. Publication is
serial by construction — one controlled writer, legacy apply order — so the
writer is the floor, and adding workers past the point where extraction stops
being the bottleneck buys nothing.

## Scaling position: the superlinear term is NOT removed

Per-translation-unit cost against corpus size, 64 -> 1000 units (15.6x):

| Arm | 64 TU | 1000 TU | Growth | Implied exponent |
| --- | --- | --- | --- | --- |
| `--jobs 1` | 21.99 ms | 39.68 ms | 1.80x | ~N^1.21 |
| automatic | 12.28 ms | 28.97 ms | 2.36x | ~N^1.31 |

Two things follow, and neither is favourable:

1. **The residual superlinear per-unit term survives parallel extraction.**
   Serial per-unit cost still grows ~1.8x over a 15.6x corpus. Parallelism
   distributes that work across cores; it does not remove the term.
2. **The parallel arm's per-unit cost grows FASTER than the serial arm's**, so
   the speedup decays with corpus size — 1.79x at 64 units, 1.37x at 1000.
   Extrapolating the reported speedup from a small corpus to a production one
   would overstate it.

`cidx index --jobs N` therefore does **not** close the root-cause requirement in
S-068 (HSE-103). The superlinear term must still be characterised and fixed on
its own; this story's remit is bounded parallel extraction, and it is reported
here explicitly rather than hidden behind a headline speedup.
