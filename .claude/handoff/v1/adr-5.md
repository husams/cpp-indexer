# ADR-5: cxg-daemon REST contract — axum + RFC-7807 + bearer-token on writes

Status: accepted
Date: 2026-05-17
Resolves: M7-S1, M7-S2 REST contract; AC-M7-1..11

## Context

`cxg-daemon` exposes a REST control plane (CHARTER locked-in). Contract requirements:

- Endpoints: `POST /v1/ingest`, `GET /v1/jobs/{id}`, `GET /v1/jobs?state=&limit=`, `POST /v1/reset`, `GET /v1/repos`, `GET /v1/status`, `GET /metrics` (AC-M7-1..5, AC-M7-17..19).
- Bearer-token auth required on **writes** (`POST /v1/ingest`, `POST /v1/reset`); reads (`GET /v1/jobs`, `GET /v1/status`, `GET /v1/repos`) and `GET /metrics` are unauthenticated (AC-M7-6, AC-M7-18). Per PRD this is "auth on writes".
- Error response body conforms to RFC-7807 `application/problem+json` (AC-M7-7).
- `POST /v1/ingest` returns `202 Accepted` with `job_id` within 50 ms p99 (AC-M7-1).
- `GET /v1/jobs/{id}` returns within 20 ms p99 (AC-M7-2).
- Bind only to address from `[api].listen` (default `127.0.0.1:7878`); no built-in TLS (AC-M7-8).
- `POST /v1/reset` requires a `confirm_token = sha256(target_name)` (AC-M7-9..11).
- Backpressure: `cxg_queue_depth > [api].job_queue_max` returns `429 Too Many Requests` (AC-M7-19).

## Decision

Stack: `axum 0.7` + `tower-http` middleware on top of `tokio`.

### Route definition

```rust
Router::new()
  .route("/v1/ingest",      post(ingest))
  .route("/v1/jobs",        get(list_jobs))
  .route("/v1/jobs/:id",    get(get_job))
  .route("/v1/reset",       post(reset))
  .route("/v1/repos",       get(list_repos))
  .route("/v1/status",      get(status))
  .route("/metrics",        get(metrics))
  .layer(from_fn(problem_json_errors))         // global; maps any error to RFC-7807
  .layer(from_fn_with_state(state, bearer_auth_writes_only))
  .layer(TraceLayer::new_for_http())
  .with_state(app_state)
```

### Auth

- `bearer_auth_writes_only` middleware: if request method is `POST` AND path starts with `/v1/`, require `Authorization: Bearer <token>` where `<token>` equals the value of the env var named in `[api].auth_token_env`. Constant-time compare. Missing/wrong → `401 Unauthorized` with RFC-7807 body. AC-M7-6.
- `GET /metrics` and all other GETs bypass auth. AC-M7-18.
- Token is read from env at daemon startup; if env var is unset, daemon refuses to start with a clear error (no insecure mode).

### Job queue + state machine

- In-process `tokio::sync::mpsc<JobRequest>` bounded by `[api].job_queue_max` (default 64).
- Job states: `queued → running → done|failed`. Persisted in-memory only for v1 (single-tenant; see ADR-10). Survives the process only via the `manifest.json` cache for resume.
- `POST /v1/ingest` body: `{ "source": { "path": "..." } | { "git_url": "...", "ref": "..." }, "options": { "sink": "neo4j|indradb", "skip_phase2": bool } }`. Validates synchronously, enqueues, returns `202 Accepted` with `job_id` (UUID v7). If queue full → `429 Too Many Requests`. AC-M7-1, AC-M7-19.
- Workers pull from the channel; one worker = one `pipeline::run` call. Concurrency = `[index].workers` (which itself controls rayon inside the run).

### Reset

- `POST /v1/reset` body: `{ "target": "repo"|"all", "repo_name": "<name>"?, "confirm_token": "<hex>" }`.
- Server computes `expected = hex(sha256(target_name_or_"ALL"))` and compares against `confirm_token` in constant time. Mismatch → `400 Bad Request`. AC-M7-10.
- On match: invoke `sink.reset(target)` and clear `.cxg-cache/stage/<repo>/`. Returns `204 No Content`. AC-M7-9, AC-M7-11.

### Error model (RFC-7807)

`src/api/problem.rs` defines `Problem { type_uri, title, status, detail, instance, extensions }`. Every handler returns `Result<impl IntoResponse, Problem>`; `Problem: IntoResponse` produces `application/problem+json`. Domain errors (`Error::Workspace`, `Error::Sink`, ...) map via `From<Error> for Problem`. AC-M7-7.

### TLS

- No built-in TLS (AC-M7-8). Operator runs the daemon behind nginx / istio / Caddy. Documented in runbook.md (DevOps stage).

### OpenAPI

- Route table is exposed as `GET /v1/openapi.json` for ops convenience, generated from the handler signatures via `utoipa`. Not on any AC; treated as a convenience feature.

### Multi-tenant scope

- v1 daemon is single-tenant: one bearer token, one workspace dir, one sink config. Multi-tenant is deferred — see ADR-10 for Q7 resolution.

## Alternatives considered

- **actix-web**: rejected. axum has the simpler tower-middleware ecosystem and is more widely used inside the Rust async standard stack.
- **gRPC instead of REST**: rejected by PRD/CHARTER (REST is locked-in).
- **Persistent job queue (sqlite / sled)**: rejected for v1. Adds operational complexity; in-memory queue with manifest-driven resume covers AC-M7-25 (responsiveness over 7 days) given the daemon will not naturally crash inside that window. Revisit if crash recovery for queued-but-not-yet-running jobs becomes a requirement.
- **Auth on reads too**: rejected per PRD wording ("auth on writes"). Reads expose only public metadata.
- **TLS built into the daemon**: rejected. Adds cert-management responsibility; not the daemon's job. AC-M7-8 explicitly excludes it.

## Consequences

Positive:
- Standard axum patterns; low onboarding cost.
- Errors uniform across endpoints via single middleware.
- Reads are scrape-friendly without token plumbing in monitoring tools.

Negative:
- Queued jobs lost on daemon crash; documented in runbook.
- No built-in TLS forces ops to add a reverse proxy. Acceptable given the deployment context (services Istio gateway already terminates TLS).

Follow-ups:
- Add `utoipa`-generated OpenAPI doc behind `--enable-openapi` flag.
- Consider sqlite-backed job persistence in v2 (only if a real outage demonstrates need).

Revisit if: AC-M7-1's 50 ms p99 is missed (would imply the queue path needs a redesign) or if multi-tenant scope is approved (ADR-10).

## References

- requirements.md AC-M7-1..11, AC-M7-17..19
- engineering plan v1.1 §api/
- PRD v1.1 §6.7 FR-API-1..11
- Cognee tags: `task:cpp-indexer role:architect`
