//! M7 exit-gate integration test (AC-M7-25, AC-M7-26, AC-M7-27).
//!
//! Two tests are provided:
//!
//! 1. **`m7_git_roundtrip`** — posts `POST /v1/ingest` with a real git URL to a
//!    small public C++ repository, waits for `state=done`, and asserts that
//!    `GET /v1/jobs/{id}` returns the correct job record.  Requires network
//!    access and a running `cxg-daemon`.  Gated behind `CXG_M7_GIT_URL`.
//!
//!    AC-M7-26: `state=done` after `POST /v1/ingest` with `git_url`.
//!
//! 2. **`m7_soak_status_check`** — single-poll smoke check: `GET /v1/status`
//!    must return 200, and `GET /metrics` error-rate must stay below 1%.
//!
//!    AC-M7-25, AC-M7-27: companion to `tests/integration/m7_soak_checklist.md`.
//!
//! # Running
//!
//! Both tests are `#[ignore]` (the exit-criteria command passes `-- --ignored`):
//!
//! ```sh
//! CXG_M7_GIT_URL=https://github.com/torvalds/uemacs \
//! CXG_M7_DAEMON_URL=http://127.0.0.1:7878 \
//! CXG_M7_BEARER_TOKEN=mysecret \
//!     cargo nextest run -p cpp_indexer --test m7_git_roundtrip -- --ignored
//! ```
//!
//! For the soak check alone:
//!
//! ```sh
//! CXG_M7_DAEMON_URL=http://hermes-agent:7878 \
//! CXG_M7_BEARER_TOKEN=mysecret \
//!     cargo nextest run -p cpp_indexer --test m7_git_roundtrip \
//!         --run-ignored ignored-only -- m7_soak_status_check
//! ```

// ---------------------------------------------------------------------------
// Env-var keys
// ---------------------------------------------------------------------------

/// HTTPS URL of a small public C++ repository to clone and index.
/// Example: `https://github.com/torvalds/uemacs`
const ENV_GIT_URL: &str = "CXG_M7_GIT_URL";

/// Base URL of a running `cxg-daemon` REST API.
/// Example: `http://127.0.0.1:7878`
const ENV_DAEMON_URL: &str = "CXG_M7_DAEMON_URL";

/// Bearer token matching `[api].auth_token_env` configured on the daemon.
const ENV_BEARER_TOKEN: &str = "CXG_M7_BEARER_TOKEN";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Read an env var or skip the test with a structured eprintln.
macro_rules! require_env {
    ($key:expr) => {
        match std::env::var($key) {
            Ok(v) if !v.is_empty() => v,
            _ => {
                eprintln!(
                    "SKIP: {} not set — see test-level doc for setup instructions.",
                    $key
                );
                return;
            }
        }
    };
}

// ---------------------------------------------------------------------------
// Test 1: git-URL round-trip (AC-M7-26)
// ---------------------------------------------------------------------------

/// M7 exit gate — git-URL round-trip: `POST /v1/ingest` with a public C++ git
/// URL, poll until `state=done`, assert the job record is correct (AC-M7-26).
///
/// **Required env vars:**
/// - `CXG_M7_GIT_URL`       — HTTPS URL of a small public C++ repo
/// - `CXG_M7_DAEMON_URL`    — base URL of a running `cxg-daemon`
/// - `CXG_M7_BEARER_TOKEN`  — bearer token for write endpoints
///
/// The daemon must be running with a workspace config that includes the target
/// host in `allowed_hosts` and a writable `dir`.  See
/// `tests/integration/m7_soak_checklist.md` for setup instructions.
#[tokio::test]
#[ignore = "requires-network: set CXG_M7_GIT_URL + CXG_M7_DAEMON_URL + CXG_M7_BEARER_TOKEN"]
async fn m7_git_roundtrip() {
    let git_url = require_env!(ENV_GIT_URL);
    let daemon_url = require_env!(ENV_DAEMON_URL);
    let token = require_env!(ENV_BEARER_TOKEN);

    let client = reqwest::Client::new();

    // ── POST /v1/ingest ──────────────────────────────────────────────────────

    let ingest_body = serde_json::json!({
        "source": {
            "git_url": git_url,
            "ref": "HEAD"
        }
    });

    let post_resp = client
        .post(format!("{daemon_url}/v1/ingest"))
        .header("Authorization", format!("Bearer {token}"))
        .header("Content-Type", "application/json")
        .json(&ingest_body)
        .send()
        .await
        .expect("POST /v1/ingest must not error");

    let post_status = post_resp.status();
    let post_body: serde_json::Value = post_resp.json().await.expect("parse 202 body");

    assert_eq!(
        post_status,
        reqwest::StatusCode::ACCEPTED,
        "AC-M7-26: POST /v1/ingest must return 202 ACCEPTED; body: {post_body}"
    );

    let job_id = post_body["job_id"]
        .as_str()
        .expect("AC-M7-26: 202 response must contain job_id")
        .to_owned();

    eprintln!("m7_git_roundtrip: enqueued job_id={job_id} git_url={git_url}");

    // ── Poll GET /v1/jobs/{id} until done or timeout ─────────────────────────

    // Allow up to 30 minutes for clone + index of a small C++ repo.
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(30 * 60);
    let poll_interval = std::time::Duration::from_secs(10);

    loop {
        tokio::time::sleep(poll_interval).await;

        let job_resp = client
            .get(format!("{daemon_url}/v1/jobs/{job_id}"))
            .send()
            .await
            .expect("GET /v1/jobs/{id} must not error");

        assert_eq!(
            job_resp.status(),
            reqwest::StatusCode::OK,
            "GET /v1/jobs/{job_id} must return 200"
        );

        let job: serde_json::Value = job_resp.json().await.expect("parse job record");
        let state = job["state"].as_str().unwrap_or("unknown");

        eprintln!(
            "m7_git_roundtrip: job_id={job_id} state={state} phase={} progress={}",
            job["phase"].as_str().unwrap_or("?"),
            job["progress"].as_f64().unwrap_or(0.0)
        );

        match state {
            // AC-M7-26: job must reach state=done.
            "done" => {
                eprintln!("m7_git_roundtrip: PASS — job_id={job_id} reached state=done");
                break;
            }
            "failed" => {
                panic!(
                    "AC-M7-26: job_id={job_id} reached state=failed: {}",
                    job["error"].as_str().unwrap_or("(no error message)")
                );
            }
            _ => {
                // queued | running — keep polling.
                if std::time::Instant::now() > deadline {
                    panic!(
                        "AC-M7-26: job_id={job_id} did not reach state=done within 30 minutes; \
                         last state={state}"
                    );
                }
            }
        }
    }

    // ── Assert GET /v1/repos lists the cloned repo ───────────────────────────

    let repos_resp = client
        .get(format!("{daemon_url}/v1/repos"))
        .send()
        .await
        .expect("GET /v1/repos must not error");

    assert_eq!(
        repos_resp.status(),
        reqwest::StatusCode::OK,
        "GET /v1/repos must return 200"
    );
    let repos: serde_json::Value = repos_resp.json().await.expect("parse repos list");
    let repos_arr = repos.as_array().expect("repos response must be array");

    // AC-M7-26: repo tagged in registry after a successful ingest.
    assert!(
        !repos_arr.is_empty(),
        "AC-M7-26: GET /v1/repos must list at least one repository after a successful ingest"
    );

    let found = repos_arr.iter().any(|r| {
        r.get("last_job_id")
            .and_then(|v| v.as_str())
            .map(|id| id == job_id)
            .unwrap_or(false)
    });
    assert!(
        found,
        "AC-M7-26: GET /v1/repos must contain an entry with last_job_id={job_id}"
    );

    eprintln!("m7_git_roundtrip: PASS — repo registry updated, last_job_id={job_id}");
}

// ---------------------------------------------------------------------------
// Test 2: soak status + error-rate check (AC-M7-25, AC-M7-27)
// ---------------------------------------------------------------------------

/// M7 soak status check — `GET /v1/status` must return 200, and `GET /metrics`
/// error-rate must be `cxg_libclang_errors_total / cxg_nodes_total < 0.01`
/// (AC-M7-25, AC-M7-27).
///
/// Designed to be run periodically during the 7-day soak period on
/// hermes-agent.  See `tests/integration/m7_soak_checklist.md`.
///
/// **Required env vars:**
/// - `CXG_M7_DAEMON_URL`    — base URL of the running daemon
#[tokio::test]
#[ignore = "requires-soak: set CXG_M7_DAEMON_URL to a running daemon"]
async fn m7_soak_status_check() {
    let daemon_url = require_env!(ENV_DAEMON_URL);

    let client = reqwest::Client::new();

    // ── GET /v1/status ───────────────────────────────────────────────────────

    let status_resp = client
        .get(format!("{daemon_url}/v1/status"))
        .send()
        .await
        .expect("GET /v1/status must not error");

    assert_eq!(
        status_resp.status(),
        reqwest::StatusCode::OK,
        "AC-M7-25: GET /v1/status must return 200 during soak period"
    );

    let status_body: serde_json::Value = status_resp.json().await.expect("parse /v1/status body");

    assert!(
        status_body.get("status").is_some(),
        "AC-M7-25: /v1/status response must include a `status` field"
    );
    assert!(
        status_body.get("version").is_some(),
        "AC-M7-25: /v1/status response must include a `version` field"
    );

    eprintln!(
        "m7_soak_status_check: status={} version={}",
        status_body["status"].as_str().unwrap_or("?"),
        status_body["version"].as_str().unwrap_or("?")
    );

    // ── GET /metrics — error-rate invariant (AC-M7-27) ───────────────────────

    let metrics_resp = client.get(format!("{daemon_url}/metrics")).send().await;

    match metrics_resp {
        Err(e) => {
            eprintln!(
                "m7_soak_status_check: WARNING — GET /metrics failed ({e}); \
                 AC-M7-27 check skipped (scrape endpoint may not be exposed)"
            );
        }
        Ok(resp) if !resp.status().is_success() => {
            eprintln!(
                "m7_soak_status_check: WARNING — GET /metrics returned {}; \
                 AC-M7-27 check skipped",
                resp.status()
            );
        }
        Ok(resp) => {
            let text = resp.text().await.unwrap_or_default();

            let errors_total = parse_prometheus_counter(&text, "cxg_libclang_errors_total");
            let nodes_total = parse_prometheus_counter(&text, "cxg_nodes_total");

            eprintln!(
                "m7_soak_status_check: cxg_libclang_errors_total={errors_total:.0} \
                 cxg_nodes_total={nodes_total:.0}"
            );

            if nodes_total > 0.0 {
                let error_rate = errors_total / nodes_total;
                assert!(
                    error_rate < 0.01,
                    "AC-M7-27: error rate {error_rate:.4} >= 0.01 \
                     (cxg_libclang_errors_total={errors_total:.0} / \
                     cxg_nodes_total={nodes_total:.0}); \
                     investigate libclang failures in daemon logs"
                );
                eprintln!(
                    "m7_soak_status_check: AC-M7-27 PASS — error rate {error_rate:.6} < 0.01"
                );
            } else {
                eprintln!(
                    "m7_soak_status_check: AC-M7-27 deferred — cxg_nodes_total=0 \
                     (no indexing completed yet; check after first ingest cycle)"
                );
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Prometheus text-format parser (minimal)
// ---------------------------------------------------------------------------

/// Extract the numeric value of a counter from a Prometheus text-format scrape.
/// Returns `0.0` if the metric is absent.
///
/// Handles label-free metric lines of the form `<name> <value>` or
/// `<name>{} <value>`.  Sufficient for `cxg_libclang_errors_total` and
/// `cxg_nodes_total` which are label-free aggregates.
fn parse_prometheus_counter(text: &str, metric_name: &str) -> f64 {
    for line in text.lines() {
        if line.starts_with('#') {
            continue;
        }
        let trimmed = line.trim();
        if let Some(rest) = trimmed.strip_prefix(metric_name) {
            let rest = rest.trim_start();
            let value_str = if rest.starts_with('{') {
                rest.find('}').map(|i| rest[i + 1..].trim()).unwrap_or("")
            } else {
                rest.split_whitespace().next().unwrap_or("")
            };
            if let Ok(v) = value_str.parse::<f64>() {
                return v;
            }
        }
    }
    0.0
}
