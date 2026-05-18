---
run_id: cpp-indexer-m8-v2
task-slug: cpp-indexer-m8
role: operator
date: 2026-05-18
resolves: QD-1
target: LOCAL — Rust CLI at bolt://192.168.1.200:7687
---

# Operator Re-Index Log — QD-1 Resolution

## Objective

Clear QD-1: `schema_drift_live_neo4j` was failing (exit 101) because `template_params` and
`template_args` were absent from the live Neo4j (bolt://192.168.1.200:7687) property-key
registry. Root cause: no v5-schema corpus with TEMPLATE_DECL or SPECIALIZATION nodes had
been indexed against the dev cluster.

## Pre-flight checks

| Check | Result |
|-------|--------|
| Disk free (/Users/husam/workspace) | 40 GiB — OK |
| RAM free (vm_stat Pages free) | ~104 MiB free pages; ~1.8 GiB inactive — acceptable |
| TCP 192.168.1.200:7687 | OPEN |
| kubectl context | admin@hs-cluster (current) |
| neo4j-auth-external secret | Present in namespace infrastructure |
| leveldb corpus | NOT PRESENT (cloned during run) |
| cxg-daemon binary | Pre-M8 (2026-05-17) — rebuilt during run |

## Execution trace

### Step 1: cargo build --release

```
cd /Users/husam/workspace/cpp-indexer
cargo build --release
```

Result: Finished in 17.35s. Three binaries rebuilt at 19:40 UTC 2026-05-18:
- target/release/cxg-daemon (29 MiB)
- target/release/cxg-index (26 MiB)
- target/release/cxg-resolve-cross-repo (21 MiB)

### Step 2: Corpus selection

LevelDB cloned to /Users/husam/workspace/leveldb (--depth=1).
CMake configured with -DLEVELDB_BUILD_TESTS=OFF -DLEVELDB_BUILD_BENCHMARKS=OFF to avoid
submodule requirements. compile_commands.json (235 lines, 30 TUs) copied to repo root.

Note: leveldb has no explicit template specializations — SPECIALIZATION nodes were not
generated from it. template_params was populated (6 TEMPLATE_DECL nodes), but template_args
required a second corpus (see below).

### Step 3: Neo4j credential retrieval

Credentials extracted from: kubectl --context admin@hs-cluster get secret neo4j-auth-external
-n infrastructure. User: neo4j. Password: not logged.

Note: context name is admin@hs-cluster (not hs-cluster as listed in runbook.md — the colon
form is not valid in this environment).

### Step 4: Schema version check

```
MATCH (v:SchemaVersion {id: 'singleton'}) RETURN v.version AS version
```
Result: "cxg-schema-v4" — full wipe required.

### Step 5: Graph wipe

```
MATCH (n) DETACH DELETE n
MATCH (n) RETURN count(n) AS cnt
```
Result: cnt = 0 — graph empty.

### Step 6: Daemon start

libclang.dylib not in rpath — required DYLD_LIBRARY_PATH:

```
DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
  NEO4J_PASSWORD=<from-k8s> CXG_API_TOKEN=<random-hex32> \
  target/release/cxg-daemon --config /tmp/cxg-daemon-dev.toml
```

Daemon ready: "cxg-daemon listening on 127.0.0.1:7878" logged within 3 seconds.

### Step 7: LevelDB ingest

```
POST /v1/reset  (confirm_token = sha256("ALL")) → HTTP 204
POST /v1/ingest {"source": {"path": "/Users/husam/workspace/leveldb"}} → job_id: 019e3c67-91be-77d0-b69c-d34e79ff9ecd
```

Job completed in ~60 seconds (state=done). Schema in Neo4j: "cxg-schema-v5".
Property registry after leveldb: template_params PRESENT, template_args ABSENT.
TEMPLATE_DECL nodes: 6 (all with template_params populated).
SPECIALIZATION nodes: 0 (leveldb has no explicit template specializations).

### Step 8: boost_optional fixture ingest (required for template_args)

The runbook states "May be 0 if leveldb has no explicit specializations" for template_args.
The schema_drift test requires ALL 12 promoted fields including template_args to be in the
property-key registry.

Solution: indexed the boost_optional test fixture which contains a SPECIALIZATION node with
template_args JSON:

```
POST /v1/ingest {"source": {"path": "/Users/husam/workspace/cpp-indexer/tests/fixtures/boost_optional"}}
→ job_id: 019e3c6a-a2d2-78d1-9d92-a85cee53b197
```

Fixture has compile_commands.json at tests/fixtures/boost_optional/compile_commands.json.
Job completed immediately (state=done).

SPECIALIZATION node found: name="optional", template_args="[{\"kind\":\"type\",\"value\":\"type-parameter-0-0 *\"}]"
Property registry: template_args PRESENT.

Note: the template fixture (tests/fixtures/template/) also has SPECIALIZATION nodes but
failed to parse cleanly (libclang parse errors — missing STL includes). boost_optional
fixture was clean.

### Step 9: Schema drift test

```
cd /Users/husam/workspace/cpp-indexer
CPP_INDEXER_LIVE_NEO4J=1 NEO4J_URI=bolt://192.168.1.200:7687 NEO4J_PASSWORD=<from-k8s> \
  cargo test --test schema_drift -- --ignored --nocapture
```

Result:
```
running 1 test
test schema_drift_live_neo4j ... ok
test result: ok. 1 passed; 0 failed; 0 ignored; 0 measured; 3 filtered out; finished in 0.05s
```

Exit code: 0. QD-1 RESOLVED.

### Step 10: Cleanup

- Daemon stopped: pkill -f "cxg-daemon.*cxg-daemon-dev.toml"
- Staging dir removed: /tmp/cxg-stage-dev
- Config removed: /tmp/cxg-daemon-dev.toml
- Credential temp files removed

## Final Neo4j state

| Property | Present |
|----------|---------|
| return_type | yes |
| params | yes |
| signature | yes |
| code | yes |
| code_truncated | yes |
| template_params | yes (from leveldb TUs) |
| template_args | yes (from boost_optional fixture) |
| is_virtual | yes |
| is_pure_virtual | yes |
| is_static | yes |
| source_association_type | yes |
| target_association_type | yes |

Schema version: cxg-schema-v5.

## Deviations from runbook.md

1. Context name in runbook: `hs-cluster`. Actual context: `admin@hs-cluster`. The --context
   flag must use `admin@hs-cluster`. Recommendation: update runbook.md §Neo4j credentials.

2. libclang.dylib not found at runtime — DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib
   required. Runbook does not mention this. Recommendation: add to runbook.md §Start daemon
   as a macOS gotcha note.

3. LevelDB alone insufficient to populate template_args — no SPECIALIZATION nodes in leveldb.
   Required second ingest of boost_optional fixture. Runbook notes this is acceptable but
   the test will still fail without template_args. Recommendation: either (a) update runbook
   to recommend indexing boost_optional fixture in addition to leveldb, OR (b) add a
   synthetic C++ file with explicit specialization to the leveldb ingest.

## Test-report update

test-report.md QD-1 status updated from "open" to "resolved" with resolution addendum.

## References

- runbook: .claude/handoff/v2/runbook.md
- deploy-notes: .claude/handoff/v2/deploy-notes.md
- test-report: .claude/handoff/v2/test-report.md (QD-1 now resolved)
- Cognee tags: task:cpp-indexer-m8, role:operator
