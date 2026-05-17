//! Phase 0 — parse and dedup `compile_commands.json`.
//!
//! Design: §Phase 0
//! AC: AC-M1-5 (parse), AC-M1-6 (dedup), AC-M1-7 (error with path).

use std::collections::HashSet;
use std::path::{Path, PathBuf};

use serde::Deserialize;

use crate::{Error, Result};

/// A single translation unit entry, deduplicated and ready for Phase 1.
#[derive(Debug, Clone)]
pub struct TuEntry {
    /// Canonicalised absolute path to the source file.
    pub file: PathBuf,
    /// Compiler arguments (from `"arguments"` list or split `"command"` string).
    pub args: Vec<String>,
    /// `blake3(file_canonical || NUL || args.join(NUL))` — stable dedup key.
    pub hash: blake3::Hash,
}

/// Raw JSON record from `compile_commands.json` (either form per JSON Compilation Database spec).
#[derive(Debug, Deserialize)]
struct RawEntry {
    /// Working directory; used to resolve relative `file` paths.
    directory: String,
    /// Source file path (may be relative to `directory`).
    file: String,
    /// Shell-quoted command string (mutually exclusive with `arguments`).
    command: Option<String>,
    /// Pre-split argument list (preferred form).
    arguments: Option<Vec<String>>,
}

/// Parse `compile_commands.json` at `path` and return a deduplicated `Vec<TuEntry>`.
///
/// Dedup key: `blake3(canonical_file || 0x00 || args.join(0x00))`.
/// Entries with the same key are silently collapsed to the first occurrence.
///
/// # Errors
/// Returns [`Error::CompileCommands`] if the file cannot be read or contains invalid JSON.
pub fn parse(path: &Path) -> Result<Vec<TuEntry>> {
    let content = std::fs::read_to_string(path).map_err(|e| Error::CompileCommands {
        path: path.to_owned(),
        message: e.to_string(),
    })?;

    let raw: Vec<RawEntry> =
        serde_json::from_str(&content).map_err(|e| Error::CompileCommands {
            path: path.to_owned(),
            message: e.to_string(),
        })?;

    let mut seen: HashSet<[u8; 32]> = HashSet::new();
    let mut entries = Vec::with_capacity(raw.len());

    for record in raw {
        let args = resolve_args(&record);
        let canonical_file = resolve_file(&record.directory, &record.file);
        let hash = compute_hash(&canonical_file, &args);

        if seen.insert(*hash.as_bytes()) {
            entries.push(TuEntry {
                file: canonical_file,
                args,
                hash,
            });
        }
    }

    Ok(entries)
}

/// Resolve `file` relative to `directory` and canonicalise the path.
/// Falls back to the joined path (without canonicalisation) when the file does not exist on disk,
/// which is acceptable for unit tests using synthetic paths.
fn resolve_file(directory: &str, file: &str) -> PathBuf {
    let raw = Path::new(file);
    let joined = if raw.is_absolute() {
        raw.to_owned()
    } else {
        Path::new(directory).join(raw)
    };
    // Best-effort canonicalise; fall back to the joined path (e.g. in tests).
    joined.canonicalize().unwrap_or(joined)
}

/// Produce the args list from either the `arguments` field (preferred) or
/// the `command` field (split on ASCII whitespace).
fn resolve_args(record: &RawEntry) -> Vec<String> {
    if let Some(args) = &record.arguments {
        return args.clone();
    }
    if let Some(cmd) = &record.command {
        return cmd.split_whitespace().map(str::to_owned).collect();
    }
    Vec::new()
}

/// Compute the dedup hash: `blake3(canonical_file || NUL || args.join(NUL))`.
fn compute_hash(file: &Path, args: &[String]) -> blake3::Hash {
    let mut hasher = blake3::Hasher::new();
    hasher.update(file.as_os_str().as_encoded_bytes());
    hasher.update(b"\x00");
    for (i, arg) in args.iter().enumerate() {
        if i > 0 {
            hasher.update(b"\x00");
        }
        hasher.update(arg.as_bytes());
    }
    hasher.finalize()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write as _;
    use tempfile::NamedTempFile;

    fn write_temp(content: &str) -> NamedTempFile {
        let mut f = NamedTempFile::new().expect("tempfile");
        f.write_all(content.as_bytes()).expect("write");
        f
    }

    // (a) parse valid file → expected Vec<TuEntry>
    #[test]
    fn parse_valid_arguments_form() {
        let json = r#"[
            {
                "directory": "/tmp",
                "file": "/tmp/foo.cpp",
                "arguments": ["/usr/bin/c++", "-std=c++17", "-c", "/tmp/foo.cpp"]
            }
        ]"#;
        let f = write_temp(json);
        let entries = parse(f.path()).expect("parse");
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].file, PathBuf::from("/tmp/foo.cpp"));
        assert_eq!(entries[0].args[0], "/usr/bin/c++");
    }

    #[test]
    fn parse_valid_command_form() {
        let json = r#"[
            {
                "directory": "/tmp",
                "file": "/tmp/bar.cpp",
                "command": "/usr/bin/c++ -std=c++17 -c /tmp/bar.cpp"
            }
        ]"#;
        let f = write_temp(json);
        let entries = parse(f.path()).expect("parse");
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].file, PathBuf::from("/tmp/bar.cpp"));
        assert!(entries[0].args.contains(&"-std=c++17".to_owned()));
    }

    // (b) duplicate (file, args) entries collapse to one
    #[test]
    fn dedup_collapses_identical_entries() {
        let json = r#"[
            {
                "directory": "/tmp",
                "file": "/tmp/foo.cpp",
                "arguments": ["/usr/bin/c++", "-c", "/tmp/foo.cpp"]
            },
            {
                "directory": "/tmp",
                "file": "/tmp/foo.cpp",
                "arguments": ["/usr/bin/c++", "-c", "/tmp/foo.cpp"]
            }
        ]"#;
        let f = write_temp(json);
        let entries = parse(f.path()).expect("parse");
        assert_eq!(entries.len(), 1, "duplicate entries must collapse to one");
    }

    // Two entries that differ only in args must both be kept
    #[test]
    fn dedup_keeps_distinct_args() {
        let json = r#"[
            {
                "directory": "/tmp",
                "file": "/tmp/foo.cpp",
                "arguments": ["/usr/bin/c++", "-DFOO", "-c", "/tmp/foo.cpp"]
            },
            {
                "directory": "/tmp",
                "file": "/tmp/foo.cpp",
                "arguments": ["/usr/bin/c++", "-DBAR", "-c", "/tmp/foo.cpp"]
            }
        ]"#;
        let f = write_temp(json);
        let entries = parse(f.path()).expect("parse");
        assert_eq!(entries.len(), 2, "distinct-args entries must both be kept");
    }

    // (c) malformed JSON → Error::CompileCommands with file path
    #[test]
    fn malformed_json_returns_error_with_path() {
        let f = write_temp("not json at all {{{");
        let err = parse(f.path()).expect_err("should fail on malformed JSON");
        match err {
            Error::CompileCommands { path, .. } => {
                assert_eq!(path, f.path());
            }
            other => panic!("expected Error::CompileCommands, got {other:?}"),
        }
    }

    // (d) Blake3 hash stable across runs (same input → same hash every time)
    #[test]
    fn hash_stable_across_calls() {
        let json = r#"[
            {
                "directory": "/tmp",
                "file": "/tmp/stable.cpp",
                "arguments": ["/usr/bin/c++", "-c", "/tmp/stable.cpp"]
            }
        ]"#;
        let f = write_temp(json);
        let first = parse(f.path()).expect("parse 1");
        let second = parse(f.path()).expect("parse 2");
        assert_eq!(
            first[0].hash, second[0].hash,
            "hash must be deterministic across calls"
        );
    }
}
