# ADR-12: Code-snippet representation — inline on node with 32 KiB cap

Status: accepted
Date: 2026-05-18
Resolves: requirements OQ-2 (node vs sidecar), AC-S41-5/6, PRD §9 (storage model open question)

## Context

S41 promotes a verbatim source-code snippet (`code: Option<String>`) onto every FUNCTION and METHOD node. PRD §9 leaves open whether `code` should live on the node or in a sidecar blob keyed by `usr`. PRD §7 sizes the worst case at ~25 MiB for the leveldb corpus (2,838 nodes × ~30% callables × 32 KiB cap), which is unremarkable for Neo4j but flagged for the IndraDB memory backend.

Sidecar would add: a second table/vertex type, a second write path per sink, a second read path in every consumer query, and the JOIN semantics that PRD §1 explicitly went to native properties to avoid. The cap (32 KiB) bounds growth deterministically; the rare oversize callable is signaled by `code_truncated: true` so a consumer can fall back to reading the file directly via `file_path` + `line`.

The 32 KiB boundary is computed on the raw UTF-8 byte length of the slice returned by `entity.get_range()` reading `file_path` (requirements §Assumption 2). The boundary is inclusive (`≤ 32768` keeps the code; `≥ 32769` truncates) per AC-S41-5/6.

## Decision

1. **Inline on the node.** `code: Option<String>` and `code_truncated: Option<bool>` are columns on `NodeRecord`, written as native properties on every sink. No sidecar table, no sidecar vertex kind, no separate fetch.
2. **Hard cap = 32_768 bytes** (`const CODE_SNIPPET_MAX_BYTES: usize = 32 * 1024;` in `src/visit/shallow.rs` or a new `src/schema/limits.rs`).
3. **Truncation semantics.** If `range.end_byte - range.start_byte > CODE_SNIPPET_MAX_BYTES`, set `code = None` and `code_truncated = Some(true)`. If within cap, set `code = Some(slice)` and `code_truncated = Some(false)`. If the cursor is not FUNCTION/METHOD, both fields are `None`.
4. **I/O.** Read the slice once per file per visit pass. Cache the file's source bytes in the visitor's per-TU scratch area (same lifetime as the existing source-buffer in `src/visit/shallow.rs`). Do not re-open per cursor.
5. **No content normalization.** Store verbatim bytes (including original line endings, whitespace). Consumers that want canonical form re-normalize downstream.
6. **IndraDB memory-backend guard.** Tests that exercise the memory backend with large fixtures use a smaller cap (1 KiB) injected via `cfg(test)` helper. The 32 KiB constant remains the production value. AC-S45-5 (`code_truncated: true` does not panic) is exercised by a fixture that exceeds the test cap.

## Alternatives considered

- **Sidecar blob keyed by `usr` (PRD §9 alternative).** Rejected: doubles the sink-write paths; doubles the consumer-read paths; the worst-case sizing (≈25 MiB for leveldb; ≈1 GiB for a 1M-node repo at ~33% callable ratio) is acceptable on Neo4j disk and survivable on IndraDB persistent backend. Memory-backend pressure is a test concern, addressed by point 6.
- **Sidecar in object storage (S3/local FS) referenced by URL.** Rejected: introduces a third dependency for what is fundamentally a string column; consumers would need a fetcher; defeats the agent-loop latency goal in PRD §2.2.
- **No cap (store unbounded).** Rejected: a single 200 KiB monster function (rare but real — codegen output, lex tables) would blow up the IndraDB memory backend and inflate Bolt frame sizes; deterministic cap is cheaper than producing a 99th-percentile bound from a corpus survey.
- **Smaller cap (8 KiB).** Rejected: empirically misses common multi-screen callables (e.g., `leveldb::DBImpl::Open` is ~140 lines, ≈4 KiB — fine — but `leveldb::DBImpl::RecoverLogFile` is ~250 lines, ~9 KiB, would truncate). 32 KiB clears typical callables; truncation is the escape valve for true outliers.

## Consequences

Positive:
- Single write path, single read path, query-by-property works in one Cypher pattern.
- AC-S41-7 satisfiable with a single MATCH; AC-S45-3 satisfiable with one `get_vertex_properties`.
- `code_truncated` flag gives consumers explicit "go to file_path" signal — no silent partial data.

Negative:
- Graph bloat (~25 MiB for leveldb today; scales linearly with callable count). Acceptable per PRD §7.
- IndraDB memory-backend tests must be sized below the cap or use the test-cap shim.
- Bolt frame sizes grow; per-batch payload increases ~10-30% on callable-heavy repos. Existing Phase 4 batching parameter (`DEFAULT_BATCH_SIZE`) is unchanged but operators may need to halve it for repos with many large callables. Documented in deploy-notes follow-up.

Follow-ups:
- After M8 lands, measure actual `code_truncated == true` rate on a 100k-file repo. If >5%, revisit the 32 KiB cap.
- Sidecar remains an option for M9+ if storage pressure materializes.

## References

- `[[pages/planning/cpp-indexer-structured-attrs-prd]]` §AC-S1.4, §7, §9
- requirements.md AC-S41-5/6, AC-S45-5, OQ-2, Assumption 2
- `src/visit/shallow.rs` (source-buffer per-TU cache, point 4)
- Cognee tags: `task:cpp-indexer-m8 role:architect`
