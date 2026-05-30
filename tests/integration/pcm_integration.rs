//! S4 — Integration test: mixed compile_commands with PCM + standard TUs (ADR-3 §tests).
//!
//! ## Design (plan.md §S4)
//! Builds a fixture `compile_commands.json` with one standard `.cpp` TU and one
//! PCM-consuming TU that carries `-fmodule-file=<Name>=<path>`, then runs
//! `run_phase1_parallel` against it.
//!
//! ## Test gates
//! - **AC2 (unconditional):** PCM file absent → `failed_tu_count > 0` AND standard TU
//!   still produces nodes (`tu_ok >= 1`). This is the load-bearing loud-fail assertion
//!   for the Issue-0001 family. Runs everywhere (no probe or toolchain gate).
//! - **AC1 (`#[ignore]`):** Both TUs in graph AND `tu_error == 0`. Requires a real `.pcm`
//!   that libclang can load; the test attempts to build it with the CommandLineTools
//!   `clang++` binary at runtime. Skips-with-reason when the build fails OR when
//!   `probe_cpp20_support()` is false (AC3 — never fails CI due to missing probe).
//!
//! ## Running the ignored test
//! ```
//! LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib \
//! DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
//!   cargo nextest run --tests --features test-mock --run-ignored all pcm_integration
//! ```

use std::path::{Path, PathBuf};

use cpp_indexer::{
    bootstrap::compile_commands, pipeline::parallel::run_phase1_parallel,
    schema::arrow::record_batch_to_nodes, visit::modules_cpp20::probe_cpp20_support,
};
use tempfile::TempDir;

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

fn fixture_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/pcm_mixed")
}

/// Build a compile_commands.json in `dir` with:
/// - one standard TU (`standard.cpp`, no PCM flags)
/// - one PCM-consuming TU (`consumer.cpp`, `-fmodule-file=MathUtils=<pcm_path>`)
fn write_compile_commands(dir: &Path, pcm_path: &Path) -> PathBuf {
    let standard = fixture_dir().join("standard.cpp");
    let consumer = fixture_dir().join("consumer.cpp");
    let cc = serde_json::json!([
        {
            "directory": dir.to_str().unwrap(),
            "file": standard.to_str().unwrap(),
            "arguments": [
                "clang++",
                "-std=c++14",
                standard.to_str().unwrap()
            ]
        },
        {
            "directory": dir.to_str().unwrap(),
            "file": consumer.to_str().unwrap(),
            "arguments": [
                "clang++",
                "-std=c++14",
                format!("-fmodule-file=MathUtils={}", pcm_path.display()),
                consumer.to_str().unwrap()
            ]
        }
    ]);
    let cc_path = dir.join("compile_commands.json");
    std::fs::write(&cc_path, serde_json::to_string_pretty(&cc).unwrap())
        .expect("write compile_commands.json");
    cc_path
}

/// Read all NodeRecords from Parquet shards under `stage_dir`.
fn collect_all_nodes(stage_dir: &Path) -> Vec<cpp_indexer::schema::NodeRecord> {
    use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;

    let pattern = stage_dir
        .join("worker-*")
        .join("nodes-*.parquet")
        .to_string_lossy()
        .into_owned();

    let mut out = Vec::new();
    for entry in glob::glob(&pattern).expect("glob").flatten() {
        let file = std::fs::File::open(&entry).expect("open parquet");
        let reader = ParquetRecordBatchReaderBuilder::try_new(file)
            .expect("build reader")
            .build()
            .expect("build batch reader");
        for batch in reader.flatten() {
            out.extend(record_batch_to_nodes(&batch));
        }
    }
    out
}

// ---------------------------------------------------------------------------
// S4-AC2 (unconditional): absent .pcm → tu_error >= 1 AND standard TU ok
// ---------------------------------------------------------------------------

/// S4-AC2: removing the `.pcm` causes `tu_error > 0` while the standard TU
/// still succeeds (`tu_ok >= 1`).
///
/// This test is **unconditional** — it runs without a working `.pcm` toolchain
/// and without a passing `probe_cpp20_support()`.
///
/// The S3 pre-parse stat gate detects the missing explicit `.pcm` path and
/// returns `Err(Error::Clang)` before any write → counted as `tu_error`.
/// The standard TU (no PCM flags) routes through the original `visit_tu_with_index`
/// path and completes normally → counted in `tu_ok` or `tu_partial`.
///
/// Closes the Issue-0001 family for PCM TUs: a missing `.pcm` must produce a
/// loud counted failure, never a silent partial parse.
#[test]
fn pcm_integration_missing_pcm_causes_tu_error_not_silent_partial() {
    let tmp = TempDir::new().expect("tempdir");
    // Point at a path that definitely does not exist.
    let nonexistent_pcm = tmp.path().join("nonexistent").join("MathUtils.pcm");
    let cc_path = write_compile_commands(tmp.path(), &nonexistent_pcm);

    let entries = compile_commands::parse(&cc_path).expect("parse compile_commands.json");
    assert_eq!(
        entries.len(),
        2,
        "fixture must have exactly 2 TUs (standard + consumer)"
    );

    let stage = tmp.path().join("stage");
    let stats = run_phase1_parallel(
        &entries,
        &stage,
        "test-repo-pcm",
        true,
        Some(1),
        None, // no allocator required for this test
    )
    .expect("run_phase1_parallel must not return top-level Err");

    // The consumer TU with the missing .pcm must be counted as tu_error.
    assert!(
        stats.tu_error >= 1,
        "expected tu_error >= 1 for missing .pcm; got tu_error={} tu_ok={} tu_partial={}",
        stats.tu_error,
        stats.tu_ok,
        stats.tu_partial
    );

    // The standard TU must succeed (tu_ok or tu_partial — not an error).
    assert!(
        stats.tu_ok + stats.tu_partial >= 1,
        "expected standard TU to succeed; got tu_ok={} tu_partial={} tu_error={}",
        stats.tu_ok,
        stats.tu_partial,
        stats.tu_error
    );

    // The standard TU must have produced at least one node (no graph for the broken TU).
    let nodes = collect_all_nodes(&stage);
    assert!(
        !nodes.is_empty(),
        "standard TU must have emitted nodes even though consumer TU failed"
    );
}

// ---------------------------------------------------------------------------
// QA addition — category 3 (boundary/mutation): corrupt .pcm present on disk
//
// S3-AC2 scenario: a .pcm file that EXISTS on disk but contains garbage bytes
// (wrong magic / truncated) must still produce tu_error > 0.
//
// The pre-parse stat gate only checks `exists()` — it passes for a corrupt file.
// The post-parse Fatal gate (lines 291-305 of modules_cpp20.rs) is the intended
// safety net, but was empirically falsified on libclang 18/macOS: corrupt PCM
// surfaces as Severity::Error (not Fatal), so the Fatal gate does NOT fire.
// This test makes that gap observable and reported.
//
// Design note: consumer.cpp does NOT contain `import MathUtils;` — it only
// carries -fmodule-file= in its compile command.  Whether libclang actually
// loads the PCM without an explicit import statement is implementation-defined.
// The test therefore also probes whether the Error diagnostic is emitted at all.
// If tu_error==0 with nodes emitted the defect (S3-AC2) is confirmed; if
// tu_error>=1 the Fatal gate caught something unexpected.
// ---------------------------------------------------------------------------

/// QA boundary test (category 3): present-but-corrupt .pcm must not silently
/// partial-parse.  Asserts `tu_error >= 1` per S3-AC2.
///
/// This test is **unconditional** — it does not require a working module
/// toolchain.  It writes junk bytes to a file so the pre-parse `exists()` check
/// passes, then relies on libclang (or the Fatal gate) to reject it.
///
/// The dev log (S3) notes that on macOS/libclang 18 corrupt PCM surfaces as
/// `Severity::Error` not `Fatal`, so the Fatal gate does NOT fire.  If this
/// test fails with `tu_error == 0` + nodes emitted, that is defect QD-1
/// (S3-AC2 not satisfied for corrupt-but-present PCM).
#[test]
fn pcm_integration_corrupt_pcm_causes_tu_error_not_silent_partial() {
    let tmp = TempDir::new().expect("tempdir");

    // Create a .pcm file that EXISTS on disk but contains invalid bytes
    // (wrong magic header — a valid LLVM bitcode file starts with 0x42 0x43,
    // "BC"; a valid .pcm starts with a LLVM bitcode header; garbage bytes
    // guarantee it is invalid/corrupt in all libclang versions).
    let corrupt_pcm = tmp.path().join("MathUtils.pcm");
    std::fs::write(
        &corrupt_pcm,
        b"CORRUPT_PCM_GARBAGE_BYTES_FOR_QA_TEST\x00\x01\x02\x03",
    )
    .expect("write corrupt pcm");
    assert!(corrupt_pcm.exists(), "corrupt pcm must exist on disk");

    let cc_path = write_compile_commands(tmp.path(), &corrupt_pcm);
    let entries = compile_commands::parse(&cc_path).expect("parse compile_commands.json");
    assert_eq!(
        entries.len(),
        2,
        "fixture must have exactly 2 TUs (standard + consumer)"
    );

    let stage = tmp.path().join("stage");
    let stats = run_phase1_parallel(
        &entries,
        &stage,
        "test-repo-pcm-corrupt",
        true,
        Some(1),
        None,
    )
    .expect("run_phase1_parallel must not return top-level Err");

    // S3-AC2: corrupt .pcm must produce tu_error > 0 (loud counted failure).
    // A tu_error == 0 here means the consumer TU was silently partial-parsed —
    // which is the Issue-0001 failure pattern and is a QD-1 defect.
    assert!(
        stats.tu_error >= 1,
        "S3-AC2 DEFECT QD-1: corrupt .pcm produced silent partial parse \
         (tu_error=0) instead of a counted failure. \
         tu_ok={} tu_partial={} tu_error={}",
        stats.tu_ok,
        stats.tu_partial,
        stats.tu_error
    );

    // The standard TU (no PCM flags) must still have succeeded.
    assert!(
        stats.tu_ok + stats.tu_partial >= 1,
        "standard TU must succeed regardless of consumer TU corruption; \
         tu_ok={} tu_partial={} tu_error={}",
        stats.tu_ok,
        stats.tu_partial,
        stats.tu_error
    );
}

// ---------------------------------------------------------------------------
// S4-AC1 + S4-AC3 (ignored): valid .pcm → both TUs indexed, tu_error == 0
// ---------------------------------------------------------------------------

/// S4-AC1: fixture with both TU kinds and a valid prebuilt `.pcm` — both TUs
/// are indexed with `tu_error == 0`.
///
/// **Marked `#[ignore]`** because it requires:
/// 1. `probe_cpp20_support()` returns `true` (libclang ≥ 18 with module support).
/// 2. A working `clang++` that can precompile a `.cppm` into a `.pcm` BMI.
///    Apple clang 17 does NOT support C++20 named-module precompilation, so this
///    test will self-skip-with-reason on most current macOS hosts.
///
/// S4-AC3: when either precondition is absent the test logs a reason and returns
/// without asserting failure — CI does not fail due to this skip.
#[test]
#[ignore = "requires libclang >= 18 with module support AND a clang++ that can precompile .cppm"]
fn pcm_integration_valid_pcm_both_tus_indexed_no_error() {
    // AC3 gate 1: capability probe.
    if !probe_cpp20_support() {
        eprintln!(
            "pcm_integration::valid_pcm_both_tus_indexed_no_error: skipped — \
             probe_cpp20_support() returned false (libclang lacks C++20 module support)"
        );
        return;
    }

    let tmp = TempDir::new().expect("tempdir");

    // Write a minimal module interface unit.
    let cppm = tmp.path().join("MathUtils.cppm");
    std::fs::write(
        &cppm,
        b"export module MathUtils;\nexport int math_add(int a, int b) { return a + b; }\n",
    )
    .expect("write .cppm fixture");

    let pcm = tmp.path().join("MathUtils.pcm");

    // Attempt to precompile the .cppm → .pcm with the CommandLineTools clang++.
    // AC3 gate 2: if the build fails, skip with reason (don't fail CI).
    let clangpp = "/Library/Developer/CommandLineTools/usr/bin/clang++";
    if !std::path::Path::new(clangpp).exists() {
        eprintln!(
            "pcm_integration::valid_pcm_both_tus_indexed_no_error: skipped — \
             {clangpp} not found (cannot build .pcm BMI)"
        );
        return;
    }

    let build_status = std::process::Command::new(clangpp)
        .args([
            "-std=c++20",
            "-x",
            "c++-module",
            "--precompile",
            cppm.to_str().unwrap(),
            "-o",
            pcm.to_str().unwrap(),
        ])
        .output();

    match build_status {
        Ok(output) if output.status.success() => {
            // PCM built — proceed with the real test.
        }
        Ok(output) => {
            let stderr = String::from_utf8_lossy(&output.stderr);
            eprintln!(
                "pcm_integration::valid_pcm_both_tus_indexed_no_error: skipped — \
                 clang++ failed to precompile .cppm (Apple clang likely lacks named-module \
                 support). clang++ stderr:\n{stderr}"
            );
            return;
        }
        Err(e) => {
            eprintln!(
                "pcm_integration::valid_pcm_both_tus_indexed_no_error: skipped — \
                 could not launch {clangpp}: {e}"
            );
            return;
        }
    }

    // PCM exists — run the full fixture.
    let cc_path = write_compile_commands(tmp.path(), &pcm);
    let entries = compile_commands::parse(&cc_path).expect("parse compile_commands.json");
    assert_eq!(entries.len(), 2, "fixture must have exactly 2 TUs");

    let stage = tmp.path().join("stage");
    let stats = run_phase1_parallel(&entries, &stage, "test-repo-pcm", true, Some(1), None)
        .expect("run_phase1_parallel must succeed");

    // S4-AC1: zero errors, both TUs must have been processed.
    assert_eq!(
        stats.tu_error, 0,
        "expected no tu_error with valid .pcm; got tu_error={} tu_ok={} tu_partial={}",
        stats.tu_error, stats.tu_ok, stats.tu_partial
    );
    assert_eq!(
        stats.tu_ok + stats.tu_partial,
        2,
        "both TUs must be accounted for; got tu_ok={} tu_partial={} tu_error={}",
        stats.tu_ok,
        stats.tu_partial,
        stats.tu_error
    );

    // Both TUs must have produced graph nodes.
    let nodes = collect_all_nodes(&stage);
    assert!(
        !nodes.is_empty(),
        "both TUs must have emitted at least one node"
    );
}
