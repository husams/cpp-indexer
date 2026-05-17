//! Integration tests for the content-hash incremental cache (S19).
//!
//! AC covered: AC-M3-7 (second run → 0 TUs parsed), AC-M3-8 (edit → only affected TU re-parsed),
//! AC-M3-9 (libclang version bump → full invalidation),
//! AC-M3-10 (schema version bump → full invalidation).
//!
//! Strategy: use a temporary copy of the m1_5file fixture (shapes.cpp + utils.cpp + broken.cpp)
//! so tests can mutate source files without touching the committed fixture.
//! broken.cpp is intentionally excluded from the incremental scenarios to keep assertions simple.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use cpp_indexer::pipeline::{run, RunOptions};
use cpp_indexer::stage::manifest::Manifest;
use cpp_indexer::{schema::version::SCHEMA_VERSION, sink::mock::MockSink};
use tempfile::TempDir;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn m1_fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/m1_5file")
}

/// Copy the m1_5file fixture into a writable temp directory so individual
/// tests can modify source files.
fn copy_fixture_to_temp() -> TempDir {
    let tmp = TempDir::new().expect("create temp dir");
    let fixture = m1_fixture_dir();
    for entry in walkdir::WalkDir::new(&fixture)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_file())
    {
        let rel = entry.path().strip_prefix(&fixture).unwrap();
        let dest = tmp.path().join(rel);
        if let Some(parent) = dest.parent() {
            std::fs::create_dir_all(parent).unwrap();
        }
        std::fs::copy(entry.path(), &dest).unwrap();
    }
    tmp
}

/// Write a `compile_commands.json` for only the listed source files inside `dir`.
///
/// Uses the canonicalized form of `dir` so that the paths in the JSON match what
/// `compile_commands::parse` produces after calling `fs::canonicalize`.  On macOS
/// the OS temp dir is a symlink (`/var/folders/…` → `/private/var/folders/…`);
/// without canonicalization `filter_compiler_args` cannot strip the source-file
/// positional argument, causing `AstDeserialization` errors.
fn write_compile_commands(dir: &Path, files: &[&str]) {
    // Canonicalize so `/var/…` → `/private/var/…` on macOS (resolves symlinks).
    let canonical_dir = dir.canonicalize().unwrap_or_else(|_| dir.to_owned());
    let entries: Vec<serde_json::Value> = files
        .iter()
        .map(|name| {
            let abs = canonical_dir.join(name);
            serde_json::json!({
                "directory": canonical_dir.to_string_lossy(),
                "file": abs.to_string_lossy(),
                "arguments": ["clang++", "-std=c++14",
                              format!("-I{}", canonical_dir.display()),
                              abs.to_string_lossy()]
            })
        })
        .collect();
    let json = serde_json::to_string_pretty(&entries).unwrap();
    std::fs::write(dir.join("compile_commands.json"), json).unwrap();
}

/// Build `RunOptions` for one run.  `stage_dir` is always kept across runs so
/// the manifest is preserved.
///
/// `fixture_dir` is the (possibly symlinked) temp dir; we canonicalize it here
/// so the path in `input_path` and `compile_commands` matches what the JSON
/// already has after `write_compile_commands` canonicalized it.
fn make_run_opts(fixture_dir: &Path, stage_dir: PathBuf, skip_cache: bool) -> RunOptions {
    let canonical = fixture_dir
        .canonicalize()
        .unwrap_or_else(|_| fixture_dir.to_owned());
    RunOptions {
        input_path: canonical.clone(),
        compile_commands: Some(canonical.join("compile_commands.json")),
        repo_name: "incremental-test-repo".to_owned(),
        stage_dir: Some(stage_dir),
        skip_phase2: true,
        skip_system_headers: true,
        skip_cache,
        skip_repo_node: true,
    }
}

// ---------------------------------------------------------------------------
// Scenario (a): second run with no changes → 0 TUs parsed (AC-M3-7)
// ---------------------------------------------------------------------------

/// Run the pipeline twice against the same unchanged sources; the second run
/// must produce zero parsed TUs (all hits) and complete quickly.
#[tokio::test]
async fn incremental_cache_no_change_all_hits() {
    let fixture_tmp = copy_fixture_to_temp();
    let fixture_dir = fixture_tmp.path();
    // Use only the two clean TUs; exclude broken.cpp to keep node/edge counts
    // deterministic.
    write_compile_commands(fixture_dir, &["shapes.cpp", "utils.cpp"]);

    let stage_tmp = TempDir::new().unwrap();
    let stage_dir = stage_tmp.path().to_owned();

    let sink1 = Arc::new(MockSink::default());
    let opts1 = make_run_opts(fixture_dir, stage_dir.clone(), false);
    let stats1 = run(sink1, opts1).await.expect("first run must succeed");

    // First run: 2 TUs, 0 cache hits.
    assert_eq!(stats1.tu_count, 2, "first run must see 2 TUs");
    assert_eq!(stats1.cache_hits, 0, "first run must have 0 cache hits");

    // Second run: same fixture, same stage dir → manifest already populated.
    let sink2 = Arc::new(MockSink::default());
    let opts2 = make_run_opts(fixture_dir, stage_dir.clone(), false);
    let stats2 = run(sink2, opts2).await.expect("second run must succeed");

    assert_eq!(stats2.tu_count, 2, "second run still sees 2 TUs");
    assert_eq!(
        stats2.cache_hits, 2,
        "second run with no changes must have 2 cache hits (AC-M3-7)"
    );

    // Verify the manifest has exactly 2 entries (one per TU, first-run writes).
    let manifest = Manifest::load(&stage_dir.join("manifest.json"))
        .expect("manifest must be loadable after second run");
    assert_eq!(
        manifest.entries.len(),
        2,
        "manifest must have 2 entries after two identical runs"
    );
}

// ---------------------------------------------------------------------------
// Scenario (b): edit one source → only affected TU re-parsed (AC-M3-8)
// ---------------------------------------------------------------------------

/// After an initial warm-up run, touch one of the two source files. The second
/// run must re-parse only that file (1 hit, 1 miss → 1 parsed).
#[tokio::test]
async fn incremental_cache_single_edit_reparsed() {
    let fixture_tmp = copy_fixture_to_temp();
    let fixture_dir = fixture_tmp.path();
    write_compile_commands(fixture_dir, &["shapes.cpp", "utils.cpp"]);

    let stage_tmp = TempDir::new().unwrap();
    let stage_dir = stage_tmp.path().to_owned();

    // Warm-up run.
    let sink1 = Arc::new(MockSink::default());
    let opts1 = make_run_opts(fixture_dir, stage_dir.clone(), false);
    let stats1 = run(sink1, opts1).await.expect("warm-up run must succeed");
    assert_eq!(stats1.tu_count, 2);
    assert_eq!(stats1.cache_hits, 0);

    // Mutate shapes.cpp — append a comment so the content hash changes.
    let shapes_path = fixture_dir.join("shapes.cpp");
    let original = std::fs::read_to_string(&shapes_path).unwrap();
    std::fs::write(&shapes_path, format!("{original}\n// cache-bust comment\n")).unwrap();

    // Second run: shapes.cpp is a cache miss; utils.cpp is a hit.
    let sink2 = Arc::new(MockSink::default());
    let opts2 = make_run_opts(fixture_dir, stage_dir.clone(), false);
    let stats2 = run(sink2, opts2).await.expect("second run must succeed");

    assert_eq!(stats2.tu_count, 2, "second run still sees 2 TUs");
    assert_eq!(
        stats2.cache_hits, 1,
        "second run with one edit must have exactly 1 cache hit (AC-M3-8)"
    );
}

// ---------------------------------------------------------------------------
// Scenario (c): schema version bump → full invalidation (AC-M3-10)
// ---------------------------------------------------------------------------

/// Simulate a schema version bump by rewriting the manifest.json with a
/// different `schema_version`.  `Manifest::load_or_invalidate` must treat this
/// as a full miss; all TUs must be re-parsed.
#[tokio::test]
async fn incremental_cache_schema_version_bump_full_invalidation() {
    let fixture_tmp = copy_fixture_to_temp();
    let fixture_dir = fixture_tmp.path();
    write_compile_commands(fixture_dir, &["shapes.cpp", "utils.cpp"]);

    let stage_tmp = TempDir::new().unwrap();
    let stage_dir = stage_tmp.path().to_owned();

    // Warm-up run to populate the manifest.
    let sink1 = Arc::new(MockSink::default());
    let opts1 = make_run_opts(fixture_dir, stage_dir.clone(), false);
    let stats1 = run(sink1, opts1).await.expect("warm-up run must succeed");
    assert_eq!(stats1.tu_count, 2);
    assert_eq!(stats1.cache_hits, 0);

    // Corrupt the manifest: write a schema_version that does not match the
    // compile-time SCHEMA_VERSION constant.  The next run must fully invalidate.
    let manifest_path = stage_dir.join("manifest.json");
    let raw = std::fs::read_to_string(&manifest_path).unwrap();
    let mut m: serde_json::Value = serde_json::from_str(&raw).unwrap();
    let bad_version = SCHEMA_VERSION.wrapping_add(1);
    m["schema_version"] = serde_json::json!(bad_version);
    // Also corrupt entries so they carry the bad version.
    if let Some(entries) = m["entries"].as_array_mut() {
        for entry in entries.iter_mut() {
            entry["schema_version"] = serde_json::json!(bad_version);
        }
    }
    std::fs::write(&manifest_path, serde_json::to_string_pretty(&m).unwrap()).unwrap();

    // Second run must ignore the corrupt manifest and re-parse all TUs.
    let sink2 = Arc::new(MockSink::default());
    let opts2 = make_run_opts(fixture_dir, stage_dir.clone(), false);
    let stats2 = run(sink2, opts2).await.expect("second run must succeed");

    assert_eq!(stats2.tu_count, 2, "second run still sees 2 TUs");
    assert_eq!(
        stats2.cache_hits, 0,
        "schema version bump must cause full invalidation — 0 cache hits (AC-M3-10)"
    );
}

// ---------------------------------------------------------------------------
// Scenario (d): libclang version mismatch → full invalidation (AC-M3-9)
// ---------------------------------------------------------------------------

/// Simulate a libclang version change by injecting a different
/// `libclang_version` string into every manifest entry.  The cache lookup
/// must return no hits.
#[tokio::test]
async fn incremental_cache_libclang_version_bump_full_invalidation() {
    let fixture_tmp = copy_fixture_to_temp();
    let fixture_dir = fixture_tmp.path();
    write_compile_commands(fixture_dir, &["shapes.cpp", "utils.cpp"]);

    let stage_tmp = TempDir::new().unwrap();
    let stage_dir = stage_tmp.path().to_owned();

    // Warm-up run.
    let sink1 = Arc::new(MockSink::default());
    let opts1 = make_run_opts(fixture_dir, stage_dir.clone(), false);
    let stats1 = run(sink1, opts1).await.expect("warm-up run must succeed");
    assert_eq!(stats1.tu_count, 2);
    assert_eq!(stats1.cache_hits, 0);

    // Rewrite every entry's libclang_version to a fake value.
    let manifest_path = stage_dir.join("manifest.json");
    let raw = std::fs::read_to_string(&manifest_path).unwrap();
    let mut m: serde_json::Value = serde_json::from_str(&raw).unwrap();
    if let Some(entries) = m["entries"].as_array_mut() {
        for entry in entries.iter_mut() {
            entry["libclang_version"] = serde_json::json!("0.0.0-fake");
        }
    }
    std::fs::write(&manifest_path, serde_json::to_string_pretty(&m).unwrap()).unwrap();

    // Second run: libclang_version in every entry is wrong → 0 hits.
    let sink2 = Arc::new(MockSink::default());
    let opts2 = make_run_opts(fixture_dir, stage_dir.clone(), false);
    let stats2 = run(sink2, opts2).await.expect("second run must succeed");

    assert_eq!(stats2.tu_count, 2, "second run still sees 2 TUs");
    assert_eq!(
        stats2.cache_hits, 0,
        "libclang version change must cause full invalidation — 0 cache hits (AC-M3-9)"
    );
}
