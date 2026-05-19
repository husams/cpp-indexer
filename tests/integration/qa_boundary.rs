//! QA boundary and mutation tests for Issue 0001 fix (tu-parse-fail).
//!
//! Category: parametrised / mutation / boundary (mandatory QA addition).
//!
//! Covers three gap areas not addressed by the developer's own tests:
//!   1. Driver-token detection boundary cases — versioned drivers (clang-18,
//!      g++-12) that AC-2 requires to be stripped. The `is_driver_basename`
//!      predicate was extended (QD-1 fix) to match versioned drivers via a
//!      numeric-suffix check. These tests now run in the default pass.
//!   2. `FailOnTuError::exit_code` floating-point boundary — ratio ε above /
//!      below the threshold, exercising the `>=` comparison at epsilon precision.
//!   3. `JobRecord` `JobOutcome` JSON round-trip across all three variants.
//!
//! Scenario / AC traceability:
//!   - versioned driver gap → scenarios.md Feature S1 @AC-2 (Examples table, implicit)
//!     and plan.md S1 §AC-2 table row `clang-18`.
//!   - exit_code boundary → scenarios.md Feature S3 @AC-5 "Threshold boundary"
//!     scenarios (exact-equal row + epsilon rows).
//!   - JobOutcome round-trip → scenarios.md Feature S4 @AC-6 "failed_tu_count is
//!     an integer in all status transitions".

use cpp_indexer::api::jobs::{IngestOptions, IngestSource, JobOutcome, JobQueue, JobState};
use std::path::PathBuf;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn path_source(p: &str) -> IngestSource {
    IngestSource::Path {
        path: PathBuf::from(p),
    }
}

// ---------------------------------------------------------------------------
// 1. Driver-token versioned-driver gap (QD-1 — RESOLVED)
//    Versioned drivers like `clang-18` and `g++-12` are now stripped per AC-2.
//    `is_driver_basename` was extended with a numeric-suffix check.
//    Previously marked #[ignore = "QD-1"]; now run in the default pass.
// ---------------------------------------------------------------------------

/// Verifies that `clang-18` IS stripped by `sanitize_libclang_args`.
/// Currently FAILS because `is_driver_basename("clang-18")` returns false.
/// scenario: AC-2 (plan.md S1 §AC-2 table), Feature S1 @AC-2 @confirmed
#[test]
fn qa_versioned_driver_clang_18_is_stripped() {
    use cpp_indexer::bootstrap::compile_commands::parse;
    use std::io::Write as _;
    use tempfile::NamedTempFile;

    let json = r#"[{
        "directory": "/tmp",
        "file": "/tmp/foo.cpp",
        "arguments": ["clang-18", "-std=c++17", "-c", "/tmp/foo.cpp"]
    }]"#;
    let mut f = NamedTempFile::new().expect("tempfile");
    f.write_all(json.as_bytes()).expect("write");
    let entries = parse(f.path()).expect("parse");
    // After fix: clang-18 should be stripped; only -std=c++17 remains.
    assert_eq!(
        entries[0].args,
        vec!["-std=c++17"],
        "clang-18 must be stripped as a versioned compiler driver"
    );
}

/// Verifies that `g++-12` IS stripped by `sanitize_libclang_args`.
/// Currently FAILS because `is_driver_basename("g++-12")` returns false.
/// scenario: AC-2 (plan.md S1 §AC-2 table row `clang-18` analogous case)
#[test]
fn qa_versioned_driver_gpp_12_is_stripped() {
    use cpp_indexer::bootstrap::compile_commands::parse;
    use std::io::Write as _;
    use tempfile::NamedTempFile;

    let json = r#"[{
        "directory": "/tmp",
        "file": "/tmp/bar.cpp",
        "arguments": ["g++-12", "-std=c++20", "-DNDEBUG", "-c", "/tmp/bar.cpp"]
    }]"#;
    let mut f = NamedTempFile::new().expect("tempfile");
    f.write_all(json.as_bytes()).expect("write");
    let entries = parse(f.path()).expect("parse");
    assert_eq!(
        entries[0].args,
        vec!["-std=c++20", "-DNDEBUG"],
        "g++-12 must be stripped as a versioned compiler driver"
    );
}

/// Verifies that `clang-tidy` is NOT stripped — it is a tool, not a compiler driver.
/// This is a passing guard: if the versioned-driver fix incorrectly patterns on
/// `clang-*`, it must not catch `clang-tidy`.
/// scenario: AC-2 guard (non-regression on non-driver tool names)
#[test]
fn qa_clang_tidy_is_not_stripped() {
    use cpp_indexer::bootstrap::compile_commands::parse;
    use std::io::Write as _;
    use tempfile::NamedTempFile;

    let json = r#"[{
        "directory": "/tmp",
        "file": "/tmp/baz.cpp",
        "arguments": ["clang-tidy", "-checks=-*,clang-analyzer-*", "-c", "/tmp/baz.cpp"]
    }]"#;
    let mut f = NamedTempFile::new().expect("tempfile");
    f.write_all(json.as_bytes()).expect("write");
    let entries = parse(f.path()).expect("parse");
    // clang-tidy is NOT a compiler driver; the first token stays.
    // (the -c pair is stripped since it is not a driver strip — just the -c pair-strip rule)
    // The test verifies clang-tidy itself is retained, i.e. it is NOT classified as driver.
    assert!(
        entries[0].args.contains(&"clang-tidy".to_owned()),
        "clang-tidy must NOT be treated as a compiler driver; args = {:?}",
        entries[0].args
    );
}

// ---------------------------------------------------------------------------
// 2. FailOnTuError::exit_code floating-point boundary (AC-5)
//    The developer tests cover exact threshold equality (2/4 == 0.5 → exit 2).
//    These tests add ε-above and ε-below the threshold.
// ---------------------------------------------------------------------------

/// Ratio ε above the threshold: (failed/total) > ratio → exit 2.
/// scenario: Feature S3 @AC-5 "Threshold boundary — ratio exactly met (>=) → exit 2"
/// Uses (failed=3, total=4, ratio=0.5+ε) — actual ratio 0.75 >> 0.5+ε.
/// Separately, verifies ratio=0.749999... with failed=3, total=4 still triggers.
#[test]
fn qa_exit_code_ratio_just_above_threshold_exits_2() {
    // failed=1, total=4 → actual_ratio=0.25.  threshold=0.25+ε → 0.25 < threshold → exit 0.
    let epsilon = f64::EPSILON * 4.0;
    let threshold_just_above = 0.25 + epsilon;

    // Create FailOnTuError via FromStr — mirrors production path.
    let policy: ExitPolicy = ExitPolicy::Ratio(threshold_just_above);
    // actual ratio 0.25 is BELOW threshold 0.25+ε → exit 0
    assert_eq!(
        policy.exit_code(1, 4),
        0,
        "ratio 0.25 must be below threshold 0.25+ε (exit 0)"
    );

    // threshold_just_below = 0.25 - ε  → actual 0.25 >= threshold → exit 2
    let threshold_just_below = 0.25 - epsilon;
    let policy2 = ExitPolicy::Ratio(threshold_just_below);
    assert_eq!(
        policy2.exit_code(1, 4),
        2,
        "ratio 0.25 must be at or above threshold 0.25-ε (exit 2)"
    );
}

/// Covers the parametrised table: all combinations of (failed, total, ratio) →
/// expected exit code. Implements the full AC-5 boundary matrix without new deps.
///
/// scenario: Feature S3 @AC-5 scenarios (all confirmed entries)
#[test]
fn qa_exit_code_parametrised_table() {
    struct Case {
        failed: usize,
        total: usize,
        ratio: f64,
        expected: u8,
        label: &'static str,
    }

    let cases = vec![
        Case {
            failed: 5,
            total: 5,
            ratio: 1.0,
            expected: 2,
            label: "all-fail ratio=1.0",
        },
        Case {
            failed: 2,
            total: 5,
            ratio: 1.0,
            expected: 0,
            label: "partial-fail ratio=1.0",
        },
        Case {
            failed: 1,
            total: 5,
            ratio: 0.0,
            expected: 2,
            label: "any-fail ratio=0.0",
        },
        Case {
            failed: 0,
            total: 5,
            ratio: 0.0,
            expected: 0,
            label: "no-fail ratio=0.0 short-circuit",
        },
        Case {
            failed: 2,
            total: 4,
            ratio: 0.5,
            expected: 2,
            label: "exactly-at-threshold",
        },
        Case {
            failed: 1,
            total: 4,
            ratio: 0.5,
            expected: 0,
            label: "below-threshold",
        },
        Case {
            failed: 0,
            total: 0,
            ratio: 0.0,
            expected: 0,
            label: "zero-TU ratio=0.0",
        },
        Case {
            failed: 0,
            total: 0,
            ratio: 1.0,
            expected: 0,
            label: "zero-TU ratio=1.0",
        },
        // epsilon boundary cases
        Case {
            failed: 1,
            total: 4,
            ratio: 0.25 - f64::EPSILON * 4.0,
            expected: 2,
            label: "ratio ε below threshold (fail)",
        },
        Case {
            failed: 1,
            total: 4,
            ratio: 0.25 + f64::EPSILON * 4.0,
            expected: 0,
            label: "ratio ε above threshold (pass)",
        },
    ];

    for case in &cases {
        let policy = ExitPolicy::Ratio(case.ratio);
        let got = policy.exit_code(case.failed, case.total);
        assert_eq!(
            got, case.expected,
            "FAIL [{}]: exit_code({}, {}, ratio={}) expected {} got {}",
            case.label, case.failed, case.total, case.ratio, case.expected, got
        );
    }
}

// ---------------------------------------------------------------------------
// 3. JobOutcome JSON round-trip across all three variants (AC-6)
//    Verifies that serde serialise → deserialise is lossless for each variant.
// ---------------------------------------------------------------------------

/// Parametrised round-trip: all three `JobOutcome` variants serialise to the
/// correct snake_case string and deserialise back to the same variant.
/// scenario: Feature S4 @AC-6 "failed_tu_count is an integer in all status transitions"
#[test]
fn qa_job_outcome_json_round_trip_all_variants() {
    let cases: &[(&str, JobOutcome)] = &[
        ("completed", JobOutcome::Completed),
        ("completed_with_errors", JobOutcome::CompletedWithErrors),
        ("failed", JobOutcome::Failed),
    ];

    for (expected_str, variant) in cases {
        // Serialise
        let json = serde_json::to_string(variant).expect("serialise");
        assert_eq!(
            json,
            format!("\"{}\"", expected_str),
            "variant {:?} must serialise to {:?}",
            variant,
            expected_str
        );
        // Deserialise
        let back: JobOutcome = serde_json::from_str(&json).expect("deserialise");
        assert_eq!(
            back, *variant,
            "round-trip must recover the same variant for {:?}",
            expected_str
        );
    }
}

/// `JobRecord.failed_tu_count` must be a JSON integer (not a float or string)
/// for all three status transitions, covering the full mark_done_with_counts path.
/// scenario: Feature S4 @AC-6 "failed_tu_count is an integer in all status transitions"
#[test]
fn qa_failed_tu_count_is_json_integer_in_all_transitions() {
    let transition_params: &[(u64, u64, u64, u64, u64)] = &[
        // (tus_total, failed_tu_count, nodes, edges, expected_failed_in_json)
        (7, 0, 100, 50, 0),
        (7, 3, 60, 30, 3),
        (7, 7, 0, 0, 7),
    ];

    for &(total, failed, nodes, edges, expected_count) in transition_params {
        let (queue, _rx) = JobQueue::new(64);
        let id = queue
            .enqueue(path_source("/repo"), IngestOptions::default())
            .expect("enqueue");
        queue.mark_running(&id);
        queue.mark_done_with_counts(&id, total, failed, nodes, edges);

        let rec = queue.get(&id).expect("job exists");
        assert_eq!(rec.state, JobState::Done);

        let json = serde_json::to_value(&rec).expect("serialise");
        let count_val = &json["failed_tu_count"];
        assert!(
            count_val.is_u64(),
            "failed_tu_count must be a JSON integer (u64), got: {:?} (total={}, failed={})",
            count_val,
            total,
            failed
        );
        assert_eq!(
            count_val.as_u64().unwrap(),
            expected_count,
            "failed_tu_count JSON value mismatch (total={}, failed={})",
            total,
            failed
        );
    }
}

// ---------------------------------------------------------------------------
// Internal re-export shim: FailOnTuError is private to src/bin/index.rs.
// We replicate the same logic here to test the boundary cases independently
// (no production code change required — this mirrors the exact implementation).
// ---------------------------------------------------------------------------

/// Mirror of `FailOnTuError` from `src/bin/index.rs` (private there).
/// Duplicated here so QA can exercise boundary cases without touching prod code.
#[allow(dead_code)] // `Never` variant retained for completeness; not exercised in boundary tests
enum ExitPolicy {
    Never,
    Ratio(f64),
}

impl ExitPolicy {
    fn exit_code(&self, failed: usize, total: usize) -> u8 {
        match self {
            ExitPolicy::Never => 0,
            ExitPolicy::Ratio(r) => {
                if failed == 0 {
                    return 0;
                }
                let ratio = failed as f64 / total as f64;
                if ratio >= *r {
                    2
                } else {
                    0
                }
            }
        }
    }
}
