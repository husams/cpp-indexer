# Operator Runbook: Sink Failover

**Audience:** Operators  
**Last updated:** 2026-05-17

---

## Overview

`cxg-daemon` uses exactly one graph sink backend per run, selected by
`[sink].backend` in `cxg-daemon.toml`. The daemon does not switch backends
at runtime. Failover means: stop the daemon, edit config, restart against the
new backend, and reset + re-index.

> **Note:** The pipeline enforces a single-backend rule per indexing run. If
> any `REPO` node in the graph records `sink=neo4j` and another records
> `sink=indradb`, the daemon will refuse that ingest with a mixed-backend
> error. A full reset (see [staging-recovery runbook](staging-recovery.md)) is
> required before switching backends.

---

## Checking Current Sink Health

```bash
curl -s http://127.0.0.1:7878/v1/status | jq .
```

If `status` reports sink errors, check the backend directly:

**Neo4j:**

```bash
# Bolt ping (requires cypher-shell or equivalent)
cypher-shell -a bolt://localhost:7687 -u neo4j -p "$NEO4J_PASSWORD" "RETURN 1;"
```

**IndraDB:**

```bash
# gRPC health check (requires grpc-health-probe)
grpc-health-probe -addr=localhost:27615
```

---

## Failing Over from Neo4j to IndraDB

1. Stop the daemon:

   ```bash
   sudo systemctl stop cxg-daemon
   ```

2. Reset staging to avoid mixed-backend state. Because the old graph data stays
   in Neo4j, you only need to clear staging:

   ```bash
   rm -rf /var/lib/cxg-daemon/stage
   mkdir -p /var/lib/cxg-daemon/stage
   ```

3. Edit `cxg-daemon.toml`:

   ```toml
   [sink]
   backend = "indradb"

   [sink.indradb]
   endpoint = "http://localhost:27615"
   token_env = "INDRADB_TOKEN"    # omit if IndraDB instance has no auth
   ```

4. Export the IndraDB token if required:

   ```bash
   export INDRADB_TOKEN="<indradb-auth-token>"
   ```

5. Restart the daemon:

   ```bash
   sudo systemctl start cxg-daemon
   ```

6. Re-index all repos against the new backend. For each repo, use
   `POST /v1/reset` (see [staging-recovery §3](staging-recovery.md#3-trigger-a-full-re-index-via-post-v1reset)),
   then submit a new ingest job.

---

## Failing Over from IndraDB to Neo4j

Follow the same steps as above but swap the backend direction:

```toml
[sink]
backend = "neo4j"

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"
```

Export `NEO4J_PASSWORD` before restarting.

---

## Sink Configuration Reference

### Neo4j

| Field | Required | Description |
|-------|----------|-------------|
| `uri` | Yes | Bolt URI, e.g. `bolt://localhost:7687` |
| `user` | Yes | Neo4j username |
| `password_env` | Yes | Name of env var holding the password |
| `sessions` | No | Connection pool size (default 16) |

### IndraDB

| Field | Required | Description |
|-------|----------|-------------|
| `endpoint` | Yes | gRPC endpoint, e.g. `http://localhost:27615` |
| `token_env` | No | Name of env var holding the auth token |

---

## Verifying the New Sink

After restart and re-index, verify:

```bash
# Daemon is healthy
curl -s http://127.0.0.1:7878/v1/status | jq .

# Nodes written to new backend
curl -s http://127.0.0.1:7878/metrics \
  | grep -E "cxg_nodes_total|cxg_edges_total"
```

Non-zero `cxg_nodes_total` confirms the new sink is receiving data.
