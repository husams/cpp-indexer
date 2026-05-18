# Operator Runbook: Corrupted Staging Recovery

**Covers:** AC-M7-23, AC-M7-24  
**Audience:** Operators running `cxg-daemon` in production  
**Last updated:** 2026-05-17

---

## Overview

The cpp-indexer pipeline stages Phase 1 Parquet shards and a RocksDB USR map
under a configurable directory (the *staging directory*). Corruption of this
directory — caused by a crash mid-write, disk failure, or unexpected
termination — does not corrupt the graph database itself, but it can leave the
pipeline in a state where subsequent ingest attempts fail. This runbook covers
the full recovery loop: detect → clear staging → reset graph → re-index →
verify.

---

## 1. Identify a Corrupted Staging Directory

### 1.1 Symptoms

Look for any of the following in `journalctl -u cxg-daemon` or stderr output:

```
ERROR cpp_indexer::stage  failed to open shard: ...
ERROR cpp_indexer::resolve phase 2 USR map open failed: ...
ERROR cpp_indexer::pipeline  phase 1 panicked on <file.cpp>: ...
```

A job stuck in `state: running` for more than the expected parse time (tens of
minutes for large repos) with no progress lines to stderr is also a signal.

### 1.2 Locate the staging directory

The staging directory is controlled by `[index].stage_dir` in your
`cxg-daemon.toml`. If not set, it defaults to a temporary directory chosen by
the OS at start time (not stable across restarts; avoid leaving it unset in
production).

```toml
# cxg-daemon.toml example
[index]
stage_dir = "/var/lib/cxg-daemon/stage"
```

Confirm the live value:

```bash
grep -A5 '\[index\]' /etc/cxg-daemon/cxg-daemon.toml
```

The actual RocksDB spill file is at:

```
<stage_dir>/.cxg-cache/usr_map.rocks
```

Per-worker Parquet shards are under:

```
<stage_dir>/worker-NNN/
```

### 1.3 Quick health probe

```bash
STAGE=/var/lib/cxg-daemon/stage   # adjust to your stage_dir

# Check for incomplete/zero-byte shards
find "$STAGE" -name "*.parquet" -size 0

# Check if RocksDB directory is non-empty (may be locked open by a crashed process)
ls -la "$STAGE/.cxg-cache/usr_map.rocks/" 2>/dev/null || echo "no RocksDB dir"
```

If you see zero-byte Parquet files or a RocksDB `LOCK` file left by a dead
process, the staging directory is corrupted and needs to be cleared.

---

## 2. Clear the Staging Directory Safely

### 2.1 Stop the daemon

Before removing staging data, stop `cxg-daemon` to release any open file
handles and RocksDB locks:

```bash
sudo systemctl stop cxg-daemon
```

Confirm it is stopped:

```bash
systemctl is-active cxg-daemon   # expected: inactive
```

### 2.2 Remove staging data for a single repo

To clear only one repo's staging data (least-disruptive; other repos are
unaffected):

```bash
STAGE=/var/lib/cxg-daemon/stage
REPO=my-repo

rm -rf "$STAGE/$REPO"
```

### 2.3 Remove all staging data

For a global recovery (all repos):

```bash
STAGE=/var/lib/cxg-daemon/stage

rm -rf "$STAGE"
mkdir -p "$STAGE"
```

### 2.4 Restart the daemon

```bash
sudo systemctl start cxg-daemon
systemctl is-active cxg-daemon   # expected: active (running)
```

---

## 3. Trigger a Full Re-Index via POST /v1/reset

The `POST /v1/reset` endpoint resets the graph sink (drops all nodes and edges
for the target) and clears the corresponding staging cache. After this call,
the next ingest will perform a full re-index from source.

### 3.1 Derive the confirm_token

The server computes `expected = hex(sha256(target_name))` and compares it
against the token you send (constant-time). You must derive the token the same
way before calling the endpoint.

**For a single repo reset** (`target = "repo"`), `target_name` is the
repository name (the value you will pass as `repo_name`):

```bash
REPO=my-repo
TOKEN=$(printf '%s' "$REPO" | sha256sum | awk '{print $1}')
echo "$TOKEN"
# Example output: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
```

**For a full reset** (`target = "all"`), `target_name` is the literal string
`ALL` (uppercase):

```bash
TOKEN=$(printf '%s' 'ALL' | sha256sum | awk '{print $1}')
echo "$TOKEN"
# Known value: b5c7aed7cd2a308523e7d2847b7815909e864b2fd9c4ea88b00d35adb2ecdfd7
```

On macOS, replace `sha256sum` with `shasum -a 256`:

```bash
TOKEN=$(printf '%s' "$REPO" | shasum -a 256 | awk '{print $1}')
```

> **Important:** `printf '%s'` is used deliberately — `echo` appends a
> newline on most shells, which changes the hash. Always use `printf '%s'`.

### 3.2 Call POST /v1/reset

Set your bearer token in the environment before running:

```bash
export CXG_TOKEN="<your-api-bearer-token>"
export DAEMON_URL="http://127.0.0.1:7878"
```

**Reset a single repo:**

```bash
REPO=my-repo
TOKEN=$(printf '%s' "$REPO" | sha256sum | awk '{print $1}')

curl -s -X POST "$DAEMON_URL/v1/reset" \
  -H "Authorization: Bearer $CXG_TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"target\": \"repo\", \"repo_name\": \"$REPO\", \"confirm_token\": \"$TOKEN\"}" \
  -w "\nHTTP %{http_code}\n"
```

Expected response: `HTTP 204` (no response body).

**Reset all repos:**

```bash
TOKEN=$(printf '%s' 'ALL' | sha256sum | awk '{print $1}')

curl -s -X POST "$DAEMON_URL/v1/reset" \
  -H "Authorization: Bearer $CXG_TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"target\": \"all\", \"confirm_token\": \"$TOKEN\"}" \
  -w "\nHTTP %{http_code}\n"
```

Expected response: `HTTP 204`.

A `400 Bad Request` response means the token was wrong. Re-derive using the
exact repo name as stored in `repo_name`, taking care not to include trailing
whitespace or newlines.

### 3.3 Trigger re-index

Submit an ingest job for the repo. For a local path:

```bash
curl -s -X POST "$DAEMON_URL/v1/ingest" \
  -H "Authorization: Bearer $CXG_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"source": {"path": "/workspace/my-repo"}}' \
  | jq .
```

For a git URL:

```bash
curl -s -X POST "$DAEMON_URL/v1/ingest" \
  -H "Authorization: Bearer $CXG_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"source": {"git_url": "https://github.com/org/my-repo"}}' \
  | jq .
```

Note the returned `job_id`.

---

## 4. Verify the Graph is Complete After Re-Index

### 4.1 Poll job status

```bash
JOB_ID="<job_id from ingest response>"

curl -s "$DAEMON_URL/v1/jobs/$JOB_ID" | jq .
```

Poll until `state` is `done`. A `failed` state means the ingest failed; check
daemon logs for the root cause before retrying.

### 4.2 Check daemon status

```bash
curl -s "$DAEMON_URL/v1/status" | jq .
```

The response must be `HTTP 200`. Any `5xx` indicates the daemon is unhealthy.

### 4.3 Verify Prometheus metrics

```bash
curl -s "$DAEMON_URL/metrics" | grep -E "cxg_nodes_total|cxg_edges_total|cxg_libclang_errors_total"
```

After a successful re-index you should see non-zero values for `cxg_nodes_total`
and `cxg_edges_total`. The error rate check:

```
cxg_libclang_errors_total / cxg_nodes_total < 0.01
```

A ratio above 1 % warrants investigation before marking recovery complete.

### 4.4 Verify registered repos

```bash
curl -s "$DAEMON_URL/v1/repos" | jq .
```

Confirm your repo appears in the list with the expected metadata.

---

## 5. Escalation

If re-index fails after following this runbook:

1. Collect daemon logs: `journalctl -u cxg-daemon --since "1 hour ago" > /tmp/cxg-daemon.log`
2. Check `cxg_libclang_errors_total` — a high error count may indicate a
   `compile_commands.json` mismatch or a toolchain path problem.
3. Verify the sink backend is reachable: for Neo4j, `bolt://host:7687`; for
   IndraDB, the gRPC endpoint configured in `[sink.indradb]`.
4. Open an issue with the collected log and metrics snapshot.

---

## 6. Full Re-Index Against v5 Schema (M8 Upgrade Recipe)

**When to use:** upgrading from any pre-v5 graph (schema-version `cxg-schema-v4`
or earlier) to `cxg-schema-v5`. The v5 promotion of 10 node fields and 2 edge
fields is incompatible with existing graphs — there is no automatic migration
(ADR-11). Old data must be wiped and re-indexed.

### 6.1 Confirm the current schema version

```bash
# Neo4j: check the SchemaVersion node
NEO4J_URI="bolt://127.0.0.1:7687"
cypher-shell -a "$NEO4J_URI" -u neo4j -p "$NEO4J_PASSWORD" \
  "MATCH (v:SchemaVersion {id: 'singleton'}) RETURN v.version AS version"
# Expected for v5: cxg-schema-v5
```

If the result is not `cxg-schema-v5`, proceed with this recipe.

### 6.2 Stop the daemon and clear staging

```bash
sudo systemctl stop cxg-daemon

STAGE=/var/lib/cxg-daemon/stage
rm -rf "$STAGE"
mkdir -p "$STAGE"
```

### 6.3 Wipe the old graph (all repos)

```bash
export CXG_TOKEN="<your-api-bearer-token>"
export DAEMON_URL="http://127.0.0.1:7878"

# Derive the confirm token for a full reset
TOKEN=$(printf '%s' 'ALL' | sha256sum | awk '{print $1}')
# On macOS: TOKEN=$(printf '%s' 'ALL' | shasum -a 256 | awk '{print $1}')

curl -s -X POST "$DAEMON_URL/v1/reset" \
  -H "Authorization: Bearer $CXG_TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"target\": \"all\", \"confirm_token\": \"$TOKEN\"}" \
  -w "\nHTTP %{http_code}\n"
# Expected: HTTP 204
```

Alternatively, for Neo4j, wipe directly if the daemon is unreachable:

```bash
cypher-shell -a "$NEO4J_URI" -u neo4j -p "$NEO4J_PASSWORD" \
  "MATCH (n) DETACH DELETE n"
```

### 6.4 Start the daemon and trigger re-index

```bash
sudo systemctl start cxg-daemon
systemctl is-active cxg-daemon   # expected: active (running)

# Submit an ingest for each repo (repeat per repo)
REPO_PATH="/workspace/my-repo"
curl -s -X POST "$DAEMON_URL/v1/ingest" \
  -H "Authorization: Bearer $CXG_TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"source\": {\"path\": \"$REPO_PATH\"}}" \
  | jq .
# Note the returned job_id and poll until state == "done" (see §4.1)
```

### 6.5 Verify v5 schema and promoted fields

After the re-index completes, confirm that promoted properties are present:

```bash
# Check schema version node
cypher-shell -a "$NEO4J_URI" -u neo4j -p "$NEO4J_PASSWORD" \
  "MATCH (v:SchemaVersion {id: 'singleton'}) RETURN v.version"
# Expected: cxg-schema-v5

# Spot-check a promoted field (return_type on FUNCTION nodes)
cypher-shell -a "$NEO4J_URI" -u neo4j -p "$NEO4J_PASSWORD" \
  "MATCH (n:Node {kind: 'FUNCTION'}) WHERE n.return_type IS NOT NULL RETURN count(n) AS cnt"
# Expected: cnt > 0 for repos with functions

# Spot-check USES classifier field
cypher-shell -a "$NEO4J_URI" -u neo4j -p "$NEO4J_PASSWORD" \
  "MATCH ()-[r:EDGE {kind:'USES'}]->() WHERE r.source_association_type IS NOT NULL RETURN count(r) AS cnt"
# Expected: cnt > 0 for repos with USES edges

# Verify M8 covering indexes are present
cypher-shell -a "$NEO4J_URI" -u neo4j -p "$NEO4J_PASSWORD" \
  "SHOW INDEXES WHERE name IN ['node_return_type_idx','node_is_virtual_idx','node_is_static_idx','node_kind_return_type_idx'] RETURN name, state"
# Expected: all 4 indexes in state ONLINE
```

See `docs/schema/SCHEMA.md` for the full promoted-field reference and example
queries (Q1, Q5).
