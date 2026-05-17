/// Parquet staging layer (Phase 1 output): per-worker shard writers and manifest.
///
/// Stories: S07-stage-parquet (AC-M1-14, AC-M1-17, AC-M3-7 skeleton).
/// ADR: ADR-3 (schema + disk layout).
///
/// ## Typical usage
///
/// ```rust,ignore
/// use cpp_indexer::stage::writer::StageWriter;
/// use cpp_indexer::stage::manifest::Manifest;
///
/// let mut writer = StageWriter::new(&stage_dir, worker_id)?;
/// writer.write_nodes(&node_records)?;
/// writer.write_edges(&edge_records)?;
/// let shards = writer.finish()?;
///
/// let mut manifest = Manifest::load(&manifest_path)?;
/// manifest.append_and_save(&manifest_path, entry)?;
/// ```
pub mod manifest;
pub mod schema;
pub mod writer;
