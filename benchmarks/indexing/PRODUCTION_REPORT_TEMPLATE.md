# HSE-103 production indexing report

Raw corpora, databases, logs, and profiler traces:
`<absolute path outside checkout; .noindex suffix on macOS>`

## Verdict

- Measurement host quiescent: `<yes/no; list competing processes and load average>`
- Superlinear cold term: `<reproduced / isolated / characterised / conservatively bounded / unresolved>`
- Quantitative attribution: `<dominant term, estimate, uncertainty, evidence>`
- Final cold SLO approval: `<blocked/eligible>`
  Approval remains blocked while the dominant scaling term is neither
  characterised nor conservatively bounded with quantitative attribution.
- Production SQLite profile change recommended: **no**
  HSE-103 measures disposable alternatives; adoption belongs in a separate
  reviewed change with integrity, interruption, and recovery evidence.

## Exact identities

| Identity | Value |
| --- | --- |
| Command | `<production.py command>` |
| Executable path / SHA-256 / version | `<value>` |
| Git checkout / commit / dirty state | `<value>` |
| Schema / catalog version / catalog hash | `<value>` |
| Self-index compile database path / SHA-256 | `<value>` |
| Synthetic compile database identities | `<value>` |
| Host / CPU / OS / Python | `<value>` |
| SQLite version / compile options | `<value>` |
| Clang version / resource directory | `<value>` |
| Trial count | `3 or more` |

## Corpus contract

| Shape | TUs | Target distinct newly indexed owned headers (`D`) | Include/fan-in design |
| --- | ---: | ---: | --- |
| HSE-95 baseline | 32 | 2 | `shared.hpp` in all TUs; `coverage.hpp` in TU 0 |
| HSE-95 baseline | 1,000 | 2 | same controlled shape |
| Header-heavy | 32 | 18 | 16 additional headers included by every TU |
| Many owned headers | 32 | 128 | owned headers distributed across TUs |
| Fan-in controlled | 32 | 4 | one header in two TUs; one header in every TU |
| `cpp-indexer` self-index | `<count>` | `<observed>` | recorded checkout and compile database |

## Scenario medians and dispersion

Retain all individual trial values beside each median. The disabled-profiling
comparison is accepted only when the three-trial cold 1,000-TU median is within
1% of an otherwise equivalent build without instrumentation, the spread is
reported, and canonical semantic digests plus execution order are identical.

| Corpus | Scenario | Wall median (individuals) | In-process CPU | Child/driver CPU or wall | Peak RSS | Canonical digest |
| --- | --- | --- | --- | --- | --- | --- |
| `<shape>` | cold | `<median (t1,t2,t3)>` | `<value>` | `<value>` | `<value>` | `<value>` |
| `<shape>` | unchanged warm | `<value>` | `<value>` | `<value>` | `<value>` | `<value>` |
| `<shape>` | one source | `<value>` | `<value>` | `<value>` | `<value>` | `<value>` |
| fan-in | low-fan-in header | `<value>` | `<value>` | `<value>` | `<value>` | `<value>` |
| fan-in | high-fan-in header | `<value>` | `<value>` | `<value>` | `<value>` | `<value>` |
| `<shape>` | configuration change | `<value>` | `<value>` | `<value>` | `<value>` | `<value>` |
| `<shape>` | generated input | `<value>` | `<value>` | `<value>` | `<value>` | `<value>` |

## Per-TU position series

Include the complete 32- and 1,000-TU cold series. Each row must contain start
position, database and fact cardinalities before the TU, source and
preprocessed-size proxies, include count, new/already indexed headers,
configuration state, wall time, in-process CPU, child/driver work, and peak
RSS.

| Position | TU | DB pages/bytes before | Facts before | Source/preprocessed bytes | Includes | New/already headers | Config | Wall / CPU / RSS |
| ---: | --- | --- | --- | --- | ---: | --- | --- | --- |
| `<n>` | `<path>` | `<value>` | `<value>` | `<value>` | `<n>` | `<n>/<n>` | `<hash/state>` | `<value>` |

## Scaling models and controlled experiments

For constant, linear, and quadratic-superlinear models report coefficients,
coefficient standard errors, residuals, RMSE, R², and AIC. Compare forward and
reverse processing orders and controlled corpus shapes. State which conclusion
is measured and which is inferred.

| Series | Preferred model | Coefficients ± uncertainty | RMSE / R² / AIC | Interpretation |
| --- | --- | --- | --- | --- |
| 32-TU forward | `<value>` | `<value>` | `<value>` | `<value>` |
| 1,000-TU forward | `<value>` | `<value>` | `<value>` | `<value>` |
| 1,000-TU reverse | `<value>` | `<value>` | `<value>` | `<value>` |

## End-to-end attribution

| Category | Wall / CPU | Counts | Notes |
| --- | ---: | ---: | --- |
| import | `<value>` | `<value>` | |
| source validation / hashing | `<value>` | `<value>` | |
| workspace snapshot / configuration | `<value>` | `<value>` | |
| driver subprocesses | `<value>` | `<value>` | report separately from in-process CPU |
| Clang front end | `<exclusive value>` | `<value>` | excludes registered pass timings; retain `clang_tool_inclusive` separately |
| root AST pass: symbols | `<value>` | `<TraverseDecl calls>` | |
| root AST pass: definitions | `<value>` | `<TraverseDecl calls>` | |
| root AST pass: declarations/edges | `<value>` | `<TraverseDecl calls>` | |
| root AST pass: namespaces | `<value>` | `<TraverseDecl calls>` | |
| body extraction | `<value>` | `<value>` | |
| fact persistence | `<value>` | `<attempted/persisted/duplicate by family>` | |
| include persistence | `<value>` | `<include path-resolution queries>` | |
| applicability association | `<value>` | `attempted/inserted/ignored/deleted/temporary_rows=<value>` | |
| include persistence | `<value>` | `attempted/inserted_or_updated/ignored/deleted/cascade_deleted/path_resolution_queries=<value>` | |
| external identity reconciliation | `<value>` | `<calls/rows matched/changed>` | HSE-114 is the hot-path baseline consumer |
| SQLite prepare | `<value>` | `<prepare/reprepare>` | |
| SQLite VDBE execution | `<value>` | `<step/VM/full-scan>` | |
| transactions / commits | `<value>` | `<begin/commit>` | |
| transforms | `<value>` | `<value>` | |
| verification | `<value>` | `<value>` | |
| toolchain/cache | `<value>` | `<hits/misses>` | |

Fact-family counts in profile JSON are attributed to their producing pass.
They are incremented at typed family emit/persist operations; multi-family pass
totals are never copied onto every declared output family. Include path
resolution is counted at each preprocessor resolution callback.

## Targeted attribution experiments

Quantitatively cover identity reconciliation, applicability maintenance,
include persistence, workspace/file validation, and database-growth-sensitive
lookup/write work. Record the intervention, controlled variables, estimate,
uncertainty, and semantic parity for each.

## SQLite indexing-profile matrix

The shipped profile is the control.

| Profile | Throughput | Peak RSS | Page writes / commits | Integrity | Interruption / recovery | Decision |
| --- | ---: | ---: | ---: | --- | --- | --- |
| shipped control | `<value>` | `<value>` | `<value>` | `<value>` | `<value>` | control |
| cache size | `<value>` | `<value>` | `<value>` | `<value>` | `<value>` | measure only |
| mmap size | `<value>` | `<value>` | `<value>` | `<value>` | `<value>` | measure only |
| temp store | `<value>` | `<value>` | `<value>` | `<value>` | `<value>` | measure only |
| journal mode | `<value>` | `<value>` | `<value>` | `<value>` | `<value>` | measure only |
| synchronous | `<value>` | `<value>` | `<value>` | `<value>` | `<value>` | measure only |

## External profiler evidence

- macOS `xctrace`: `<raw path and summary>`
- Linux `perf`: `<raw path and summary or not run>`
- Linux `strace`: `<raw path and summary or not run>`
- SQLite trace: `<raw path and summary or not run; exclude from authoritative timing>`

## Baseline attachment checklist for HSE-102

- [ ] `cpp-indexer` self-index baseline attached
- [ ] 1,000-TU production harness baseline attached
- [ ] HSE-114 external-identity hot-path baseline attached
- [ ] Raw artifacts remain outside the checkout
