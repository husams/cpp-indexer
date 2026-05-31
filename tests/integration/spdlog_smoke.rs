//! S5 — spdlog integration smoke test (AC-3).
//!
//! Clones `gabime/spdlog` at HEAD, runs CMake to produce
//! `compile_commands.json`, then exercises the full indexing pipeline
//! in-process and asserts that at least 6 of 7 TUs parse successfully.
//!
//! # Gating
//! This test is `#[ignore]` because it requires network access (`git clone`)
//! and a working C++ toolchain (`cmake`).  Run it explicitly with:
//!
//! ```sh
//! cargo test --features test-mock --test spdlog_smoke -- --ignored
//! ```
//!
//! On CI, macOS arm64 and Linux x86_64 runners install git and cmake before
//! invoking this test (DevOps concern — see deploy-notes.md).
//!
//! The test skips cleanly (prints a reason and returns `Ok`) when:
//! - `git` is not on PATH
//! - `cmake` is not on PATH
//! - `git clone` fails (e.g. no network)
//! - `cmake` configuration fails (e.g. no C++ compiler present)

#![cfg(not(target_os = "windows"))]

use std::process::Command;
use std::sync::Arc;

use cpp_indexer::pipeline::{run, RunOptions};
use cpp_indexer::sink::mock::MockSink;
use cpp_indexer::sink::GraphSink;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Return `true` if `program` resolves on the current PATH.
fn is_on_path(program: &str) -> bool {
    Command::new(program)
        .arg("--version")
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

#[tokio::test]
#[ignore]
async fn spdlog_smoke() {
    // ── Pre-flight: check required tools are on PATH ──────────────────────
    if !is_on_path("git") {
        println!("spdlog_smoke: skip — `git` not on PATH");
        return;
    }
    if !is_on_path("cmake") {
        println!("spdlog_smoke: skip — `cmake` not on PATH");
        return;
    }

    // ── Create temp workspace ─────────────────────────────────────────────
    let tmp = tempfile::tempdir().expect("failed to create tempdir");
    let src_dir = tmp.path().join("spdlog");
    let build_dir = tmp.path().join("spdlog-build");

    // ── Clone spdlog at HEAD ──────────────────────────────────────────────
    let clone_status = Command::new("git")
        .args([
            "clone",
            "--depth=1",
            "https://github.com/gabime/spdlog.git",
            src_dir.to_str().expect("src_dir is valid UTF-8"),
        ])
        .status();

    match clone_status {
        Ok(s) if s.success() => {}
        Ok(s) => {
            println!("spdlog_smoke: skip — `git clone` exited with status {}", s);
            return;
        }
        Err(e) => {
            println!("spdlog_smoke: skip — `git clone` failed to spawn: {e}");
            return;
        }
    }

    // ── CMake configure (Release, export compile_commands.json) ──────────
    std::fs::create_dir_all(&build_dir).expect("failed to create build dir");

    let cmake_status = Command::new("cmake")
        .args([
            "-S",
            src_dir.to_str().expect("src_dir is valid UTF-8"),
            "-B",
            build_dir.to_str().expect("build_dir is valid UTF-8"),
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DSPDLOG_BUILD_EXAMPLE=OFF",
            "-DSPDLOG_BUILD_TESTS=OFF",
            "-DSPDLOG_BUILD_BENCH=OFF",
        ])
        .status();

    match cmake_status {
        Ok(s) if s.success() => {}
        Ok(s) => {
            println!(
                "spdlog_smoke: skip — `cmake` configure exited with status {}",
                s
            );
            return;
        }
        Err(e) => {
            println!("spdlog_smoke: skip — `cmake` failed to spawn: {e}");
            return;
        }
    }

    let cc_path = build_dir.join("compile_commands.json");
    assert!(
        cc_path.exists(),
        "cmake did not produce compile_commands.json at {cc_path:?}"
    );

    // ── Run the indexer pipeline in-process ───────────────────────────────
    let sink: Arc<dyn GraphSink> = Arc::new(MockSink::default());
    let opts = RunOptions {
        input_path: src_dir.clone(),
        compile_commands: Some(cc_path),
        repo_name: "spdlog".to_owned(),
        stage_dir: None,
        skip_phase2: true,
        skip_system_headers: true,
        workers: None,
        skip_cache: true,
        skip_repo_node: true,
        symbol_db_path: None,
        symbol_cache_size: 100_000,
        phase1_tuning: Default::default(),
        write_buffer_bytes: cpp_indexer::config::DEFAULT_WRITE_BUFFER_BYTES,
        write_only: false,
    };

    let stats = run(sink, opts)
        .await
        .expect("pipeline::run must not return Err for spdlog");

    // ── AC-3 assertions ───────────────────────────────────────────────────
    let ok_tu_count = stats.tu_count.saturating_sub(stats.failed_tu_count);
    assert!(
        stats.tu_count > 0,
        "spdlog: expected at least 1 TU in compile_commands.json, got 0"
    );
    assert!(
        ok_tu_count >= 6,
        "spdlog: AC-3 requires ok_tu_count >= 6; got {ok_tu_count} \
         (total={}, failed={})",
        stats.tu_count,
        stats.failed_tu_count,
    );

    println!(
        "spdlog_smoke: PASS — {} TUs total, {} failed, {} ok",
        stats.tu_count, stats.failed_tu_count, ok_tu_count
    );
}
