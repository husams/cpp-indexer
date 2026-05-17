//! USR-map spill layer — transparent HashMap / RocksDB backend (AC-M3-12).
//!
//! ## Design
//!
//! [`UsrMap`] is an enum-dispatch wrapper that presents a uniform
//! `get/insert/contains_key` interface over two backends:
//!
//! - **`InMemory`**: A plain `HashMap<String, NodeMeta>`.  Used when the
//!   estimated footprint stays below the spill threshold.
//! - **`Spilled`**: A RocksDB instance under `.cxg-cache/usr_map.rocks`.  All
//!   key-value pairs written so far are migrated to RocksDB when the threshold
//!   is first crossed; subsequent `insert`s go directly to RocksDB.
//!
//! ## Threshold calculation
//!
//! We estimate the in-memory footprint as:
//!
//! ```text
//! entries × (avg_key_bytes + size_of::<NodeMeta>() + PER_ENTRY_OVERHEAD)
//! ```
//!
//! where `avg_key_bytes` is updated incrementally as the running average of all
//! keys inserted so far, and `PER_ENTRY_OVERHEAD = 64` bytes accounts for
//! HashMap slot and pointer bookkeeping.  This is a conservative over-estimate;
//! it is intentionally not exact because:
//!
//! 1. The actual per-slot cost depends on the HashMap load-factor and system
//!    allocator alignment, which we cannot query portably.
//! 2. Over-estimating is safe: we spill slightly earlier than strictly
//!    necessary, which only costs RocksDB startup latency.
//!
//! ## Spill path
//!
//! The RocksDB directory is placed at `<stage_dir>/.cxg-cache/usr_map.rocks`
//! (design.md §Phase 3, ADR-7 §Memory budget).  The caller supplies
//! `stage_dir` so the path is co-located with the staging artefacts.
//!
//! ## Threshold injection
//!
//! Production code passes [`DEFAULT_SPILL_THRESHOLD_BYTES`] (8 GiB).  Tests
//! pass a small value (e.g. 1 byte) to trigger the spill path without actually
//! allocating 8 GiB.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use rocksdb::{Options, WriteBatch, DB};
use tracing::info;

use crate::resolve::per_repo::NodeMeta;
use crate::{Error, Result};

// ---------------------------------------------------------------------------
// Public constants
// ---------------------------------------------------------------------------

/// Default spill threshold: 8 GiB (AC-M3-12, ADR-7 §Memory budget).
pub const DEFAULT_SPILL_THRESHOLD_BYTES: usize = 8 * 1024 * 1024 * 1024;

/// Per-entry overhead added to the size estimate to account for HashMap slot
/// bookkeeping (raw pointer + next-slot pointer + alignment padding).
const PER_ENTRY_OVERHEAD: usize = 64;

// ---------------------------------------------------------------------------
// RocksDB serialisation helpers for NodeMeta
// ---------------------------------------------------------------------------

/// Serialise `NodeMeta` to bytes: `kind_str\x00repo_name`.
///
/// Uses the canonical Arrow string for `kind` (e.g. `"FUNCTION"`) so the
/// stored bytes are human-readable when inspected with `rocksdb-ldb`.
fn meta_to_bytes(meta: &NodeMeta) -> Vec<u8> {
    let kind_str = meta.kind.as_str();
    let mut buf = Vec::with_capacity(kind_str.len() + 1 + meta.repo_name.len());
    buf.extend_from_slice(kind_str.as_bytes());
    buf.push(0); // NUL separator
    buf.extend_from_slice(meta.repo_name.as_bytes());
    buf
}

/// Deserialise `NodeMeta` from bytes produced by [`meta_to_bytes`].
fn meta_from_bytes(bytes: &[u8]) -> Option<NodeMeta> {
    let sep = bytes.iter().position(|&b| b == 0)?;
    let kind_str = std::str::from_utf8(&bytes[..sep]).ok()?;
    let repo_name = std::str::from_utf8(&bytes[sep + 1..]).ok()?.to_owned();
    let kind = crate::schema::NodeKind::try_from_arrow_str(kind_str)?;
    Some(NodeMeta { kind, repo_name })
}

// ---------------------------------------------------------------------------
// UsrMap
// ---------------------------------------------------------------------------

/// Transparent USR map that spills to RocksDB when the in-memory estimate
/// exceeds the configured threshold.
pub enum UsrMap {
    /// Below threshold — all data in a `HashMap`.
    InMemory {
        map: HashMap<String, NodeMeta>,
        /// Running sum of all key lengths inserted so far.
        total_key_bytes: usize,
        /// Spill threshold in bytes.
        threshold: usize,
        /// Path used when a transition to RocksDB is needed.
        rocks_path: PathBuf,
    },
    /// Threshold crossed — data live in RocksDB.
    Spilled { db: DB },
}

impl UsrMap {
    /// Create a new in-memory map.
    ///
    /// - `stage_dir`: staging directory; the RocksDB path is derived from it as
    ///   `<stage_dir>/.cxg-cache/usr_map.rocks`.
    /// - `threshold`: byte threshold above which the map spills.  Pass
    ///   [`DEFAULT_SPILL_THRESHOLD_BYTES`] in production; pass a small value
    ///   in tests.
    pub fn new(stage_dir: &Path, threshold: usize) -> Self {
        UsrMap::InMemory {
            map: HashMap::new(),
            total_key_bytes: 0,
            threshold,
            rocks_path: stage_dir.join(".cxg-cache/usr_map.rocks"),
        }
    }

    /// Insert a `(usr, meta)` pair, spilling to RocksDB if necessary.
    ///
    /// After the spill transition, all future inserts go directly to RocksDB.
    ///
    /// # Errors
    /// Returns `Error::Cache` if RocksDB initialisation or a write fails.
    pub fn insert(&mut self, usr: String, meta: NodeMeta) -> Result<()> {
        match self {
            UsrMap::InMemory {
                map,
                total_key_bytes,
                threshold,
                rocks_path,
            } => {
                *total_key_bytes += usr.len();
                // n+1 because we are inserting into the map below.
                let entries = map.len() + 1;
                let avg_key = *total_key_bytes / entries;
                let estimate = estimated_bytes(entries, avg_key);

                map.insert(usr, meta);

                if estimate > *threshold {
                    info!(
                        estimate_bytes = estimate,
                        threshold = *threshold,
                        entries = map.len(),
                        "USR map exceeded threshold; spilling to RocksDB"
                    );
                    let path = rocks_path.clone();
                    let existing = std::mem::take(map);
                    *self = spill_to_rocksdb(existing, &path)?;
                }
            }
            UsrMap::Spilled { db } => {
                db.put(usr.as_bytes(), meta_to_bytes(&meta))
                    .map_err(|e| Error::Cache(format!("RocksDB put: {e}")))?;
            }
        }
        Ok(())
    }

    /// Returns `true` if `usr` exists in the map (either backend).
    ///
    /// # Errors
    /// Returns `Error::Cache` if a RocksDB read fails.
    pub fn contains_key(&self, usr: &str) -> Result<bool> {
        match self {
            UsrMap::InMemory { map, .. } => Ok(map.contains_key(usr)),
            UsrMap::Spilled { db } => {
                let v = db
                    .get(usr.as_bytes())
                    .map_err(|e| Error::Cache(format!("RocksDB get: {e}")))?;
                Ok(v.is_some())
            }
        }
    }

    /// Retrieve metadata for `usr`, or `None` if not found.
    ///
    /// # Errors
    /// Returns `Error::Cache` if a RocksDB read or deserialisation fails.
    pub fn get(&self, usr: &str) -> Result<Option<NodeMeta>> {
        match self {
            UsrMap::InMemory { map, .. } => Ok(map.get(usr).cloned()),
            UsrMap::Spilled { db } => {
                let opt = db
                    .get(usr.as_bytes())
                    .map_err(|e| Error::Cache(format!("RocksDB get: {e}")))?;
                match opt {
                    None => Ok(None),
                    Some(bytes) => {
                        let meta = meta_from_bytes(&bytes)
                            .ok_or_else(|| Error::Cache("RocksDB value corrupt".to_owned()))?;
                        Ok(Some(meta))
                    }
                }
            }
        }
    }

    /// Returns `true` when the map has spilled to RocksDB.
    ///
    /// Available in tests only; production code does not rely on this for
    /// correctness.
    #[cfg(test)]
    pub fn is_spilled(&self) -> bool {
        matches!(self, UsrMap::Spilled { .. })
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Estimate total heap bytes for `n` entries whose average key length is `avg_key`.
#[inline]
fn estimated_bytes(n: usize, avg_key: usize) -> usize {
    n.saturating_mul(avg_key + std::mem::size_of::<NodeMeta>() + PER_ENTRY_OVERHEAD)
}

/// Migrate `existing` HashMap into a fresh RocksDB at `path`.
fn spill_to_rocksdb(existing: HashMap<String, NodeMeta>, path: &Path) -> Result<UsrMap> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }

    let mut opts = Options::default();
    opts.create_if_missing(true);
    // Tune for write-heavy migration then read-heavy lookups.
    opts.set_write_buffer_size(64 * 1024 * 1024); // 64 MiB
    opts.set_max_write_buffer_number(3);

    let db = DB::open(&opts, path)
        .map_err(|e| Error::Cache(format!("open RocksDB at {}: {e}", path.display())))?;

    const BATCH_SIZE: usize = 4096;
    let mut batch = WriteBatch::default();
    let mut count = 0usize;

    for (usr, meta) in existing {
        batch.put(usr.as_bytes(), meta_to_bytes(&meta));
        count += 1;
        if count.is_multiple_of(BATCH_SIZE) {
            db.write(std::mem::take(&mut batch))
                .map_err(|e| Error::Cache(format!("RocksDB batch write during spill: {e}")))?;
            batch = WriteBatch::default();
        }
    }
    // Flush the final partial batch.
    if !count.is_multiple_of(BATCH_SIZE) || count == 0 {
        db.write(batch)
            .map_err(|e| Error::Cache(format!("RocksDB final batch write: {e}")))?;
    }

    info!(entries = count, path = %path.display(), "USR map spill complete");
    Ok(UsrMap::Spilled { db })
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::schema::NodeKind;

    fn meta(kind: NodeKind) -> NodeMeta {
        NodeMeta {
            kind,
            repo_name: "test-repo".to_owned(),
        }
    }

    // -----------------------------------------------------------------------
    // Serialisation round-trip
    // -----------------------------------------------------------------------

    #[test]
    fn meta_roundtrip_function() {
        let m = meta(NodeKind::Function);
        let bytes = meta_to_bytes(&m);
        let back = meta_from_bytes(&bytes).expect("deserialise");
        assert_eq!(back.kind, NodeKind::Function);
        assert_eq!(back.repo_name, "test-repo");
    }

    #[test]
    fn meta_roundtrip_class() {
        let m = meta(NodeKind::Class);
        let bytes = meta_to_bytes(&m);
        let back = meta_from_bytes(&bytes).expect("deserialise");
        assert_eq!(back.kind, NodeKind::Class);
    }

    #[test]
    fn meta_from_bytes_empty_returns_none() {
        assert!(meta_from_bytes(&[]).is_none());
    }

    // -----------------------------------------------------------------------
    // In-memory happy path (below threshold)
    // -----------------------------------------------------------------------

    #[test]
    fn insert_and_contains_key_in_memory() {
        let dir = tempfile::tempdir().unwrap();
        let mut map = UsrMap::new(dir.path(), DEFAULT_SPILL_THRESHOLD_BYTES);

        map.insert("c:@F@fn_a".to_owned(), meta(NodeKind::Function))
            .unwrap();
        map.insert("c:@S@ClassB".to_owned(), meta(NodeKind::Class))
            .unwrap();

        assert!(map.contains_key("c:@F@fn_a").unwrap());
        assert!(map.contains_key("c:@S@ClassB").unwrap());
        assert!(!map.contains_key("c:@F@missing").unwrap());
        assert!(!map.is_spilled());
    }

    #[test]
    fn get_returns_correct_meta_in_memory() {
        let dir = tempfile::tempdir().unwrap();
        let mut map = UsrMap::new(dir.path(), DEFAULT_SPILL_THRESHOLD_BYTES);

        map.insert("c:@F@fn_a".to_owned(), meta(NodeKind::Function))
            .unwrap();

        let got = map.get("c:@F@fn_a").unwrap().expect("should be present");
        assert_eq!(got.kind, NodeKind::Function);
        assert_eq!(got.repo_name, "test-repo");
        assert!(map.get("c:@F@missing").unwrap().is_none());
    }

    // -----------------------------------------------------------------------
    // Spill threshold triggered (mock the size check with tiny threshold)
    // -----------------------------------------------------------------------

    /// Use a threshold of 1 byte so even 1 entry triggers the spill path.
    /// This simulates the 8 GB synthetic-map condition without allocating memory.
    #[test]
    fn spill_triggered_by_low_threshold() {
        let dir = tempfile::tempdir().unwrap();
        let mut map = UsrMap::new(dir.path(), 1);

        map.insert("c:@F@fn_a".to_owned(), meta(NodeKind::Function))
            .unwrap();

        assert!(map.is_spilled(), "map should have spilled to RocksDB");
    }

    /// Data inserted before the threshold must be readable after the spill.
    #[test]
    fn data_survives_spill() {
        let dir = tempfile::tempdir().unwrap();
        // threshold=1 overflows immediately; all pre-spill data migrates.
        let mut map = UsrMap::new(dir.path(), 1);

        let keys = ["c:@F@fn_0", "c:@F@fn_1", "c:@F@fn_2"];
        for k in &keys {
            map.insert((*k).to_owned(), meta(NodeKind::Function))
                .unwrap();
        }

        assert!(map.is_spilled());
        for k in &keys {
            assert!(map.contains_key(k).unwrap(), "key {k} must survive spill");
        }
    }

    /// Inserts after the spill point must also be readable.
    #[test]
    fn insert_after_spill_readable() {
        let dir = tempfile::tempdir().unwrap();
        let mut map = UsrMap::new(dir.path(), 1);

        // First insert triggers spill.
        map.insert("c:@F@fn_pre".to_owned(), meta(NodeKind::Function))
            .unwrap();
        assert!(map.is_spilled());

        // Subsequent inserts go directly to RocksDB.
        map.insert("c:@F@fn_post".to_owned(), meta(NodeKind::Class))
            .unwrap();

        assert!(map.contains_key("c:@F@fn_pre").unwrap());
        assert!(map.contains_key("c:@F@fn_post").unwrap());
        let got = map.get("c:@F@fn_post").unwrap().expect("present");
        assert_eq!(got.kind, NodeKind::Class);
    }

    // -----------------------------------------------------------------------
    // estimated_bytes helper
    // -----------------------------------------------------------------------

    #[test]
    fn estimated_bytes_zero_entries() {
        assert_eq!(estimated_bytes(0, 32), 0);
    }

    #[test]
    fn estimated_bytes_grows_linearly() {
        let per = std::mem::size_of::<NodeMeta>() + PER_ENTRY_OVERHEAD + 32;
        assert_eq!(estimated_bytes(1, 32), per);
        assert_eq!(estimated_bytes(10, 32), 10 * per);
    }
}
