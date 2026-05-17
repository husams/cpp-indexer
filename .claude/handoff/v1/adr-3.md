# ADR-3: Parquet staging schema — per-worker shards with explicit resolution flags

Status: accepted
Date: 2026-05-17
Resolves: Phase 1 → Phase 3 → Phase 4 data contract

## Context

Phase 1 writes nodes/edges to Parquet shards instead of directly to the DB (CHARTER locked-in). The schema must:

- Round-trip with the Rust `NodeKind` / `EdgeKind` enums (AC-M1-4).
- Carry `usr`, `mangled_name`, and per-kind attributes without losing typing.
- Distinguish `resolved`, `cross_repo_candidate`, and `unresolved` edge states (AC-M1-19, AC-M2-13, AC-M4-2).
- Be writeable per-worker without coordination (rayon).
- Be readable in a single Phase 3 pass.
- Survive an interrupted run so incremental cache (AC-M3-7..9) can detect partial shards.

## Decision

Two Parquet schemas, both columnar with snappy compression:

### `nodes.parquet` schema

| Column | Arrow type | Notes |
|---|---|---|
| `usr` | `Utf8` (not null) | global primary key |
| `kind` | `Dictionary<Int8, Utf8>` | `NodeKind` enum as string |
| `name` | `Utf8` | display name |
| `qualified_name` | `Utf8` | namespace::class::name |
| `mangled_name` | `Utf8` | nullable |
| `file_path` | `Utf8` (not null) | absolute |
| `line` | `UInt32` | nullable |
| `col` | `UInt32` | nullable |
| `repo_name` | `Utf8` (not null) | from `[repo].name` |
| `attrs_json` | `Utf8` | per-kind extra attributes (virtual, scoped, template_args, etc.) serialised as canonical JSON |
| `partial` | `Boolean` | TU had parse errors (M5) |
| `phase` | `UInt8` | 1 (shallow) or 2 (decorated) |
| `tu_hash` | `FixedSizeBinary(32)` | Blake3(source + args); enables cache invalidation |

### `edges.parquet` schema

| Column | Arrow type | Notes |
|---|---|---|
| `src_usr` | `Utf8` (not null) | |
| `dst_usr` | `Utf8` | nullable when unresolved at emit time |
| `dst_placeholder` | `Utf8` | spelling captured by libclang when no USR; nullable |
| `kind` | `Dictionary<Int8, Utf8>` | EdgeKind enum |
| `resolved` | `Boolean` (not null) | set true by Phase 3 on lookup hit |
| `cross_repo_candidate` | `Boolean` (not null) | set true by Phase 3 when target USR not in current repo's map |
| `repo_name` | `Utf8` (not null) | edge's owning repo |
| `attrs_json` | `Utf8` | edge attrs (vtable_slot, access, virtual, via, ...) |
| `tu_hash` | `FixedSizeBinary(32)` | source TU hash |

### Layout on disk

```
.cxg-cache/
├── manifest.json                  # per-TU: source_hash, args_hash, libclang_version, schema_version, output_shards[]
├── stage/<run_id>/
│   ├── worker-000/
│   │   ├── nodes-0001.parquet
│   │   ├── nodes-0002.parquet
│   │   ├── edges-0001.parquet
│   │   └── edges-0002.parquet
│   ├── worker-001/...
│   └── final/
│       ├── nodes.parquet            # Phase 3 consolidates; idempotent
│       └── final-edges.parquet      # post-resolution
```

Rules:
- One worker writes only to its own subdirectory; no cross-worker locking.
- Shards rotate at 256 MiB to keep individual file reads bounded.
- `manifest.json` is written incrementally per TU completion with `fsync`, so an interrupted run can resume.
- Phase 3 reads `worker-*/nodes-*.parquet` into the USR map, then walks `worker-*/edges-*.parquet` and writes `final/final-edges.parquet`.
- Phase 3 also writes `final/nodes.parquet` as a deduplicated stream (USR-keyed; first-write-wins within a repo, with a diagnostic on collision).

Versioning:
- A magic header `cxg_parquet_v1` is stored in Parquet KV metadata; Phase 3 refuses to read mismatched-version shards (forces clean re-index).

## Alternatives considered

- **One Parquet file per TU**: rejected. ~25 k files for LLVM hits filesystem inode limits and balloons open-file count during Phase 3 scan.
- **JSON Lines staging**: rejected. ~3–5× larger on disk; serial read becomes the bottleneck in Phase 3.
- **Apache Avro**: rejected. Tooling in Rust is weaker than Parquet; Arrow integration is the dominant ecosystem.
- **Per-kind nodes table (separate parquet per NodeKind)**: rejected. Adds N file-open costs per TU; `kind` dictionary column gives the same query speed with one file.
- **Embed full typed columns per kind (no `attrs_json`)**: rejected. Schema would balloon to 50+ columns with sparse data; `attrs_json` keeps the file narrow and is fast enough (Phase 3 reads it only for resolved edges).

## Consequences

Positive:
- Phase 1 writers never coordinate; pure parallel throughput.
- Schema evolution is straightforward: bump `cxg_parquet_v1` to `v2` when a column is added/removed.
- Incremental re-index can drop a single TU's shards by `tu_hash` filter without rewriting.

Negative:
- `attrs_json` defers some validation to Phase 3 / Phase 4.
- `final/nodes.parquet` consolidation is a single-threaded step; mitigated by streaming write.

Follow-ups:
- After M1, profile Phase 3 read time; if it dominates, switch `final/` to per-shard read directly in Phase 4.
- Consider zstd compression in M3 if disk is a bottleneck.

Revisit if: Phase 3 read time exceeds 5 % of total wall time on LLVM, or shard file count exceeds 10 k per repo.

## References

- requirements.md AC-M1-14..19, AC-M2-13, AC-M3-7..10, AC-M4-2
- engineering plan v1.1 §Phase 1, §Phase 3, §stage/
- Cognee tags: `task:cpp-indexer role:architect`
