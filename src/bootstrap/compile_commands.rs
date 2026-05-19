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
        let raw_args = resolve_args(&record);
        let canonical_file = resolve_file(&record.directory, &record.file);
        let directory = Path::new(&record.directory);
        let args = sanitize_libclang_args(&raw_args, &canonical_file, directory);
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

/// Return `true` if `token` looks like a compiler-driver basename.
///
/// Match is performed on the **basename only** (never a path-substring) so that
/// `/usr/bin/gcc` → basename `gcc` matches, while `foo.cc` → basename `foo.cc`
/// does not.
///
/// Matched literal stems: `cc | c++ | clang | clang++ | gcc | g++`.
/// Matched suffix patterns: `*-gcc | *-g++ | *-clang | *-clang++`
/// (covers cross-compiler triplets like `arm-linux-gnueabi-g++`).
/// Matched versioned patterns: `<driver>-<N>[.<N>]*` where `<driver>` is one of the
/// six literal stems above (e.g. `clang-18`, `g++-12`, `gcc-10`, `clang++-18.1`).
/// The version suffix must be all-numeric with optional dot separators so that
/// tool names like `clang-tidy` and `clang-format` are NOT matched.
fn is_driver_basename(token: &str) -> bool {
    let Some(stem) = Path::new(token).file_name().and_then(|n| n.to_str()) else {
        return false;
    };
    if matches!(stem, "cc" | "c++" | "clang" | "clang++" | "gcc" | "g++") {
        return true;
    }
    if stem.ends_with("-gcc")
        || stem.ends_with("-g++")
        || stem.ends_with("-clang")
        || stem.ends_with("-clang++")
    {
        return true;
    }
    // Versioned driver: `<known-stem>-<version>` where version matches
    // `[0-9]+(\.[0-9]+)*`.  This handles clang-18, g++-12, gcc-10,
    // clang++-18.1 while rejecting clang-tidy, clang-format (non-numeric suffix).
    is_versioned_driver(stem)
}

/// Return `true` if `stem` is a known driver stem followed by a hyphen and an
/// all-numeric version string (`[0-9]+(\.[0-9]+)*`).
fn is_versioned_driver(stem: &str) -> bool {
    const KNOWN_STEMS: &[&str] = &["cc", "c++", "clang", "clang++", "gcc", "g++"];
    for known in KNOWN_STEMS {
        // stem must be exactly `<known>-<version>`
        if let Some(rest) = stem.strip_prefix(known) {
            if let Some(version) = rest.strip_prefix('-') {
                if is_numeric_version(version) {
                    return true;
                }
            }
        }
    }
    false
}

/// Return `true` if `s` matches `[0-9]+(\.[0-9]+)*` (e.g. "18", "18.1", "10.2.3").
fn is_numeric_version(s: &str) -> bool {
    if s.is_empty() {
        return false;
    }
    for part in s.split('.') {
        if part.is_empty() || !part.bytes().all(|b| b.is_ascii_digit()) {
            return false;
        }
    }
    true
}

/// Sanitise a raw args list for consumption by libclang.
///
/// libclang's `Index::parser(file)` API expects only **flags** — it supplies
/// the driver, `-c`, and the source file itself.  CMake-emitted arg lists
/// contain all three, causing `AstDeserialization` errors (Issue 0001, Bug B).
///
/// Algorithm (single linear pass, O(n)):
///
/// 1. **Driver strip.** If `raw_args[0]` does not start with `-` and its
///    basename matches [`is_driver_basename`], drop it.
/// 2. **Pair strip.** For each remaining token: if it is `-c` or `-o`, skip it
///    **and** the following token (if any).
/// 3. **Source-repeat strip.** For each surviving non-flag token, canonicalise
///    `directory.join(token)` (best-effort; fall through to literal join when
///    the path does not exist) and drop it if it equals `canonical_file`.
///
/// All tokens starting with `-` are kept unchanged (steps 2 and 3 only inspect
/// non-`-` tokens for step 3, and only `-c`/`-o` for step 2).
fn sanitize_libclang_args(
    raw_args: &[String],
    canonical_file: &Path,
    directory: &Path,
) -> Vec<String> {
    if raw_args.is_empty() {
        return Vec::new();
    }

    // Step 1: optionally drop leading driver token.
    let rest = if !raw_args[0].starts_with('-') && is_driver_basename(&raw_args[0]) {
        &raw_args[1..]
    } else {
        raw_args
    };

    // Step 2: pair-strip -c / -o and their following argument.
    let mut after_pair_strip: Vec<&str> = Vec::with_capacity(rest.len());
    let mut i = 0;
    while i < rest.len() {
        let t = rest[i].as_str();
        if t == "-c" || t == "-o" {
            // Skip this token and the next one (if present).
            i += 2;
        } else {
            after_pair_strip.push(t);
            i += 1;
        }
    }

    // Step 3: source-repeat strip — drop non-flag tokens that resolve to
    // canonical_file.
    let mut result: Vec<String> = Vec::with_capacity(after_pair_strip.len());
    for t in after_pair_strip {
        if t.starts_with('-') {
            result.push(t.to_owned());
        } else {
            // Best-effort canonicalise: try directory.join(t), fall back to
            // literal join when the path does not exist (e.g., synthetic test
            // paths under /tmp).
            let joined = directory.join(t);
            let resolved = joined.canonicalize().unwrap_or(joined);
            if resolved != canonical_file {
                result.push(t.to_owned());
            }
        }
    }

    result
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
    // After sanitisation: driver "/usr/bin/c++" is stripped, "-c" and the
    // source-file repeat "/tmp/foo.cpp" are stripped → only "-std=c++17" remains.
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
        // Driver and noise tokens are stripped by sanitize_libclang_args.
        assert_eq!(entries[0].args, vec!["-std=c++17"]);
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

    // ---------------------------------------------------------------------------
    // Unit tests for sanitize_libclang_args (AC-2)
    // ---------------------------------------------------------------------------

    /// Helper: call sanitize_libclang_args with a synthetic absolute canonical
    /// file path and a directory of "/project".
    fn sanitize(raw: &[&str]) -> Vec<String> {
        let canonical = PathBuf::from("/project/src/foo.cpp");
        let directory = Path::new("/project");
        let raw_owned: Vec<String> = raw.iter().map(|s| s.to_string()).collect();
        sanitize_libclang_args(&raw_owned, &canonical, directory)
    }

    /// Same as `sanitize` but with a custom canonical_file.
    fn sanitize_with_file(raw: &[&str], canonical_file: &Path) -> Vec<String> {
        let directory = Path::new("/project");
        let raw_owned: Vec<String> = raw.iter().map(|s| s.to_string()).collect();
        sanitize_libclang_args(&raw_owned, canonical_file, directory)
    }

    #[test]
    fn sanitize_empty_input_returns_empty() {
        assert!(sanitize(&[]).is_empty());
    }

    #[test]
    fn sanitize_driver_only_returns_empty() {
        assert!(sanitize(&["clang++"]).is_empty());
    }

    // Table-driven: each literal driver stem must be detected and stripped (AC-2).
    #[test]
    fn sanitize_driver_stems_are_stripped() {
        let drivers = [
            "cc",
            "c++",
            "clang",
            "clang++",
            "gcc",
            "g++",
            // Absolute path forms
            "/usr/bin/clang++",
            "/usr/local/bin/gcc",
            // Cross-compiler triplet
            "arm-linux-gnueabi-g++",
        ];
        for driver in drivers {
            let result = sanitize(&[driver, "-std=c++17"]);
            assert_eq!(
                result,
                vec!["-std=c++17"],
                "driver {driver:?} should be stripped"
            );
        }
    }

    // Versioned drivers are stripped (QD-1 fix): clang-18, g++-12, gcc-10, clang++-18.1.
    #[test]
    fn sanitize_versioned_driver_clang_18_is_stripped() {
        let result = sanitize(&["clang-18", "-std=c++17"]);
        assert_eq!(
            result,
            vec!["-std=c++17"],
            "clang-18 must be stripped as a versioned driver"
        );
    }

    #[test]
    fn sanitize_versioned_driver_gpp_12_is_stripped() {
        let result = sanitize(&["g++-12", "-std=c++20", "-DNDEBUG"]);
        assert_eq!(
            result,
            vec!["-std=c++20", "-DNDEBUG"],
            "g++-12 must be stripped as a versioned driver"
        );
    }

    #[test]
    fn sanitize_versioned_driver_gcc_10_is_stripped() {
        let result = sanitize(&["gcc-10", "-std=c++14"]);
        assert_eq!(result, vec!["-std=c++14"], "gcc-10 must be stripped");
    }

    #[test]
    fn sanitize_versioned_driver_clangpp_18_1_is_stripped() {
        let result = sanitize(&["clang++-18.1", "-std=c++17"]);
        assert_eq!(result, vec!["-std=c++17"], "clang++-18.1 must be stripped");
    }

    // clang-tidy is NOT a versioned driver — its suffix "tidy" is not numeric.
    #[test]
    fn sanitize_clang_tidy_is_not_stripped() {
        let result = sanitize(&["clang-tidy", "-checks=-*"]);
        assert!(
            result.contains(&"clang-tidy".to_owned()),
            "clang-tidy must NOT be treated as a compiler driver"
        );
    }

    // clang-format is NOT a versioned driver — its suffix "format" is not numeric.
    #[test]
    fn sanitize_clang_format_is_not_stripped() {
        let result = sanitize(&["clang-format", "--style=file"]);
        assert!(
            result.contains(&"clang-format".to_owned()),
            "clang-format must NOT be treated as a compiler driver"
        );
    }

    #[test]
    fn sanitize_source_file_as_token0_is_kept() {
        // "foo.cc" basename is not in the driver set → must NOT be stripped.
        let result = sanitize_with_file(
            &["foo.cc", "-std=c++17", "-DFOO=1"],
            Path::new("/other/file.cpp"),
        );
        assert_eq!(result, vec!["foo.cc", "-std=c++17", "-DFOO=1"]);
    }

    #[test]
    fn sanitize_c_flag_pair_is_stripped() {
        let result = sanitize(&["clang++", "-c", "/tmp/x.cpp", "-std=c++17"]);
        assert_eq!(result, vec!["-std=c++17"]);
    }

    #[test]
    fn sanitize_o_flag_pair_is_stripped() {
        let result = sanitize(&["clang++", "-o", "x.o", "-std=c++17"]);
        assert_eq!(result, vec!["-std=c++17"]);
    }

    #[test]
    fn sanitize_trailing_source_repeat_absolute_is_stripped() {
        // The trailing absolute path matches canonical_file → stripped.
        let result = sanitize_with_file(
            &["gcc", "-std=c++14", "/project/src/foo.cpp"],
            Path::new("/project/src/foo.cpp"),
        );
        assert_eq!(result, vec!["-std=c++14"]);
    }

    #[test]
    fn sanitize_trailing_source_repeat_relative_is_stripped() {
        // Relative "src/foo.cpp" joined to "/project" → "/project/src/foo.cpp".
        // Falls back to literal join (synthetic path, no file on disk) and
        // compares against canonical_file.
        let result = sanitize_with_file(
            &["gcc", "-std=c++14", "src/foo.cpp"],
            Path::new("/project/src/foo.cpp"),
        );
        assert_eq!(result, vec!["-std=c++14"]);
    }

    #[test]
    fn sanitize_non_driver_token0_zig_is_kept() {
        // "zig" is not in the driver deny-list; it should pass through.
        let result = sanitize(&["zig", "-std=c++17"]);
        assert_eq!(result, vec!["zig", "-std=c++17"]);
    }

    #[test]
    fn sanitize_all_flags_pass_through_unchanged() {
        // All enumerated pass-through prefixes from AC-1.
        let flags = [
            "-DNDEBUG",
            "-I/usr/include",
            "-std=c++17",
            "-Wall",
            "-fPIC",
            "-isystem/sys",
            "-include",
            "pch.h",
            "-arch",
            "arm64",
            "-target",
            "aarch64-apple-macos12",
        ];
        let mut input = vec!["clang++"];
        input.extend_from_slice(&flags);
        let result = sanitize_with_file(&input, Path::new("/nowhere/nonexistent.cpp"));
        // All flags and their non-flag arguments must survive unchanged.
        for flag in flags {
            assert!(
                result.contains(&flag.to_owned()),
                "flag {flag:?} should be kept"
            );
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
