//! Integration tests for S15 — `skip_system_headers` config flag.
//!
//! AC covered: AC-M2-14 (skip_system_headers=true excludes sys-header nodes),
//!             AC-M2-15 (skip_system_headers=false includes them).
//!
//! # Fixture layout
//! `tests/fixtures/sys_header_filter/`
//!   `sysinc/fake_sys.h`  — fake system header (SysStruct, sys_function)
//!   `user_code.cpp`      — includes fake_sys.h; also declares UserClass, user_func
//!
//! The parent directory `sysinc/` is passed via `-isystem` so libclang marks
//! headers inside it as system headers on all platforms (no dependency on
//! `/usr/include/`).

use std::collections::HashSet;
use std::path::PathBuf;

use clang::Clang;
use tempfile::TempDir;

use cpp_indexer::{
    schema::arrow::record_batch_to_nodes,
    stage::writer::StageWriter,
    visit::shallow::{visit_tu, VisitOptions},
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/sys_header_filter")
}

/// Run Phase 1 on `user_code.cpp` and collect all emitted node names.
fn collect_node_names(skip_system_headers: bool) -> HashSet<String> {
    let dir = TempDir::new().unwrap();
    let stage_dir = dir.path().join("stage");

    let fixture = fixture_dir();
    let sysinc_dir = fixture.join("sysinc");
    let source = fixture
        .join("user_code.cpp")
        .canonicalize()
        .unwrap_or_else(|_| fixture.join("user_code.cpp"));

    let clang = Clang::new().expect("libclang must be available");

    let opts = VisitOptions {
        repo_name: "test-repo",
        tu_hash: [0u8; 32],
        file_path: &source,
        args: &[
            "clang++".to_owned(),
            "-std=c++14".to_owned(),
            // Pass sysinc/ as a system-include directory so libclang marks
            // everything inside it as being in a system header.
            format!("-isystem{}", sysinc_dir.display()),
        ],
        skip_system_headers,
    };

    let mut writer = StageWriter::new(&stage_dir, 0).unwrap();
    visit_tu(&clang, &opts, &mut writer).unwrap();
    writer.finish().unwrap();

    // Read back node names from all parquet shards.
    let mut names = HashSet::new();
    for entry in walkdir::WalkDir::new(&stage_dir)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| {
            e.file_name()
                .to_str()
                .map(|s| s.starts_with("nodes-") && s.ends_with(".parquet"))
                .unwrap_or(false)
        })
    {
        let file = std::fs::File::open(entry.path()).unwrap();
        let reader = parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(file)
            .unwrap()
            .build()
            .unwrap();
        for batch in reader {
            let batch = batch.unwrap();
            let records = record_batch_to_nodes(&batch);
            for node in records {
                if !node.name.is_empty() {
                    names.insert(node.name);
                }
            }
        }
    }
    names
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/// AC-M2-14: with `skip_system_headers = true`, entities from fake_sys.h
/// must NOT appear in Parquet output; user_code entities must still appear.
#[test]
fn system_header_nodes_excluded_when_flag_true() {
    let names = collect_node_names(true);

    // User-code symbols must be present.
    assert!(
        names.contains("UserClass"),
        "UserClass must be emitted; got: {:?}",
        names
    );
    assert!(
        names.contains("user_func"),
        "user_func must be emitted; got: {:?}",
        names
    );

    // System-header symbols must be absent (AC-M2-14).
    assert!(
        !names.contains("SysStruct"),
        "SysStruct must be filtered out when skip_system_headers=true; got: {:?}",
        names
    );
    assert!(
        !names.contains("sys_function"),
        "sys_function must be filtered out when skip_system_headers=true; got: {:?}",
        names
    );
}

/// AC-M2-15: with `skip_system_headers = false`, system-header entities are
/// included alongside user-code entities.
#[test]
fn system_header_nodes_included_when_flag_false() {
    let names = collect_node_names(false);

    // User-code symbols must be present.
    assert!(
        names.contains("UserClass"),
        "UserClass must be emitted; got: {:?}",
        names
    );
    assert!(
        names.contains("user_func"),
        "user_func must be emitted; got: {:?}",
        names
    );

    // System-header symbols must also be present (AC-M2-15).
    assert!(
        names.contains("SysStruct") || names.contains("sys_function"),
        "at least one system-header symbol must appear when skip_system_headers=false; got: {:?}",
        names
    );
}
