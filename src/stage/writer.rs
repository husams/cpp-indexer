/// Per-worker Parquet shard writer (ADR-3, AC-M1-14, AC-M1-17).
///
/// `StageWriter` owns two open `ArrowWriter` handles (one for nodes, one for edges)
/// and rotates to a new shard when the current file exceeds `SHARD_ROTATE_BYTES` (256 MiB).
/// Each writer instance is confined to its own `worker-NNN/` subdirectory — no cross-worker
/// locking is needed.
use std::{
    fs::{self, File},
    path::{Path, PathBuf},
    sync::Arc,
};

use parquet::arrow::ArrowWriter;

use crate::{
    schema::arrow::{edge_schema, edges_to_record_batch, node_schema, nodes_to_record_batch},
    schema::{EdgeRecord, NodeRecord},
    stage::schema::{
        edge_shard_name, node_shard_name, worker_dir, writer_properties, SHARD_ROTATE_BYTES,
    },
    Error, Result,
};

// ---------------------------------------------------------------------------
// StageWriter
// ---------------------------------------------------------------------------

/// Per-worker Parquet shard writer.
///
/// # Lifecycle
/// 1. Call `StageWriter::new(stage_dir, worker_id)`.
/// 2. Push records with `write_nodes` / `write_edges`.
/// 3. Call `finish()` to flush + close all open files and return the list of shards written.
///
/// Rotation happens automatically when a shard exceeds `SHARD_ROTATE_BYTES`.
pub struct StageWriter {
    /// Directory for this worker (`<stage_dir>/worker-NNN/`).
    worker_dir: PathBuf,
    /// Current shard index for nodes (1-based).
    node_shard_idx: u32,
    /// Current shard index for edges (1-based).
    edge_shard_idx: u32,
    /// Open `ArrowWriter` for the current node shard.
    node_writer: Option<SharWriter<NodeRecord>>,
    /// Open `ArrowWriter` for the current edge shard.
    edge_writer: Option<SharWriter<EdgeRecord>>,
    /// Paths of all shards written (nodes + edges, in creation order).
    shards_written: Vec<PathBuf>,
    /// Index into `shards_written` marking where the last cache checkpoint was taken.
    ///
    /// Used by `shards_written_since_last_cache_record` to return only the shards
    /// created for the most-recently-parsed TU.
    cache_checkpoint: usize,
}

/// Internal state for one open shard file.
struct SharWriter<T> {
    path: PathBuf,
    writer: ArrowWriter<File>,
    bytes_written: u64,
    _phantom: std::marker::PhantomData<T>,
}

impl StageWriter {
    /// Creates a new per-worker writer.
    ///
    /// `stage_dir` is the run-level staging directory (e.g. `.cxg-cache/stage/<run_id>/`).
    /// `worker_id` selects the `worker-NNN/` subdirectory.
    ///
    /// # Errors
    /// Returns `Error::Io` when the worker directory cannot be created.
    pub fn new(stage_dir: &Path, worker_id: u32) -> Result<Self> {
        let dir = worker_dir(stage_dir, worker_id);
        fs::create_dir_all(&dir)?;
        Ok(Self {
            worker_dir: dir,
            node_shard_idx: 0,
            edge_shard_idx: 0,
            node_writer: None,
            edge_writer: None,
            shards_written: Vec::new(),
            cache_checkpoint: 0,
        })
    }

    // ------------------------------------------------------------------
    // Public write API
    // ------------------------------------------------------------------

    /// Appends `records` to the current node shard, rotating to a new shard when needed.
    ///
    /// # Errors
    /// Returns `Error::Schema` on Arrow/Parquet serialisation failure, `Error::Io` on write
    /// failure.
    pub fn write_nodes(&mut self, records: &[NodeRecord]) -> Result<()> {
        if records.is_empty() {
            return Ok(());
        }
        let batch = nodes_to_record_batch(records)
            .map_err(|e| Error::Schema(format!("node batch serialise: {e}")))?;

        self.ensure_node_writer()?;
        let sw = self
            .node_writer
            .as_mut()
            .expect("node writer must be initialised");
        sw.writer
            .write(&batch)
            .map_err(|e| Error::Schema(format!("node ArrowWriter::write: {e}")))?;
        // Estimate bytes written from the writer's in-progress byte count.
        sw.bytes_written = sw
            .writer
            .bytes_written()
            .try_into()
            .unwrap_or(sw.bytes_written);

        if sw.bytes_written >= SHARD_ROTATE_BYTES {
            self.rotate_node_writer()?;
        }
        Ok(())
    }

    /// Appends `records` to the current edge shard, rotating to a new shard when needed.
    ///
    /// # Errors
    /// Returns `Error::Schema` on serialisation failure, `Error::Io` on write failure.
    pub fn write_edges(&mut self, records: &[EdgeRecord]) -> Result<()> {
        if records.is_empty() {
            return Ok(());
        }
        let batch = edges_to_record_batch(records)
            .map_err(|e| Error::Schema(format!("edge batch serialise: {e}")))?;

        self.ensure_edge_writer()?;
        let sw = self
            .edge_writer
            .as_mut()
            .expect("edge writer must be initialised");
        sw.writer
            .write(&batch)
            .map_err(|e| Error::Schema(format!("edge ArrowWriter::write: {e}")))?;
        sw.bytes_written = sw
            .writer
            .bytes_written()
            .try_into()
            .unwrap_or(sw.bytes_written);

        if sw.bytes_written >= SHARD_ROTATE_BYTES {
            self.rotate_edge_writer()?;
        }
        Ok(())
    }

    /// Returns shard paths written since the last call to this method (or since
    /// construction if never called).
    ///
    /// Used by the cache layer to record which shards were produced for the
    /// most-recently-parsed TU. Advances the internal checkpoint so consecutive
    /// calls return non-overlapping slices.
    pub fn shards_written_since_last_cache_record(&mut self) -> Vec<PathBuf> {
        let result = self.shards_written[self.cache_checkpoint..].to_vec();
        self.cache_checkpoint = self.shards_written.len();
        result
    }

    /// Flushes and closes all open shards.
    ///
    /// Returns the list of shard paths written (relative to the worker dir), in creation order.
    ///
    /// # Errors
    /// Returns `Error::Schema` / `Error::Io` on flush/close failure.
    pub fn finish(mut self) -> Result<Vec<PathBuf>> {
        if let Some(sw) = self.node_writer.take() {
            close_shard(sw)?;
        }
        if let Some(sw) = self.edge_writer.take() {
            close_shard(sw)?;
        }
        Ok(self.shards_written)
    }

    // ------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------

    fn ensure_node_writer(&mut self) -> Result<()> {
        if self.node_writer.is_some() {
            return Ok(());
        }
        self.node_shard_idx += 1;
        let name = node_shard_name(self.node_shard_idx);
        let path = self.worker_dir.join(&name);
        let file = File::create(&path)?;
        let schema = Arc::new(node_schema());
        let props = writer_properties();
        let writer = ArrowWriter::try_new(file, schema, Some(props))
            .map_err(|e| Error::Schema(format!("create node ArrowWriter: {e}")))?;
        self.shards_written.push(path.clone());
        self.node_writer = Some(SharWriter {
            path,
            writer,
            bytes_written: 0,
            _phantom: std::marker::PhantomData,
        });
        Ok(())
    }

    fn ensure_edge_writer(&mut self) -> Result<()> {
        if self.edge_writer.is_some() {
            return Ok(());
        }
        self.edge_shard_idx += 1;
        let name = edge_shard_name(self.edge_shard_idx);
        let path = self.worker_dir.join(&name);
        let file = File::create(&path)?;
        let schema = Arc::new(edge_schema());
        let props = writer_properties();
        let writer = ArrowWriter::try_new(file, schema, Some(props))
            .map_err(|e| Error::Schema(format!("create edge ArrowWriter: {e}")))?;
        self.shards_written.push(path.clone());
        self.edge_writer = Some(SharWriter {
            path,
            writer,
            bytes_written: 0,
            _phantom: std::marker::PhantomData,
        });
        Ok(())
    }

    fn rotate_node_writer(&mut self) -> Result<()> {
        if let Some(sw) = self.node_writer.take() {
            close_shard(sw)?;
        }
        self.ensure_node_writer()
    }

    fn rotate_edge_writer(&mut self) -> Result<()> {
        if let Some(sw) = self.edge_writer.take() {
            close_shard(sw)?;
        }
        self.ensure_edge_writer()
    }
}

fn close_shard<T>(sw: SharWriter<T>) -> Result<()> {
    sw.writer
        .close()
        .map_err(|e| Error::Schema(format!("ArrowWriter::close ({}): {e}", sw.path.display())))?;
    Ok(())
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::schema::{EdgeKind, NodeKind};

    fn make_node(i: usize) -> NodeRecord {
        NodeRecord {
            usr: format!("c:@F@fn_{i}"),
            kind: NodeKind::Function,
            name: format!("fn_{i}"),
            qualified_name: format!("ns::fn_{i}"),
            mangled_name: Some(format!("_ZN2nsfn{i}Ev")),
            file_path: format!("/repo/src/file_{i}.cpp"),
            line: Some(u32::try_from(i).unwrap_or(0) + 1),
            col: Some(1),
            repo_name: "test-repo".to_owned(),
            attrs_json: "{}".to_owned(),
            partial: false,
            phase: 1,
            tu_hash: [u8::try_from(i % 256).unwrap_or(0); 32],
            return_type: None,
            params: None,
            signature: None,
            code: None,
            code_truncated: None,
            template_params: None,
            template_args: None,
            is_virtual: None,
            is_pure_virtual: None,
            is_static: None,
            symbol_id: i as i64 + 1,
            file_id: i as i64 + 1,
        }
    }

    fn make_edge(i: usize) -> EdgeRecord {
        EdgeRecord {
            src_usr: format!("c:@F@src_{i}"),
            dst_usr: Some(format!("c:@F@dst_{i}")),
            dst_placeholder: None,
            kind: EdgeKind::Calls,
            resolved: true,
            cross_repo_candidate: false,
            repo_name: "test-repo".to_owned(),
            attrs_json: "{}".to_owned(),
            tu_hash: [u8::try_from(i % 256).unwrap_or(0); 32],
            source_association_type: None,
            target_association_type: None,
            src_id: i as i64 + 1,
            dst_id: Some(i as i64 + 2),
            dst_repo_name: "test-repo".to_owned(),
        }
    }

    /// Write N nodes + M edges, read back via `parquet::arrow::async_reader`, assert row counts
    /// and column types match ADR-3.
    #[tokio::test]
    async fn write_and_read_back_row_counts() {
        let dir = tempfile::tempdir().unwrap();
        let stage_dir = dir.path().join("stage");

        const N_NODES: usize = 5;
        const M_EDGES: usize = 3;

        let nodes: Vec<NodeRecord> = (0..N_NODES).map(make_node).collect();
        let edges: Vec<EdgeRecord> = (0..M_EDGES).map(make_edge).collect();

        let mut writer = StageWriter::new(&stage_dir, 0).unwrap();
        writer.write_nodes(&nodes).unwrap();
        writer.write_edges(&edges).unwrap();
        let shards = writer.finish().unwrap();

        // Should have one node shard + one edge shard.
        assert_eq!(shards.len(), 2);

        // Read back nodes via async reader.
        let node_shard = shards
            .iter()
            .find(|p| {
                p.file_name()
                    .and_then(|n| n.to_str())
                    .map(|s| s.starts_with("nodes-"))
                    .unwrap_or(false)
            })
            .expect("node shard must exist");

        use futures::TryStreamExt;
        use parquet::arrow::ParquetRecordBatchStreamBuilder;

        let node_file = tokio::fs::File::open(node_shard).await.unwrap();
        let builder = ParquetRecordBatchStreamBuilder::new(node_file)
            .await
            .unwrap();
        // Verify schema column count: 13 original + 10 M8 native fields + 2 v6 integer ID fields
        // (symbol_id, file_id) = 25 (S40-S43, graph-symbol-ids Story 3).
        assert_eq!(builder.schema().fields().len(), 25);
        let mut stream = builder.build().unwrap();
        let mut total_rows = 0usize;
        while let Some(batch) = stream.try_next().await.unwrap() {
            total_rows += batch.num_rows();
        }
        assert_eq!(total_rows, N_NODES);

        // Read back edges.
        let edge_shard = shards
            .iter()
            .find(|p| {
                p.file_name()
                    .and_then(|n| n.to_str())
                    .map(|s| s.starts_with("edges-"))
                    .unwrap_or(false)
            })
            .expect("edge shard must exist");

        let edge_file = tokio::fs::File::open(edge_shard).await.unwrap();
        let builder = ParquetRecordBatchStreamBuilder::new(edge_file)
            .await
            .unwrap();
        // Verify schema column count: 9 original + 2 M8 association columns + 3 v6 integer ID
        // fields (src_id, dst_id, dst_repo_name) = 14 (S43, graph-symbol-ids Story 3).
        assert_eq!(builder.schema().fields().len(), 14);
        let mut stream = builder.build().unwrap();
        let mut edge_rows = 0usize;
        while let Some(batch) = stream.try_next().await.unwrap() {
            edge_rows += batch.num_rows();
        }
        assert_eq!(edge_rows, M_EDGES);
    }

    /// Writing no records produces no shards.
    #[test]
    fn empty_write_produces_no_shards() {
        let dir = tempfile::tempdir().unwrap();
        let stage_dir = dir.path().join("stage");
        let writer = StageWriter::new(&stage_dir, 1).unwrap();
        let shards = writer.finish().unwrap();
        assert!(shards.is_empty());
    }

    /// Multiple workers write to separate subdirectories without interfering.
    #[test]
    fn multiple_workers_isolated() {
        let dir = tempfile::tempdir().unwrap();
        let stage_dir = dir.path().join("stage");

        for worker_id in 0..3u32 {
            let nodes: Vec<NodeRecord> = (0..2).map(make_node).collect();
            let mut writer = StageWriter::new(&stage_dir, worker_id).unwrap();
            writer.write_nodes(&nodes).unwrap();
            let shards = writer.finish().unwrap();
            // Each worker writes exactly one node shard.
            assert_eq!(shards.len(), 1);
            // The shard is inside the correct worker dir.
            let expected_prefix = stage_dir.join(format!("worker-{worker_id:03}"));
            for shard in &shards {
                assert!(
                    shard.starts_with(&expected_prefix),
                    "shard {shard:?} must be under {expected_prefix:?}"
                );
            }
        }
    }

    /// Shard names follow the 4-digit zero-padded convention.
    #[test]
    fn shard_filenames_are_zero_padded() {
        let dir = tempfile::tempdir().unwrap();
        let stage_dir = dir.path().join("stage");

        let nodes = vec![make_node(0)];
        let edges = vec![make_edge(0)];

        let mut writer = StageWriter::new(&stage_dir, 0).unwrap();
        writer.write_nodes(&nodes).unwrap();
        writer.write_edges(&edges).unwrap();
        let shards = writer.finish().unwrap();

        let names: Vec<&str> = shards
            .iter()
            .filter_map(|p| p.file_name().and_then(|n| n.to_str()))
            .collect();
        assert!(names.contains(&"nodes-0001.parquet"));
        assert!(names.contains(&"edges-0001.parquet"));
    }

    /// Shards written by `StageWriter::finish` have the ADR-3 KV magic header.
    #[test]
    fn shards_have_magic_kv_metadata() {
        use parquet::file::reader::{FileReader, SerializedFileReader};

        let dir = tempfile::tempdir().unwrap();
        let stage_dir = dir.path().join("stage");

        let mut writer = StageWriter::new(&stage_dir, 0).unwrap();
        writer.write_nodes(&[make_node(0)]).unwrap();
        let shards = writer.finish().unwrap();

        let shard = &shards[0];
        let file = File::open(shard).unwrap();
        let reader = SerializedFileReader::new(file).unwrap();
        let meta = reader.metadata().file_metadata().clone();
        assert!(
            crate::stage::schema::has_magic(&meta),
            "shard must carry cxg_parquet_v1 KV magic"
        );
    }
}
