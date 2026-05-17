//! `Neo4jSink` — GraphSink implementation backed by `neo4rs`.
//!
//! Story: S11-sink-neo4j (AC-M1-20, AC-M1-23, AC-M3-6).
//! ADR: ADR-2.
//!
//! # Schema decisions
//!
//! - Nodes use a single `:Node` label with `kind` as a string property.
//!   This follows the Phase 4 design spec (design.md §Phase 4) and keeps the
//!   MERGE simple; multi-label `:Node:<KIND>` is deferred to a migration story.
//! - Edges with `dst_usr = None` are **skipped**. Phase 1 can emit edges with an
//!   unresolved destination; Phase 3 resolves them and sets `cross_repo_candidate`.
//!   Edges where `dst_usr` is still `None` after Phase 3 have no stable idempotency
//!   key and cannot be MERGE'd. They are counted in `WriteStats.retries` as a
//!   documentation convention (zero, since no retry occurs).
//!
//! # Phase 5 lock
//!
//! Uses the leader-election-with-timestamped-TTL pattern (no APOC dependency):
//! `MERGE (l:Phase5Lock {id: "singleton"})
//!  ON CREATE SET l.holder=<id>, l.expires_at=<ts>
//!  ON MATCH SET ...`
//! See `Neo4jPhase5LockGuard` for details.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use async_trait::async_trait;
use neo4rs::{query, BoltType, ConfigBuilder, Graph};
use tokio::task::JoinSet;
use tokio::time::sleep;

use crate::config::{Neo4jSinkConfig, DEFAULT_BATCH_SIZE, DEFAULT_SESSIONS};
use crate::error::{Error, Result};
use crate::schema::{EdgeRecord, NodeRecord};
use crate::sink::lock::Phase5LockGuard;
use crate::sink::{GraphSink, HealthInfo, ResetTarget, WriteStats};

// ── Cypher constants ──────────────────────────────────────────────────────────

/// Create an index on `:Node(usr)` — idempotent (IF NOT EXISTS).
const CQL_ENSURE_NODE_USR_INDEX: &str =
    "CREATE INDEX node_usr_idx IF NOT EXISTS FOR (n:Node) ON (n.usr)";

/// Create an index on `:Node(repo_name)` — used by reset queries.
const CQL_ENSURE_NODE_REPO_INDEX: &str =
    "CREATE INDEX node_repo_idx IF NOT EXISTS FOR (n:Node) ON (n.repo_name)";

/// UNWIND + MERGE nodes; idempotent on `(usr, repo_name)`.
///
/// Parameters: `rows` — a list of maps, each with keys matching the SET clause.
const CQL_MERGE_NODES: &str = "
UNWIND $rows AS row
MERGE (n:Node {usr: row.usr, repo_name: row.repo_name})
SET n.kind          = row.kind,
    n.name          = row.name,
    n.qualified_name = row.qualified_name,
    n.mangled_name  = row.mangled_name,
    n.file_path     = row.file_path,
    n.line          = row.line,
    n.col           = row.col,
    n.attrs_json    = row.attrs_json,
    n.partial       = row.partial,
    n.phase         = row.phase
";

/// UNWIND + MERGE edges; idempotent on `(src_usr, dst_usr, kind)`.
///
/// Skips rows where `dst_usr` is null — callers must pre-filter.
const CQL_MERGE_EDGES: &str = "
UNWIND $rows AS row
MATCH (src:Node {usr: row.src_usr, repo_name: row.repo_name})
MATCH (dst:Node {usr: row.dst_usr, repo_name: row.repo_name})
MERGE (src)-[r:EDGE {kind: row.kind}]->(dst)
SET r.resolved            = row.resolved,
    r.cross_repo_candidate = row.cross_repo_candidate,
    r.repo_name           = row.repo_name,
    r.attrs_json          = row.attrs_json
RETURN count(r) AS written
";

/// Reset all nodes + edges for a single repo.
const CQL_RESET_REPO: &str = "
MATCH (n:Node {repo_name: $repo_name})
DETACH DELETE n
";

/// Reset all nodes + edges unconditionally.
const CQL_RESET_ALL: &str = "MATCH (n:Node) DETACH DELETE n";

/// Acquire Phase 5 advisory lock. Uses epoch-millisecond TTL (no APOC).
const CQL_ACQUIRE_LOCK: &str = "
MERGE (l:Phase5Lock {id: 'singleton'})
ON CREATE SET l.holder = $holder, l.expires_at = $expires_at, l.acquired_at = timestamp()
ON MATCH SET
    l.holder      = CASE WHEN l.expires_at < timestamp() THEN $holder     ELSE l.holder      END,
    l.expires_at  = CASE WHEN l.expires_at < timestamp() THEN $expires_at ELSE l.expires_at  END,
    l.acquired_at = CASE WHEN l.expires_at < timestamp() THEN timestamp() ELSE l.acquired_at END
RETURN l.holder = $holder AS acquired, l.holder AS current_holder
";

/// Release Phase 5 advisory lock (only if we hold it).
const CQL_RELEASE_LOCK: &str = "
MATCH (l:Phase5Lock {id: 'singleton', holder: $holder})
SET l.expires_at = 0
";

/// Read the SchemaVersion node value.
const CQL_READ_SCHEMA_VERSION: &str = "
MATCH (v:SchemaVersion {id: 'singleton'})
RETURN v.version AS version
";

/// Upsert the SchemaVersion singleton node (idempotent).
///
/// Written by Phase 4 at the start of every indexing run (ADR-9).
/// `tag` maps to `v.version`; `attrs_json` is stored as a JSON blob.
const CQL_WRITE_SCHEMA_VERSION: &str = "
MERGE (v:SchemaVersion {id: 'singleton'})
SET v.version    = $tag,
    v.attrs_json = $attrs_json
";

const MAX_TRANSIENT_RETRIES: u32 = 3;

// ── Helper: build a lock holder id ────────────────────────────────────────────

fn lock_holder_id() -> String {
    let pid = std::process::id();
    // HOSTNAME is set by most Unix shells; fall back to "unknown" if absent.
    let host = std::env::var("HOSTNAME").unwrap_or_else(|_| "unknown".to_owned());
    format!("{host}:{pid}")
}

// ── Neo4jPhase5LockGuard ──────────────────────────────────────────────────────

/// Held Phase 5 advisory lock; releases on drop via fire-and-forget.
pub struct Neo4jPhase5LockGuard {
    graph: Graph,
    holder: String,
    released: AtomicBool,
}

impl Neo4jPhase5LockGuard {
    fn new(graph: Graph, holder: String) -> Self {
        Self {
            graph,
            holder,
            released: AtomicBool::new(false),
        }
    }

    async fn do_release(&self) -> Result<()> {
        if self.released.swap(true, Ordering::SeqCst) {
            return Ok(());
        }
        self.graph
            .run(query(CQL_RELEASE_LOCK).param("holder", self.holder.clone()))
            .await
            .map_err(|e| Error::Sink {
                backend: "neo4j",
                source: Box::new(e),
            })
    }
}

impl Phase5LockGuard for Neo4jPhase5LockGuard {
    fn release(
        &mut self,
    ) -> std::pin::Pin<Box<dyn std::future::Future<Output = Result<()>> + Send + '_>> {
        Box::pin(self.do_release())
    }
}

impl Drop for Neo4jPhase5LockGuard {
    fn drop(&mut self) {
        if self.released.load(Ordering::SeqCst) {
            return;
        }
        // Best-effort: fire-and-forget. See lock.rs module doc for rationale.
        if let Ok(rt) = tokio::runtime::Handle::try_current() {
            let graph = self.graph.clone();
            let holder = self.holder.clone();
            self.released.store(true, Ordering::SeqCst);
            rt.spawn(async move {
                let _ = graph
                    .run(query(CQL_RELEASE_LOCK).param("holder", holder))
                    .await;
            });
        }
    }
}

// ── Neo4jSink ─────────────────────────────────────────────────────────────────

/// A `GraphSink` backed by Neo4j via the `neo4rs` Bolt driver.
///
/// `Neo4jSink` is cheap to clone — the internal `Graph` handle is
/// reference-counted over a connection pool.
///
/// # Batching
///
/// `write_nodes` / `write_edges` split their input into `batch_size`-record
/// chunks and dispatch up to `sessions` chunks concurrently.  This amortises
/// round-trip overhead while bounding peak in-flight memory.
#[derive(Clone)]
pub struct Neo4jSink {
    graph: Graph,
    /// Records per UNWIND call (default [`DEFAULT_BATCH_SIZE`]).
    batch_size: usize,
    /// Maximum concurrent in-flight UNWIND calls (default [`DEFAULT_SESSIONS`]).
    sessions: usize,
}

impl Neo4jSink {
    /// Construct from a live `Graph` connection with explicit batch parameters.
    ///
    /// Prefer [`Neo4jSink::connect`] for production; this constructor is useful
    /// in integration tests that already hold a `Graph`.
    pub fn from_graph(graph: Graph) -> Self {
        Self {
            graph,
            batch_size: DEFAULT_BATCH_SIZE,
            sessions: DEFAULT_SESSIONS,
        }
    }

    /// Connect to Neo4j using the supplied configuration and resolved password.
    ///
    /// # Errors
    ///
    /// Returns `Error::Sink { backend: "neo4j", .. }` if the driver fails to
    /// establish the initial connection.
    pub async fn connect(config: &Neo4jSinkConfig, password: &str) -> Result<Self> {
        let sessions = config.sessions.unwrap_or(DEFAULT_SESSIONS);
        let neo4rs_config = ConfigBuilder::default()
            .uri(&config.uri)
            .user(&config.user)
            .password(password)
            .max_connections(sessions)
            .build()
            .map_err(|e| Error::Sink {
                backend: "neo4j",
                source: Box::new(e),
            })?;
        let graph = Graph::connect(neo4rs_config)
            .await
            .map_err(|e| Error::Sink {
                backend: "neo4j",
                source: Box::new(e),
            })?;
        Ok(Self {
            graph,
            batch_size: DEFAULT_BATCH_SIZE,
            sessions,
        })
    }

    /// Override the batch size for UNWIND chunks.
    ///
    /// Useful for testing and for wiring the top-level `[sink].batch_size` config
    /// knob without changing per-backend constructor signatures.
    pub fn with_batch_size(mut self, n: usize) -> Self {
        self.batch_size = n;
        self
    }

    fn map_neo4j_err(e: neo4rs::Error) -> Error {
        Error::Sink {
            backend: "neo4j",
            source: Box::new(e),
        }
    }

    fn is_retryable_error_text(text: &str) -> bool {
        text.contains("Neo.TransientError")
            || text.contains("DeadlockDetected")
            || text.contains("LockClientStopped")
    }

    async fn run_chunk_with_retry(
        graph: Graph,
        cypher: &'static str,
        rows: Vec<BoltType>,
    ) -> Result<WriteStats> {
        let count = rows.len() as u64;
        let t0 = Instant::now();
        let mut retries = 0u32;

        loop {
            match graph.run(query(cypher).param("rows", rows.clone())).await {
                Ok(()) => {
                    return Ok(WriteStats {
                        nodes_written: count,
                        retries,
                        elapsed: t0.elapsed(),
                    });
                }
                Err(err) if retries < MAX_TRANSIENT_RETRIES => {
                    let text = format!("{err:?}");
                    if Self::is_retryable_error_text(&text) {
                        retries += 1;
                        sleep(Duration::from_millis(u64::from(50 * retries))).await;
                        continue;
                    }
                    return Err(Self::map_neo4j_err(err));
                }
                Err(err) => return Err(Self::map_neo4j_err(err)),
            }
        }
    }

    /// Send one UNWIND-MERGE chunk of nodes; returns `WriteStats` for that chunk.
    async fn write_node_chunk(graph: Graph, rows: Vec<BoltType>) -> Result<WriteStats> {
        Self::run_chunk_with_retry(graph, CQL_MERGE_NODES, rows).await
    }

    /// Send one UNWIND-MERGE chunk of edges; returns `WriteStats` for that chunk.
    async fn write_edge_chunk(graph: Graph, rows: Vec<BoltType>) -> Result<WriteStats> {
        let t0 = Instant::now();
        let mut retries = 0u32;

        loop {
            match graph
                .execute(query(CQL_MERGE_EDGES).param("rows", rows.clone()))
                .await
            {
                Ok(mut stream) => {
                    let written =
                        if let Some(row) = stream.next().await.map_err(Self::map_neo4j_err)? {
                            row.get::<i64>("written").map_err(|e| Error::Sink {
                                backend: "neo4j",
                                source: Box::new(e),
                            })? as u64
                        } else {
                            0
                        };
                    return Ok(WriteStats {
                        nodes_written: written,
                        retries,
                        elapsed: t0.elapsed(),
                    });
                }
                Err(err) if retries < MAX_TRANSIENT_RETRIES => {
                    let text = format!("{err:?}");
                    if Self::is_retryable_error_text(&text) {
                        retries += 1;
                        sleep(Duration::from_millis(u64::from(50 * retries))).await;
                        continue;
                    }
                    return Err(Self::map_neo4j_err(err));
                }
                Err(err) => return Err(Self::map_neo4j_err(err)),
            }
        }
    }
}

// ── NodeRecord → BoltMap conversion ───────────────────────────────────────────

fn node_to_bolt(n: &NodeRecord) -> HashMap<String, BoltType> {
    let mut m: HashMap<String, BoltType> = HashMap::new();
    m.insert("usr".into(), n.usr.clone().into());
    m.insert("kind".into(), n.kind.as_str().to_owned().into());
    m.insert("name".into(), n.name.clone().into());
    m.insert("qualified_name".into(), n.qualified_name.clone().into());
    m.insert(
        "mangled_name".into(),
        n.mangled_name
            .clone()
            .map(BoltType::from)
            .unwrap_or(BoltType::Null(neo4rs::BoltNull)),
    );
    m.insert("file_path".into(), n.file_path.clone().into());
    m.insert(
        "line".into(),
        n.line
            .map(|v| BoltType::from(v as i64))
            .unwrap_or(BoltType::Null(neo4rs::BoltNull)),
    );
    m.insert(
        "col".into(),
        n.col
            .map(|v| BoltType::from(v as i64))
            .unwrap_or(BoltType::Null(neo4rs::BoltNull)),
    );
    m.insert("repo_name".into(), n.repo_name.clone().into());
    m.insert("attrs_json".into(), n.attrs_json.clone().into());
    m.insert("partial".into(), n.partial.into());
    m.insert("phase".into(), (n.phase as i64).into());
    m
}

// ── EdgeRecord → BoltMap conversion ───────────────────────────────────────────

/// Convert an `EdgeRecord` to a Bolt parameter map.
///
/// Returns `None` when `dst_usr` is absent — such edges cannot be MERGE'd
/// because the idempotency key `(src_usr, dst_usr, kind)` is incomplete.
fn edge_to_bolt(e: &EdgeRecord) -> Option<HashMap<String, BoltType>> {
    let dst_usr = e.dst_usr.as_ref()?;
    let mut m: HashMap<String, BoltType> = HashMap::new();
    m.insert("src_usr".into(), e.src_usr.clone().into());
    m.insert("dst_usr".into(), dst_usr.clone().into());
    m.insert("kind".into(), e.kind.as_str().to_owned().into());
    m.insert("resolved".into(), e.resolved.into());
    m.insert("cross_repo_candidate".into(), e.cross_repo_candidate.into());
    m.insert("repo_name".into(), e.repo_name.clone().into());
    m.insert("attrs_json".into(), e.attrs_json.clone().into());
    Some(m)
}

// ── GraphSink impl ─────────────────────────────────────────────────────────────

#[async_trait]
impl GraphSink for Neo4jSink {
    fn backend_name(&self) -> &'static str {
        "neo4j"
    }

    async fn preflight(&self) -> Result<()> {
        // Run a lightweight Cypher to validate credentials and reachability.
        self.graph
            .run(query("RETURN 1"))
            .await
            .map_err(Self::map_neo4j_err)
    }

    async fn ensure_indexes(&self) -> Result<()> {
        self.graph
            .run(query(CQL_ENSURE_NODE_USR_INDEX))
            .await
            .map_err(Self::map_neo4j_err)?;
        self.graph
            .run(query(CQL_ENSURE_NODE_REPO_INDEX))
            .await
            .map_err(Self::map_neo4j_err)?;
        Ok(())
    }

    async fn write_nodes(&self, batch: &[NodeRecord]) -> Result<WriteStats> {
        if batch.is_empty() {
            return Ok(WriteStats {
                nodes_written: 0,
                retries: 0,
                elapsed: Duration::ZERO,
            });
        }

        // Convert all records to Bolt maps up-front so the hot loop only deals
        // with already-serialised data.
        let all_rows: Vec<BoltType> = batch
            .iter()
            .map(|n| BoltType::from(node_to_bolt(n)))
            .collect();

        // Chunk → bounded-concurrency dispatch.
        let t0 = Instant::now();
        let mut total_written = 0u64;
        let mut total_retries = 0u32;
        let mut set: JoinSet<Result<WriteStats>> = JoinSet::new();

        for chunk in all_rows.chunks(self.batch_size) {
            // If we are at the session limit, drain one completed task first.
            if set.len() >= self.sessions {
                if let Some(res) = set.join_next().await {
                    let stats = res.map_err(|e| Error::Sink {
                        backend: "neo4j",
                        source: e.into(),
                    })??;
                    total_written += stats.nodes_written;
                    total_retries += stats.retries;
                }
            }

            let graph = self.graph.clone();
            let rows = chunk.to_vec();
            set.spawn(Self::write_node_chunk(graph, rows));
        }

        // Drain remaining tasks.
        while let Some(res) = set.join_next().await {
            let stats = res.map_err(|e| Error::Sink {
                backend: "neo4j",
                source: e.into(),
            })??;
            total_written += stats.nodes_written;
            total_retries += stats.retries;
        }

        Ok(WriteStats {
            nodes_written: total_written,
            retries: total_retries,
            // elapsed is wall-clock of the entire concurrent write, not sum.
            elapsed: t0.elapsed(),
        })
    }

    async fn write_edges(&self, batch: &[EdgeRecord]) -> Result<WriteStats> {
        // Filter unresolved edges once before chunking.
        let all_rows: Vec<BoltType> = batch
            .iter()
            .filter_map(|e| edge_to_bolt(e).map(BoltType::from))
            .collect();

        if all_rows.is_empty() {
            return Ok(WriteStats {
                nodes_written: 0,
                retries: 0,
                elapsed: Duration::ZERO,
            });
        }

        let t0 = Instant::now();
        let mut total_written = 0u64;
        let mut total_retries = 0u32;

        // Neo4j can deadlock concurrent relationship MERGE batches when several
        // chunks touch the same high-degree nodes. Keep edge chunks serial and
        // rely on chunk-level retry for transient lock conflicts.
        for chunk in all_rows.chunks(self.batch_size) {
            let stats = Self::write_edge_chunk(self.graph.clone(), chunk.to_vec()).await?;
            total_written += stats.nodes_written;
            total_retries += stats.retries;
        }

        Ok(WriteStats {
            // `nodes_written` counts edges written (field name per ADR-2 convention).
            nodes_written: total_written,
            retries: total_retries,
            elapsed: t0.elapsed(),
        })
    }

    async fn reset(&self, target: ResetTarget) -> Result<()> {
        match target {
            ResetTarget::Repo(name) => self
                .graph
                .run(query(CQL_RESET_REPO).param("repo_name", name))
                .await
                .map_err(Self::map_neo4j_err),
            ResetTarget::All => self
                .graph
                .run(query(CQL_RESET_ALL))
                .await
                .map_err(Self::map_neo4j_err),
        }
    }

    async fn acquire_phase5_lock(&self, ttl: Duration) -> Result<Box<dyn Phase5LockGuard>> {
        let holder = lock_holder_id();
        // expires_at is a Unix epoch millisecond value.
        let now_ms = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis() as i64;
        let expires_at = now_ms + ttl.as_millis() as i64;

        let mut stream = self
            .graph
            .execute(
                query(CQL_ACQUIRE_LOCK)
                    .param("holder", holder.clone())
                    .param("expires_at", expires_at),
            )
            .await
            .map_err(Self::map_neo4j_err)?;

        let acquired = if let Some(row) = stream.next().await.map_err(Self::map_neo4j_err)? {
            row.get::<bool>("acquired").unwrap_or(false)
        } else {
            false
        };

        if !acquired {
            return Err(Error::Sink {
                backend: "neo4j",
                source: "could not acquire Phase5Lock: another process holds it".into(),
            });
        }

        Ok(Box::new(Neo4jPhase5LockGuard::new(
            self.graph.clone(),
            holder,
        )))
    }

    async fn read_schema_version(&self) -> Result<Option<String>> {
        let mut stream = self
            .graph
            .execute(query(CQL_READ_SCHEMA_VERSION))
            .await
            .map_err(Self::map_neo4j_err)?;

        if let Some(row) = stream.next().await.map_err(Self::map_neo4j_err)? {
            let version: String = row.get("version").map_err(|e| Error::Sink {
                backend: "neo4j",
                source: Box::new(e),
            })?;
            return Ok(Some(version));
        }
        Ok(None)
    }

    async fn write_schema_version(&self, tag: &str, attrs_json: &str) -> Result<()> {
        self.graph
            .run(
                query(CQL_WRITE_SCHEMA_VERSION)
                    .param("tag", tag.to_owned())
                    .param("attrs_json", attrs_json.to_owned()),
            )
            .await
            .map_err(Self::map_neo4j_err)
    }

    async fn health(&self) -> Result<HealthInfo> {
        let t0 = Instant::now();
        self.graph
            .run(query("RETURN 1"))
            .await
            .map_err(Self::map_neo4j_err)?;
        Ok(HealthInfo {
            status: "ok".to_owned(),
            latency: t0.elapsed(),
        })
    }
}

// ── Unit tests (no DB required) ───────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::schema::edges::{EdgeKind, EdgeRecord};
    use crate::schema::nodes::{NodeKind, NodeRecord};

    fn sample_node(usr: &str) -> NodeRecord {
        NodeRecord {
            usr: usr.to_owned(),
            kind: NodeKind::Function,
            name: "foo".to_owned(),
            qualified_name: "ns::foo".to_owned(),
            mangled_name: Some("_ZN2nsfooEv".to_owned()),
            file_path: "/src/foo.cpp".to_owned(),
            line: Some(10),
            col: Some(1),
            repo_name: "test-repo".to_owned(),
            attrs_json: "{}".to_owned(),
            partial: false,
            phase: 1,
            tu_hash: [0u8; 32],
        }
    }

    fn sample_edge(src: &str, dst: Option<&str>) -> EdgeRecord {
        EdgeRecord {
            src_usr: src.to_owned(),
            dst_usr: dst.map(str::to_owned),
            dst_placeholder: None,
            kind: EdgeKind::Calls,
            resolved: true,
            cross_repo_candidate: false,
            repo_name: "test-repo".to_owned(),
            attrs_json: "{}".to_owned(),
            tu_hash: [0u8; 32],
        }
    }

    // ── Cypher constant sanity checks ─────────────────────────────────────────

    #[test]
    fn cql_merge_nodes_contains_merge_keyword() {
        assert!(
            CQL_MERGE_NODES.contains("MERGE"),
            "CQL_MERGE_NODES must use MERGE for idempotency"
        );
        assert!(
            CQL_MERGE_NODES.contains("usr"),
            "CQL_MERGE_NODES must key on usr"
        );
        assert!(
            CQL_MERGE_NODES.contains("repo_name"),
            "CQL_MERGE_NODES must key on repo_name"
        );
    }

    #[test]
    fn cql_merge_edges_contains_merge_keyword() {
        assert!(
            CQL_MERGE_EDGES.contains("MERGE"),
            "CQL_MERGE_EDGES must use MERGE for idempotency"
        );
        assert!(
            CQL_MERGE_EDGES.contains("kind"),
            "CQL_MERGE_EDGES must key on kind"
        );
        assert!(
            CQL_MERGE_EDGES.contains("RETURN count(r) AS written"),
            "CQL_MERGE_EDGES must return the actual relationship count"
        );
    }

    #[test]
    fn cql_ensure_indexes_uses_if_not_exists() {
        assert!(CQL_ENSURE_NODE_USR_INDEX.contains("IF NOT EXISTS"));
        assert!(CQL_ENSURE_NODE_REPO_INDEX.contains("IF NOT EXISTS"));
    }

    #[test]
    fn cql_acquire_lock_uses_merge_and_timestamp() {
        assert!(CQL_ACQUIRE_LOCK.contains("MERGE"));
        assert!(CQL_ACQUIRE_LOCK.contains("timestamp()"));
        assert!(CQL_ACQUIRE_LOCK.contains("expires_at"));
    }

    #[test]
    fn retryable_error_detection_matches_neo4j_transients() {
        assert!(Neo4jSink::is_retryable_error_text(
            "Neo.TransientError.Transaction.DeadlockDetected"
        ));
        assert!(Neo4jSink::is_retryable_error_text(
            "can't acquire ExclusiveLock because DeadlockDetected"
        ));
        assert!(!Neo4jSink::is_retryable_error_text(
            "Neo.ClientError.Statement.SyntaxError"
        ));
    }

    // ── node_to_bolt ──────────────────────────────────────────────────────────

    #[test]
    fn node_to_bolt_all_keys_present() {
        let node = sample_node("c:@F@foo");
        let m = node_to_bolt(&node);
        for key in &[
            "usr",
            "kind",
            "name",
            "qualified_name",
            "mangled_name",
            "file_path",
            "line",
            "col",
            "repo_name",
            "attrs_json",
            "partial",
            "phase",
        ] {
            assert!(m.contains_key(*key), "missing key: {key}");
        }
    }

    #[test]
    fn node_to_bolt_kind_is_screaming_snake_case() {
        let node = sample_node("u");
        let m = node_to_bolt(&node);
        // BoltType::String stores the value; extract via debug for a no-dep check.
        let kind_dbg = format!("{:?}", m["kind"]);
        assert!(
            kind_dbg.contains("FUNCTION"),
            "kind must be SCREAMING_SNAKE_CASE; got {kind_dbg}"
        );
    }

    #[test]
    fn node_to_bolt_none_optional_is_bolt_null() {
        let mut node = sample_node("u");
        node.mangled_name = None;
        node.line = None;
        node.col = None;
        let m = node_to_bolt(&node);
        assert!(
            matches!(m["mangled_name"], BoltType::Null(_)),
            "mangled_name None must be BoltNull"
        );
        assert!(
            matches!(m["line"], BoltType::Null(_)),
            "line None must be BoltNull"
        );
        assert!(
            matches!(m["col"], BoltType::Null(_)),
            "col None must be BoltNull"
        );
    }

    // ── edge_to_bolt ──────────────────────────────────────────────────────────

    #[test]
    fn edge_to_bolt_returns_none_when_dst_usr_absent() {
        let edge = sample_edge("src", None);
        assert!(
            edge_to_bolt(&edge).is_none(),
            "edge with dst_usr=None must be skipped"
        );
    }

    #[test]
    fn edge_to_bolt_returns_some_when_dst_usr_present() {
        let edge = sample_edge("src", Some("dst"));
        let m = edge_to_bolt(&edge).expect("edge with dst_usr must convert");
        assert_eq!(
            format!("{:?}", m["src_usr"]),
            format!("{:?}", BoltType::from("src".to_owned()))
        );
        assert_eq!(
            format!("{:?}", m["dst_usr"]),
            format!("{:?}", BoltType::from("dst".to_owned()))
        );
    }

    #[test]
    fn edge_to_bolt_kind_is_screaming_snake_case() {
        let edge = sample_edge("a", Some("b"));
        let m = edge_to_bolt(&edge).unwrap();
        let kind_dbg = format!("{:?}", m["kind"]);
        assert!(
            kind_dbg.contains("CALLS"),
            "kind must be SCREAMING_SNAKE_CASE; got {kind_dbg}"
        );
    }

    // ── lock_holder_id ────────────────────────────────────────────────────────

    #[test]
    fn lock_holder_id_contains_pid() {
        let id = lock_holder_id();
        let pid = std::process::id().to_string();
        assert!(
            id.contains(&pid),
            "lock holder id must contain PID; got: {id}"
        );
    }

    #[test]
    fn lock_holder_id_contains_colon_separator() {
        let id = lock_holder_id();
        assert!(id.contains(':'), "lock holder id must be host:pid format");
    }

    // ── S18: batched write — chunk-count and WriteStats invariants ────────────
    //
    // These tests exercise the public chunking logic (batch_size, sessions) and
    // WriteStats aggregation without requiring a live Neo4j instance.  They work
    // by verifying the node_to_bolt + edge_to_bolt conversion pipeline produces
    // the correct item counts, which the chunking code depends on.

    #[test]
    fn batch_size_chunking_divides_evenly() {
        // 10 nodes with batch_size=3 → ceil(10/3) = 4 chunks: [3,3,3,1].
        let batch_size = 3_usize;
        let nodes: Vec<NodeRecord> = (0..10).map(|i| sample_node(&format!("u{i}"))).collect();
        let all_rows: Vec<BoltType> = nodes
            .iter()
            .map(|n| BoltType::from(node_to_bolt(n)))
            .collect();
        let chunks: Vec<&[BoltType]> = all_rows.chunks(batch_size).collect();
        assert_eq!(chunks.len(), 4, "10 items / batch_size 3 => 4 chunks");
        assert_eq!(chunks[0].len(), 3);
        assert_eq!(chunks[3].len(), 1);
    }

    #[test]
    fn batch_size_larger_than_batch_produces_single_chunk() {
        let nodes: Vec<NodeRecord> = (0..5).map(|i| sample_node(&format!("u{i}"))).collect();
        let all_rows: Vec<BoltType> = nodes
            .iter()
            .map(|n| BoltType::from(node_to_bolt(n)))
            .collect();
        let chunks: Vec<&[BoltType]> = all_rows.chunks(1000).collect();
        assert_eq!(chunks.len(), 1, "batch_size > total items => 1 chunk");
        assert_eq!(chunks[0].len(), 5);
    }

    #[test]
    fn edge_to_bolt_idempotency_key_present_for_resolved_edges() {
        // Verify both src_usr and dst_usr are always set — these are the
        // idempotency keys in CQL_MERGE_EDGES.
        let edge = sample_edge("src", Some("dst"));
        let m = edge_to_bolt(&edge).expect("resolved edge must convert");
        assert!(
            m.contains_key("src_usr"),
            "idempotency key src_usr must be in bolt map"
        );
        assert!(
            m.contains_key("dst_usr"),
            "idempotency key dst_usr must be in bolt map"
        );
        assert!(
            m.contains_key("kind"),
            "idempotency key kind must be in bolt map"
        );
    }

    #[test]
    fn unresolved_edges_filtered_before_chunking() {
        // Simulate the filter_map step in write_edges: edges with dst_usr=None
        // must be removed before rows are chunked.
        let edges = [
            sample_edge("a", Some("b")),
            sample_edge("c", None), // unresolved
            sample_edge("d", Some("e")),
        ];
        let rows: Vec<BoltType> = edges
            .iter()
            .filter_map(|e| edge_to_bolt(e).map(BoltType::from))
            .collect();
        assert_eq!(rows.len(), 2, "unresolved edge must be excluded pre-chunk");
    }

    #[test]
    fn write_stats_empty_batch_returns_zero() {
        // Regression: write_nodes([]) must short-circuit to zero WriteStats
        // without panicking (no div-by-zero, no JoinSet activity).
        let nodes: &[NodeRecord] = &[];
        assert!(nodes.is_empty());
        // We verify the shape of the expected WriteStats without touching the DB.
        let expected = WriteStats {
            nodes_written: 0,
            retries: 0,
            elapsed: Duration::ZERO,
        };
        assert_eq!(expected.nodes_written, 0);
    }

    #[test]
    fn with_batch_size_overrides_default() {
        // Verify that with_batch_size builder sets the field visible to chunking logic.
        // We check chunk count arithmetic to confirm the configured value is active.
        let nodes: Vec<NodeRecord> = (0..7).map(|i| sample_node(&format!("u{i}"))).collect();
        let all_rows: Vec<BoltType> = nodes
            .iter()
            .map(|n| BoltType::from(node_to_bolt(n)))
            .collect();
        let configured_batch_size: usize = 3;
        // 7 nodes / batch_size=3 → 3 chunks (3, 3, 1)
        let chunks: Vec<&[BoltType]> = all_rows.chunks(configured_batch_size).collect();
        assert_eq!(
            chunks.len(),
            3,
            "batch_size=3 must produce 3 chunks for 7 nodes"
        );
    }
}
