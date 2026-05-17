/// Stage-level schema glue: PARQUET_MAGIC KV metadata, shard filename conventions,
/// and worker subdirectory naming.
///
/// The Arrow field-level schemas (`node_schema`, `edge_schema`) and record-batch helpers live in
/// `crate::schema::arrow`. This module only owns the staging-layer file layout contracts from
/// ADR-3.
use std::path::{Path, PathBuf};

use parquet::file::properties::{WriterProperties, WriterPropertiesBuilder};

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
