//! Integration tests for Phase 0.5: auto-detection of `compile_commands.json`.
//!
//! Each test synthesises a temporary directory layout using `tempfile::tempdir()`
//! and exercises `bootstrap::autodetect::find_compile_commands`.
//!
//! AC covered: AC-M1-8, AC-M1-9, AC-M1-10, AC-M1-11, AC-M1-12, AC-M1-13.

use std::fs;
use std::path::Path;

use tempfile::tempdir;
use tracing_test::traced_test;

use cpp_indexer::bootstrap::autodetect::find_compile_commands;
use cpp_indexer::Error;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Write a minimal `compile_commands.json` at `path` listing `files` as entries.
fn write_cc(path: &Path, files: &[&str]) {
    let parent = path.parent().unwrap();
    fs::create_dir_all(parent).unwrap();
    let entries: Vec<serde_json::Value> = files
        .iter()
        .map(|f| {
            serde_json::json!({
                "directory": parent.display().to_string(),
                "command": "clang++ -c foo.cpp",
                "file": f
            })
        })
        .collect();
    fs::write(path, serde_json::to_string_pretty(&entries).unwrap()).unwrap();
}

/// Create an empty file, creating parent directories as needed.
fn touch(path: &Path) {
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, "").unwrap();
}

// ---------------------------------------------------------------------------
// (a) File under <repo>/src/, cc.json at <repo>/build/ → resolved + INFO log
// ---------------------------------------------------------------------------
#[test]
#[traced_test]
fn test_a_file_in_src_cc_in_build() {
    let root = tempdir().unwrap();
    let repo = root.path();

    // Simulate a repo with src/foo.cpp and build/compile_commands.json
    let src_file = repo.join("src").join("foo.cpp");
    touch(&src_file);

    let cc_path = repo.join("build").join("compile_commands.json");
    write_cc(&cc_path, &[src_file.to_str().unwrap()]);

    // Provide src/foo.cpp as input; autodetect should walk up and find build/
    let resolved = find_compile_commands(&src_file).unwrap();

    assert_eq!(
        resolved.canonicalize().unwrap(),
        cc_path.canonicalize().unwrap(),
        "should resolve to build/compile_commands.json"
    );

    // INFO log should mention the resolved path (AC-M1-8)
    assert!(
        logs_contain("resolved compile_commands.json"),
        "expected INFO log about resolved path"
    );
}

// ---------------------------------------------------------------------------
// (b) No cc.json anywhere → error listing every dir probed (AC-M1-9)
// ---------------------------------------------------------------------------
#[test]
fn test_b_no_cc_error_lists_searched_dirs() {
    let root = tempdir().unwrap();
    let sub = root.path().join("project").join("src");
    fs::create_dir_all(&sub).unwrap();

    let err = find_compile_commands(&sub).unwrap_err();

    // Must be the Autodetect variant
    let Error::Autodetect { searched } = err else {
        panic!("expected Error::Autodetect, got: {:?}", err);
    };

    // Every ancestor up to root should be listed
    assert!(!searched.is_empty(), "searched list must not be empty");

    let sub_canon = sub.canonicalize().unwrap();
    assert!(
        searched.contains(&sub_canon),
        "searched list should contain the starting directory"
    );

    // Error display must list paths
    let display = Error::Autodetect {
        searched: searched.clone(),
    }
    .to_string();
    assert!(
        display.contains(sub_canon.to_str().unwrap()),
        "Display should contain starting dir; got: {display}"
    );
}

// ---------------------------------------------------------------------------
// (c) Walk stops at .git (AC-M1-10)
// ---------------------------------------------------------------------------
#[test]
fn test_c_walk_stops_at_git() {
    let root = tempdir().unwrap();
    let repo = root.path();

    // Create .git at repo root — this is the stop boundary
    fs::create_dir(repo.join(".git")).unwrap();

    // Create a cc.json ABOVE the repo root — should NOT be found
    // (We can't go above tempdir root in a portable way, so we verify
    // the walk stops by placing cc.json only above the .git level, which
    // is the root of the tempdir — unreachable above OS FS root. Instead,
    // we verify by ensuring the error's searched list does not include
    // the parent of the repo.)
    let src = repo.join("src").join("foo.cpp");
    touch(&src);

    let err = find_compile_commands(&src).unwrap_err();
    let Error::Autodetect { searched } = err else {
        panic!("expected Error::Autodetect");
    };

    // The parent of repo should NOT appear in the searched list
    if let Some(parent) = repo.parent() {
        let parent_canon = parent.canonicalize().unwrap_or(parent.to_path_buf());
        assert!(
            !searched.contains(&parent_canon),
            "walk must not cross .git boundary; parent {:?} must not be in searched list",
            parent_canon
        );
    }
}

// ---------------------------------------------------------------------------
// (d) Two candidates build/ + out/, file in build/ entry list → build/ chosen
// ---------------------------------------------------------------------------
#[test]
fn test_d_prefer_candidate_containing_input_file() {
    let root = tempdir().unwrap();
    let repo = root.path();

    let src_file = repo.join("src").join("main.cpp");
    touch(&src_file);

    // build/ lists the input file
    let build_cc = repo.join("build").join("compile_commands.json");
    write_cc(&build_cc, &[src_file.to_str().unwrap()]);

    // out/ does NOT list the input file
    let out_cc = repo.join("out").join("compile_commands.json");
    write_cc(&out_cc, &["/other/unrelated.cpp"]);

    let resolved = find_compile_commands(&src_file).unwrap();

    assert_eq!(
        resolved.canonicalize().unwrap(),
        build_cc.canonicalize().unwrap(),
        "should prefer build/ because it lists the input file"
    );
}

// ---------------------------------------------------------------------------
// (e) Dir input: autodetect finds cc.json for the directory (AC-M1-12 scope)
// ---------------------------------------------------------------------------
#[test]
fn test_e_dir_input_finds_cc() {
    let root = tempdir().unwrap();
    let repo = root.path();

    // cc.json at build/
    let cc_path = repo.join("build").join("compile_commands.json");
    write_cc(&cc_path, &["/some/file.cpp"]);

    // Input is the repo directory itself
    let resolved = find_compile_commands(repo).unwrap();

    assert_eq!(
        resolved.canonicalize().unwrap(),
        cc_path.canonicalize().unwrap(),
        "dir input should resolve cc.json from build/"
    );
}

// ---------------------------------------------------------------------------
// (f) File input → finds cc.json that exactly matches the file entry (AC-M1-13)
// ---------------------------------------------------------------------------
#[test]
fn test_f_file_input_prefers_exact_entry() {
    let root = tempdir().unwrap();
    let repo = root.path();

    let src_file = repo.join("lib").join("util.cpp");
    touch(&src_file);

    // only cc.json that mentions the file
    let cc_path = repo.join("build").join("compile_commands.json");
    write_cc(&cc_path, &[src_file.to_str().unwrap()]);

    let resolved = find_compile_commands(&src_file).unwrap();

    assert_eq!(
        resolved.canonicalize().unwrap(),
        cc_path.canonicalize().unwrap(),
        "file input should find cc.json listing that file"
    );
}
