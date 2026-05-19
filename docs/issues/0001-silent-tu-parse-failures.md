# Issue 0001 — Silent total TU parse-failure on clean `compile_commands.json`

- **Reported:** 2026-05-19
- **Found by:** live test against `gabime/spdlog` (HEAD, 7 TUs)
- **Severity:** High — pipeline reports success while indexing nothing
- **Affected binaries:** `cxg-index`, `cxg-daemon` (shared `pipeline::run()`)
- **Affected version:** main @ `c9c7fde` (M8 merged)

---

## Summary

On macOS with libclang 18 from `CommandLineTools`, running `cxg-index` against a freshly-built spdlog produces **0 successfully-parsed TUs out of 7**, every TU fails with `AstDeserialization`, **but the process exits 0** and the final summary reads `7 TUs | 0 partial | 1 nodes | 0 edges` — which looks like a successful run.

Two distinct defects compound:

1. **Bug A — silent failure / wrong exit code.** All TUs failing is reported only as `WARN` lines; the summary line and exit code make the run look successful. The `1 node` written is just the `REPO` + `SchemaVersion` housekeeping node.
2. **Bug B — root cause of the parse failures.** Raw `compile_commands.json` `arguments` are passed to `libclang` unchanged, including the compiler driver (`/usr/bin/c++`), `-c <src>`, `-o <out>.o`, and the source-file path itself. libclang's parser API expects only flags; the leading driver positional argument triggers `CXError_ASTReadError` (Rust bindings name: `AstDeserialization`).

---

## Reproduction

```bash
# Clean spdlog checkout + compile_commands.json
git clone --depth 1 https://github.com/gabime/spdlog.git /tmp/spdlog-test
cmake -S /tmp/spdlog-test -B /tmp/spdlog-test/build \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF

# Throwaway Neo4j (sink is mandatory today)
docker run -d --rm --name neo4j-tmp \
  -p 7687:7687 -p 7474:7474 \
  -e NEO4J_AUTH=neo4j/testtest123 -e NEO4J_PLUGINS='[]' neo4j:5
sleep 8

# Run the indexer
export DYLD_FALLBACK_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib
export NEO4J_PASSWORD=testtest123
cd /tmp/spdlog-test
cxg-index \
  --backend neo4j --db-uri bolt://localhost:7687 --neo4j-user neo4j \
  --compile-commands build/compile_commands.json \
  --repo-name spdlog --stage-dir /tmp/cxg-stage .
echo "EXIT=$?"   # observed: 0
```

### Observed output (truncated)

```
WARN  Phase 1 parallel: hard libclang failure, skipping TU
       file=/private/tmp/spdlog-test/src/spdlog.cpp
       error=libclang failed to parse ...: AstDeserialization
... (×7, one per TU) ...
WARN  Phase 1: not updating cache manifest because one or more TUs failed errors=7
INFO  Phase 1: complete (0 ok TU(s), 0 partial TU(s), 7 failed TU(s), 0 cache hit(s))
INFO  Phase 4: complete — 1 nodes, 0 edges written to 'neo4j'
cxg-index: done — 7 TUs | 0 partial | 1 nodes | 0 edges
EXIT=0
```

The internal Phase-1 log line **does** report `7 failed TU(s)`. The final summary discards that counter and the process exits 0.

### Sanity check on the input

A direct `/usr/bin/clang++ -fsyntax-only -DSPDLOG_COMPILED_LIB -I./include -std=c++11 src/spdlog.cpp` parses cleanly (exit 0). So the source is fine; the failure is in how the indexer hands args to libclang.

---

## Root cause — Bug B (args not sanitised)

- `src/bootstrap/compile_commands.rs:92` — `resolve_args()` returns the raw `arguments` array unchanged.
- `src/visit/shallow.rs:194` — `index.parser(opts.file_path).arguments(opts.args)` passes that raw array to libclang.
- Sample `arguments` for one TU after CMake export:

  ```
  /usr/bin/c++   -DSPDLOG_COMPILED_LIB   -I/tmp/spdlog-test/include
  -O3   -DNDEBUG   -std=c++11   -arch arm64
  -o CMakeFiles/spdlog.dir/src/spdlog.cpp.o
  -c /tmp/spdlog-test/src/spdlog.cpp
  ```

- The four problematic tokens for libclang are: `/usr/bin/c++` (driver, positional, treated as a possible serialised-AST file), `-c <src>` (input filename — already passed as `parser(file_path)`), `-o <obj>` (output filename — not valid for `clang_parseTranslationUnit`), and the trailing source-file repeat.

The same three call sites have the same defect: `src/visit/shallow.rs:194`, `src/visit/decorate.rs:216`, `src/visit/modules_cpp20.rs:219`.

---

## Root cause — Bug A (silent failure)

- Phase 1 stores `failed_tu_count` internally and logs it once, but `pipeline::run()`'s closing line in the summary printer omits it.
- No exit-code mapping: today the binary exits 0 even when `failed_tu_count == total_tu_count`. There is no `--strict` or `--fail-on-error` flag either.

---

## Acceptance criteria (for the fix)

1. **AC-1 — sanitised libclang args.** After `resolve_args()`, the returned `Vec<String>` MUST NOT contain: the leading compiler-driver token (path ending in `cc`, `c++`, `clang`, `clang++`, `gcc`, `g++`), nor any `-c` followed by its operand, nor any `-o` followed by its operand, nor the source-file path itself (matches `entry.file` canonicalised). Existing tokens like `-D…`, `-I…`, `-std=…`, `-W…`, `-f…`, `-isystem`, `-include`, `-arch`, `-target` MUST pass through unchanged.

2. **AC-2 — regression test for AC-1.** A unit test in `src/bootstrap/compile_commands.rs` MUST assert that `resolve_args()` on a CMake-style entry (driver + `-c` + `-o` + source) returns only the flag set.

3. **AC-3 — spdlog smoke test.** An integration test (gated behind `--ignored` so CI can opt in) MUST clone spdlog, generate `compile_commands.json` via CMake, and assert `ok_tu_count >= 6` out of 7 (allow 1 partial for header-only edge cases). Skipped on Windows.

4. **AC-4 — failed-TU counter in summary.** The closing summary MUST include `failed: <N>` between `partial` and `nodes`:
   `cxg-index: done — <T> TUs | <P> partial | <F> failed | <N> nodes | <E> edges`.

5. **AC-5 — exit-code policy.** New flag `--fail-on-tu-error <ratio>` (default `1.0`) — exit non-zero (`2`) when `failed_tu_count / total_tu_count >= ratio`. Default behaviour: exit 2 only when all TUs fail. Existing partial-success runs continue to exit 0. Flag override accepts `0.0` (any failure → non-zero) up to `1.0` (only all-fail → non-zero) and `never` (always exit 0, today's behaviour, for back-compat).

6. **AC-6 — daemon job status.** `cxg-daemon` `GET /v1/jobs/<id>` MUST surface `failed_tu_count` and a new `status = "completed_with_errors"` when `failed > 0 && failed < total`, and `status = "failed"` when `failed == total`. Existing `status = "completed"` remains for `failed == 0`.

7. **AC-7 — back-compat.** Existing JSON job records that lack the new field MUST continue to deserialise (serde default). The new summary field MUST NOT break the parser used by the release workflow (`tools/release/parse-summary.sh` if present — confirm during PR).

---

## Out of scope

- Changing the libclang version or shipping a private build.
- A null/dry-run sink (separate request; tracked elsewhere if filed).
- Anything macOS-specific beyond verifying the smoke test runs there.

---

## Files-to-touch (preliminary)

- `src/bootstrap/compile_commands.rs` — `resolve_args()` + new `sanitize_libclang_args()` + tests.
- `src/visit/shallow.rs`, `src/visit/decorate.rs`, `src/visit/modules_cpp20.rs` — confirm they consume the sanitised list (no per-call-site filters).
- `src/pipeline/mod.rs` — summary line, exit-code mapping, plumb `failed_tu_count` through `PipelineStats`.
- `src/bin/index.rs` — new `--fail-on-tu-error` CLI flag.
- `src/api/jobs.rs` (or equivalent) — job-status field + state mapping for daemon.
- `tests/integration/spdlog_smoke.rs` (new, `#[ignore]`).
- `docs/runbooks/libclang-setup.md` — note the args-sanitisation contract.

---

## References

- libclang `CXError_ASTReadError`: <https://clang.llvm.org/doxygen/group__CINDEX.html#gaab39f9486678f1cb16ba14721315fb50>
- Rust binding mapping: `clang::SourceError::AstDeserialization` ↔ `CXError_ASTReadError`.
- Repro environment: macOS 25.1.0 (Darwin), libclang 18 (`/Library/Developer/CommandLineTools/usr/lib/libclang.dylib`), Apple clang 17.0.0 as the `compile_commands.json` producer.
