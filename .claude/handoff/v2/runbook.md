---
run_id: cpp-indexer-m8-v2
title: "M8 Re-Index Recipe: Template-Rich Corpus (QD-1 Resolution)"
author: devops
date: 2026-05-18
target: LOCAL — Rust CLI pointing at bolt://192.168.1.200:7687
resolves: QD-1 (template_params / template_args absent from live Neo4j property registry)
references:
  - docs/runbooks/staging-recovery.md §6 (canonical upgrade recipe)
  - docs/schema/SCHEMA.md (promoted field reference)
  - .claude/handoff/v2/test-report.md (QD-1 defect detail)
---

# Runbook: M8 Local Re-Index — Template-Rich Corpus Against Dev Neo4j

## Trigger

Run this runbook when `schema_drift_live_neo4j` (the `#[ignore]`-gated integration test) fails
with exit code 101 and the failure message indicates `template_params` and/or `template_args` are
absent from the live Neo4j property-key registry at `bolt://192.168.1.200:7687`.

This is the precise symptom of QD-1 (test-report.md §Defects). The fix is a one-time v5 re-index
of a template-rich C++ corpus against the dev graph.

## Prerequisites

### Resource check

```bash
# Run on the operator machine before starting
df -h /Users/husam/workspace                       # need >= 10 GiB free
vm_stat | grep "Pages free"                        # Darwin; Linux: free -m
```

Abort and switch to pve01/pve02 if disk < 10 GiB or RAM < 4 GiB free.

Current probe (2026-05-18): disk ~40 GiB free, RAM borderline (~1.8 GiB inactive).
Recommend closing other memory-heavy processes before starting.

### Binaries

Three release binaries are required. They exist in `target/release/` but must be rebuilt from the
M8 branch to include all schema v5 changes:

```bash
cd /Users/husam/workspace/cpp-indexer

cargo build --release
# Produces:
#   target/release/cxg-daemon            — HTTP + ingest daemon
#   target/release/cxg-index             — standalone indexer CLI
#   target/release/cxg-resolve-cross-repo — Phase 5 EXTERNAL_REF synthesis
```

### Corpus

A template-rich C++ corpus is required to populate `template_params` and `template_args` in Neo4j.
The canonical example is Google LevelDB. If not already cloned:

```bash
git clone --depth=1 https://github.com/google/leveldb.git /Users/husam/workspace/leveldb
```

LevelDB requires `compile_commands.json` for libclang. Generate it with CMake:

```bash
cd /Users/husam/workspace/leveldb
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cp build/compile_commands.json .
```

### Neo4j credentials

The dev Neo4j password is stored in the cluster secret `neo4j-auth-external` in namespace
`infrastructure`. Extract at runtime — do not echo or store in shell history:

```bash
# Extract user and password from NEO4J_AUTH (format: user/password)
_NEO4J_AUTH=$(kubectl --context hs-cluster get secret neo4j-auth-external \
  -n infrastructure -o jsonpath='{.data.NEO4J_AUTH}' | base64 -d)

export NEO4J_USER=$(echo "$_NEO4J_AUTH" | cut -d'/' -f1)
export NEO4J_PASSWORD=$(echo "$_NEO4J_AUTH" | cut -d'/' -f2-)
unset _NEO4J_AUTH

# Verify connectivity (read-only probe)
echo "MATCH (n) RETURN count(n) AS cnt LIMIT 1" | \
  cypher-shell -a bolt://192.168.1.200:7687 -u "$NEO4J_USER" -p "$NEO4J_PASSWORD" 2>&1 | head -3
```

If `cypher-shell` is unavailable, the re-index itself will fail fast with a connection error.
Install: `brew install neo4j` or run via the Neo4j Desktop client.

### API bearer token

The daemon API requires a bearer token. Set it before running any `curl` commands:

```bash
export CXG_API_TOKEN="<choose a random token for local dev>"
# Example: CXG_API_TOKEN=$(openssl rand -hex 32)
```

---

## Steps

### Step 1 — Confirm current schema version

```bash
echo "MATCH (v:SchemaVersion {id: 'singleton'}) RETURN v.version AS version" | \
  cypher-shell -a bolt://192.168.1.200:7687 -u "$NEO4J_USER" -p "$NEO4J_PASSWORD"
```

- If output is `cxg-schema-v5`: the daemon has already indexed with v5 schema. Jump to Step 6
  to check whether template_params/template_args are now present.
- If output is `cxg-schema-v4` or empty: proceed with full wipe in Step 3.

### Step 2 — Write daemon config

Create a minimal config pointing at the dev Neo4j. Based on
`tests/fixtures/config/cxg-daemon-golden.toml`:

```bash
cat > /tmp/cxg-daemon-dev.toml <<'EOF'
[repo]
path = "/Users/husam/workspace/leveldb"

[index]
workers = 4
skip_phase2 = false
stage_dir = "/tmp/cxg-stage-dev"

[sink]
backend = "neo4j"

[sink.neo4j]
uri = "bolt://192.168.1.200:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"

[api]
listen = "127.0.0.1:7878"
auth_token_env = "CXG_API_TOKEN"
job_queue_max = 64

[workspace]
dir = "/tmp/cxg-clones-dev"
allowed_hosts = ["github.com"]
default_clone_depth = 1
EOF

mkdir -p /tmp/cxg-stage-dev /tmp/cxg-clones-dev
```

### Step 3 — Wipe old graph (v4 or any pre-v5 data)

This follows `docs/runbooks/staging-recovery.md §6.3`.

If the daemon is not yet running, wipe directly via Cypher:

```bash
echo "MATCH (n) DETACH DELETE n" | \
  cypher-shell -a bolt://192.168.1.200:7687 -u "$NEO4J_USER" -p "$NEO4J_PASSWORD"
```

Confirm empty graph:

```bash
echo "MATCH (n) RETURN count(n) AS cnt" | \
  cypher-shell -a bolt://192.168.1.200:7687 -u "$NEO4J_USER" -p "$NEO4J_PASSWORD"
# Expected: cnt = 0
```

### Step 4 — Start daemon

```bash
/Users/husam/workspace/cpp-indexer/target/release/cxg-daemon \
  --config /tmp/cxg-daemon-dev.toml \
  > /tmp/cxg-daemon.log 2>&1 &

CXG_DAEMON_PID=$!
echo "cxg-daemon PID: $CXG_DAEMON_PID"

# Wait for daemon to be ready (polls /v1/status)
until curl -sf -H "Authorization: Bearer $CXG_API_TOKEN" \
    http://127.0.0.1:7878/v1/status > /dev/null 2>&1; do
  sleep 2
done
echo "daemon ready"
```

If the daemon fails to start, check `/tmp/cxg-daemon.log`. Common causes:
- `NEO4J_PASSWORD` not exported — export it before starting the daemon process.
- Port 7878 already in use — `lsof -i :7878`.

### Step 5 — Trigger full re-index of leveldb

Derive the confirm token and call the reset + ingest sequence. This follows
`docs/runbooks/staging-recovery.md §6.3–6.4`.

```bash
export DAEMON_URL="http://127.0.0.1:7878"

# Full graph wipe via daemon API (preferred — daemon owns the graph state)
# On macOS use shasum -a 256 instead of sha256sum
TOKEN=$(printf '%s' 'ALL' | shasum -a 256 | awk '{print $1}')

curl -s -X POST "$DAEMON_URL/v1/reset" \
  -H "Authorization: Bearer $CXG_API_TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"target\": \"all\", \"confirm_token\": \"$TOKEN\"}" \
  -w "\nHTTP %{http_code}\n"
# Expected: HTTP 204

# Submit leveldb ingest
JOB_RESPONSE=$(curl -s -X POST "$DAEMON_URL/v1/ingest" \
  -H "Authorization: Bearer $CXG_API_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"source": {"path": "/Users/husam/workspace/leveldb"}}')

echo "$JOB_RESPONSE" | python3 -c "import sys,json; print(json.load(sys.stdin).get('job_id',''))"
JOB_ID=$(echo "$JOB_RESPONSE" | python3 -c "import sys,json; print(json.load(sys.stdin).get('job_id',''))")
```

### Step 5a — Poll until indexing completes

```bash
# Poll every 10 seconds; leveldb (~30 k LOC) typically takes 2–8 minutes
while true; do
  STATE=$(curl -sf -H "Authorization: Bearer $CXG_API_TOKEN" \
    "$DAEMON_URL/v1/jobs/$JOB_ID" | python3 -c "import sys,json; print(json.load(sys.stdin).get('state',''))")
  echo "$(date): state=$STATE"
  [ "$STATE" = "done" ] && break
  [ "$STATE" = "failed" ] && echo "INGEST FAILED — check /tmp/cxg-daemon.log" && exit 1
  sleep 10
done
```

### Step 6 — Verify promoted fields in live Neo4j

Follow `docs/runbooks/staging-recovery.md §6.5`:

```bash
NEO4J_URI="bolt://192.168.1.200:7687"

# Schema version must be v5
echo "MATCH (v:SchemaVersion {id: 'singleton'}) RETURN v.version" | \
  cypher-shell -a "$NEO4J_URI" -u "$NEO4J_USER" -p "$NEO4J_PASSWORD"
# Expected: cxg-schema-v5

# TEMPLATE_DECL nodes must carry template_params
echo "MATCH (n:Node {kind:'TEMPLATE_DECL'}) WHERE n.template_params IS NOT NULL RETURN count(n) AS cnt" | \
  cypher-shell -a "$NEO4J_URI" -u "$NEO4J_USER" -p "$NEO4J_PASSWORD"
# Expected: cnt > 0 (leveldb has template functions)

# Spot-check: template_args on SPECIALIZATION nodes
echo "MATCH (n:Node {kind:'SPECIALIZATION'}) WHERE n.template_args IS NOT NULL RETURN count(n) AS cnt" | \
  cypher-shell -a "$NEO4J_URI" -u "$NEO4J_USER" -p "$NEO4J_PASSWORD"
# May be 0 if leveldb has no explicit specializations; non-zero is a stronger signal

# Check property key registry (this is what schema_drift_live_neo4j tests)
echo "CALL db.propertyKeys() YIELD propertyKey RETURN propertyKey ORDER BY propertyKey" | \
  cypher-shell -a "$NEO4J_URI" -u "$NEO4J_USER" -p "$NEO4J_PASSWORD" | \
  grep -E "template_params|template_args"
# Expected: both appear in output
```

### Step 7 — Re-run schema_drift test (QD-1 verification)

```bash
cd /Users/husam/workspace/cpp-indexer

NEO4J_PASSWORD="$NEO4J_PASSWORD" \
CPP_INDEXER_LIVE_NEO4J=1 \
NEO4J_URI=bolt://192.168.1.200:7687 \
  cargo test --test schema_drift -- --ignored --nocapture
```

Expected exit code: 0, all tests pass including `schema_drift_live_neo4j`.

Record the result in `deploy-notes.md`.

### Step 8 — Stop daemon and clean up

```bash
kill "$CXG_DAEMON_PID" 2>/dev/null || pkill -f "cxg-daemon.*cxg-daemon-dev.toml"

# Optionally remove local staging (graph data stays in Neo4j)
rm -rf /tmp/cxg-stage-dev /tmp/cxg-clones-dev /tmp/cxg-daemon-dev.toml
```

---

## Batch size warning

If indexing a large callable-heavy corpus (much larger than leveldb), the increased payload per
write row (up to 17 items/node in the worst case per implementation-notes.md §S45) may slow
throughput significantly. If writes time out, halve the batch size by setting:

```toml
[index]
workers = 2   # reduce parallelism
```

No `batch_size` config key is exposed in v5 (it is an internal constant `DEFAULT_BATCH_SIZE`);
adjusting workers is the operator-accessible lever.

---

## Rollback

If re-indexing produces a corrupt graph state:

1. Wipe via Cypher: `MATCH (n) DETACH DELETE n`
2. Stop daemon: `kill $CXG_DAEMON_PID`
3. Clear staging: `rm -rf /tmp/cxg-stage-dev`
4. Re-index from a known-good commit of cpp-indexer.

There is no automatic rollback to v4; the schema bump (ADR-11) requires explicit wipe + re-index
for any version transition.

---

## On-call notes

- TCP 7687 reachable from macOS (verified 2026-05-18): `nc -vz 192.168.1.200 7687` → success.
- Neo4j password source: `kubectl --context hs-cluster get secret neo4j-auth-external -n infrastructure -o jsonpath='{.data.NEO4J_AUTH}' | base64 -d`
- EXPLAIN plan assertion (AC-S44-4) is gracefully skipped — HTTP port 7474 is closed on dev cluster. Not a blocker.
- `cypher-shell` must be on PATH; if not: `brew install neo4j` or use the Neo4j client container.
- The `cross_repo_access_mirror` test (mock-backed) is unaffected by this recipe; it passes without live infra.

---

## References

- `docs/runbooks/staging-recovery.md §6` — canonical v5 upgrade recipe (cited as AC-S46-2)
- `docs/schema/SCHEMA.md` — promoted field reference table + Q1/Q5 Cypher examples
- `.claude/handoff/v2/test-report.md` — QD-1 defect detail (root cause, open status)
- `.claude/handoff/v2/design.md §5` — batch size / Bolt frame size risk table
- `.claude/handoff/v2/implementation-notes.md §S45` — chunking arithmetic (17 items/node worst-case)
- `tests/fixtures/config/cxg-daemon-golden.toml` — config reference
