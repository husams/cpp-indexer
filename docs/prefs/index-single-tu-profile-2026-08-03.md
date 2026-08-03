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
is single-threaded and the host has spare cores, so contention is negligible,
but timings carry that noise.

## 2. Object profiled

Exactly one file: **`src/query/exec.cpp`** (largest TU in the tree).

| Property | Value |
| --- | --- |
| Source bytes | 187,118 |
| Preprocessed bytes | 71,966,708 (~68.6 MiB) |
| `#include` directives seen | 6,039 |
| Headers newly indexed | 19 (+798 system, 0 already indexed) |
| Symbols emitted | 194 (main file) + 1,984 (headers) |
| Wall time | **19.09 s** |
| In-process CPU | 18.59 s |
| Driver subprocess wall | 0.026 s (1 subprocess) |
| Peak RSS | 458,997,760 B (438 MiB) |

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
| `clang_tool_inclusive` | 17.60 | 92 % |
| **`sqlite_vdbe`** | **16.06** | **84 %** |
| `body_extraction` (= `pass.statements.main`) | 9.70 | 51 % |
| `pass.namespaces.headers` | 2.90 | 15 % |
| `clang_front_end` (parse/sema only) | 2.23 | 12 % |
| `pass.declarations.headers` | 2.01 | 11 % |
| `commit` | 1.45 | 8 % |
| `pass.symbols.headers` | 0.69 | 4 % |
| `sqlite_prepare` | 0.075 | 0.4 % |
| everything else (`fact_persistence`, `applicability_association`, `include_persistence`, `verification`, …) | < 0.06 each | — |

Reading: the Clang front end itself is **12 %** of the run. Storage I/O inside
the AST passes is **84 %**.

### 3.2 Self (leaf) time by binary image — 10,798 leaf samples

| Image | Samples | % | ≈ seconds |
| --- | ---: | ---: | ---: |
| `libsqlite3.dylib` | 8,867 | 82.1 % | 15.7 |
| `libsystem_platform.dylib` (memmove/memcmp/memset, ~all called from SQLite) | 1,234 | 11.4 % | 2.2 |
| `libsystem_kernel.dylib` | 407 | 3.8 % | 0.7 |
| `libsystem_malloc.dylib` | 205 | 1.9 % | 0.4 |
| **`cidx`** | **26** | **0.2 %** | **0.05** |
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
| 11,173 | 99.8 % | 19.05 | `cidx::ast::run_index_one` | `index_engine.cpp:2051` |
| 10,705 | 95.6 % | 18.25 | `cidx::SqliteStmt::step` | `sqlite.cpp:172` |
| 9,884 | 88.3 % | 16.85 | `IndexASTConsumer::HandleTranslationUnit` | `index_engine.cpp:1201` |
| 9,883 | 88.3 % | 16.85 | `ExtractionPassRegistry::run` | `pass_registry.cpp:586` |
| 7,248 | 64.7 % | 12.36 | `FunctionDefinitionVisitor::run_statement_pass` | `function_definition_visitor.cpp:67` |
| 7,241 | 64.7 % | 12.34 | `StatementEdgeVisitor::walk` | `statement_edge_visitor.cpp:101` |
| **6,988** | **62.4 %** | **11.91** | **`StorageEdgeSink::lookup_symbol_id`** | `storage_edge_sink.cpp:83` |
| **6,978** | **62.3 %** | **11.90** | **`SqliteStorageService::lookup_symbol`** | `storage_symbols.cpp:302-333` |
| 5,315 | 47.5 % | 9.06 | `CallEdgeEmitter::emit_resolved_call` | `call_edge_emitter.cpp:200` |
| 5,129 | 45.8 % | 8.74 | `CallEdgeEmitter::mint_resolved_target` | `call_edge_emitter.cpp:139` |
| 4,097 | 36.6 % | 6.98 | `StatementEdgeVisitor::emit_call` | `statement_edge_visitor.cpp:168` |
| 2,148 | 19.2 % | 3.66 | `emit_owner_promotion` | `instantiation_edges.cpp:126` |
| 1,944 | 17.4 % | 3.31 | `RoutedRootEventBuffer::replay_namespaces` | `routed_root_events.cpp:404` |
| 1,774 | 15.8 % | 3.02 | `emit_callable_template_identity` | `instantiation_edges.cpp:182` |
| 1,705 | 15.2 % | 2.91 | `StatementEdgeVisitor::VisitDeclRefExpr` | `statement_edge_visitor.cpp:421` |
| 1,340 | 12.0 % | 2.28 | `TemplateArgumentEncoder::encode` | `template_argument_encoder.cpp:49` |
| 1,263 | 11.3 % | 2.15 | `Transaction::commit` | `storage.cpp:95` |
| 1,260 | 11.3 % | 2.15 | `DeclarationEdgeVisitor::emit_signature_types` | `declaration_edge_visitor.cpp:416` |
| 1,258 | 11.2 % | 2.14 | `reconcile_pending_symbol_identities` | `storage.cpp:664` |
| 1,245 | 11.1 % | 2.12 | `TypeInterner::intern` | `type_graph.cpp:73` |
| 1,129 | 10.1 % | 1.92 | `NamespaceUseVisitor::emit_ns_use` | `namespace_use_visitor.cpp:138` |
| 1,099 | 9.8 % | 1.87 | `SqliteStorageService::intern_type_node` | `storage_types.cpp:74` |
| 813 | 7.3 % | 1.39 | `reconcile_type_identity` | `storage.cpp:806-834` |

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
(`src/storage/storage_symbols.cpp:302`), 62.3 % inclusive (~11.9 s of 19.1 s),
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
`sqlite3BtreeNext` / `RecordCompareWithSkip` leaf profile. **The cost grows
linearly with the number of symbols already stored, making whole-repo indexing
quadratic in symbol count.**

Measured on the produced database, same connection (full data in
`index-single-tu-sql-evidence-2026-08-03.txt`):

| Run | Query as shipped | `+ AND identity_key <> ''` | Ratio |
| --- | ---: | ---: | ---: |
| 20,000 lookups, light host load | 232.8 µs/lookup | **5.8 µs/lookup** | 40.1× |
| 5 × 5,000 lookups, heavy host load (median) | 521.7 µs/lookup | **9.2 µs/lookup** | 56.5× |

Absolute numbers move with host load; the ratio sits in the **40–57×** band at
only 2,729 rows, and the gap widens with table size.

## 5. Secondary findings

1. **No type-intern memoization.** 88,058 `types` facts were attempted with
   **0 duplicates suppressed**, yet the run produced only 1,773 distinct
   `type_node` rows — ~98 % of `intern_type_node` calls re-do an upsert plus a
   `SELECT id` (`storage_types.cpp:46-72`) for a key already interned in this
   TU. Inclusive cost 1.87 s.
2. **`reconcile_type_identity` fan-out** (`storage.cpp:800-834`): 5 UPDATE
   statements per interned type that carries a `decl_usr`, 1.39 s inclusive.
   All five plans are index-driven — the cost is call volume, not a missing
   index, so it follows finding 1.
3. **Commit-time reconciliation**: `Transaction::commit` 2.15 s, essentially
   all of it `reconcile_pending_symbol_identities` (`storage.cpp:664`).
4. **Header passes re-run per TU**: `pass.namespaces.headers` 2.90 s +
   `pass.declarations.headers` 2.01 s + `pass.symbols.headers` 0.69 s — 30 % of
   the run spent on 19 headers that every other TU in the component will also
   pull in.
5. **`include_facts` is the only family with duplicate suppression working**
   (17,546 attempted → 314 persisted).
6. The Clang front end (`clang_front_end` 2.23 s) is *not* the bottleneck; AST
   traversal plus storage is. Front-end reuse would recover at most 12 %.

## 6. Suggested order of attack

1. Add `AND identity_key <> ''` to the two `lookup_symbol` identity queries
   (`storage_symbols.cpp:311`) — or drop the partial index's `WHERE` clause.
   One-line change, ~40× on the single hottest path, and it removes the
   quadratic scaling. Highest value by a wide margin.
2. Memoize `intern_type_node` per TU (key → `type_id`) to collapse 88 k calls
   toward the 1.8 k distinct keys; this also removes most of
   `reconcile_type_identity`.
3. Cache/skip header passes across TUs sharing already-indexed headers.

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
