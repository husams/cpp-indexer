# M7 Soak Gate — Manual Procedure

Story: S39-m7-soak-gate  
AC covered: AC-M7-25, AC-M7-26, AC-M7-27  
Status: **MANUAL RUN REQUIRED** — results must be recorded in `test-report.md`

---

## Purpose

Verify that `cxg-daemon` runs unattended for 7 consecutive days on hermes-agent,
completes at least one successful ingest cycle per day, and maintains a
libclang error rate below 1% throughout the soak period.

---

## Prerequisites

### 1. Build and install the daemon

```sh
cargo build --release
sudo install -m 0755 target/release/cxg-daemon /usr/local/bin/cxg-daemon
```

### 2. Write `cxg-daemon.toml` on hermes-agent

```toml
[api]
listen           = "0.0.0.0:7878"
auth_token_env   = "CXG_DAEMON_TOKEN"
job_queue_max    = 16

[workspace]
dir           = "/data/cxg-workspace"
allowed_hosts = ["github.com"]

[sink]
backend       = "neo4j"
neo4j_uri     = "bolt://localhost:7687"
neo4j_password_env = "NEO4J_PASSWORD"
```

### 3. Start the daemon

```sh
export CXG_DAEMON_TOKEN="<strong-random-token>"
export NEO4J_PASSWORD="<neo4j-password>"
cxg-daemon --config /etc/cxg-daemon.toml &
```

Or as a systemd unit — the unit file should `EnvironmentFile=/etc/cxg-daemon.env`.

### 4. Verify the daemon is healthy

```sh
curl -s http://localhost:7878/v1/status | jq .
# Expected: {"status":"ok","version":"...","listen":"...","queue_depth":0,"sink_health":"ok"}
```

---

## Git-URL round-trip (AC-M7-26)

Post a git-URL ingest against the live daemon and verify the job reaches
`state=done`:

```sh
export CXG_M7_GIT_URL=https://github.com/torvalds/uemacs
export CXG_M7_DAEMON_URL=http://hermes-agent:7878
export CXG_M7_BEARER_TOKEN="${CXG_DAEMON_TOKEN}"

cargo nextest run -p cpp_indexer --test m7_git_roundtrip -- --ignored \
    -- m7_git_roundtrip
```

Record the output in `test-report.md` under S39.

---

## 7-Day Soak Schedule

Run the automated status + error-rate check every 6 hours over 7 days.
The simplest method is a cron job on hermes-agent:

```cron
0 */6 * * * cd /opt/cpp-indexer && \
    CXG_M7_DAEMON_URL=http://localhost:7878 \
    cargo nextest run -p cpp_indexer --test m7_git_roundtrip \
        --run-ignored ignored-only -- m7_soak_status_check \
    >> /var/log/cxg-soak.log 2>&1
```

A passing run logs:

```
m7_soak_status_check: status=ok version=0.1.0
m7_soak_status_check: cxg_libclang_errors_total=12 cxg_nodes_total=98432
m7_soak_status_check: AC-M7-27 PASS — error rate 0.000122 < 0.01
```

A failing run exits non-zero and logs:

```
m7_soak_status_check: AC-M7-27: error rate 0.023 >= 0.01 ...
```

---

## Pass criteria

| AC      | Criterion                                                                                 |
|---------|-------------------------------------------------------------------------------------------|
| AC-M7-25 | `GET /v1/status` returns 200 in every scheduled poll over 7 days; at least one completed ingest cycle per day visible in `GET /v1/jobs?state=done`. |
| AC-M7-26 | `m7_git_roundtrip` test passes: `POST /v1/ingest` with `git_url` reaches `state=done`, and `GET /v1/repos` lists the repo with `last_job_id` matching. |
| AC-M7-27 | At end of 7-day period, `GET /metrics` shows `cxg_libclang_errors_total / cxg_nodes_total < 0.01`. |

---

## Recording results

For each day of the soak, append a row to `test-report.md` under the S39 section:

```
| Day | Date       | /v1/status | ingest cycle | error rate | Notes |
|-----|------------|------------|--------------|------------|-------|
|   1 | 2026-05-18 | PASS       | PASS         | 0.0001     |       |
|   2 | ...        |            |              |            |       |
```

Tag any non-PASS entries as `QA_DEFECT` per the CHARTER taxonomy, with the
daemon log excerpt attached.

---

## Escalation

If the daemon crashes or the error rate exceeds 1%, page the on-call developer
and capture:

```sh
# Daemon logs (journald or file):
journalctl -u cxg-daemon --since "7 days ago" > /tmp/cxg-daemon-soak.log

# Final metrics snapshot:
curl -s http://localhost:7878/metrics > /tmp/cxg-metrics-final.txt
```

Attach both files to the `test-report.md` QA_DEFECT entry.
