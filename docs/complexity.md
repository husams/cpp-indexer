# C++ Code & Architecture Complexity Assessment

Assessment of the `cidx` C++ indexer (`src/`). Snapshot at commit `b911463`
(2026-08-07), compared against the previous snapshot recorded in this file.

## Scale

| Metric | Previous | Current | Δ |
|---|---|---|---|
| Production C++ (`src/`) | ~91,300 LOC, 283 files | 91,301 LOC, 283 files (139 `.cpp`, 144 `.hpp`) | unchanged |
| Test C++ (`tests/`) | ~41,300 LOC | 41,319 LOC | unchanged |
| Test registrations | 37 + e2e/golden suites | 37 (15 executables + 22 `add_test`) + e2e/golden | confirmed |
| Modules under `src/` | 19 | 19 | unchanged |
| Manifest modules | (not assessed) | 21 logical modules in `architecture/cidx-module-manifest.json` | new |
| Classes/structs | ~657 | 621 named definitions (excl. forward decls) | within counting noise |
| File-size concentration | (not assessed) | median 140 LOC; 19 files ≥1k LOC hold **38.7%** of production code; top-10 files 26.5% | new |
| Language/stack | C++23, Clang LibTooling, SQLite | same | unchanged |

### LOC by module

Unchanged from the previous snapshot — every module's line count is
identical:

| Module | LOC | | Module | LOC |
|---|---|---|---|---|
| `storage/` | 21,523 | | `include_hygiene/` | 2,652 |
| `ast/` | 16,935 | | `graph/` | 2,642 |
| `query/` | 8,046 | | `astgraph/` | 1,783 |
| `cli/` | 7,936 | | `catalogs/` | 1,304 |
| `application/` | 5,655 | | `index/` | 1,252 |
| `extract/` | 4,470 | | `workspace/` | 870 |
| `ui/` | 4,207 | | `compiledb/` | 759 |
| `diff/` | 3,797 | | `toolchain/` | 741 |
| `util/` | 3,023 | | `profile/` | 725 |
| `analysis/` | 2,879 | | | |

### Largest files

Unchanged — same files, same sizes:

| LOC | File |
|---|---|
| 4,735 | `src/query/exec.cpp` |
| 3,255 | `src/ui/graph_view.cpp` |
| 2,920 | `src/storage/storage_entity_rollup.cpp` |
| 2,789 | `src/storage/fact_batch_writer.cpp` |
| 2,404 | `src/ast/index_engine.cpp` |
| 1,787 | `src/graph/query.cpp` |
| 1,727 | `src/diff/analyze.cpp` |
| 1,671 | `src/ast/fact_batch_artifact.cpp` |
| 1,478 | `src/storage/artifacts.cpp` |
| 1,471 | `src/query/plan.cpp` |

## Architecture

- **Clean, documented layering — now quantified.** The manifest
  (`architecture/cidx-module-manifest.json`) declares 21 logical modules
  across layers (model → foundation → workspace → frontend → extraction →
  persistence → analysis/query-analysis → product-surface →
  compatibility/sdk), with 11 ADRs, per-module docs (`docs/modules/`, 9
  files), and data-flow docs (`docs/data-flow.md`, `docs/data-model.md`).
- **Boundary debt is audited and time-boxed.** The manifest carries **18
  dependency exceptions**, each with an owner, expiry date, and removal
  issue. They cluster in:

  | Cluster | Exceptions | Removal issue(s) | Expires |
  |---|---|---|---|
  | `query.plan` → `product.cli` / `analysis.graph` | 5 | HSE-24, HSE-68 | 2026-12-31 |
  | `analysis.astgraph` → `product.cli` / `persistence` | 3 | HSE-62, HSE-68 | 2026-12-31 |
  | `analysis.diff` → `product.cli` | 2 | HSE-68 | 2026-12-31 |
  | `analysis.graph` → `product.cli` | 2 | HSE-24 | 2026-12-31 |
  | `extraction.ast` → `persistence.sqlite` | 2 | HSE-63 | 2026-12-31 |
  | `persistence.sqlite` → `extraction.ast` (`storage/fact_batch_writer.hpp`) | 1 | HSE-62 | 2027-08-03 |
  | other (`toolchain`, `util`, `include_hygiene`) | 3 | HSE-61, HSE-68 | 2026-12-31 |

- **Correction to the previous snapshot:** the `storage ↔ ast` bidirectional
  coupling it flagged as an unmitigated smell is in fact a **single audited
  exception** (`fact_batch_writer.hpp` → `ast/`, HSE-62, expiring
  2027-08-03); the reverse direction (`ast/` → `storage/`, 5 files) is
  covered by HSE-63. The larger live debt is the **`query.plan` cluster**
  (5 exceptions), all expiring 2026-12-31.
- **Fan-in/fan-out** (include-level): `util` fan-in 16, `storage` fan-in 15
  (expected for base layers); `cli` fan-out 12 (top-level aggregator).
  `extract/` has fan-in **0** inside `src/` — nothing includes its headers;
  it is consumed at target level by the compatibility bundle
  (`extraction.dsl` appears only in `compat.core-bundle`'s allowed
  dependencies) plus its own tests. Worth a conscious keep/inline decision.
- **Governance unchanged and strong:** self-host architecture tests, TLA
  counterexample regression tests, golden-index gates, clang-tidy gating.

## Complexity hotspots

- **Monolithic files:** unchanged list, led by `query/exec.cpp` (4,735 LOC,
  1,027 decision points — the single riskiest change site).
- **Long scopes** (balanced-brace blocks ≥100 lines) and **decision
  density** (`if/for/while/case/catch/&&/||` per 100 LOC), measured on the
  current tree:

  | File | LOC | scopes ≥100 ln | max depth | decisions | per 100 LOC |
  |---|---|---|---|---|---|
  | `query/exec.cpp` | 4,735 | 22 | 10 | 1,027 | 21.7 |
  | `ui/graph_view.cpp` | 3,255 | 7 | 9 | 558 | 17.1 |
  | `storage/storage_entity_rollup.cpp` | 2,920 | 12 | 8 | 423 | 14.5 |
  | `storage/fact_batch_writer.cpp` | 2,789 | 10 | 6 | 244 | 8.8 |
  | `ast/index_engine.cpp` | 2,404 | 5 | 8 | 232 | 9.7 |
  | `graph/query.cpp` | 1,787 | 4 | 9 | 294 | 16.5 |
  | `diff/analyze.cpp` | 1,727 | 6 | 8 | 406 | 23.5 |
  | `query/plan.cpp` | 1,471 | 7 | 8 | 306 | 20.8 |

  Whole-tree density is 12.4 decisions/100 LOC. `diff/analyze.cpp` is the
  densest large file; `cli/kind_names.cpp` (89/100) is a lookup-table
  switch and is benign.
- **Error handling is exception-based, unchanged — previous figure
  corrected:** 546 `throw` statements (554 keyword hits) versus the
  previously reported 524 (not reproducible with the current counting).
  `std::expected` uses: **0** (previous "~9" was an overcount); the only
  Expected-style handling is 3 `llvm::Expected` sites in
  `include_hygiene/executor.cpp` at the clang-format API boundary. There is
  no `Result<T>` type. The `std::expected` migration has not started.

## Positive signals

- Tech-debt markers effectively zero: **1** FIXME in `src/`
  (`ast/symbol_visitor.cpp`), 0 in `tests/`. The previous "14" count is not
  reproducible in the current tree.
- Test-to-production ratio ≈ 0.45 by LOC, with contract, migration, golden,
  parity, and architecture coverage — well above average.
- Moderate template use (30 template definitions); no over-generic code.
- No vendored C++ dependencies in the hot path (SQLite + LLVM only).

## Comparison vs previous snapshot

The production tree is **scale-identical** to the previous snapshot: same
file count, same per-module LOC, same largest files. The differences are
measurement corrections and deeper architecture evidence, not code drift:

1. Exception inventory replaces the informal "layering smell" note —
   `storage ↔ ast` is one audited exception; `query.plan` is the bigger
   cluster, and 17 of 18 exceptions expire 2026-12-31.
2. `std::expected` count corrected from ~9 to 0; `throw` count restated as
   546 statements.
3. Debt markers corrected from 14 to 1.
4. New concentration metrics: 38.7% of production code lives in 19 files
   ≥1k LOC.

## Verdict

**Architectural complexity: moderate and well-controlled — assessment
unchanged, evidence strengthened.** The dependency graph is layered,
documented, enforced, and its known violations are owned and time-boxed.
Risk remains *intra-file*: a handful of 2–5k-line files in `query`,
`storage`, `ast`, `diff`, and `ui` carry most of the semantic logic.

Recommended focus areas (updated):

1. Decompose `query/exec.cpp` first (22 long scopes, highest decision
   count), then `storage/storage_entity_rollup.cpp`,
   `storage/fact_batch_writer.cpp`, and `ast/index_engine.cpp`.
2. Burn down the `query.plan` exception cluster (HSE-24/HSE-68) and the
   `extraction.ast` → `persistence` pair (HSE-63) before their 2026-12-31
   expiry; move fact-batch records into `model.contracts` to clear HSE-62.
3. Decide on the error-handling direction: either start the
   `std::expected` migration at the `storage` boundary or retire the
   recommendation — it is currently unstarted with zero footprint.
4. Watch `ui/graph_view.cpp` (3,255 LOC, depth 9) before it reaches
   `exec.cpp` territory.
