/// Parquet staging manifest (ADR-3, AC-M3-7 skeleton).
///
/// The manifest is written per-TU completion with `fsync` + atomic rename so an interrupted
/// run can resume without reading corrupt/partial entries. Phase 3 reads the manifest to
/// discover shards and perform incremental cache invalidation by `tu_hash`.
///
/// This module owns the struct definitions and JSON round-trip; the incremental append logic
/// is implemented in `crate::stage::writer` and deferred fully to M3.
use std::{
    fs::{self, File},
    io::{BufReader, BufWriter, Write as _},
    path::{Path, PathBuf},
};

use serde::{Deserialize, Serialize};

use crate::schema::version::{PARQUET_MAGIC, SCHEMA_VERSION};
use crate::Result;

// ---------------------------------------------------------------------------
// Manifest types
// ---------------------------------------------------------------------------

/// Per-TU entry in `manifest.json`.
///
/// Stored as a JSON array so the file is human-readable and independently parseable by
/// Phase 3 without the full binary.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ManifestEntry {
    /// Blake3 hash of the source file bytes at parse time (hex).
    pub source_hash: String,
    /// Blake3 hash of the compiler arguments string (hex).
    pub args_hash: String,
    /// libclang version string (e.g. `"18.1.0"`).
    pub libclang_version: String,
    /// Schema version integer matching `SCHEMA_VERSION`.
    pub schema_version: u32,
    /// Relative paths (from the run staging dir) of shards written for this TU.
    pub output_shards: Vec<PathBuf>,
}

impl ManifestEntry {
    /// Constructs an entry with `schema_version` fixed to the crate constant.
    pub fn new(
        source_hash: String,
        args_hash: String,
        libclang_version: String,
        output_shards: Vec<PathBuf>,
    ) -> Self {
        Self {
            source_hash,
            args_hash,
            libclang_version,
            schema_version: SCHEMA_VERSION,
            output_shards,
        }
    }
}

// ---------------------------------------------------------------------------
// Manifest file I/O
// ---------------------------------------------------------------------------

/// In-memory representation of `manifest.json`.
#[derive(Debug, Default, Clone, PartialEq, Serialize, Deserialize)]
pub struct Manifest {
    /// Magic tag so Phase 3 can refuse wrong-version manifests.
    pub magic: String,
    /// Schema version of the run that produced this manifest.
    pub schema_version: u32,
    /// One entry per completed TU.
    pub entries: Vec<ManifestEntry>,
}

impl Manifest {
    /// Creates an empty manifest with the current magic and schema version.
    pub fn new() -> Self {
        Self {
            magic: PARQUET_MAGIC.to_owned(),
            schema_version: SCHEMA_VERSION,
            entries: Vec::new(),
        }
    }

    /// Deserialises a manifest from `path`. Returns `Ok(Manifest::new())` when the file does not
    /// exist (fresh run).
    ///
    /// # Errors
    /// Returns `Error::Io` on read errors, `Error::Schema` on JSON parse failures or version
    /// mismatches.
    pub fn load(path: &Path) -> Result<Self> {
        if !path.exists() {
            return Ok(Self::new());
        }
        let file = File::open(path)?;
        let reader = BufReader::new(file);
        let manifest: Self = serde_json::from_reader(reader)
            .map_err(|e| crate::Error::Schema(format!("manifest parse error: {e}")))?;
        if manifest.magic != PARQUET_MAGIC {
            return Err(crate::Error::Schema(format!(
                "manifest magic mismatch: expected '{}', got '{}'",
                PARQUET_MAGIC, manifest.magic,
            )));
        }
        if manifest.schema_version != SCHEMA_VERSION {
            return Err(crate::Error::Schema(format!(
                "manifest schema_version mismatch: expected {SCHEMA_VERSION}, got {}",
                manifest.schema_version,
            )));
        }
        Ok(manifest)
    }

    /// Appends `entry` and persists atomically to `path` via `fsync` + rename.
    ///
    /// Uses a `.tmp` sibling file to avoid leaving a half-written manifest on crash.
    ///
    /// # Errors
    /// Returns `Error::Io` on any filesystem failure.
    pub fn append_and_save(&mut self, path: &Path, entry: ManifestEntry) -> Result<()> {
        self.entries.push(entry);
        self.save(path)
    }

    /// Persists the manifest atomically to `path`.
    ///
    /// Write → fsync → rename so readers always see either old or new file, never a partial write.
    ///
    /// # Errors
    /// Returns `Error::Io` on any filesystem failure.
    pub fn save(&self, path: &Path) -> Result<()> {
        let tmp = path.with_extension("json.tmp");
        {
            let file = File::create(&tmp)?;
            let mut writer = BufWriter::new(&file);
            serde_json::to_writer_pretty(&mut writer, self)
                .map_err(|e| crate::Error::Schema(format!("manifest serialise error: {e}")))?;
            writer.flush()?;
            // fsync before rename so crash cannot leave an empty file.
            file.sync_all()?;
        }
        fs::rename(&tmp, path)?;
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_entry(suffix: &str) -> ManifestEntry {
        ManifestEntry::new(
            format!("src_hash_{suffix}"),
            format!("args_hash_{suffix}"),
            "18.1.0".to_owned(),
            vec![
                PathBuf::from(format!("worker-000/nodes-0001_{suffix}.parquet")),
                PathBuf::from(format!("worker-000/edges-0001_{suffix}.parquet")),
            ],
        )
    }

    #[test]
    fn manifest_entry_new_sets_schema_version() {
        let e = sample_entry("a");
        assert_eq!(e.schema_version, SCHEMA_VERSION);
    }

    #[test]
    fn manifest_json_round_trip() {
        let mut m = Manifest::new();
        m.entries.push(sample_entry("x"));
        m.entries.push(sample_entry("y"));

        let json = serde_json::to_string(&m).unwrap();
        let decoded: Manifest = serde_json::from_str(&json).unwrap();
        assert_eq!(decoded, m);
    }

    #[test]
    fn manifest_magic_and_version_in_json() {
        let m = Manifest::new();
        let json = serde_json::to_string(&m).unwrap();
        assert!(json.contains(PARQUET_MAGIC), "magic must appear in JSON");
        assert!(
            json.contains(&SCHEMA_VERSION.to_string()),
            "schema_version must appear in JSON"
        );
    }

    #[test]
    fn manifest_save_and_load_round_trip() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("manifest.json");

        let mut m = Manifest::new();
        m.append_and_save(&path, sample_entry("a")).unwrap();
        m.append_and_save(&path, sample_entry("b")).unwrap();

        let loaded = Manifest::load(&path).unwrap();
        assert_eq!(loaded.entries.len(), 2);
        assert_eq!(loaded, m);
    }

    #[test]
    fn manifest_load_missing_returns_empty() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("no_such_manifest.json");
        let m = Manifest::load(&path).unwrap();
        assert!(m.entries.is_empty());
        assert_eq!(m.magic, PARQUET_MAGIC);
    }

    #[test]
    fn manifest_load_magic_mismatch_errors() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("manifest.json");

        let bad = serde_json::json!({
            "magic": "wrong_magic",
            "schema_version": SCHEMA_VERSION,
            "entries": []
        });
        fs::write(&path, serde_json::to_string(&bad).unwrap()).unwrap();

        let err = Manifest::load(&path).unwrap_err();
        assert!(err.to_string().contains("magic mismatch"));
    }

    #[test]
    fn manifest_load_version_mismatch_errors() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("manifest.json");

        let bad = serde_json::json!({
            "magic": PARQUET_MAGIC,
            "schema_version": 999u32,
            "entries": []
        });
        fs::write(&path, serde_json::to_string(&bad).unwrap()).unwrap();

        let err = Manifest::load(&path).unwrap_err();
        assert!(err.to_string().contains("schema_version mismatch"));
    }

    #[test]
    fn manifest_tmp_file_cleaned_up_on_success() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("manifest.json");

        let m = Manifest::new();
        m.save(&path).unwrap();

        let tmp = path.with_extension("json.tmp");
        assert!(
            !tmp.exists(),
            ".tmp file must be removed after atomic rename"
        );
    }
}
