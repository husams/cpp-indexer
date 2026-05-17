story: S07-stage-parquet
date: 2026-05-17

## Files changed
- src/stage/mod.rs (new) — module declarations for manifest, schema, writer submodules
- src/stage/schema.rs (new) — shard filename conventions (nodes-NNNN/edges-NNNN), worker_dir(), WriterProperties with snappy+KV magic, has_magic() helper
- src/stage/manifest.rs (new) — ManifestEntry + Manifest structs, fsync+atomic-rename save, load with magic+version validation
- src/stage/writer.rs (new) — StageWriter: per-worker dual ArrowWriter (nodes + edges), 256 MiB rotation, finish() returns shard list
- src/lib.rs — replaced `pub mod stage {}` with `pub mod stage;`
- Cargo.toml — added `features = ["async"]` to parquet dep; added `futures = "0.3"` dev-dependency

## Tests added/run
- `cargo nextest run -p cpp_indexer stage::` — 20 passed, 0 failed
- `cargo fmt --all -- --check` — exit 0
- `cargo clippy --all-targets --all-features -- -D warnings` — exit 0

## Deviations from plan
- `parquet::arrow::async_reader::ParquetObjectReader` does not exist in parquet 53.4.1 — plan referenced it but the correct API for tokio::fs::File async reads is `ParquetRecordBatchStreamBuilder::new(tokio_file)` directly. Removed unused import.
- Cargo.toml parquet dep now has `features = ["async"]` — parallel-merge risk with other worktrees (S08 etc.) editing Cargo.toml. Coordinator resolves on merge.
- `futures = "0.3"` added to dev-dependencies for `TryStreamExt` in async reader test — same merge risk.

## Follow-ups
- [sr-dev] Cargo.toml merge: `parquet = { version = "53", features = ["async"] }` and `futures = "0.3"` dev-dep must survive merge from other worktrees.
- [sr-dev] `StageWriter::bytes_written` tracks ArrowWriter in-memory buffered bytes, not final compressed on-disk bytes — rotation fires on buffer fill. Acceptable for M1; revisit in M3.
- Incremental manifest append (per-TU, M3) is a skeleton here — struct + round-trip + fsync only; full incremental update logic deferred to M3.

## References
plan.md S07, ADR-3, src/schema/arrow.rs (reused node_schema/edge_schema/batch helpers), src/schema/version.rs (PARQUET_MAGIC/SCHEMA_VERSION)
