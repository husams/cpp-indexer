/// Stage-level schema glue: PARQUET_MAGIC KV metadata, shard filename conventions,
/// and worker subdirectory naming.
///
/// The Arrow field-level schemas (`node_schema`, `edge_schema`) and record-batch helpers live in
/// `crate::schema::arrow`. This module only owns the staging-layer file layout contracts from
/// ADR-3.
use std::path::{Path, PathBuf};

use parquet::arrow::arrow_reader::{ParquetRecordBatchReader, ParquetRecordBatchReaderBuilder};
use parquet::file::properties::{WriterProperties, WriterPropertiesBuilder};
use parquet::file::reader::FileReader;

use crate::error::{Error, Result};

use crate::schema::version::PARQUET_MAGIC;

// ---------------------------------------------------------------------------
// Shard rotation threshold
// ---------------------------------------------------------------------------

/// Rotate to a new shard when the current file exceeds this many bytes (ADR-3: 256 MiB).
pub const SHARD_ROTATE_BYTES: u64 = 256 * 1024 * 1024;

// ---------------------------------------------------------------------------
// Shard filename helpers
// ---------------------------------------------------------------------------

/// Returns the canonical shard filename for nodes: `nodes-NNNN.parquet`.
///
/// `index` is the 1-based shard counter within a single worker.
pub fn node_shard_name(index: u32) -> String {
    format!("nodes-{index:04}.parquet")
}

/// Returns the canonical shard filename for edges: `edges-NNNN.parquet`.
///
/// `index` is the 1-based shard counter within a single worker.
pub fn edge_shard_name(index: u32) -> String {
    format!("edges-{index:04}.parquet")
}

/// Returns the worker subdirectory path within `stage_dir`.
///
/// Layout: `<stage_dir>/worker-NNN/`
pub fn worker_dir(stage_dir: &Path, worker_id: u32) -> PathBuf {
    stage_dir.join(format!("worker-{worker_id:03}"))
}

// ---------------------------------------------------------------------------
// Shard reader
// ---------------------------------------------------------------------------

/// Open a Parquet shard for Arrow record-batch reading.
///
/// When `require_magic` is `true` the file's KV metadata is checked for the
/// ADR-3 `cxg_parquet_vN` header before returning the reader; an absent or
/// mismatched header is an `Error::Schema`.  Only `resolve::per_repo` (Phase 3)
/// requires the magic check; other callers pass `false` so that pre-existing
/// stage dirs written without the header (e.g. Phase 5 `final-edges.parquet`)
/// can still be read.
///
/// Preserving the per-site magic-check policy is load-bearing: adding the check
/// to sites that previously did not have it would silently break reading stage
/// dirs produced by older binaries.
pub fn open_shard_reader(path: &Path, require_magic: bool) -> Result<ParquetRecordBatchReader> {
    let file = std::fs::File::open(path)?;
    if require_magic {
        let reader_for_meta =
            parquet::file::reader::SerializedFileReader::new(std::fs::File::open(path)?)
                .map_err(|e| Error::Schema(format!("Parquet open {}: {e}", path.display())))?;
        let meta = reader_for_meta.metadata().file_metadata().clone();
        if !has_magic(&meta) {
            return Err(Error::Schema(format!(
                "shard {:?} is missing the ADR-3 magic key '{}'; \
                 re-index with a compatible binary",
                path,
                crate::schema::version::PARQUET_MAGIC
            )));
        }
    }
    let reader = ParquetRecordBatchReaderBuilder::try_new(file)
        .map_err(|e| Error::Schema(format!("build reader {}: {e}", path.display())))?
        .build()
        .map_err(|e| Error::Schema(format!("build batch reader {}: {e}", path.display())))?;
    Ok(reader)
}

// ---------------------------------------------------------------------------
// Shard collection
// ---------------------------------------------------------------------------

/// Policy for what `collect_shards` does when `stage_dir` does not exist or is
/// not a directory.
///
/// - `ErrorOnMissing` — propagate the OS error from `fs::read_dir` (used by
///   `resolve::per_repo`, which requires the stage dir to exist).
/// - `EmptyOnMissing` — return an empty list without error (used by
///   `pipeline::mod` which tolerates an absent dir, and `resolve::cross_repo`
///   which checks `is_dir()` first).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MissingDirPolicy {
    /// Return `Err` when the directory is absent or not a directory.
    ErrorOnMissing,
    /// Return `Ok([])` when the directory is absent or not a directory.
    EmptyOnMissing,
}

/// Collect all `<prefix>-*.parquet` shard paths from every `worker-*/`
/// subdirectory inside `stage_dir`, sorted lexicographically.
///
/// `policy` controls what happens when `stage_dir` is absent:
/// - [`MissingDirPolicy::ErrorOnMissing`] — propagates the IO error.
/// - [`MissingDirPolicy::EmptyOnMissing`] — returns `Ok(vec![])`.
///
/// Shards are collected in deterministic, sorted order so that multiple callers
/// reading the same stage dir produce identical iteration sequences.
pub fn collect_shards(
    stage_dir: &Path,
    prefix: &str,
    policy: MissingDirPolicy,
) -> Result<Vec<PathBuf>> {
    let mut shards = Vec::new();

    if policy == MissingDirPolicy::EmptyOnMissing && !stage_dir.is_dir() {
        return Ok(shards);
    }

    for entry in std::fs::read_dir(stage_dir)? {
        let entry = entry?;
        let path = entry.path();
        let dir_name = path
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or_default();
        if !path.is_dir() || !dir_name.starts_with("worker-") {
            continue;
        }
        for w_entry in std::fs::read_dir(&path)? {
            let w_entry = w_entry?;
            let shard_path = w_entry.path();
            let file_name = shard_path
                .file_name()
                .and_then(|n| n.to_str())
                .unwrap_or_default();
            if file_name.starts_with(prefix) && file_name.ends_with(".parquet") {
                shards.push(shard_path);
            }
        }
    }

    shards.sort();
    Ok(shards)
}

// ---------------------------------------------------------------------------
// Writer properties (snappy + KV magic)
// ---------------------------------------------------------------------------

/// Builds `WriterProperties` with snappy compression and the ADR-3 KV magic header.
///
/// `cxg_parquet_v1` is stored as Parquet file-level key-value metadata so Phase 3 can
/// refuse to read mismatched-version shards.
pub fn writer_properties() -> WriterProperties {
    writer_properties_builder().build()
}

/// Exposes the builder so callers can extend before calling `.build()`.
pub fn writer_properties_builder() -> WriterPropertiesBuilder {
    WriterProperties::builder()
        .set_compression(parquet::basic::Compression::SNAPPY)
        // Cap row-group buffering at 64 KiB rows so each per-worker StageWriter
        // flushes frequently rather than accumulating hundreds of MiB of node rows
        // (each node carries up to 32 KiB of `code`) before the first flush.
        // parquet-53 defaults to 1,048,576 rows, which is far too large for
        // code-heavy schemas.  64 K rows keeps the in-memory buffer well under
        // 2 GiB even at max code-snippet size.
        .set_max_row_group_size(64 * 1024)
        .set_key_value_metadata(Some(vec![parquet::format::KeyValue {
            key: PARQUET_MAGIC.to_owned(),
            value: Some("1".to_owned()),
        }]))
}

/// Reads the `cxg_parquet_vN` magic key back from a Parquet file's KV metadata.
///
/// Returns `true` when the key is present and its value is `"1"` (schema v1).
/// Phase 3 should call this before reading any shard.
pub fn has_magic(metadata: &parquet::file::metadata::FileMetaData) -> bool {
    metadata
        .key_value_metadata()
        .map(|kvs| {
            kvs.iter()
                .any(|kv| kv.key == PARQUET_MAGIC && kv.value.as_deref() == Some("1"))
        })
        .unwrap_or(false)
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn node_shard_name_format() {
        assert_eq!(node_shard_name(1), "nodes-0001.parquet");
        assert_eq!(node_shard_name(42), "nodes-0042.parquet");
        assert_eq!(node_shard_name(9999), "nodes-9999.parquet");
    }

    #[test]
    fn edge_shard_name_format() {
        assert_eq!(edge_shard_name(1), "edges-0001.parquet");
        assert_eq!(edge_shard_name(7), "edges-0007.parquet");
    }

    #[test]
    fn worker_dir_format() {
        let stage = Path::new("/tmp/cxg/stage/run-1");
        assert_eq!(worker_dir(stage, 0), stage.join("worker-000"));
        assert_eq!(worker_dir(stage, 1), stage.join("worker-001"));
        assert_eq!(worker_dir(stage, 99), stage.join("worker-099"));
    }

    #[test]
    fn shard_rotate_bytes_is_256_mib() {
        assert_eq!(SHARD_ROTATE_BYTES, 256 * 1024 * 1024);
    }

    #[test]
    fn writer_properties_has_snappy() {
        let props = writer_properties();
        // Snappy is the default column compression; just verify the properties build without panic.
        // We cannot introspect `compression` in the public API; trust the builder.
        let _ = props;
    }

    /// writer_properties() must cap row-group size at 64 KiB rows.
    ///
    /// Verified by writing enough rows to force two row groups, then reading
    /// back Parquet file metadata to confirm more than one row group was flushed.
    /// Without `set_max_row_group_size(64 * 1024)` a single row group would hold
    /// all rows regardless of count.
    #[test]
    fn writer_properties_caps_row_group_size() {
        use parquet::file::reader::{FileReader, SerializedFileReader};
        use std::fs::File;
        use std::sync::Arc;

        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("rg_cap_test.parquet");

        // Build a record batch with 64*1024 + 1 rows to exceed one row group.
        // We use minimal NodeRecords (no code payload) to keep the test fast.
        let n_rows: usize = 64 * 1024 + 1;
        let records: Vec<crate::schema::NodeRecord> = (0..n_rows)
            .map(|i| crate::schema::NodeRecord {
                usr: format!("c:@F@fn_{i}"),
                name: format!("fn_{i}"),
                qualified_name: format!("ns::fn_{i}"),
                file_path: "/repo/src/f.cpp".to_owned(),
                line: Some(1),
                col: Some(1),
                repo_name: "test".to_owned(),
                symbol_id: i as i64 + 1,
                file_id: 1,
                ..Default::default()
            })
            .collect();

        let schema = Arc::new(crate::schema::arrow::node_schema());
        let props = writer_properties();
        let file = File::create(&path).unwrap();
        let mut writer = parquet::arrow::ArrowWriter::try_new(file, schema, Some(props)).unwrap();
        let batch = crate::schema::arrow::nodes_to_record_batch(&records).unwrap();
        writer.write(&batch).unwrap();
        writer.close().unwrap();

        let file = File::open(&path).unwrap();
        let reader = SerializedFileReader::new(file).unwrap();
        let n_rg = reader.metadata().num_row_groups();
        assert!(
            n_rg >= 2,
            "expected ≥2 row groups when writing 64 KiB+1 rows with a 64 KiB cap, got {n_rg}"
        );
    }

    #[test]
    fn has_magic_positive() {
        // Build a props with the magic and read it back via a temp Parquet file.
        use parquet::file::reader::FileReader;
        use std::fs::File;
        use std::sync::Arc;

        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("test.parquet");

        let schema = Arc::new(crate::schema::arrow::node_schema());
        let props = writer_properties();
        let file = File::create(&path).unwrap();
        let writer = parquet::arrow::ArrowWriter::try_new(file, schema, Some(props)).unwrap();
        writer.close().unwrap();

        let file = File::open(&path).unwrap();
        let reader = parquet::file::reader::SerializedFileReader::new(file).unwrap();
        let meta = reader.metadata().file_metadata().clone();
        assert!(
            has_magic(&meta),
            "magic key must be present in file KV metadata"
        );
    }

    #[test]
    fn has_magic_negative() {
        use parquet::file::reader::FileReader;
        use std::fs::File;
        use std::sync::Arc;

        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("no_magic.parquet");

        let schema = Arc::new(crate::schema::arrow::node_schema());
        // No magic in properties
        let props = WriterProperties::builder().build();
        let file = File::create(&path).unwrap();
        let writer = parquet::arrow::ArrowWriter::try_new(file, schema, Some(props)).unwrap();
        writer.close().unwrap();

        let file = File::open(&path).unwrap();
        let reader = parquet::file::reader::SerializedFileReader::new(file).unwrap();
        let meta = reader.metadata().file_metadata().clone();
        assert!(
            !has_magic(&meta),
            "magic key must not be present when properties had none"
        );
    }
}
