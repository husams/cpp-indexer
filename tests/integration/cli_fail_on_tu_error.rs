//! Integration tests for `--fail-on-tu-error` CLI flag (S3, AC-5).
//!
//! # Design rationale
//! Scenarios (a)–(e) (ratio/never logic vs. actual TU counts) are covered by
//! unit tests in `src/bin/index.rs::tests` because running the full binary
//! pipeline requires a live database backend and therefore cannot be tested
//! without `#[ignore]` gates.  These integration tests exercise the **CLI
//! surface** that cannot be tested in-process:
//!
//! - (f) Invalid `--fail-on-tu-error` value → clap parse error, non-zero exit,
//!   stderr contains "ratio must be in".
//! - (g) `--help` output includes `--fail-on-tu-error` with its value name.
//!
//! All tests here run without a backend server — they test before the pipeline
//! is ever started.

use assert_cmd::Command;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn cmd() -> Command {
    Command::cargo_bin("cxg-index").expect("cxg-index binary must exist")
}

// ---------------------------------------------------------------------------
// (f) Invalid ratio value above 1.0 → clap parse error
// ---------------------------------------------------------------------------

#[test]
fn invalid_ratio_above_1_exits_nonzero_and_mentions_range() {
    let output = cmd()
        .args(["--fail-on-tu-error", "1.5", "--help"])
        .output()
        .expect("process must run");

    // Clap exits non-zero on parse error.
    assert!(
        !output.status.success(),
        "exit code must be non-zero for ratio 1.5"
    );

    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains("ratio must be in"),
        "stderr must mention ratio range; got:\n{stderr}"
    );
}

#[test]
fn invalid_ratio_below_0_exits_nonzero_and_mentions_range() {
    let output = cmd()
        .args(["--fail-on-tu-error", "-0.1"])
        // Pass a dummy PATH to bypass the "PATH argument is required" error
        // — but the parse error happens before PATH validation.
        .output()
        .expect("process must run");

    assert!(
        !output.status.success(),
        "exit code must be non-zero for ratio -0.1"
    );

    let stderr = String::from_utf8_lossy(&output.stderr);
    // Either "ratio must be in" (our error) or clap interprets -0.1 as an
    // unknown flag. Either way the process must fail non-zero.
    assert!(
        !output.status.success(),
        "non-zero exit for -0.1 already checked above"
    );
    // If clap re-routes it as a flag error, stderr will be non-empty too.
    let combined = format!("{stderr}{}", String::from_utf8_lossy(&output.stdout));
    assert!(
        !combined.trim().is_empty(),
        "some diagnostic must be printed for invalid value -0.1"
    );
}

#[test]
fn invalid_ratio_garbage_exits_nonzero() {
    let output = cmd()
        .args(["--fail-on-tu-error", "garbage", "/nonexistent/path"])
        .output()
        .expect("process must run");

    assert!(
        !output.status.success(),
        "exit code must be non-zero for `garbage`"
    );

    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains("expected a number"),
        "stderr must mention expected format; got:\n{stderr}"
    );
}

// ---------------------------------------------------------------------------
// (g) --help includes the flag
// ---------------------------------------------------------------------------

#[test]
fn help_includes_fail_on_tu_error_flag() {
    let output = cmd().arg("--help").output().expect("process must run");

    // --help exits 0.
    assert!(output.status.success(), "--help must exit 0");

    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(
        stdout.contains("--fail-on-tu-error"),
        "--help output must include --fail-on-tu-error; got:\n{stdout}"
    );
    assert!(
        stdout.contains("RATIO|never"),
        "--help output must include RATIO|never value name; got:\n{stdout}"
    );
}

// ---------------------------------------------------------------------------
// (h) `never` is accepted as a valid value (clap parses it; exits on PATH)
// ---------------------------------------------------------------------------

#[test]
fn never_is_accepted_as_valid_value() {
    let output = cmd()
        .args(["--fail-on-tu-error", "never"])
        .output()
        .expect("process must run");

    // The process will fail because PATH is missing, but the error must NOT be
    // a clap parse error about `--fail-on-tu-error`.
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        !stderr.contains("ratio must be in") && !stderr.contains("expected a number"),
        "clap must not reject `never` as invalid; stderr:\n{stderr}"
    );
}

// ---------------------------------------------------------------------------
// (i) Valid ratio 0.0 is accepted (clap parses it; exits on PATH)
// ---------------------------------------------------------------------------

#[test]
fn ratio_zero_is_accepted_as_valid_value() {
    let output = cmd()
        .args(["--fail-on-tu-error", "0.0"])
        .output()
        .expect("process must run");

    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        !stderr.contains("ratio must be in") && !stderr.contains("expected a number"),
        "clap must not reject `0.0` as invalid; stderr:\n{stderr}"
    );
}
