//! Axum route definitions for `cxg-daemon`.
//!
//! Routes (S33):
//! - `POST /v1/ingest`   — enqueue an indexing job
//! - `GET  /v1/jobs`     — list jobs (optional `?state=` filter, `?limit=` cap)
//! - `GET  /v1/jobs/:id` — get a single job by ID
//! - `GET  /v1/repos`    — list known repositories
//! - `GET  /v1/status`   — daemon health + version
//!
//! Auth middleware (`bearer_auth_writes_only`) is applied at the router level
//! in [`build_router`]; individual handlers do not re-check.
//!
//! ADR-5.

use std::{
    net::SocketAddr,
    path::{Path as FsPath, PathBuf},
    sync::Arc,
};

use axum::{
    extract::{Path, Query, State},
    http::StatusCode,
    middleware,
    response::{IntoResponse, Json},
    routing::{get, post},
    Router,
};
use serde::{Deserialize, Serialize};
use tower_http::trace::TraceLayer;

use super::{
    auth::{bearer_auth_writes_only, BearerToken},
    jobs::{IngestRequest, IngestSource, JobQueue, JobState, RepoInfo, RepoRegistry},
    problem::Problem,
    reset::{self, ResetState},
};
use crate::{config::WorkspaceConfig, sink::GraphSink};

// ── AppState ───────────────────────────────────────────────────────────────────

/// Shared state injected into every handler via `axum::extract::State`.
#[derive(Clone)]
pub struct AppState {
    pub queue: JobQueue,
    pub repos: RepoRegistry,
    pub sink: Arc<dyn GraphSink>,
    pub listen: SocketAddr,
    pub version: String,
    /// Optional workspace config (required for git_url ingest).
    pub workspace_cfg: Option<WorkspaceConfig>,
    /// Root directory for staging cache cleanup.
    pub stage_root: PathBuf,
}

impl ResetState for AppState {
    fn sink(&self) -> Arc<dyn GraphSink> {
        Arc::clone(&self.sink)
    }

    fn stage_root(&self) -> &FsPath {
        &self.stage_root
    }
}

// ── Router builder ─────────────────────────────────────────────────────────────

/// Build the axum [`Router`] for `cxg-daemon`.
///
/// `token` is the bearer token loaded from the env var named in
/// `[api].auth_token_env`.
pub fn build_router(state: AppState, token: BearerToken) -> Router {
    let state = Arc::new(state);
    Router::new()
        .route("/v1/ingest", post(ingest))
        .route("/v1/reset", post(reset::handle_reset::<AppState>))
        .route("/v1/jobs", get(list_jobs))
        .route("/v1/jobs/:id", get(get_job))
        .route("/v1/repos", get(list_repos))
        .route("/v1/status", get(status))
        .route("/metrics", get(super::metrics::handler))
        .layer(middleware::from_fn_with_state(
            token,
            bearer_auth_writes_only,
        ))
        .layer(TraceLayer::new_for_http())
        .with_state(state)
}

// ── POST /v1/ingest ────────────────────────────────────────────────────────────

/// Response body for a successful `POST /v1/ingest`.
#[derive(Debug, Serialize)]
struct IngestAccepted {
    job_id: String,
}

async fn ingest(
    State(state): State<Arc<AppState>>,
    Json(req): Json<IngestRequest>,
) -> Result<impl IntoResponse, Problem> {
    // git_url source: validate allowlist before enqueue. Clone/fetch happens
    // in the worker so git failures become job failures.
    if let IngestSource::GitUrl {
        ref git_url,
        git_ref: _,
    } = req.source
    {
        let ws_cfg = state.workspace_cfg.as_ref().ok_or_else(|| {
            Problem::new(
                StatusCode::NOT_IMPLEMENTED,
                "git_url ingest not configured",
            )
            .detail("no [workspace] section in daemon config; configure workspace.dir and allowed_hosts")
        })?;

        if !crate::workspace::allowlist::is_allowed(git_url, &ws_cfg.allowed_hosts) {
            return Err(
                Problem::new(StatusCode::FORBIDDEN, "git host not in allowlist")
                    .detail("git_url host is not listed in [workspace].allowed_hosts"),
            );
        }
    }

    match state.queue.enqueue(req.source, req.options) {
        Some(job_id) => {
            let body = Json(IngestAccepted { job_id });
            Ok((StatusCode::ACCEPTED, body).into_response())
        }
        None => Err(
            Problem::new(StatusCode::TOO_MANY_REQUESTS, "job queue full")
                .detail("retry after a running job completes"),
        ),
    }
}

// ── GET /v1/jobs ───────────────────────────────────────────────────────────────

#[derive(Debug, Deserialize)]
struct ListJobsQuery {
    /// Filter by job state: `queued`, `running`, `done`, `failed`.
    state: Option<String>,
    /// Maximum number of results (default: no cap).
    limit: Option<usize>,
}

async fn list_jobs(
    State(state): State<Arc<AppState>>,
    Query(params): Query<ListJobsQuery>,
) -> Result<impl IntoResponse, Problem> {
    let state_filter = match params.state.as_deref() {
        None => None,
        Some("queued") => Some(JobState::Queued),
        Some("running") => Some(JobState::Running),
        Some("done") => Some(JobState::Done),
        Some("failed") => Some(JobState::Failed),
        Some(other) => {
            return Err(
                Problem::new(StatusCode::BAD_REQUEST, "invalid state parameter").detail(format!(
                    "unknown state `{other}`; valid values: queued, running, done, failed"
                )),
            );
        }
    };

    let mut jobs = state.queue.list(state_filter);
    if let Some(limit) = params.limit {
        jobs.truncate(limit);
    }

    Ok(Json(jobs).into_response())
}

// ── GET /v1/jobs/:id ───────────────────────────────────────────────────────────

async fn get_job(
    State(state): State<Arc<AppState>>,
    Path(id): Path<String>,
) -> Result<impl IntoResponse, Problem> {
    match state.queue.get(&id) {
        Some(record) => Ok(Json(record).into_response()),
        None => Err(Problem::new(StatusCode::NOT_FOUND, "job not found")
            .detail(format!("job `{id}` not found"))),
    }
}

// ── GET /v1/repos ──────────────────────────────────────────────────────────────

async fn list_repos(State(state): State<Arc<AppState>>) -> impl IntoResponse {
    let repos: Vec<RepoInfo> = state.repos.list();
    Json(repos)
}

// ── GET /v1/status ─────────────────────────────────────────────────────────────

#[derive(Debug, Serialize)]
struct StatusResponse {
    status: String,
    version: String,
    listen: String,
    queue_depth: usize,
    sink_health: String,
}

async fn status(State(state): State<Arc<AppState>>) -> Result<impl IntoResponse, Problem> {
    let health = state.sink.health().await.map_err(Problem::from)?;
    Ok(Json(StatusResponse {
        status: health.status,
        version: state.version.clone(),
        listen: state.listen.to_string(),
        queue_depth: state.queue.depth(),
        sink_health: "ok".to_owned(),
    }))
}

// ── Tests ──────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use std::{net::SocketAddr, path::PathBuf, sync::Arc};

    use axum::{
        body::Body,
        http::{self, Request},
    };
    use tower::ServiceExt as _;

    use crate::api::jobs::IngestOptions;

    #[cfg(any(test, feature = "test-mock"))]
    use crate::sink::mock::MockSink;

    /// Returns `(Router, receiver)`. Caller must keep receiver alive for the
    /// duration of the test so `try_send` inside `JobQueue::enqueue` succeeds.
    fn make_app(token: &str) -> (Router, crate::api::jobs::JobReceiver) {
        let (queue, rx) = JobQueue::new(64);
        let repos = RepoRegistry::new();
        let sink = Arc::new(MockSink::default());
        let listen: SocketAddr = "127.0.0.1:7878".parse().unwrap();

        let state = AppState {
            queue,
            repos,
            sink,
            listen,
            version: "0.1.0-test".to_owned(),
            workspace_cfg: None,
            stage_root: std::env::temp_dir(),
        };
        (build_router(state, BearerToken(token.to_owned())), rx)
    }

    fn make_app_small_queue(token: &str) -> (Router, crate::api::jobs::JobReceiver) {
        let (queue, rx) = JobQueue::new(1);
        let repos = RepoRegistry::new();
        let sink = Arc::new(MockSink::default());
        let listen: SocketAddr = "127.0.0.1:7878".parse().unwrap();

        let state = AppState {
            queue,
            repos,
            sink,
            listen,
            version: "0.1.0-test".to_owned(),
            workspace_cfg: None,
            stage_root: std::env::temp_dir(),
        };
        (build_router(state, BearerToken(token.to_owned())), rx)
    }

    const TOKEN: &str = "test-bearer-token";

    // (a) POST /v1/ingest returns 202 + job_id
    #[tokio::test]
    async fn post_ingest_returns_202_with_job_id() {
        let (app, _rx) = make_app(TOKEN);
        let body = serde_json::json!({
            "source": { "path": "/workspace/my-repo" }
        });
        let req = Request::builder()
            .method(http::Method::POST)
            .uri("/v1/ingest")
            .header(http::header::CONTENT_TYPE, "application/json")
            .header(http::header::AUTHORIZATION, format!("Bearer {TOKEN}"))
            .body(Body::from(serde_json::to_vec(&body).unwrap()))
            .unwrap();

        let resp = app.oneshot(req).await.unwrap();
        assert_eq!(resp.status(), StatusCode::ACCEPTED);

        let bytes = axum::body::to_bytes(resp.into_body(), 1024 * 64)
            .await
            .unwrap();
        let json: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        assert!(json.get("job_id").is_some(), "response must include job_id");
    }

    // (b) GET /v1/jobs/{id} returns state/phase/progress
    #[tokio::test]
    async fn get_job_returns_state_phase_progress() {
        let (queue, _rx) = JobQueue::new(64);
        let repos = RepoRegistry::new();
        let sink = Arc::new(MockSink::default());
        let listen: SocketAddr = "127.0.0.1:7878".parse().unwrap();

        // Pre-insert a job.
        let job_id = queue
            .enqueue(
                IngestSource::Path {
                    path: PathBuf::from("/repo"),
                },
                IngestOptions::default(),
            )
            .unwrap();

        let state = AppState {
            queue,
            repos,
            sink,
            listen,
            version: "0.1.0-test".to_owned(),
            workspace_cfg: None,
            stage_root: std::env::temp_dir(),
        };
        let app = build_router(state, BearerToken(TOKEN.to_owned()));

        let req = Request::builder()
            .method(http::Method::GET)
            .uri(format!("/v1/jobs/{job_id}"))
            .body(Body::empty())
            .unwrap();

        let resp = app.oneshot(req).await.unwrap();
        assert_eq!(resp.status(), StatusCode::OK);

        let bytes = axum::body::to_bytes(resp.into_body(), 1024 * 64)
            .await
            .unwrap();
        let json: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(json["state"], "queued");
        assert_eq!(json["phase"], "bootstrap");
        assert!(json["progress"].is_number());
        assert_eq!(json["tus_done"], 0);
        assert_eq!(json["tus_total"], 0);
        assert_eq!(json["nodes"], 0);
        assert_eq!(json["edges"], 0);
    }

    // (c) POST /v1/ingest without bearer → 401 with problem+json
    #[tokio::test]
    async fn post_ingest_without_bearer_returns_401() {
        let (app, _rx) = make_app(TOKEN);
        let body = serde_json::json!({
            "source": { "path": "/repo" }
        });
        let req = Request::builder()
            .method(http::Method::POST)
            .uri("/v1/ingest")
            .header(http::header::CONTENT_TYPE, "application/json")
            .body(Body::from(serde_json::to_vec(&body).unwrap()))
            .unwrap();

        let resp = app.oneshot(req).await.unwrap();
        assert_eq!(resp.status(), StatusCode::UNAUTHORIZED);
        assert_eq!(
            resp.headers().get(http::header::CONTENT_TYPE).unwrap(),
            "application/problem+json"
        );
    }

    // (d) errors are application/problem+json
    #[tokio::test]
    async fn get_unknown_job_returns_problem_json() {
        let (app, _rx) = make_app(TOKEN);
        let req = Request::builder()
            .method(http::Method::GET)
            .uri("/v1/jobs/nonexistent-id")
            .body(Body::empty())
            .unwrap();

        let resp = app.oneshot(req).await.unwrap();
        assert_eq!(resp.status(), StatusCode::NOT_FOUND);
        assert_eq!(
            resp.headers().get(http::header::CONTENT_TYPE).unwrap(),
            "application/problem+json"
        );

        let bytes = axum::body::to_bytes(resp.into_body(), 1024 * 64)
            .await
            .unwrap();
        let json: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        assert!(json.get("type").is_some());
        assert!(json.get("title").is_some());
        assert_eq!(json["status"], 404);
    }

    // (e) GET /v1/repos returns expected fields
    #[tokio::test]
    async fn get_repos_returns_expected_fields() {
        let (queue, _rx) = JobQueue::new(64);
        let repos = RepoRegistry::new();
        repos.upsert(super::super::jobs::RepoInfo {
            name: "test-repo".to_owned(),
            path: "/workspace/test-repo".to_owned(),
            last_job_id: "job-1".to_owned(),
        });
        let sink = Arc::new(MockSink::default());
        let listen: SocketAddr = "127.0.0.1:7878".parse().unwrap();
        let state = AppState {
            queue,
            repos,
            sink,
            listen,
            version: "0.1.0-test".to_owned(),
            workspace_cfg: None,
            stage_root: std::env::temp_dir(),
        };
        let app = build_router(state, BearerToken(TOKEN.to_owned()));

        let req = Request::builder()
            .method(http::Method::GET)
            .uri("/v1/repos")
            .body(Body::empty())
            .unwrap();

        let resp = app.oneshot(req).await.unwrap();
        assert_eq!(resp.status(), StatusCode::OK);

        let bytes = axum::body::to_bytes(resp.into_body(), 1024 * 64)
            .await
            .unwrap();
        let json: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        let arr = json.as_array().expect("response must be array");
        assert_eq!(arr.len(), 1);
        assert!(arr[0].get("name").is_some());
        assert!(arr[0].get("path").is_some());
        assert!(arr[0].get("last_job_id").is_some());
    }

    // S5-SC-02: GET /v1/repos contains no raw integer symbol/file IDs in response
    //
    // Currently `RepoInfo` carries only `name`, `path`, `last_job_id` — no
    // symbol_id / file_id / src_id / dst_id fields.  This test asserts that
    // invariant holds so a future schema change does not silently leak raw
    // integer IDs to API consumers.
    //
    // Deferred consumers (cpp-mcp tools that read node/edge integer IDs directly
    // from the graph) are out of scope for this story — see implementation-notes.md.
    #[tokio::test]
    async fn get_repos_response_has_no_raw_integer_symbol_ids() {
        let (queue, _rx) = JobQueue::new(64);
        let repos = RepoRegistry::new();
        repos.upsert(super::super::jobs::RepoInfo {
            name: "test-repo".to_owned(),
            path: "/workspace/test-repo".to_owned(),
            last_job_id: "job-1".to_owned(),
        });
        let sink = Arc::new(MockSink::default());
        let listen: SocketAddr = "127.0.0.1:7878".parse().unwrap();
        let state = AppState {
            queue,
            repos,
            sink,
            listen,
            version: "0.1.0-test".to_owned(),
            workspace_cfg: None,
            stage_root: std::env::temp_dir(),
        };
        let app = build_router(state, BearerToken(TOKEN.to_owned()));

        let req = Request::builder()
            .method(http::Method::GET)
            .uri("/v1/repos")
            .body(Body::empty())
            .unwrap();

        let resp = app.oneshot(req).await.unwrap();
        assert_eq!(resp.status(), StatusCode::OK);

        let bytes = axum::body::to_bytes(resp.into_body(), 1024 * 64)
            .await
            .unwrap();
        let json: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        let arr = json.as_array().expect("response must be array");

        // Assert none of the raw integer id field names appear in the response objects.
        let raw_id_fields = ["symbol_id", "file_id", "src_id", "dst_id"];
        for item in arr {
            for field in &raw_id_fields {
                assert!(
                    item.get(field).is_none(),
                    "GET /v1/repos must not expose raw integer id field '{field}' (S5-SC-02)"
                );
            }
        }
    }

    // (f) [api].listen address is accessible from AppState
    #[test]
    fn listen_address_honoured_in_app_state() {
        let listen: SocketAddr = "127.0.0.1:9999".parse().unwrap();
        let (queue, _rx) = JobQueue::new(64);
        let repos = RepoRegistry::new();
        let sink = Arc::new(MockSink::default());
        let state = AppState {
            queue,
            repos,
            sink,
            listen,
            version: "0.1.0".to_owned(),
            workspace_cfg: None,
            stage_root: std::env::temp_dir(),
        };
        // The listen address is stored verbatim; real binding happens in daemon.rs.
        assert_eq!(state.listen.port(), 9999);
        assert_eq!(state.listen.ip().to_string(), "127.0.0.1");
    }

    // Queue-full returns 429
    #[tokio::test]
    async fn post_ingest_returns_429_when_queue_full() {
        let body = serde_json::json!({
            "source": { "path": "/repo" }
        });
        let make_req = || {
            Request::builder()
                .method(http::Method::POST)
                .uri("/v1/ingest")
                .header(http::header::CONTENT_TYPE, "application/json")
                .header(http::header::AUTHORIZATION, format!("Bearer {TOKEN}"))
                .body(Body::from(serde_json::to_vec(&body).unwrap()))
                .unwrap()
        };

        // Use a single app with capacity 1: first request fills the queue.
        // Keep _rx alive so the channel is not closed prematurely.
        let (app, _rx) = make_app_small_queue(TOKEN);
        let resp1 = app.clone().oneshot(make_req()).await.unwrap();
        assert_eq!(resp1.status(), StatusCode::ACCEPTED);

        // Second request should get 429.
        let resp2 = app.oneshot(make_req()).await.unwrap();
        assert_eq!(resp2.status(), StatusCode::TOO_MANY_REQUESTS);
    }

    #[tokio::test]
    async fn build_router_exposes_reset_route() {
        let (app, _rx) = make_app(TOKEN);
        let body = serde_json::json!({
            "target": "all",
            "confirm_token": "wrong"
        });
        let req = Request::builder()
            .method(http::Method::POST)
            .uri("/v1/reset")
            .header(http::header::CONTENT_TYPE, "application/json")
            .header(http::header::AUTHORIZATION, format!("Bearer {TOKEN}"))
            .body(Body::from(serde_json::to_vec(&body).unwrap()))
            .unwrap();

        let resp = app.oneshot(req).await.unwrap();
        assert_eq!(resp.status(), StatusCode::BAD_REQUEST);
    }

    #[tokio::test]
    async fn build_router_exposes_metrics_without_auth() {
        let (app, _rx) = make_app(TOKEN);
        let req = Request::builder()
            .method(http::Method::GET)
            .uri("/metrics")
            .body(Body::empty())
            .unwrap();

        let resp = app.oneshot(req).await.unwrap();
        assert_eq!(resp.status(), StatusCode::OK);
    }
}
