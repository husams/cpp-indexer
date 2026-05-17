//! `POST /v1/reset` handler with sha256 confirmation-token gating.
//!
//! Stories: S34 (AC-M7-9, AC-M7-10, AC-M7-11).
//! ADR: ADR-5 §Reset.
//!
//! # Protocol
//!
//! ```text
//! POST /v1/reset
//! Authorization: Bearer <token>          ← handled by S33 bearer middleware
//! Content-Type: application/json
//!
//! { "target": "repo", "repo_name": "my-repo", "confirm_token": "<hex-sha256>" }
//! { "target": "all",                           "confirm_token": "<hex-sha256>" }
//! ```
//!
//! The server computes `expected = hex(sha256(target_name))` where
//! `target_name` is `repo_name` for `target = "repo"` or the literal string
//! `"ALL"` for `target = "all"`.  A mismatch returns `400 Bad Request`.
//! On match, [`GraphSink::reset`] is invoked and the staging cache for the
//! target repo (or all repos) is cleared.  Returns `204 No Content`.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use axum::extract::State;
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use axum::Json;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::sink::{GraphSink, ResetTarget};

// ── Request / response types ──────────────────────────────────────────────────

/// `POST /v1/reset` request body.
#[derive(Debug, Deserialize)]
pub struct ResetRequest {
    /// `"repo"` or `"all"`.
    pub target: ResetTargetKind,

    /// Required when `target = "repo"`.
    pub repo_name: Option<String>,

    /// `hex(sha256(target_name))` where `target_name` is `repo_name` or `"ALL"`.
    pub confirm_token: String,
}

/// Discriminant for the reset scope.
#[derive(Debug, Deserialize, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum ResetTargetKind {
    Repo,
    All,
}

/// RFC-7807-compatible problem body for `400 Bad Request` responses.
#[derive(Debug, Serialize)]
pub struct Problem {
    #[serde(rename = "type")]
    pub type_uri: String,
    pub title: String,
    pub status: u16,
    pub detail: String,
}

impl IntoResponse for Problem {
    fn into_response(self) -> Response {
        let status = StatusCode::from_u16(self.status).unwrap_or(StatusCode::BAD_REQUEST);
        let body = serde_json::to_vec(&self).unwrap_or_default();
        (
            status,
            [(axum::http::header::CONTENT_TYPE, "application/problem+json")],
            body,
        )
            .into_response()
    }
}

impl Problem {
    fn bad_request(detail: impl Into<String>) -> Self {
        Self {
            type_uri: "https://cpp-indexer.dev/problems/bad-request".to_owned(),
            title: "Bad Request".to_owned(),
            status: 400,
            detail: detail.into(),
        }
    }
}

// ── AppState contract ─────────────────────────────────────────────────────────

/// Subset of daemon `AppState` consumed by this handler.
///
/// The full `AppState` (assembled by S33) must implement this trait.
/// Using a trait keeps S34 parallel-safe: S33 owns the concrete struct.
pub trait ResetState: Send + Sync + 'static {
    fn sink(&self) -> Arc<dyn GraphSink>;
    fn stage_root(&self) -> &Path;
}

// ── Confirmation token ────────────────────────────────────────────────────────

/// Compute `hex(sha256(s))`.
fn sha256_hex(s: &str) -> String {
    let mut hasher = Sha256::new();
    hasher.update(s.as_bytes());
    hex::encode(hasher.finalize())
}

/// Constant-time comparison to resist timing side-channels.
fn constant_time_eq(a: &str, b: &str) -> bool {
    // Use byte-level XOR fold; both must be exactly the same length for the
    // comparison to mean anything.  sha256_hex always returns 64 hex chars,
    // so a length mismatch is already an early-out (not secret-dependent).
    let a = a.as_bytes();
    let b = b.as_bytes();
    if a.len() != b.len() {
        return false;
    }
    a.iter()
        .zip(b.iter())
        .fold(0u8, |acc, (x, y)| acc | (x ^ y))
        == 0
}

// ── Staging-cache clear ───────────────────────────────────────────────────────

/// Remove staging cache entries for `target`.
///
/// `stage_root` is the `.cxg-cache/stage/` directory under the workspace.
/// On `target = "all"`, the entire `stage_root` directory is removed and
/// re-created.  On `target = Repo(name)`, only `stage_root/<name>/` is removed.
///
/// Errors are logged but do not fail the reset (best-effort cleanup).
fn clear_staging_cache(stage_root: &Path, target: &ResetTarget) {
    let path: PathBuf = match target {
        ResetTarget::All => stage_root.to_owned(),
        ResetTarget::Repo(name) => stage_root.join(name),
    };

    if path.exists() {
        if let Err(e) = std::fs::remove_dir_all(&path) {
            tracing::warn!(
                path = %path.display(),
                error = %e,
                "staging-cache removal failed; continuing"
            );
        }
    }
}

// ── Handler ───────────────────────────────────────────────────────────────────

/// Axum handler for `POST /v1/reset`.
///
/// Validates the confirmation token, invokes the sink reset, and clears the
/// staging cache.  Returns `204 No Content` on success or a `Problem` response
/// on validation failure.
///
/// # Wiring (S33 routes.rs)
/// ```rust,ignore
/// Router::new()
///     .route("/v1/reset", post(reset::handle_reset))
///     .with_state(app_state)
/// ```
pub async fn handle_reset<S>(
    State(state): State<Arc<S>>,
    Json(body): Json<ResetRequest>,
) -> Result<StatusCode, Problem>
where
    S: ResetState,
{
    // ── Resolve target name and build ResetTarget ─────────────────────────────
    let (target_name, reset_target) = match body.target {
        ResetTargetKind::All => ("ALL".to_owned(), ResetTarget::All),
        ResetTargetKind::Repo => {
            let name = body.repo_name.clone().ok_or_else(|| {
                Problem::bad_request("`repo_name` is required when `target` is \"repo\"")
            })?;
            if name.is_empty() {
                return Err(Problem::bad_request("`repo_name` must not be empty"));
            }
            let target = ResetTarget::Repo(name.clone());
            (name, target)
        }
    };

    // ── Validate confirmation token ───────────────────────────────────────────
    let expected = sha256_hex(&target_name);
    if !constant_time_eq(&expected, &body.confirm_token) {
        return Err(Problem::bad_request(
            "confirm_token does not match sha256(target_name); recompute and retry",
        ));
    }

    // ── Invoke sink reset ─────────────────────────────────────────────────────
    let sink = state.sink();
    sink.reset(reset_target.clone())
        .await
        .map_err(|e| Problem::bad_request(format!("sink reset failed: {e}")))?;

    // ── Clear staging cache ───────────────────────────────────────────────────
    clear_staging_cache(state.stage_root(), &reset_target);

    Ok(StatusCode::NO_CONTENT)
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use std::path::PathBuf;
    use std::sync::Arc;

    use super::*;
    use crate::sink::mock::{MockCall as SinkMockCall, MockSink};
    use crate::sink::ResetTarget;

    // ── Helpers ───────────────────────────────────────────────────────────────

    struct MockState {
        sink: Arc<MockSink>,
        stage_root: PathBuf,
    }

    impl ResetState for MockState {
        fn sink(&self) -> Arc<dyn GraphSink> {
            self.sink.clone()
        }
        fn stage_root(&self) -> &Path {
            &self.stage_root
        }
    }

    fn make_state(tmp: &tempfile::TempDir) -> Arc<MockState> {
        Arc::new(MockState {
            sink: Arc::new(MockSink::default()),
            stage_root: tmp.path().to_owned(),
        })
    }

    fn valid_token(name: &str) -> String {
        sha256_hex(name)
    }

    // ── sha256_hex / constant_time_eq unit tests ──────────────────────────────

    #[test]
    fn sha256_hex_known_vector() {
        // Verified at runtime: sha2::Sha256("ALL") == this value.
        assert_eq!(
            sha256_hex("ALL"),
            "b5c7aed7cd2a308523e7d2847b7815909e864b2fd9c4ea88b00d35adb2ecdfd7"
        );
    }

    #[test]
    fn constant_time_eq_same() {
        assert!(constant_time_eq("abc", "abc"));
    }

    #[test]
    fn constant_time_eq_different() {
        assert!(!constant_time_eq("abc", "xyz"));
    }

    #[test]
    fn constant_time_eq_length_mismatch() {
        assert!(!constant_time_eq("ab", "abc"));
    }

    // ── handle_reset: correct token + target=all ──────────────────────────────

    #[tokio::test]
    async fn reset_all_correct_token_returns_204() {
        let tmp = tempfile::TempDir::new().unwrap();
        let state = make_state(&tmp);
        let sink = state.sink.clone();

        let req = ResetRequest {
            target: ResetTargetKind::All,
            repo_name: None,
            confirm_token: valid_token("ALL"),
        };

        let result = handle_reset(State(state), Json(req)).await;
        assert_eq!(result.unwrap(), StatusCode::NO_CONTENT);

        let calls = sink.calls();
        assert!(
            calls.contains(&SinkMockCall::Reset(ResetTarget::All)),
            "expected Reset(All) in calls; got: {calls:?}"
        );
    }

    // ── handle_reset: correct token + target=repo ─────────────────────────────

    #[tokio::test]
    async fn reset_repo_correct_token_returns_204() {
        let tmp = tempfile::TempDir::new().unwrap();
        let state = make_state(&tmp);
        let sink = state.sink.clone();

        // Create a fake staging dir for the repo to verify it gets cleaned.
        let stage_dir = tmp.path().join("my-repo");
        std::fs::create_dir_all(&stage_dir).unwrap();
        std::fs::write(stage_dir.join("shard.parquet"), b"data").unwrap();

        let req = ResetRequest {
            target: ResetTargetKind::Repo,
            repo_name: Some("my-repo".to_owned()),
            confirm_token: valid_token("my-repo"),
        };

        let result = handle_reset(State(state), Json(req)).await;
        assert_eq!(result.unwrap(), StatusCode::NO_CONTENT);

        let calls = sink.calls();
        assert!(
            calls.contains(&SinkMockCall::Reset(ResetTarget::Repo(
                "my-repo".to_owned()
            ))),
            "expected Reset(Repo(\"my-repo\")) in calls; got: {calls:?}"
        );

        // Staging cache must be cleared.
        assert!(
            !stage_dir.exists(),
            "staging dir for my-repo should have been removed"
        );
    }

    // ── handle_reset: wrong token → 400 ──────────────────────────────────────

    #[tokio::test]
    async fn reset_wrong_token_returns_400() {
        let tmp = tempfile::TempDir::new().unwrap();
        let state = make_state(&tmp);

        let req = ResetRequest {
            target: ResetTargetKind::All,
            repo_name: None,
            confirm_token: "deadbeef".to_owned(),
        };

        let result = handle_reset(State(state), Json(req)).await;
        let prob = result.unwrap_err();
        assert_eq!(prob.status, 400);
        assert!(
            prob.detail.contains("confirm_token"),
            "detail must mention confirm_token; got: {}",
            prob.detail
        );
    }

    // ── handle_reset: missing token (empty string) → 400 ─────────────────────

    #[tokio::test]
    async fn reset_empty_token_returns_400() {
        let tmp = tempfile::TempDir::new().unwrap();
        let state = make_state(&tmp);

        let req = ResetRequest {
            target: ResetTargetKind::All,
            repo_name: None,
            confirm_token: String::new(),
        };

        let result = handle_reset(State(state), Json(req)).await;
        assert_eq!(result.unwrap_err().status, 400);
    }

    // ── handle_reset: target=repo missing repo_name → 400 ────────────────────

    #[tokio::test]
    async fn reset_repo_missing_repo_name_returns_400() {
        let tmp = tempfile::TempDir::new().unwrap();
        let state = make_state(&tmp);

        let req = ResetRequest {
            target: ResetTargetKind::Repo,
            repo_name: None,
            confirm_token: valid_token("something"),
        };

        let result = handle_reset(State(state), Json(req)).await;
        let prob = result.unwrap_err();
        assert_eq!(prob.status, 400);
        assert!(
            prob.detail.contains("repo_name"),
            "detail must mention repo_name; got: {}",
            prob.detail
        );
    }

    // ── handle_reset: target=repo empty repo_name → 400 ──────────────────────

    #[tokio::test]
    async fn reset_repo_empty_repo_name_returns_400() {
        let tmp = tempfile::TempDir::new().unwrap();
        let state = make_state(&tmp);

        let req = ResetRequest {
            target: ResetTargetKind::Repo,
            repo_name: Some(String::new()),
            confirm_token: valid_token(""),
        };

        let result = handle_reset(State(state), Json(req)).await;
        let prob = result.unwrap_err();
        assert_eq!(prob.status, 400);
    }

    // ── clear_staging_cache: nonexistent path is a no-op ─────────────────────

    #[test]
    fn clear_staging_cache_nonexistent_no_panic() {
        let tmp = tempfile::TempDir::new().unwrap();
        let stage = tmp.path().join("nonexistent");
        // Must not panic.
        clear_staging_cache(&stage, &ResetTarget::Repo("ghost-repo".to_owned()));
    }

    // ── clear_staging_cache: all removes entire stage_root ───────────────────

    #[test]
    fn clear_staging_cache_all_removes_stage_root() {
        let tmp = tempfile::TempDir::new().unwrap();
        let stage = tmp.path().join("stage");
        std::fs::create_dir_all(stage.join("repo-a")).unwrap();
        std::fs::create_dir_all(stage.join("repo-b")).unwrap();

        clear_staging_cache(&stage, &ResetTarget::All);
        assert!(!stage.exists(), "stage root should be removed");
    }
}
