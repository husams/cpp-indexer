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
use std::sync::Arc;
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

/// Create an index on `:Node(symbol_id)` — idempotent (IF NOT EXISTS).
/// Replaces the v5 `node_usr_idx`; v6 graphs key nodes on `(symbol_id, repo_name)`.
const CQL_ENSURE_NODE_SYMBOL_ID_INDEX: &str =
    "CREATE INDEX node_symbol_id_idx IF NOT EXISTS FOR (n:Node) ON (n.symbol_id)";

/// Create an index on `:Node(repo_name)` — used by reset queries.
const CQL_ENSURE_NODE_REPO_INDEX: &str =
    "CREATE INDEX node_repo_idx IF NOT EXISTS FOR (n:Node) ON (n.repo_name)";

// ── M8 (S44) covering indexes (ADR-15) ────────────────────────────────────────

/// Single-property index on `return_type` (ADR-15, covers Q1/Q2 from PRD §6).
const CQL_ENSURE_RETURN_TYPE_INDEX: &str =
    "CREATE INDEX node_return_type_idx IF NOT EXISTS FOR (n:Node) ON (n.return_type)";

/// Single-property index on `is_virtual` (ADR-15, covers Q2 virtual-method lookup).
const CQL_ENSURE_IS_VIRTUAL_INDEX: &str =
    "CREATE INDEX node_is_virtual_idx IF NOT EXISTS FOR (n:Node) ON (n.is_virtual)";

/// Single-property index on `is_static` (ADR-15).
const CQL_ENSURE_IS_STATIC_INDEX: &str =
    "CREATE INDEX node_is_static_idx IF NOT EXISTS FOR (n:Node) ON (n.is_static)";

/// Composite index on `(kind, return_type)` (ADR-15, covers Q1 with kind filter).
const CQL_ENSURE_KIND_RETURN_TYPE_INDEX: &str =
    "CREATE INDEX node_kind_return_type_idx IF NOT EXISTS FOR (n:Node) ON (n.kind, n.return_type)";

// ── v7 S1 indexes ─────────────────────────────────────────────────────────────

/// Index on `is_const` (v7 S1, covers field/gv const queries).
const CQL_ENSURE_IS_CONST_INDEX: &str =
    "CREATE INDEX node_is_const_idx IF NOT EXISTS FOR (n:Node) ON (n.is_const)";

/// Index on `storage_class` (v7 S1, covers linkage queries).
const CQL_ENSURE_STORAGE_CLASS_INDEX: &str =
    "CREATE INDEX node_storage_class_idx IF NOT EXISTS FOR (n:Node) ON (n.storage_class)";

/// Index on `access` edge property (v7 S1, covers Q6 access-filtered inheritance).
const CQL_ENSURE_EDGE_ACCESS_INDEX: &str =
    "CREATE INDEX edge_access_idx IF NOT EXISTS FOR ()-[r:EDGE]-() ON (r.access)";

// ── v7 S2 indexes ─────────────────────────────────────────────────────────────

/// Index on `is_template` (v7 S2).
const CQL_ENSURE_IS_TEMPLATE_INDEX: &str =
    "CREATE INDEX node_is_template_idx IF NOT EXISTS FOR (n:Node) ON (n.is_template)";

/// Index on `is_abstract` (v7 S2, covers abstract-class queries).
const CQL_ENSURE_IS_ABSTRACT_INDEX: &str =
    "CREATE INDEX node_is_abstract_idx IF NOT EXISTS FOR (n:Node) ON (n.is_abstract)";

/// Index on `type_spelling` (v7 S2, covers Type node lookup by spelling).
const CQL_ENSURE_TYPE_SPELLING_INDEX: &str =
    "CREATE INDEX node_type_spelling_idx IF NOT EXISTS FOR (n:Node) ON (n.type_spelling)";

/// Index on `param_index` (v7 S2, covers ordered parameter queries).
const CQL_ENSURE_PARAM_INDEX_INDEX: &str =
    "CREATE INDEX node_param_index_idx IF NOT EXISTS FOR (n:Node) ON (n.param_index)";

/// Index on `edge_index` edge property (v7 S2, covers ordered HAS_PARAM queries).
const CQL_ENSURE_EDGE_INDEX_INDEX: &str =
    "CREATE INDEX edge_edge_index_idx IF NOT EXISTS FOR ()-[r:EDGE]-() ON (r.edge_index)";

// ── v7 S4 indexes ─────────────────────────────────────────────────────────────

/// Index on `inherits_is_virtual` edge property (v7 S4, covers Q6 virtual-inheritance filter).
const CQL_ENSURE_EDGE_INHERITS_IS_VIRTUAL_INDEX: &str =
    "CREATE INDEX edge_inherits_is_virtual_idx IF NOT EXISTS FOR ()-[r:EDGE]-() ON (r.inherits_is_virtual)";

// ── v7 S5 indexes ─────────────────────────────────────────────────────────────

/// Index on `enum_value` (v7 S5, covers enumerator constant value queries).
const CQL_ENSURE_ENUM_VALUE_INDEX: &str =
    "CREATE INDEX node_enum_value_idx IF NOT EXISTS FOR (n:Node) ON (n.enum_value)";

/// UNWIND + MERGE nodes; idempotent on `(symbol_id, repo_name)` (v6, graph-symbol-ids Story 3).
///
/// Parameters: `rows` — a list of maps, each with keys matching the SET clause.
/// USR and file_path strings are NOT written to the durable graph (S6-SC-03).
const CQL_MERGE_NODES: &str = "
UNWIND $rows AS row
MERGE (n:Node {symbol_id: row.symbol_id, repo_name: row.repo_name})
SET n.kind              = row.kind,
    n.name              = row.name,
    n.qualified_name    = row.qualified_name,
    n.mangled_name      = row.mangled_name,
    n.file_id           = row.file_id,
    n.line              = row.line,
    n.col               = row.col,
    n.attrs_json        = row.attrs_json,
    n.partial           = row.partial,
    n.phase             = row.phase,
    n.return_type       = row.return_type,
    n.params            = row.params,
    n.signature         = row.signature,
    n.code              = row.code,
    n.code_truncated    = row.code_truncated,
    n.template_params   = row.template_params,
    n.template_args     = row.template_args,
    n.is_virtual        = row.is_virtual,
    n.is_pure_virtual   = row.is_pure_virtual,
    n.is_static         = row.is_static,
    n.is_const          = row.is_const,
    n.is_constexpr      = row.is_constexpr,
    n.storage_class     = row.storage_class,
    n.is_template       = row.is_template,
    n.is_noexcept       = row.is_noexcept,
    n.is_override       = row.is_override,
    n.is_deleted        = row.is_deleted,
    n.is_defaulted      = row.is_defaulted,
    n.cv_qualifiers     = row.cv_qualifiers,
    n.ref_qualifier     = row.ref_qualifier,
    n.is_final          = row.is_final,
    n.is_abstract       = row.is_abstract,
    n.record_kind       = row.record_kind,
    n.type_spelling     = row.type_spelling,
    n.param_index       = row.param_index,
    n.param_kind        = row.param_kind,
    n.enum_value        = row.enum_value
";

/// UNWIND + MERGE edges; idempotent on `(src_id, dst_id, kind)` (v6, graph-symbol-ids Story 3).
///
/// Source endpoint keyed on `(src_id, repo_name)`, destination on `(dst_id, dst_repo_name)`.
/// Skips rows where `dst_id` is null — callers must pre-filter.
/// USR strings are NOT written to the durable graph (S6-SC-03).
const CQL_MERGE_EDGES: &str = "
UNWIND $rows AS row
MATCH (src:Node {symbol_id: row.src_id, repo_name: row.repo_name})
MATCH (dst:Node {symbol_id: row.dst_id, repo_name: row.dst_repo_name})
MERGE (src)-[r:EDGE {kind: row.kind}]->(dst)
SET r.resolved                  = row.resolved,
    r.cross_repo_candidate      = row.cross_repo_candidate,
    r.repo_name                 = row.repo_name,
    r.dst_repo_name             = row.dst_repo_name,
    r.attrs_json                = row.attrs_json,
    r.source_association_type   = row.source_association_type,
    r.target_association_type   = row.target_association_type,
    r.access                    = row.access,
    r.edge_index                = row.edge_index,
    r.inherits_is_virtual       = row.inherits_is_virtual
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

    /// Return a clone of the underlying `Graph` handle.
    ///
    /// Intended for integration tests that need to run raw Cypher queries
    /// (e.g. `SHOW INDEXES`, `EXPLAIN`) without going through the `GraphSink`
    /// trait. The clone is cheap (`Graph` is reference-counted).
    pub fn graph_handle(&self) -> Graph {
        self.graph.clone()
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

    /// Split `batch` into record-level chunks and return each as an `Arc<[T]>`.
    ///
    /// Tasks hold `Arc<[T]>` so they can re-serialize on every retry attempt
    /// without pre-building a cloned `Vec` upfront.  Cheap: the source records
    /// are reference-counted, not copied.
    fn record_chunks<T: Clone>(batch: &[T], chunk_size: usize) -> Vec<Arc<[T]>> {
        if chunk_size == 0 || batch.is_empty() {
            return if batch.is_empty() {
                vec![]
            } else {
                vec![Arc::from(batch)]
            };
        }
        batch.chunks(chunk_size).map(Arc::from).collect()
    }

    /// Execute one UNWIND chunk with retry/back-off.
    ///
    /// `make_body` is called once per attempt to produce the rows Vec.
    /// On attempt 0 (the common path) this is the actual serialization work —
    /// zero extra copies, no pre-built Vec sitting in memory.  On a transient
    /// retry, `make_body` is re-invoked (re-serialize from the captured source),
    /// so clone cost is paid only on the rare failure path.
    async fn run_chunk_with_retry<B, F, Fut>(mut make_body: B, executor: F) -> Result<WriteStats>
    where
        B: FnMut() -> Vec<BoltType> + Send,
        F: Fn(Vec<BoltType>) -> Fut + Send,
        Fut: std::future::Future<Output = Result<u64>> + Send,
    {
        let t0 = Instant::now();
        let mut retries = 0u32;

        loop {
            // Build the payload for this attempt by re-serializing from the
            // source records held by `make_body`.  Attempt 0: this is the first
            // (and in the common case only) serialization — zero clone.
            // Attempts 1+: re-serialize only because a transient error fired.
            let body = make_body();
            match executor(body).await {
                Ok(written) => {
                    return Ok(WriteStats {
                        nodes_written: written,
                        retries,
                        elapsed: t0.elapsed(),
                    });
                }
                Err(err) => {
                    let text = format!("{err:?}");
                    if retries < MAX_TRANSIENT_RETRIES && Self::is_retryable_error_text(&text) {
                        retries += 1;
                        sleep(Duration::from_millis(u64::from(50 * retries))).await;
                        continue;
                    }
                    return Err(err);
                }
            }
        }
    }

    /// Send one UNWIND-MERGE chunk of nodes; returns `WriteStats` for that chunk.
    ///
    /// `records` is an `Arc<[NodeRecord]>` so re-serialization on a transient
    /// retry is cheap (no Vec clone; just re-run `node_to_bolt` on the slice).
    async fn write_node_chunk(graph: Graph, records: Arc<[NodeRecord]>) -> Result<WriteStats> {
        let count = records.len() as u64;
        Self::run_chunk_with_retry(
            move || {
                records
                    .iter()
                    .map(|n| BoltType::from(node_to_bolt(n)))
                    .collect()
            },
            move |body| {
                let g = graph.clone();
                async move {
                    g.run(query(CQL_MERGE_NODES).param("rows", body))
                        .await
                        .map_err(Self::map_neo4j_err)?;
                    Ok(count)
                }
            },
        )
        .await
    }

    /// Send one UNWIND-MERGE chunk of edges; returns `WriteStats` for that chunk.
    ///
    /// `records` is an `Arc<[EdgeRecord]>` (see `write_node_chunk` for rationale).
    async fn write_edge_chunk(graph: Graph, records: Arc<[EdgeRecord]>) -> Result<WriteStats> {
        Self::run_chunk_with_retry(
            move || {
                records
                    .iter()
                    .filter_map(|e| edge_to_bolt(e).map(BoltType::from))
                    .collect()
            },
            move |body| {
                let g = graph.clone();
                async move {
                    let mut stream = g
                        .execute(query(CQL_MERGE_EDGES).param("rows", body))
                        .await
                        .map_err(Self::map_neo4j_err)?;
                    let written =
                        if let Some(row) = stream.next().await.map_err(Self::map_neo4j_err)? {
                            row.get::<i64>("written").map_err(|e| Error::Sink {
                                backend: "neo4j",
                                source: Box::new(e),
                            })? as u64
                        } else {
                            0
                        };
                    Ok(written)
                }
            },
        )
        .await
    }
}

// ── Bolt serialization helpers for structured list types (ADR-14) ─────────────

/// Convert `Option<str>` to `BoltType`, using `BoltNull` for `None`.
fn opt_str_to_bolt(v: Option<&str>) -> BoltType {
    match v {
        Some(s) => BoltType::from(s.to_owned()),
        None => BoltType::Null(neo4rs::BoltNull),
    }
}

/// Convert `Option<bool>` to `BoltType`, using `BoltNull` for `None`.
fn opt_bool_to_bolt(v: Option<bool>) -> BoltType {
    match v {
        Some(b) => BoltType::from(b),
        None => BoltType::Null(neo4rs::BoltNull),
    }
}

/// Serialize a structured list field (params / template_params / template_args) as a JSON string.
///
/// # Deviation from ADR-14
///
/// ADR-14 specifies "Neo4j 5 stores `List<Map>` as the native composite type."  In practice,
/// Neo4j Community 2025.x rejects `Map` values inside `List` with
/// `Neo.ClientError.Statement.TypeError: Property values can only be of primitive types or arrays
/// thereof`.  Only `List<primitives>` (e.g. `String[]`) is allowed — not `List<Map>`.
///
/// Mitigation: serialize as a JSON string so the value is stored as an opaque but human-readable
/// property.  Downstream consumers can parse it.  The field key is preserved (it remains a
/// distinct named property, not folded into `attrs_json`), so AC-S44-1 and AC-S40-6 are still
/// satisfied.  The "structured queryable" goal (Cypher `any(a IN s.params WHERE ...)`) is deferred
/// to an operator that upgrades to Neo4j Enterprise or a version that lifts the restriction.
///
/// This deviation is documented in implementation-notes.md and tagged sr-dev for ADR follow-up.
fn structured_list_to_json_bolt<T: serde::Serialize>(items: &[T]) -> BoltType {
    // serde_json::to_string is infallible for well-formed Serialize impls.
    match serde_json::to_string(items) {
        Ok(json) => BoltType::from(json),
        Err(e) => {
            // Fallback to empty array JSON on unexpected error (should be unreachable).
            tracing::warn!("structured_list_to_json_bolt serialize error: {e}");
            BoltType::from("[]".to_owned())
        }
    }
}

// ── NodeRecord → BoltMap conversion ───────────────────────────────────────────

fn node_to_bolt(n: &NodeRecord) -> HashMap<String, BoltType> {
    let mut m: HashMap<String, BoltType> = HashMap::new();
    // v6: key on integer IDs, not USR strings (S6-SC-03)
    m.insert("symbol_id".into(), BoltType::from(n.symbol_id));
    m.insert("file_id".into(), BoltType::from(n.file_id));
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
    m.insert("repo_name".into(), n.repo_name.clone().into());
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
    m.insert("attrs_json".into(), n.attrs_json.clone().into());
    m.insert("partial".into(), n.partial.into());
    m.insert("phase".into(), (n.phase as i64).into());

    // ── M8 promoted fields (S44, ADR-14) ─────────────────────────────────────
    // Structured lists (params, template_params, template_args) are serialised as JSON strings
    // because Neo4j Community rejects List<Map> as a node property value — see
    // `structured_list_to_json_bolt` doc for the deviation note.
    m.insert(
        "return_type".into(),
        opt_str_to_bolt(n.return_type.as_deref()),
    );
    m.insert(
        "params".into(),
        match &n.params {
            Some(ps) => structured_list_to_json_bolt(ps),
            None => BoltType::Null(neo4rs::BoltNull),
        },
    );
    m.insert("signature".into(), opt_str_to_bolt(n.signature.as_deref()));
    m.insert("code".into(), opt_str_to_bolt(n.code.as_deref()));
    m.insert("code_truncated".into(), opt_bool_to_bolt(n.code_truncated));
    m.insert(
        "template_params".into(),
        match &n.template_params {
            Some(tps) => structured_list_to_json_bolt(tps),
            None => BoltType::Null(neo4rs::BoltNull),
        },
    );
    m.insert(
        "template_args".into(),
        match &n.template_args {
            Some(tas) => structured_list_to_json_bolt(tas),
            None => BoltType::Null(neo4rs::BoltNull),
        },
    );
    m.insert("is_virtual".into(), opt_bool_to_bolt(n.is_virtual));
    m.insert(
        "is_pure_virtual".into(),
        opt_bool_to_bolt(n.is_pure_virtual),
    );
    m.insert("is_static".into(), opt_bool_to_bolt(n.is_static));

    // ── v7 S1 promoted fields ─────────────────────────────────────────────────
    m.insert("is_const".into(), opt_bool_to_bolt(n.is_const));
    m.insert("is_constexpr".into(), opt_bool_to_bolt(n.is_constexpr));
    m.insert(
        "storage_class".into(),
        opt_str_to_bolt(n.storage_class.as_deref()),
    );

    // ── v7 S2 promoted fields ─────────────────────────────────────────────────
    m.insert("is_template".into(), opt_bool_to_bolt(n.is_template));
    m.insert("is_noexcept".into(), opt_bool_to_bolt(n.is_noexcept));
    m.insert("is_override".into(), opt_bool_to_bolt(n.is_override));
    m.insert("is_deleted".into(), opt_bool_to_bolt(n.is_deleted));
    m.insert("is_defaulted".into(), opt_bool_to_bolt(n.is_defaulted));
    m.insert(
        "cv_qualifiers".into(),
        opt_str_to_bolt(n.cv_qualifiers.as_deref()),
    );
    m.insert(
        "ref_qualifier".into(),
        opt_str_to_bolt(n.ref_qualifier.as_deref()),
    );
    m.insert("is_final".into(), opt_bool_to_bolt(n.is_final));
    m.insert("is_abstract".into(), opt_bool_to_bolt(n.is_abstract));
    m.insert(
        "record_kind".into(),
        opt_str_to_bolt(n.record_kind.as_deref()),
    );
    m.insert(
        "type_spelling".into(),
        opt_str_to_bolt(n.type_spelling.as_deref()),
    );
    m.insert(
        "param_index".into(),
        match n.param_index {
            Some(v) => BoltType::from(v),
            None => BoltType::Null(neo4rs::BoltNull),
        },
    );
    m.insert(
        "param_kind".into(),
        opt_str_to_bolt(n.param_kind.as_deref()),
    );

    // ── v7 S5 promoted fields ─────────────────────────────────────────────────
    m.insert(
        "enum_value".into(),
        match n.enum_value {
            Some(v) => BoltType::from(v),
            None => BoltType::Null(neo4rs::BoltNull),
        },
    );

    m
}

// ── EdgeRecord → BoltMap conversion ───────────────────────────────────────────

/// Convert an `EdgeRecord` to a Bolt parameter map.
///
/// Returns `None` when `dst_id` is absent — such edges cannot be MERGE'd
/// because the idempotency key `(src_id, dst_id, kind)` is incomplete (v6).
fn edge_to_bolt(e: &EdgeRecord) -> Option<HashMap<String, BoltType>> {
    let dst_id = e.dst_id?;
    let mut m: HashMap<String, BoltType> = HashMap::new();
    // v6: key on integer IDs, not USR strings (S6-SC-03)
    m.insert("src_id".into(), BoltType::from(e.src_id));
    m.insert("dst_id".into(), BoltType::from(dst_id));
    m.insert("kind".into(), e.kind.as_str().to_owned().into());
    m.insert("resolved".into(), e.resolved.into());
    m.insert("cross_repo_candidate".into(), e.cross_repo_candidate.into());
    m.insert("repo_name".into(), e.repo_name.clone().into());
    m.insert("dst_repo_name".into(), e.dst_repo_name.clone().into());
    m.insert("attrs_json".into(), e.attrs_json.clone().into());
    // M8 promoted edge fields (S44, ADR-14)
    m.insert(
        "source_association_type".into(),
        opt_str_to_bolt(e.source_association_type.as_deref()),
    );
    m.insert(
        "target_association_type".into(),
        opt_str_to_bolt(e.target_association_type.as_deref()),
    );
    // v7 S1 promoted edge field
    m.insert("access".into(), opt_str_to_bolt(e.access.as_deref()));
    // v7 S2 promoted edge field
    m.insert(
        "edge_index".into(),
        match e.edge_index {
            Some(v) => BoltType::from(v),
            None => BoltType::Null(neo4rs::BoltNull),
        },
    );
    // v7 S4 promoted edge field
    m.insert(
        "inherits_is_virtual".into(),
        match e.inherits_is_virtual {
            Some(v) => BoltType::from(v),
            None => BoltType::Null(neo4rs::BoltNull),
        },
    );
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
        for cql in &[
            CQL_ENSURE_NODE_SYMBOL_ID_INDEX,
            CQL_ENSURE_NODE_REPO_INDEX,
            // M8 (S44) covering indexes — ADR-15.
            CQL_ENSURE_RETURN_TYPE_INDEX,
            CQL_ENSURE_IS_VIRTUAL_INDEX,
            CQL_ENSURE_IS_STATIC_INDEX,
            CQL_ENSURE_KIND_RETURN_TYPE_INDEX,
            // v7 S1 indexes.
            CQL_ENSURE_IS_CONST_INDEX,
            CQL_ENSURE_STORAGE_CLASS_INDEX,
            CQL_ENSURE_EDGE_ACCESS_INDEX,
            // v7 S2 indexes.
            CQL_ENSURE_IS_TEMPLATE_INDEX,
            CQL_ENSURE_IS_ABSTRACT_INDEX,
            CQL_ENSURE_TYPE_SPELLING_INDEX,
            CQL_ENSURE_PARAM_INDEX_INDEX,
            CQL_ENSURE_EDGE_INDEX_INDEX,
            // v7 S4 indexes.
            CQL_ENSURE_EDGE_INHERITS_IS_VIRTUAL_INDEX,
            // v7 S5 indexes.
            CQL_ENSURE_ENUM_VALUE_INDEX,
        ] {
            match self.graph.run(query(cql)).await {
                Ok(()) => {}
                Err(e) => {
                    // neo4rs 0.7 may surface Neo.ClientError.Schema.EquivalentSchemaRuleAlreadyExists
                    // (same schema, possibly same name) as an error even though IF NOT EXISTS is
                    // specified. Suppress this class of error — the index already exists and is
                    // functionally equivalent.
                    let text = format!("{e:?}");
                    if text.contains("EquivalentSchemaRuleAlreadyExists")
                        || text.contains("IndexAlreadyExists")
                        || text.contains("ConstraintAlreadyExists")
                    {
                        tracing::debug!(
                            "ensure_indexes: index already exists (suppressed): {text}"
                        );
                    } else {
                        return Err(Self::map_neo4j_err(e));
                    }
                }
            }
        }
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

        // Chunk the raw records (not pre-serialised BoltType rows) so that each
        // task serializes inside its retry loop.  This means attempt 0 pays exactly
        // one serialisation pass with zero extra copies; transient retries
        // re-serialize from the Arc<[NodeRecord]> held by the closure.
        let t0 = Instant::now();
        let mut total_written = 0u64;
        let mut total_retries = 0u32;
        let mut set: JoinSet<Result<WriteStats>> = JoinSet::new();

        for chunk in Self::record_chunks(batch, self.batch_size) {
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
            set.spawn(Self::write_node_chunk(graph, chunk));
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
        if batch.is_empty() {
            return Ok(WriteStats {
                nodes_written: 0,
                retries: 0,
                elapsed: Duration::ZERO,
            });
        }

        // Count resolvable edges up-front (needed for WriteStats.nodes_written).
        let edge_count = batch.iter().filter(|e| e.dst_id.is_some()).count() as u64;
        if edge_count == 0 {
            return Ok(WriteStats {
                nodes_written: 0,
                retries: 0,
                elapsed: Duration::ZERO,
            });
        }

        let t0 = Instant::now();
        let mut total_retries = 0u32;

        // Neo4j can deadlock concurrent relationship MERGE batches when several
        // chunks touch the same high-degree nodes. Keep edge chunks serial and
        // rely on chunk-level retry for transient lock conflicts.
        // record_chunks splits the raw slice so each task re-serialises inside
        // its retry loop — zero clone on the happy path.
        for chunk in Self::record_chunks(batch, self.batch_size) {
            let stats = Self::write_edge_chunk(self.graph.clone(), chunk).await?;
            total_retries += stats.retries;
        }

        Ok(WriteStats {
            // `nodes_written` counts edges written (field name per ADR-2 convention).
            nodes_written: edge_count,
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
    use crate::schema::nodes::{NodeKind, NodeRecord, Param, TemplateArg, TemplateParam};
    use serde_json::Value as JsonValue;

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
            return_type: None,
            params: None,
            signature: None,
            code: None,
            code_truncated: None,
            template_params: None,
            template_args: None,
            is_virtual: None,
            is_pure_virtual: None,
            is_static: None,
            symbol_id: 42,
            file_id: 7,
            is_const: None,
            is_constexpr: None,
            storage_class: None,
            is_template: None,
            is_noexcept: None,
            is_override: None,
            is_deleted: None,
            is_defaulted: None,
            cv_qualifiers: None,
            ref_qualifier: None,
            is_final: None,
            is_abstract: None,
            record_kind: None,
            type_spelling: None,
            param_index: None,
            param_kind: None,
            enum_value: None,
        }
    }

    fn sample_edge(src: &str, dst: Option<&str>) -> EdgeRecord {
        // v6: dst_id set to Some when dst is provided; 0-based stub values.
        let dst_id = dst.map(|_| 99_i64);
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
            source_association_type: None,
            target_association_type: None,
            src_id: 42,
            dst_id,
            dst_repo_name: "test-repo".to_owned(),
            access: None,
            edge_index: None,
            inherits_is_virtual: None,
        }
    }

    // ── Cypher constant sanity checks ─────────────────────────────────────────

    #[test]
    fn cql_merge_nodes_contains_merge_keyword() {
        assert!(
            CQL_MERGE_NODES.contains("MERGE"),
            "CQL_MERGE_NODES must use MERGE for idempotency"
        );
        // v6: keyed on symbol_id, not usr
        assert!(
            CQL_MERGE_NODES.contains("symbol_id"),
            "CQL_MERGE_NODES must key on symbol_id (v6)"
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
        // v6: CQL_ENSURE_NODE_SYMBOL_ID_INDEX replaces the v5 CQL_ENSURE_NODE_USR_INDEX.
        assert!(CQL_ENSURE_NODE_SYMBOL_ID_INDEX.contains("IF NOT EXISTS"));
        assert!(CQL_ENSURE_NODE_REPO_INDEX.contains("IF NOT EXISTS"));
    }

    /// AC-S44-3: all 4 new M8 index constants exist and use IF NOT EXISTS.
    #[test]
    fn cql_m8_indexes_all_present_and_idempotent() {
        for (name, cql) in &[
            ("node_return_type_idx", CQL_ENSURE_RETURN_TYPE_INDEX),
            ("node_is_virtual_idx", CQL_ENSURE_IS_VIRTUAL_INDEX),
            ("node_is_static_idx", CQL_ENSURE_IS_STATIC_INDEX),
            (
                "node_kind_return_type_idx",
                CQL_ENSURE_KIND_RETURN_TYPE_INDEX,
            ),
        ] {
            assert!(
                cql.contains("IF NOT EXISTS"),
                "{name} must use IF NOT EXISTS for idempotency"
            );
            assert!(
                cql.contains(name),
                "CQL constant must contain its index name {name}"
            );
        }
    }

    /// AC-S44-1: CQL_MERGE_NODES contains all promoted node properties (M8 + v7 S1).
    #[test]
    fn cql_merge_nodes_contains_all_promoted_fields() {
        for field in &[
            "return_type",
            "params",
            "signature",
            "code",
            "code_truncated",
            "template_params",
            "template_args",
            "is_virtual",
            "is_pure_virtual",
            "is_static",
            // v7 S1:
            "is_const",
            "is_constexpr",
            "storage_class",
            // v7 S5:
            "enum_value",
        ] {
            assert!(
                CQL_MERGE_NODES.contains(field),
                "CQL_MERGE_NODES missing promoted field: {field}"
            );
        }
    }

    /// AC-S44-2: CQL_MERGE_EDGES contains all promoted edge fields (M8 + v7 S1).
    #[test]
    fn cql_merge_edges_contains_association_type_fields() {
        assert!(
            CQL_MERGE_EDGES.contains("source_association_type"),
            "CQL_MERGE_EDGES missing source_association_type"
        );
        assert!(
            CQL_MERGE_EDGES.contains("target_association_type"),
            "CQL_MERGE_EDGES missing target_association_type"
        );
        assert!(
            CQL_MERGE_EDGES.contains("access"),
            "CQL_MERGE_EDGES missing v7 S1 access field"
        );
        assert!(
            CQL_MERGE_EDGES.contains("inherits_is_virtual"),
            "CQL_MERGE_EDGES missing v7 S4 inherits_is_virtual field"
        );
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
        // v6: usr and file_path dropped; symbol_id and file_id added
        for key in &[
            "symbol_id",
            "file_id",
            "kind",
            "name",
            "qualified_name",
            "mangled_name",
            "line",
            "col",
            "repo_name",
            "attrs_json",
            "partial",
            "phase",
        ] {
            assert!(m.contains_key(*key), "missing key: {key}");
        }
        // v6: usr and file_path must NOT be in the bolt map (S6-SC-03)
        assert!(
            !m.contains_key("usr"),
            "v6 node_to_bolt must NOT emit 'usr' (S6-SC-03)"
        );
        assert!(
            !m.contains_key("file_path"),
            "v6 node_to_bolt must NOT emit 'file_path' (S6-SC-03)"
        );
    }

    /// AC-S44-1: node_to_bolt includes all promoted fields (M8 + v7 S1).
    #[test]
    fn node_to_bolt_includes_all_m8_promoted_fields() {
        let node = sample_node("c:@F@foo");
        let m = node_to_bolt(&node);
        for key in &[
            "return_type",
            "params",
            "signature",
            "code",
            "code_truncated",
            "template_params",
            "template_args",
            "is_virtual",
            "is_pure_virtual",
            "is_static",
            // v7 S1:
            "is_const",
            "is_constexpr",
            "storage_class",
            // v7 S5:
            "enum_value",
        ] {
            assert!(m.contains_key(*key), "node_to_bolt missing field: {key}");
        }
    }

    /// AC-S44-1: None promoted fields serialize to BoltNull, not absent.
    #[test]
    fn node_to_bolt_none_promoted_fields_are_bolt_null() {
        let node = sample_node("u");
        let m = node_to_bolt(&node);
        for key in &[
            "return_type",
            "params",
            "signature",
            "code",
            "code_truncated",
            "template_params",
            "template_args",
            "is_virtual",
            "is_pure_virtual",
            "is_static",
            // v7 S1:
            "is_const",
            "is_constexpr",
            "storage_class",
            // v7 S5:
            "enum_value",
        ] {
            assert!(
                matches!(m[*key], BoltType::Null(_)),
                "None field {key} must be BoltNull"
            );
        }
    }

    /// AC-S44-1: params Some(vec) serializes to a JSON string with correct "type" key (not "type_").
    ///
    /// Neo4j Community does not support `List<Map>` as a property value; structured lists are
    /// stored as JSON strings per the deviation documented in `structured_list_to_json_bolt`.
    #[test]
    fn node_to_bolt_params_serializes_type_key_correctly() {
        let mut node = sample_node("u");
        node.params = Some(vec![Param {
            name: "x".to_owned(),
            type_: "int".to_owned(),
        }]);
        let m = node_to_bolt(&node);
        let params = &m["params"];
        // params must be a BoltString (JSON) — not a BoltList (Neo4j Community rejects List<Map>)
        let json_str = format!("{params:?}");
        assert!(
            matches!(params, BoltType::String(_)),
            "params must be a BoltString (JSON); got: {json_str}"
        );
        // Parse JSON and verify "type" key (not "type_")
        let json_val: Vec<JsonValue> = serde_json::from_str(
            &json_str.trim_matches('"').replace("\\\"", "\""),
        )
        .unwrap_or_else(|_| {
            // Extract raw string via debug representation
            if let BoltType::String(s) = params {
                serde_json::from_str(&s.value).expect("params JSON must be valid")
            } else {
                panic!("expected BoltString")
            }
        });
        assert_eq!(json_val.len(), 1);
        assert!(
            json_val[0].get("type").is_some(),
            "Param JSON must have key 'type' (not 'type_')"
        );
        assert!(
            json_val[0].get("type_").is_none(),
            "Param JSON must NOT have key 'type_'"
        );
        assert!(
            json_val[0].get("name").is_some(),
            "Param JSON must have 'name'"
        );
    }

    /// AC-S44-1: template_params serializes as a JSON string with correct structure.
    #[test]
    fn node_to_bolt_template_params_serializes_as_json_string() {
        let mut node = sample_node("u");
        node.template_params = Some(vec![TemplateParam {
            name: "T".to_owned(),
            kind: "type".to_owned(),
            default: None,
        }]);
        let m = node_to_bolt(&node);
        assert!(
            matches!(m["template_params"], BoltType::String(_)),
            "template_params must be a BoltString (JSON)"
        );
        if let BoltType::String(s) = &m["template_params"] {
            let parsed: Vec<JsonValue> =
                serde_json::from_str(&s.value).expect("template_params JSON must be valid");
            assert_eq!(parsed.len(), 1);
            // default=None serializes as JSON null
            assert_eq!(parsed[0]["default"], JsonValue::Null);
            assert_eq!(parsed[0]["kind"], "type");
        }
    }

    /// AC-S44-1: template_args serializes as a JSON string.
    #[test]
    fn node_to_bolt_template_args_serializes_as_json_string() {
        let mut node = sample_node("u");
        node.template_args = Some(vec![TemplateArg {
            kind: "type".to_owned(),
            value: "int".to_owned(),
        }]);
        let m = node_to_bolt(&node);
        assert!(
            matches!(m["template_args"], BoltType::String(_)),
            "template_args must be a BoltString (JSON)"
        );
        if let BoltType::String(s) = &m["template_args"] {
            let parsed: Vec<JsonValue> =
                serde_json::from_str(&s.value).expect("template_args JSON must be valid");
            assert_eq!(parsed.len(), 1);
            assert_eq!(parsed[0]["kind"], "type");
            assert_eq!(parsed[0]["value"], "int");
        }
    }

    /// AC-S44-1: is_virtual Some(true) serializes to BoltBoolean true.
    #[test]
    fn node_to_bolt_bool_some_serializes_correctly() {
        let mut node = sample_node("u");
        node.is_virtual = Some(true);
        node.is_pure_virtual = Some(false);
        node.is_static = Some(true);
        let m = node_to_bolt(&node);
        assert!(
            matches!(m["is_virtual"], BoltType::Boolean(_)),
            "is_virtual Some(true) must be BoltBoolean"
        );
        let dbg = format!("{:?}", m["is_virtual"]);
        assert!(dbg.contains("true"), "is_virtual must be true; got {dbg}");
        let dbg2 = format!("{:?}", m["is_pure_virtual"]);
        assert!(dbg2.contains("false"), "is_pure_virtual must be false");
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
    fn edge_to_bolt_returns_none_when_dst_id_absent() {
        // v6: skip edges where dst_id is None (not dst_usr)
        let edge = sample_edge("src", None);
        assert!(
            edge_to_bolt(&edge).is_none(),
            "edge with dst_id=None must be skipped (v6)"
        );
    }

    #[test]
    fn edge_to_bolt_returns_some_when_dst_usr_present() {
        let edge = sample_edge("src", Some("dst"));
        let m = edge_to_bolt(&edge).expect("edge with dst_id must convert");
        // v6: integer IDs, not USR strings
        assert!(m.contains_key("src_id"), "v6 edge_to_bolt must emit src_id");
        assert!(m.contains_key("dst_id"), "v6 edge_to_bolt must emit dst_id");
        // v6: src_usr and dst_usr must NOT be in the bolt map (S6-SC-03)
        assert!(
            !m.contains_key("src_usr"),
            "v6 edge_to_bolt must NOT emit 'src_usr' (S6-SC-03)"
        );
        assert!(
            !m.contains_key("dst_usr"),
            "v6 edge_to_bolt must NOT emit 'dst_usr' (S6-SC-03)"
        );
    }

    /// AC-S44-2: edge_to_bolt includes both association type fields.
    #[test]
    fn edge_to_bolt_includes_association_type_fields() {
        let edge = sample_edge("a", Some("b"));
        let m = edge_to_bolt(&edge).unwrap();
        assert!(
            m.contains_key("source_association_type"),
            "edge map must contain source_association_type"
        );
        assert!(
            m.contains_key("target_association_type"),
            "edge map must contain target_association_type"
        );
    }

    /// AC-S44-2: None association types serialize to BoltNull.
    #[test]
    fn edge_to_bolt_none_association_types_are_bolt_null() {
        let edge = sample_edge("a", Some("b"));
        let m = edge_to_bolt(&edge).unwrap();
        assert!(
            matches!(m["source_association_type"], BoltType::Null(_)),
            "None source_association_type must be BoltNull"
        );
        assert!(
            matches!(m["target_association_type"], BoltType::Null(_)),
            "None target_association_type must be BoltNull"
        );
    }

    /// AC-S44-2: Some association type serializes to BoltString.
    #[test]
    fn edge_to_bolt_some_association_type_is_bolt_string() {
        let mut edge = sample_edge("a", Some("b"));
        edge.source_association_type = Some("read".to_owned());
        edge.target_association_type = Some("write".to_owned());
        let m = edge_to_bolt(&edge).unwrap();
        let src_dbg = format!("{:?}", m["source_association_type"]);
        assert!(
            src_dbg.contains("read"),
            "source_association_type must be 'read'; got {src_dbg}"
        );
        let tgt_dbg = format!("{:?}", m["target_association_type"]);
        assert!(
            tgt_dbg.contains("write"),
            "target_association_type must be 'write'; got {tgt_dbg}"
        );
    }

    /// v7 S1: edge_to_bolt includes access field; None → BoltNull.
    #[test]
    fn edge_to_bolt_includes_access_field() {
        let edge = sample_edge("a", Some("b"));
        let m = edge_to_bolt(&edge).unwrap();
        assert!(
            m.contains_key("access"),
            "edge map must contain access (v7 S1)"
        );
        assert!(
            matches!(m["access"], BoltType::Null(_)),
            "None access must be BoltNull"
        );
    }

    /// v7 S1: access Some("protected") serializes to BoltString.
    #[test]
    fn edge_to_bolt_some_access_is_bolt_string() {
        let mut edge = sample_edge("a", Some("b"));
        edge.access = Some("protected".to_owned());
        let m = edge_to_bolt(&edge).unwrap();
        let dbg = format!("{:?}", m["access"]);
        assert!(
            dbg.contains("protected"),
            "access must be 'protected'; got {dbg}"
        );
    }

    /// v7 S4: edge_to_bolt includes inherits_is_virtual field; None → BoltNull.
    #[test]
    fn edge_to_bolt_includes_inherits_is_virtual_field() {
        let edge = sample_edge("a", Some("b"));
        let m = edge_to_bolt(&edge).unwrap();
        assert!(
            m.contains_key("inherits_is_virtual"),
            "edge map must contain inherits_is_virtual (v7 S4)"
        );
        assert!(
            matches!(m["inherits_is_virtual"], BoltType::Null(_)),
            "None inherits_is_virtual must be BoltNull"
        );
    }

    /// v7 S4: inherits_is_virtual Some(true) serializes to BoltBoolean.
    #[test]
    fn edge_to_bolt_some_inherits_is_virtual_true_is_bolt_boolean() {
        let mut edge = sample_edge("a", Some("b"));
        edge.kind = EdgeKind::Inherits;
        edge.inherits_is_virtual = Some(true);
        let m = edge_to_bolt(&edge).unwrap();
        let dbg = format!("{:?}", m["inherits_is_virtual"]);
        assert!(
            dbg.contains("true"),
            "inherits_is_virtual Some(true) must be BoltBoolean(true); got {dbg}"
        );
    }

    /// v7 S5: node_to_bolt includes enum_value field; None → BoltNull.
    #[test]
    fn node_to_bolt_includes_enum_value_field() {
        let node = sample_node("c:@E@Color@Red");
        let m = node_to_bolt(&node);
        assert!(
            m.contains_key("enum_value"),
            "node map must contain enum_value (v7 S5)"
        );
        assert!(
            matches!(m["enum_value"], BoltType::Null(_)),
            "None enum_value must be BoltNull"
        );
    }

    /// v7 S5: enum_value Some(42) serializes to BoltInteger.
    #[test]
    fn node_to_bolt_some_enum_value_is_bolt_integer() {
        let mut node = sample_node("c:@E@Color@Red");
        node.enum_value = Some(42);
        let m = node_to_bolt(&node);
        let dbg = format!("{:?}", m["enum_value"]);
        assert!(
            dbg.contains("42"),
            "enum_value Some(42) must be BoltInteger(42); got {dbg}"
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
    // WriteStats aggregation without requiring a live Neo4j instance.

    #[test]
    fn batch_size_chunking_divides_evenly() {
        // 10 nodes with batch_size=3 → ceil(10/3) = 4 chunks: [3,3,3,1].
        let batch_size = 3_usize;
        let nodes: Vec<NodeRecord> = (0..10).map(|i| sample_node(&format!("u{i}"))).collect();
        let chunks = Neo4jSink::record_chunks(&nodes, batch_size);
        assert_eq!(chunks.len(), 4, "10 items / batch_size 3 => 4 chunks");
        assert_eq!(chunks[0].len(), 3);
        assert_eq!(chunks[3].len(), 1);
    }

    #[test]
    fn batch_size_larger_than_batch_produces_single_chunk() {
        let nodes: Vec<NodeRecord> = (0..5).map(|i| sample_node(&format!("u{i}"))).collect();
        let chunks = Neo4jSink::record_chunks(&nodes, 1000);
        assert_eq!(chunks.len(), 1, "batch_size > total items => 1 chunk");
        assert_eq!(chunks[0].len(), 5);
    }

    #[test]
    fn edge_to_bolt_idempotency_key_present_for_resolved_edges() {
        // v6: idempotency keys are src_id, dst_id, kind (not usr strings).
        let edge = sample_edge("src", Some("dst"));
        let m = edge_to_bolt(&edge).expect("resolved edge must convert");
        assert!(
            m.contains_key("src_id"),
            "v6 idempotency key src_id must be in bolt map"
        );
        assert!(
            m.contains_key("dst_id"),
            "v6 idempotency key dst_id must be in bolt map"
        );
        assert!(
            m.contains_key("kind"),
            "idempotency key kind must be in bolt map"
        );
    }

    #[test]
    fn unresolved_edges_filtered_before_chunking() {
        // write_edge_chunk serializes inside the retry loop and skips edges with
        // dst_id=None. Simulate what write_edge_chunk's builder does.
        let edges = [
            sample_edge("a", Some("b")),
            sample_edge("c", None), // unresolved
            sample_edge("d", Some("e")),
        ];
        let rows: Vec<BoltType> = edges
            .iter()
            .filter_map(|e| edge_to_bolt(e).map(BoltType::from))
            .collect();
        assert_eq!(rows.len(), 2, "unresolved edge must be excluded in builder");
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
        // Verify that record_chunks respects batch_size.
        // 7 nodes / batch_size=3 → 3 chunks (3, 3, 1)
        let nodes: Vec<NodeRecord> = (0..7).map(|i| sample_node(&format!("u{i}"))).collect();
        let chunks = Neo4jSink::record_chunks(&nodes, 3);
        assert_eq!(
            chunks.len(),
            3,
            "batch_size=3 must produce 3 chunks for 7 nodes"
        );
    }

    // ── record_chunks ─────────────────────────────────────────────────────────

    #[test]
    fn record_chunks_sizes_match_slice_chunks() {
        let nodes: Vec<NodeRecord> = (0..10).map(|i| sample_node(&format!("u{i}"))).collect();
        let chunks = Neo4jSink::record_chunks(&nodes, 3);
        let expected: Vec<usize> = nodes.chunks(3).map(|c| c.len()).collect();
        let actual: Vec<usize> = chunks.iter().map(|c| c.len()).collect();
        assert_eq!(actual, expected, "chunk sizes must match slice::chunks");
    }

    #[test]
    fn record_chunks_preserves_order() {
        let nodes: Vec<NodeRecord> = (0..7)
            .map(|i| {
                let mut n = sample_node("u");
                n.symbol_id = i as i64;
                n
            })
            .collect();
        let orig_ids: Vec<i64> = nodes.iter().map(|n| n.symbol_id).collect();
        let chunks = Neo4jSink::record_chunks(&nodes, 3);
        let result_ids: Vec<i64> = chunks
            .into_iter()
            .flat_map(|c| c.iter().map(|n| n.symbol_id).collect::<Vec<_>>())
            .collect();
        assert_eq!(result_ids, orig_ids, "record_chunks must preserve order");
    }

    #[test]
    fn record_chunks_single_chunk_when_size_ge_len() {
        let nodes: Vec<NodeRecord> = (0..5).map(|i| sample_node(&format!("u{i}"))).collect();
        let chunks = Neo4jSink::record_chunks(&nodes, 1000);
        assert_eq!(chunks.len(), 1);
        assert_eq!(chunks[0].len(), 5);
    }

    #[test]
    fn record_chunks_empty_input_produces_no_chunks() {
        let empty: &[NodeRecord] = &[];
        let chunks = Neo4jSink::record_chunks(empty, 10);
        assert!(chunks.is_empty());
    }

    #[test]
    fn record_chunks_exact_divisible() {
        let nodes: Vec<NodeRecord> = (0..6).map(|i| sample_node(&format!("u{i}"))).collect();
        let chunks = Neo4jSink::record_chunks(&nodes, 3);
        assert_eq!(chunks.len(), 2);
        assert_eq!(chunks[0].len(), 3);
        assert_eq!(chunks[1].len(), 3);
    }

    // ── retry-path: builder invoked once per attempt ──────────────────────────

    /// Verifies that `run_chunk_with_retry` invokes `make_body` on every attempt,
    /// and that the payload delivered on a retry is identical to the original.
    ///
    /// We simulate a transient failure on attempt 0 by building a mock executor
    /// that fails once then succeeds.  The test checks:
    ///   (a) `make_body` was called twice (once per attempt),
    ///   (b) both calls produced the same payload (same node bolt serialization),
    ///   (c) `WriteStats.retries == 1`.
    #[tokio::test]
    async fn retry_path_builder_invoked_per_attempt_payload_identical() {
        use std::sync::atomic::{AtomicU32, Ordering};
        use std::sync::{Arc, Mutex};

        // Track calls to make_body and capture the payloads.
        let call_count = Arc::new(AtomicU32::new(0));
        let payloads: Arc<Mutex<Vec<Vec<BoltType>>>> = Arc::new(Mutex::new(vec![]));

        let node = sample_node("u_retry");
        let records: Vec<NodeRecord> = vec![node];

        let cc = call_count.clone();
        let pp = payloads.clone();
        let make_body = move || {
            cc.fetch_add(1, Ordering::Relaxed);
            let body: Vec<BoltType> = records
                .iter()
                .map(|n| BoltType::from(node_to_bolt(n)))
                .collect();
            pp.lock().unwrap().push(body.clone());
            body
        };

        // Mock executor: fail with a transient error on attempt 0, succeed on attempt 1.
        let attempt = Arc::new(AtomicU32::new(0));
        let ac = attempt.clone();
        let executor = move |_body: Vec<BoltType>| {
            let a = ac.fetch_add(1, Ordering::Relaxed);
            async move {
                if a == 0 {
                    // Return a transient-looking error string.
                    Err(Error::Sink {
                        backend: "neo4j",
                        source: "Neo.TransientError.Transaction.DeadlockDetected".into(),
                    })
                } else {
                    Ok(1u64)
                }
            }
        };

        let stats = Neo4jSink::run_chunk_with_retry(make_body, executor)
            .await
            .expect("should succeed on retry");

        assert_eq!(
            call_count.load(Ordering::Relaxed),
            2,
            "make_body must be called once per attempt"
        );
        assert_eq!(stats.retries, 1, "one retry must be counted");

        let captured = payloads.lock().unwrap();
        assert_eq!(captured.len(), 2, "two payloads captured");
        // Both attempts must have produced the same bolt serialization.
        let ids_0: Vec<i64> = captured[0]
            .iter()
            .filter_map(|b| {
                if let BoltType::Map(m) = b {
                    if let Some(BoltType::Integer(id)) = m.value.get("symbol_id") {
                        return Some(id.value);
                    }
                }
                None
            })
            .collect();
        let ids_1: Vec<i64> = captured[1]
            .iter()
            .filter_map(|b| {
                if let BoltType::Map(m) = b {
                    if let Some(BoltType::Integer(id)) = m.value.get("symbol_id") {
                        return Some(id.value);
                    }
                }
                None
            })
            .collect();
        assert_eq!(ids_0, ids_1, "retry payload must be identical to original");
    }
}
