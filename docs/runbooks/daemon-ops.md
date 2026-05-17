# Operator Runbook: cxg-daemon Operations

**Audience:** Operators  
**Last updated:** 2026-05-17

---

## Starting and Stopping

### Start

```bash
# Set required bearer token env var (name configured in [api].auth_token_env)
export CXG_API_TOKEN="<strong-random-secret>"

cxg-daemon --config /etc/cxg-daemon/cxg-daemon.toml
```

Or via systemd:

```bash
sudo systemctl start cxg-daemon
```

The daemon refuses to start if `[api].auth_token_env` is unset or the
referenced env var is empty. There is no insecure/no-auth mode.

### Default listen address

`127.0.0.1:7878` — override with `[api].listen` in config:

```toml
[api]
listen = "0.0.0.0:7878"
auth_token_env = "CXG_API_TOKEN"
```

### Stop

```bash
sudo systemctl stop cxg-daemon
# Or send SIGTERM to the process.
```

The daemon drains in-flight jobs before exiting.

---

## Minimal cxg-daemon.toml

```toml
[sink]
backend = "neo4j"            # or "indradb"

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"

[api]
listen = "127.0.0.1:7878"
auth_token_env = "CXG_API_TOKEN"
job_queue_max = 64

[index]
stage_dir = "/var/lib/cxg-daemon/stage"
workers = 8                  # defaults to logical CPU count if omitted
```

---

## REST Endpoints

| Method | Path | Auth required | Description |
|--------|------|---------------|-------------|
| POST | `/v1/ingest` | Yes | Submit an ingest job |
| GET | `/v1/jobs/{id}` | No | Poll job status |
| POST | `/v1/reset` | Yes | Reset graph + staging cache |
| GET | `/v1/repos` | No | List indexed repos |
| GET | `/v1/status` | No | Daemon health |
| GET | `/metrics` | No | Prometheus metrics |

Auth: `Authorization: Bearer <token>` header on write endpoints (POST).

---

## Checking Health

```bash
curl -s http://127.0.0.1:7878/v1/status | jq .
```

`HTTP 200` confirms the daemon is alive and the sink is reachable.

---

## Logs and Tracing

Log level is controlled by the `RUST_LOG` environment variable:

```bash
RUST_LOG=info cxg-daemon --config cxg-daemon.toml      # default
RUST_LOG=debug cxg-daemon --config cxg-daemon.toml     # verbose
RUST_LOG=cpp_indexer::pipeline=trace cxg-daemon ...    # module-scoped
```

Structured JSON output (for log aggregators):

```bash
RUST_LOG_FORMAT=json cxg-daemon --config cxg-daemon.toml
```

---

## Job Lifecycle

1. Submit: `POST /v1/ingest` → returns `{"job_id": "<uuid>", "state": "queued"}`
2. Poll: `GET /v1/jobs/{id}` → `state` progresses: `queued` → `running` → `done` | `failed`
3. On `failed`: check daemon logs for the pipeline phase that errored; then see
   the [staging-recovery runbook](staging-recovery.md) if corruption is suspected.

---

## Common Failure Modes

### Daemon refuses to start: missing bearer token

```
env var `CXG_API_TOKEN` (named in [api].auth_token_env) is not set; refusing to start
```

Fix: export the env var before starting.

### Sink preflight fails

```
sink preflight check failed: connection refused bolt://localhost:7687
```

Fix: start the Neo4j or IndraDB service; see [sink-failover runbook](sink-failover.md).

### Port already in use

```
binding to 127.0.0.1:7878: address already in use
```

Fix: find and stop the conflicting process (`lsof -i :7878`), or change
`[api].listen` in config.
