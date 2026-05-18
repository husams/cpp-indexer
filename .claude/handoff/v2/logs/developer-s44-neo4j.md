## developer log — S44 Neo4j native property writes + covering indexes

### Skills loaded
- `rust-conventions` (Cargo.toml present)

### Skills considered but not loaded
- `cpp-conventions`: project is Rust, not C++
- `implement-story`: not available in this session
- `google-agents-cli-workflow`: no ADK agents

### Commands run + outcomes

```
# Orientation
cat src/sink/neo4j.rs          → confirmed pre-S44 structure: 2 indexes, no M8 fields
cat tests/integration/neo4j_indexes.rs  → pre-existing skeleton from S41 era

# Implementation (neo4j.rs)
# - Added 4 index CQL constants
# - Extended ensure_indexes() to 6 indexes with EquivalentSchemaRuleAlreadyExists suppression
# - Extended CQL_MERGE_NODES with 10 M8 SET clauses
# - Extended CQL_MERGE_EDGES with 2 M8 SET clauses
# - Added opt_str_to_bolt, opt_bool_to_bolt, structured_list_to_json_bolt helpers
# - Extended node_to_bolt() and edge_to_bolt()
# - Added pub fn graph_handle()
# - Added 14 unit tests + 3 serialization tests for JSON-string strategy

# First round: tried BoltList<BoltMap> for params per ADR-14
# → Neo.ClientError.Statement.TypeError on live dev cluster
# → DEVIATION: switched to structured_list_to_json_bolt (JSON string)
# → Removed unused imports: BoltList, BoltMap, BoltString

cargo build --all-targets      → OK (12s)
cargo clippy --all-targets --all-features -- -D warnings  → OK (5s)
cargo fmt --all -- --check     → FAIL (formatting diffs in neo4j.rs)
cargo fmt --all                → reformatted
cargo fmt --all -- --check     → OK
cargo test --lib sink::neo4j   → 31 passed; 0 failed

# Live gate pass 1 (background run)
CPP_INDEXER_LIVE_NEO4J=1 cargo test --test neo4j_indexes -- --ignored --nocapture
→ tests 1-3 passed; test 4 (neo4j_promoted_fields_written_and_readable) HUNG 60+s

# Diagnosis: DetachedRowStream holds pool connection; max_connections=2 exhausted
# when reset() called after 2 streams alive simultaneously
# Fix: sessions: Some(8), explicit drop(stream)/drop(estream), tokio::time::timeout wrappers

# Kill hung processes
kill <pids>

# Live gate pass 2
CPP_INDEXER_LIVE_NEO4J=1 cargo test --test neo4j_indexes -- --ignored --nocapture
→ 4 passed; 0 failed; 8.73s ✓

cargo fmt --all -- --check     → FAIL (timeout call style reformatted)
cargo fmt --all                → OK
cargo clippy --all-targets --all-features -- -D warnings  → OK
cargo test --all-targets --features test-mock  → all passed; 0 failed

# Live gate pass 3 (confirmation)
CPP_INDEXER_LIVE_NEO4J=1 cargo test --test neo4j_indexes -- --ignored --nocapture
→ 4 passed; 0 failed; 8.73s ✓
```

### Deviations from plan.md

1. **ADR-14 `List<Map>` rejected**: implemented JSON string serialization instead.
2. **Test pool size**: `sessions: Some(8)` not Some(2); explicit stream drops added.
3. **EXPLAIN gate skipped**: dev cluster port 7474 not exposed; graceful skip with warning.
4. **Two fmt passes**: rustfmt reformats long multi-arg function calls; needed 2 fmt runs.

### Tool failures / retries

- Pass 1 live gate: test 4 hung (pool exhaustion) → fix pool + drops → pass 2 OK
- Pass 1 fmt: long timeout() call style → cargo fmt → pass 2 OK
- 3 total exit gate passes; all clear on pass 3
