# ADR-2: GraphSink trait shape — async, batched, idempotent, with index + lock hooks

Status: accepted
Date: 2026-05-17
Resolves: M1-S6, M3-S2, M4-S2 cross-cutting sink concerns

## Context

Both Neo4j and IndraDB sinks are mandatory in the default binary (CHARTER locked-in). They must be runtime-selectable via `[sink].backend`. They must:

- Support batched writes (≥50 k rows/s Neo4j, ≥100 k rows/s IndraDB — AC-M3-4/5).
- Be idempotent under retry (AC-M3-6).
- Pre-create indexes before bulk write (Neo4j needs `:Node(usr)` index).
- Allow Phase 5 to acquire an advisory lock to prevent races (AC-M4-6).
- Read credentials from env vars only (AC-M1-23/24).
- Be mockable in unit tests without spinning up a real DB.

## Decision

Define `GraphSink` as an async trait in `src/sink/mod.rs`:

```rust
#[async_trait::async_trait]
pub trait GraphSink: Send + Sync {
    /// Stable name for logs, metrics labels, REPO.sink attr.
    fn backend_name(&self) -> &'static str;

    /// Validate connectivity + credentials. Called once at startup.
    /// MUST return Err before any indexing begins if credentials are missing.
    async fn preflight(&self) -> Result<()>;

    /// Create schema indexes / constraints. Idempotent.
    async fn ensure_indexes(&self) -> Result<()>;

    /// Write a batch of nodes. MUST be idempotent on (usr, repo_name).
    async fn write_nodes(&self, batch: &[NodeRecord]) -> Result<WriteStats>;

    /// Write a batch of edges. MUST be idempotent on (src_usr, dst_usr, kind).
    async fn write_edges(&self, batch: &[EdgeRecord]) -> Result<WriteStats>;

    /// Reset a single repo's data + cache, or all repos. Used by POST /v1/reset.
    async fn reset(&self, target: ResetTarget) -> Result<()>;

    /// Phase 5 cross-process lock. Implementations:
    ///   Neo4j: APOC `apoc.lock.nodes` on a sentinel node, or a leader-election node
    ///          with timestamped TTL.
    ///   IndraDB: a vertex with property `cxg_phase5_lock=<host>:<pid>:<ts>` plus
    ///            optimistic check on acquire.
    /// Held until the returned guard is dropped.
    async fn acquire_phase5_lock(&self, ttl: Duration) -> Result<Box<dyn Phase5LockGuard>>;

    /// Read SchemaVersion node value if present, for cross-repo Phase 5 check.
    async fn read_schema_version(&self) -> Result<Option<String>>;

    /// Connectivity probe for GET /v1/status. Should NOT consume a write slot.
    async fn health(&self) -> Result<HealthInfo>;
}
```

- `NodeRecord` / `EdgeRecord` are sink-agnostic structs built from the Arrow schema in `schema::arrow`.
- `WriteStats { nodes_written: u64, retries: u32, elapsed: Duration }` is fed to Prometheus.
- Trait is `async_trait::async_trait` for v1; if it becomes hot, rewrite as native async-in-trait once MSRV ≥ 1.75 is comfortable.
- `factory::create(&config) -> Result<Arc<dyn GraphSink>>` performs the runtime dispatch on `config.sink.backend ∈ {"neo4j", "indradb"}`.
- A `MockSink` impl in `#[cfg(test)]` records all calls in-memory; used by pipeline unit tests.

Concurrency:
- Multiple `write_nodes` calls may be in flight concurrently up to `[sink].sessions` (default 16). Implementations use connection pools (`neo4rs::Graph` is internally pooled; IndraDB uses tonic channel cloning).
- `ensure_indexes` is called once before the first `write_nodes`.

Error mapping:
- All sink errors wrap into `Error::Sink { backend: &'static str, source: Box<dyn StdError + Send + Sync> }`.
- Transient errors (network, deadlock) are retried with exponential backoff up to 3 attempts inside the sink; retry counter surfaced in `WriteStats.retries`.

## Alternatives considered

- **Sync trait + spawn_blocking**: rejected. Both `neo4rs` and `indradb-proto` are natively async; wrapping them in `spawn_blocking` would waste a tokio worker per call.
- **Feature-gated single-backend trait**: rejected by CHARTER (both backends mandatory in default binary).
- **Separate `NodeSink` + `EdgeSink` traits**: rejected. Adds complexity with no caller benefit; both sinks always write both.
- **No `acquire_phase5_lock` on the trait; use an external coordination service (etcd)**: rejected for v1. Adds an operational dependency that does not exist today. DB-internal locking suffices for single-DB topologies.

## Consequences

Positive:
- Pipeline code (`pipeline::run`) is sink-agnostic; switching backends is a config change.
- Adding a third backend (e.g., Postgres+AGE) is one new file in `src/sink/`.
- Testing without DBs is straightforward via `MockSink`.

Negative:
- Async trait via `async_trait::async_trait` adds a `Box<Future>` per call. Negligible at batch sizes ≥10 k.
- `Phase5LockGuard` lifetime management in async contexts requires careful Drop semantics; documented in `sink/lock.rs`.

Follow-ups:
- Benchmark `MockSink` overhead in integration tests.
- Consider native async-in-trait migration after Rust 1.75 stabilises broader trait-object support.

Revisit if: a third backend is added, or if Neo4j-only optimisations (e.g., `apoc.import.csv` bulk loader) require a backend-specific code path that doesn't fit the trait shape.

## References

- requirements.md AC-M1-20..24, AC-M3-4..6, AC-M4-3, AC-M4-6
- engineering plan v1.1 §Module layout sink/, §Phase 4
- Cognee tags: `task:cpp-indexer role:architect`
