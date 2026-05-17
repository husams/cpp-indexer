//! Phase 0.5 — upward-walk auto-detection of `compile_commands.json`.
//!
//! Starting from `input_path`, walks upward through the filesystem. At each
//! directory level it probes:
//!   1. `<dir>/compile_commands.json` (direct)
//!   2. `<dir>/build/compile_commands.json`
//!   3. `<dir>/out/compile_commands.json`
//!   4. `<dir>/cmake-build-*/compile_commands.json` (glob)
//!
//! The walk stops when a `.git` directory is found at the current level, or
//! when the filesystem root is reached. On miss the function returns
//! [`Error::Autodetect`] listing every directory probed.
//!
//! When multiple candidates exist at the same directory level:
//! - If `input_path` is a file, prefer the candidate whose `compile_commands.json`
//!   lists that file in its `"file"` entries.
//! - Break remaining ties lexicographically.
//!
//! AC: AC-M1-8, AC-M1-9, AC-M1-10, AC-M1-11, AC-M1-12, AC-M1-13.

use std::path::{Path, PathBuf};

use glob::glob;
use tracing::info;

use crate::{Error, Result};

/// Resolve the path to `compile_commands.json` by walking upward from
/// `input_path`.
///
/// `input_path` may be a file or a directory.
///
/// Returns the absolute, canonicalised path to the chosen
/// `compile_commands.json` on success.
pub fn find_compile_commands(input_path: &Path) -> Result<PathBuf> {
    // Determine the starting directory.
    let start_dir: PathBuf = if input_path.is_file() {
        input_path.parent().unwrap_or(Path::new(".")).to_path_buf()
    } else {
        input_path.to_path_buf()
    };

    // `input_file` is Some(path) only when the caller provided a file.
    let input_file: Option<PathBuf> = if input_path.is_file() {
        Some(
            input_path
                .canonicalize()
                .unwrap_or_else(|_| input_path.to_path_buf()),
        )
    } else {
        None
    };

    let mut searched: Vec<PathBuf> = Vec::new();
    let mut current = start_dir.canonicalize().map_err(Error::Io)?;

    loop {
        searched.push(current.clone());

        // Stop if we see a .git directory at this level (AC-M1-10).
        if current.join(".git").is_dir() {
            // Check this level before stopping — .git means this is the repo
            // root; we still probe it before giving up.
            if let Some(resolved) = probe_level(&current, &input_file)? {
                info!(path = %resolved.display(), "resolved compile_commands.json");
                return Ok(resolved);
            }
            break;
        }

        if let Some(resolved) = probe_level(&current, &input_file)? {
            info!(path = %resolved.display(), "resolved compile_commands.json");
            return Ok(resolved);
        }

        // Move one level up.
        match current.parent() {
            Some(parent) if parent != current => {
                current = parent.to_path_buf();
            }
            _ => break, // reached FS root
        }
    }

    Err(Error::Autodetect { searched })
}

/// Probe all candidate locations within a single directory level.
/// Returns `Some(path)` if exactly one or more candidates exist; applies
/// preference rules when multiple candidates are found.
fn probe_level(dir: &Path, input_file: &Option<PathBuf>) -> Result<Option<PathBuf>> {
    let mut candidates: Vec<PathBuf> = Vec::new();

    // 1. Direct: <dir>/compile_commands.json
    let direct = dir.join("compile_commands.json");
    if direct.is_file() {
        candidates.push(direct);
    }

    // 2. build/ subdirectory
    let build = dir.join("build").join("compile_commands.json");
    if build.is_file() {
        candidates.push(build);
    }

    // 3. out/ subdirectory
    let out = dir.join("out").join("compile_commands.json");
    if out.is_file() {
        candidates.push(out);
    }

    // 4. cmake-build-*/ subdirectories (glob)
    let pattern = format!("{}/cmake-build-*/compile_commands.json", dir.display());
    if let Ok(paths) = glob(&pattern) {
        for entry in paths.flatten() {
            if entry.is_file() {
                candidates.push(entry);
            }
        }
    }

    if candidates.is_empty() {
        return Ok(None);
    }

    // Sort lexicographically for deterministic tie-breaking.
    candidates.sort();

    // When input is a single file, prefer a candidate that lists it (AC-M1-11).
    if let Some(ref file) = *input_file {
        for candidate in &candidates {
            if candidate_lists_file(candidate, file)? {
                return Ok(Some(candidate.clone()));
            }
        }
    }

    // Fallback: lexicographically first.
    Ok(Some(candidates.into_iter().next().unwrap()))
}

/// Returns `true` if `cc_path`'s JSON entry list contains `file`.
///
/// Reads only the `"file"` fields; ignores malformed JSON silently (returns
/// `false`) so a bad candidate does not abort the search.
fn candidate_lists_file(cc_path: &Path, file: &Path) -> Result<bool> {
    let content = std::fs::read_to_string(cc_path).map_err(Error::Io)?;
    let entries: serde_json::Value = match serde_json::from_str(&content) {
        Ok(v) => v,
        Err(_) => return Ok(false),
    };
    let Some(arr) = entries.as_array() else {
        return Ok(false);
    };
    for entry in arr {
        let Some(f) = entry.get("file").and_then(|v| v.as_str()) else {
            continue;
        };
        let candidate_file = PathBuf::from(f);
        // Compare canonicalised paths where possible; fall back to raw
        // comparison if either path cannot be resolved (e.g. in tests using
        // synthesised paths).
        let a = candidate_file.canonicalize().unwrap_or(candidate_file);
        let b = file.canonicalize().unwrap_or_else(|_| file.to_path_buf());
        if a == b {
            return Ok(true);
        }
    }
    Ok(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use tempfile::tempdir;

    fn write_cc(path: &Path, files: &[&str]) {
        let entries: Vec<serde_json::Value> = files
            .iter()
            .map(|f| {
                serde_json::json!({
                    "directory": "/tmp",
                    "command": "clang++ -c foo.cpp",
                    "file": f
                })
            })
            .collect();
        fs::write(path, serde_json::to_string(&entries).unwrap()).unwrap();
    }

    #[test]
    fn direct_at_same_level() {
        let dir = tempdir().unwrap();
        let cc = dir.path().join("compile_commands.json");
        write_cc(&cc, &[]);
        let result = find_compile_commands(dir.path()).unwrap();
        assert_eq!(result, cc.canonicalize().unwrap());
    }

    #[test]
    fn no_cc_returns_err_with_paths() {
        let dir = tempdir().unwrap();
        let err = find_compile_commands(dir.path()).unwrap_err();
        let msg = err.to_string();
        assert!(
            msg.contains(dir.path().to_str().unwrap()),
            "error should list searched dir; got: {msg}"
        );
    }
}
