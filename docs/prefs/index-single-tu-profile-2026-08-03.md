# Single-TU indexing profile — 2026-08-03

CPU profile of `cidx index` on **one** translation unit, taken to find the
function and call path that dominate indexing time.

## 1. Setup

| Item | Value |
| --- | --- |
| Binary | `cidx`, built fresh at `CMAKE_BUILD_TYPE=RelWithDebInfo` (`-O2 -g`) |
| Build dir | out-of-tree scratch build (not `build/`, which is `Debug`) |
| Compiler / LLVM | `/usr/bin/clang++`; LLVM+Clang `22.1.8` (`/opt/homebrew/opt/llvm`) |
| CMake options | `-DCIDX_BUILD_EXAMPLES=OFF -DCIDX_ASTGRAPH_SOUFFLE=OFF` |
| Host | macOS 26.1 (25B78), arm64 |
| Sampling profiler | `/usr/bin/sample <pid> 60 1 -mayDie` (1 ms interval, whole process lifetime) |
| Built-in telemetry | `cidx index --profile-json` |
| Repo state | `main` @ `00d7709` |

Repro:

```bash
cmake -S . -B /tmp/build-prof -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCIDX_LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config \
  -DCIDX_BUILD_EXAMPLES=OFF -DCIDX_ASTGRAPH_SOUFFLE=OFF
ninja -C /tmp/build-prof cidx

export INDEXER_CACHE=/tmp/profrun            # fresh, empty index.db lives here
/tmp/build-prof/cidx import --db "$(pwd)/build" --name cpp-indexer

/tmp/build-prof/cidx index src/query/exec.cpp --profile-json /tmp/profile.json &
sample $! 60 1 -mayDie -f /tmp/sample.txt
```

Caveat: the index database starts **empty** (cold), so every symbol/type is
minted rather than matched. An unrelated `cidx ui open` process from another
session was pinned at 100 % CPU on a different core during the run; the indexer
is single-threaded and the host has 10 cores, so contention is limited, but
timings carry that noise. Percentages and ratios are the trustworthy figures —
which is why the root-cause evidence in §4 is stated in VDBE instructions
rather than seconds.

## 2. Object profiled

Exactly one file: **`src/query/exec.cpp`** (largest TU in the tree).

| Property | Value |
| --- | --- |
| Source bytes | 187,118 |
| Preprocessed bytes | 71,966,708 (~68.6 MiB) |
| `#include` directives seen | 6,039 |
| Headers newly indexed | 19 (+798 system, 0 already indexed) |
| Symbols emitted | 194 (main file) + 1,984 (headers) |
| Wall time | **15.98 s** |
| In-process CPU | 15.19 s |
| Driver subprocess wall | 0.020 s (1 subprocess) |
| Peak RSS | 458,014,720 B (437 MiB) |

Three timing observations were taken; all numbers in this report come from the
**sampled** run, which is the one the call graph belongs to:

| Run | Instrumentation | Wall |
| --- | --- | ---: |
| warm-up | `time` only | 15.19 s |
| unsampled | `--profile-json` only, taken while `sample` was (mistakenly) profiling an unrelated process that was pinned at 100 % CPU | 19.09 s |
| **sampled — used throughout** | `--profile-json` + `sample <pid>` | **15.98 s** |

Facts attempted → persisted for this single TU:

| Family | Attempted | Persisted | Duplicates |
| --- | ---: | ---: | ---: |
| types | 88,058 | 88,058 | 0 |
| relations | 27,119 | 27,119 | 0 |
| file_associations | 18,265 | 18,265 | 0 |
| include_facts | 17,546 | 314 | 17,232 |
| symbols | 8,679 | 8,679 | 0 |
| definitions | 183 | 183 | 0 |

SQLite work for that one TU: **447,203 `step` calls, 762,967,701 VDBE
instructions, 659,939 fullscan steps**, 411,822 prepare calls (85.8 MB of SQL
text), 1 transaction.

## 3. Where the time goes

### 3.1 Phase timings (`--profile-json`, seconds)

| Span | Seconds | % of wall |
| --- | ---: | ---: |
| `clang_tool_inclusive` | 14.46 | 90 % |
| **`sqlite_vdbe`** | **13.85** | **87 %** |
| `body_extraction` (= `pass.statements.main`) | 8.49 | 53 % |
| `pass.namespaces.headers` | 2.27 | 14 % |
| `commit` | 1.48 | 9 % |
| `pass.declarations.headers` | 1.67 | 10 % |
| `clang_front_end` (parse/sema only) | 1.45 | 9 % |
| `pass.symbols.headers` | 0.52 | 3 % |
| `sqlite_prepare` | 0.065 | 0.4 % |
| everything else (`fact_persistence`, `applicability_association`, `include_persistence`, `verification`, …) | < 0.08 each | — |

Reading: the Clang front end itself is **9 %** of the run. Storage work inside
the AST passes is **87 %**. (`sqlite_prepare` is near-zero because
`SqliteDb::prepare` pools compiled statements — the 411,822 "prepare calls" are
almost all cache hits. The cost is in stepping, not compiling.)

### 3.2 Self (leaf) time by binary image — 10,798 leaf samples

| Image | Samples | % | ≈ seconds |
| --- | ---: | ---: | ---: |
| `libsqlite3.dylib` | 8,867 | 82.1 % | 13.12 |
| `libsystem_platform.dylib` (memmove/memcmp/memset, ~all called from SQLite) | 1,234 | 11.4 % | 1.83 |
| `libsystem_kernel.dylib` | 407 | 3.8 % | 0.60 |
| `libsystem_malloc.dylib` | 205 | 1.9 % | 0.30 |
| **`cidx`** | **26** | **0.2 %** | **0.04** |
| `libclang-cpp.dylib` | 19 | 0.2 % | 0.03 |
| `libLLVM.dylib` | 7 | 0.1 % | 0.01 |

Top individual leaf functions: `sqlite3VdbeExec` 5,325 (47.6 %),
`_platform_memmove` 562, `sqlite3VdbeRecordCompareWithSkip` 528, `getCellInfo`
409, `sqlite3BtreeNext` 378, `_platform_memcmp` 352, `sqlite3BtreeTableMoveto`
313, `btreeParseCellPtr` 243, `sqlite3VdbeSerialGet` 182.

`sqlite3BtreeNext` + `RecordCompareWithSkip` + `getCellInfo` +
`btreeParseCellPtr` being that high is the signature of **row-by-row index
scanning**, not point lookups.

### 3.3 Inclusive time per frame (main thread, 11,198 samples)

| Samples | % | ≈ s | Frame | Location |
| ---: | ---: | ---: | --- | --- |
| 11,173 | 99.8 % | 15.94 | `cidx::ast::run_index_one` | `index_engine.cpp:2051` |
| 10,705 | 95.6 % | 15.28 | `cidx::SqliteStmt::step` | `sqlite.cpp:172` |
| 9,884 | 88.3 % | 14.10 | `IndexASTConsumer::HandleTranslationUnit` | `index_engine.cpp:1201` |
| 9,883 | 88.3 % | 14.10 | `ExtractionPassRegistry::run` | `pass_registry.cpp:586` |
| 7,248 | 64.7 % | 10.34 | `FunctionDefinitionVisitor::run_statement_pass` | `function_definition_visitor.cpp:67` |
| 7,241 | 64.7 % | 10.33 | `StatementEdgeVisitor::walk` | `statement_edge_visitor.cpp:101` |
| **6,988** | **62.4 %** | **9.97** | **`StorageEdgeSink::lookup_symbol_id`** | `storage_edge_sink.cpp:83` |
| **6,978** | **62.3 %** | **9.96** | **`SqliteStorageService::lookup_symbol`** | `storage_symbols.cpp:302-333` |
| 5,315 | 47.5 % | 7.58 | `CallEdgeEmitter::emit_resolved_call` | `call_edge_emitter.cpp:200` |
| 5,129 | 45.8 % | 7.32 | `CallEdgeEmitter::mint_resolved_target` | `call_edge_emitter.cpp:139` |
| 4,097 | 36.6 % | 5.85 | `StatementEdgeVisitor::emit_call` | `statement_edge_visitor.cpp:168` |
| 2,148 | 19.2 % | 3.07 | `emit_owner_promotion` | `instantiation_edges.cpp:126` |
| 1,944 | 17.4 % | 2.77 | `RoutedRootEventBuffer::replay_namespaces` | `routed_root_events.cpp:404` |
| 1,774 | 15.8 % | 2.53 | `emit_callable_template_identity` | `instantiation_edges.cpp:182` |
| 1,705 | 15.2 % | 2.43 | `StatementEdgeVisitor::VisitDeclRefExpr` | `statement_edge_visitor.cpp:421` |
| 1,340 | 12.0 % | 1.91 | `TemplateArgumentEncoder::encode` | `template_argument_encoder.cpp:49` |
| 1,263 | 11.3 % | 1.80 | `Transaction::commit` | `storage.cpp:95` |
| 1,260 | 11.3 % | 1.80 | `DeclarationEdgeVisitor::emit_signature_types` | `declaration_edge_visitor.cpp:416` |
| 1,258 | 11.2 % | 1.80 | `reconcile_pending_symbol_identities` | `storage.cpp:664` |
| 1,245 | 11.1 % | 1.78 | `TypeInterner::intern` | `type_graph.cpp:73` |
| 1,129 | 10.1 % | 1.61 | `NamespaceUseVisitor::emit_ns_use` | `namespace_use_visitor.cpp:138` |
| 1,099 | 9.8 % | 1.57 | `SqliteStorageService::intern_type_node` | `storage_types.cpp:74` |
| 813 | 7.3 % | 1.16 | `reconcile_type_identity` | `storage.cpp:792-834` |

### 3.4 Hottest path

```
main                                              main.cpp:66
 └ cidx::cli::run_application_request              application_adapter.cpp:708
   └ ApplicationService::execute                   services.cpp:799
     └ StorageApplicationOperations::execute       services.cpp:270
       └ cidx::ast::run_index_one                  index_engine.cpp:2051      99.8%
         └ clang::tooling::ClangTool::run
           └ clang::ParseAST
             └ IndexASTConsumer::HandleTranslationUnit   index_engine.cpp:1201  88.3%
               └ TranslationUnitIndexer::run              index_engine.cpp:243
                 └ ExtractionPassRegistry::run            pass_registry.cpp:586
                   └ FunctionDefinitionVisitor::run_statement_pass
                                                          function_definition_visitor.cpp:67  64.7%
                     └ StatementEdgeVisitor::walk         statement_edge_visitor.cpp:101
                       └ RecursiveASTVisitor::TraverseStmt   (deep TraverseIfStmt recursion)
                         └ StatementEdgeVisitor::emit_call   statement_edge_visitor.cpp:168  36.6%
                           └ CallEdgeEmitter::emit_resolved_call   call_edge_emitter.cpp:200  47.5%
                             └ CallEdgeEmitter::mint_resolved_target call_edge_emitter.cpp:139  45.8%
                               └ BudgetedStatementFactPorts::lookup_symbol_id  pass_registry.cpp:155
                                 └ StorageEdgeSink::lookup_symbol_id  storage_edge_sink.cpp:83   62.4%
                                   └ SqliteStorageService::lookup_symbol storage_symbols.cpp:302
                                     └ cidx::SqliteStmt::step        sqlite.cpp:172
                                       └ sqlite3_step → sqlite3VdbeExec   47.6% SELF
```

**Single hottest method: `cidx::SqliteStorageService::lookup_symbol`
(`src/storage/storage_symbols.cpp:302`), 62.3 % inclusive (~9.96 s of 15.98 s),
reached from the statement pass via `CallEdgeEmitter::mint_resolved_target`.**

## 4. Root cause

`lookup_symbol` runs (twice per call — "local" identity, then "external"):

```sql
SELECT <cols> FROM symbol WHERE semantic_universe_id = ? AND identity_key = ?
```
`src/storage/storage_symbols.cpp:311-314`

The matching index is **partial**:

```sql
CREATE UNIQUE INDEX idx_symbol_identity
  ON symbol(semantic_universe_id, identity_key) WHERE identity_key <> '';
```

SQLite may only use a partial index when the query's `WHERE` clause provably
implies the index predicate. The query never says `identity_key <> ''`, so the
planner rejects it and falls back to `idx_symbol_scope(semantic_universe_id)` —
which matches **every** row in the universe:

```
sqlite> EXPLAIN QUERY PLAN
        SELECT id FROM symbol WHERE semantic_universe_id = 1 AND identity_key = 'x';
SEARCH symbol USING INDEX idx_symbol_scope (semantic_universe_id=?)

sqlite> EXPLAIN QUERY PLAN
        SELECT id FROM symbol WHERE semantic_universe_id = 1
          AND identity_key = 'x' AND identity_key <> '';
SEARCH symbol USING INDEX idx_symbol_identity (semantic_universe_id=? AND identity_key=?)
```

All 2,729 rows in this database sit in one `semantic_universe_id`, so each
lookup walks the whole symbol table and string-compares `identity_key` per row.
That explains the 762 M VDBE instructions, the 659,939 fullscan steps, and the
`sqlite3BtreeNext` / `RecordCompareWithSkip` leaf profile.

Worse, `lookup_symbol` probes **two** keys — the TU-local identity first, then
the external one. For any symbol declared outside the current TU the local
probe cannot match, so it always pays a *full* scan before the second probe
even starts.

### Cost as a function of table size

Measured as **VDBE instructions per lookup** (`sqlite3_progress_handler`, so it
is exact and independent of host load), with `LIMIT 1` to mirror the C++ path,
which calls `SqliteStmt::step()` exactly once:

| Symbol rows `N` | as shipped, hit | as shipped, miss | `+ identity_key <> ''`, hit | miss |
| ---: | ---: | ---: | ---: | ---: |
| 2,839 (this run's DB, 2 TUs) | 8,087 | 14,206 | **19.1** | **16.0** |
| 15,015 (repo `index.db`) | 40,844 | 75,086 | **19.1** | **16.0** |

A miss costs exactly `5.00 × N` VDBE instructions (14,206/2,839 = 5.004;
75,086/15,015 = 5.001); a hit averages `≈2.8 × N`. With the predicate the cost
is **constant** — 16–19 instructions regardless of `N`. **As shipped the lookup
is O(N) in stored symbols, so indexing a repository is quadratic in symbol
count; with the predicate it is O(log N).**

Wall-clock confirms the same shape (absolute numbers move with host load, so
the ratio is the meaningful figure; full data in `raw/sql-evidence.txt`):

| Symbol rows | as shipped | `+ identity_key <> ''` | Ratio |
| ---: | ---: | ---: | ---: |
| 2,729 (light load) | 232.8 µs | **5.8 µs** | 40× |
| 2,839 (heavy load) | 1,335.6 µs | **16.6 µs** | 80× |
| 15,015 (heavy load) | 11,314.0 µs | **13.4 µs** | 844× |

## 5. Secondary findings

1. **Misses are never cached.** `StorageEdgeSink::lookup_symbol_id`
   (`storage_edge_sink.cpp:66-92`) memoizes hits but deliberately not misses
   ("a later `mint_symbol()` in this TU may create the symbol"). In a cold
   database most probes miss — and a miss is the *most* expensive case
   (full scan, no early exit).
2. **No type-intern memoization across call sites.** 88,058 `types` facts were
   attempted with **0 duplicates suppressed**, yet the run produced only 1,773
   distinct `type_node` rows. `CallEdgeEmitter::mint_resolved_target`
   (`call_edge_emitter.cpp:148-150`) constructs a **fresh `DeclarationEdgeVisitor`
   — and therefore a fresh `TypeInterner` with an empty memo
   (`declaration_edge_visitor.hpp:163`) — at every call site, then re-emits the
   callee's whole signature type graph. Inclusive: `emit_signature_types` 1.80 s,
   `intern_type_node` 1.57 s.
3. **`reconcile_type_identity` fan-out** (`storage.cpp:792-834`): 5 UPDATE
   statements per interned type that carries a `decl_usr`, 1.16 s inclusive.
   All five plans are index-driven — the cost is call volume, not a missing
   index, so it follows finding 2.
4. **Two statements where one would do.** `mint_symbol_id`
   (`storage_symbols.cpp:695-702`) and `intern_type_node`
   (`storage_types.cpp:67-71`) each follow their upsert with a separate
   `SELECT id`, while `add_edge` (`storage_symbols.cpp:705-720`) already uses
   `RETURNING id` on the same SQLite build (3.51.0).
5. **Commit-time reconciliation**: `Transaction::commit` 1.80 s, essentially
   all of it `reconcile_pending_symbol_identities` (`storage.cpp:664`).
6. **Header passes do not amortize.** `pass.namespaces.headers` 2.27 s +
   `pass.declarations.headers` 1.67 s + `pass.symbols.headers` 0.52 s = 28 % of
   this run. Indexing a second TU (`src/query/plan.cpp`) into the same database
   — `new_headers=0`, 10 headers *already indexed* — still spent 1.65 s + 0.65 s
   + 0.39 s = 2.69 s in those passes.
7. **`include_facts` is the only family with duplicate suppression working**
   (17,546 attempted → 314 persisted).
8. The Clang front end (`clang_front_end` 1.45 s) is *not* the bottleneck; AST
   traversal plus storage is. Front-end reuse would recover at most 9 %.

## 6. Suggested order of attack

Full write-up with proposed changes, risks and validation:
[`docs/improvements/indexing-performance.md`](../improvements/indexing-performance.md).

1. Add `AND identity_key <> ''` to the two `lookup_symbol` identity queries
   (`storage_symbols.cpp:311`) — or drop the partial index's `WHERE` clause.
   One-line change; turns an O(N) scan into an O(log N) seek on the single
   hottest path. Highest value by a wide margin.
2. Cache negative lookups in `StorageEdgeSink`, invalidated on `mint_symbol`.
3. Memoize per-callee work in `CallEdgeEmitter::mint_resolved_target` so the
   signature type graph is emitted once per callee per TU, not once per call
   site.
4. Use `RETURNING id` in `mint_symbol_id` / `intern_type_node`.

## 7. Artifacts

All raw data for this run is in [`raw/`](raw/); see [`README.md`](README.md)
for the experiment design, method, and threats to validity.

| File | What it is |
| --- | --- |
| `raw/sample-callgraph.txt.gz` | Verbatim `/usr/bin/sample` output (8.8 MB uncompressed) |
| `raw/profile-json.json` | Verbatim `cidx index --profile-json` output |
| `raw/hotspots-derived.txt` | Full self / inclusive rankings derived from the call graph |
| `raw/sql-evidence.txt` | Index definitions, `EXPLAIN QUERY PLAN`, micro-benchmark runs |
| `raw/analyze_sample.py` | Call-graph parser used for the derived tables |
| `raw/environment.txt` | Host, toolchain, repo commit, compile command for the TU |
| `raw/index-stdout.log`, `raw/cmake-configure.log`, `raw/ninja-build.log` | Run and build logs |
