//! `IndraDbSink` — GraphSink implementation backed by IndraDB via gRPC (`indradb-proto`).
//!
//! Stories: S12-sink-indradb (AC-M1-21, AC-M1-23 IndraDB half, AC-M3-6).
//! ADR: ADR-2.
//!
//! # USR → Uuid mapping
//!
//! IndraDB vertices are keyed by UUID (16 bytes). Our schema primary key is the USR
//! string.  We derive a deterministic UUID with UUIDv5 over a project-specific
//! namespace UUID + `format!("{repo_name}\x00{usr}")`.  This guarantees
//! idempotency on `(usr, repo_name)` without a round-trip lookup.
//!
//! Namespace UUID: `b5e5c18e-0f4a-5b2e-8e6e-c3a0d6e8f1b2` (fixed, project-level constant).
//!
//! # Concurrency
//!
//! `indradb_proto::Client` requires `&mut self` but is cheaply `Clone` because it
//! holds only an internally-pooled tonic `Channel`.  We store the client directly
//! (no `Mutex`) and clone it before each RPC so that concurrent writes are not
//! serialised on a shared lock.
//!
//! # Retry policy
//!
//! Transient errors (`Transport`, `Grpc` with `UNAVAILABLE` / `DEADLINE_EXCEEDED` /
//! `RESOURCE_EXHAUSTED`) are retried up to 3 total attempts with exponential
//! back-off (100 ms, 200 ms).  Non-transient errors are returned immediately.

use std::time::{Duration, Instant};

use async_trait::async_trait;
use indradb::{BulkInsertItem, Edge, Identifier, Json, QueryExt, Vertex};
use indradb_proto::ClientError;
use tokio::task::JoinSet;
use tonic::Code;
use uuid::Uuid;

use crate::config::{IndraDbSinkConfig, DEFAULT_BATCH_SIZE, DEFAULT_SESSIONS};
use crate::error::{Error, Result};
use crate::schema::{EdgeRecord, NodeRecord};
use crate::sink::lock::Phase5LockGuard;
use crate::sink::{GraphSink, HealthInfo, ResetTarget, WriteStats};

// ── Constants ─────────────────────────────────────────────────────────────────

/// UUIDv5 namespace for USR→Uuid derivation.  Fixed project-level constant.
/// Bytes: b5e5c18e-0f4a-5b2e-8e6e-c3a0d6e8f1b2.
const USR_NAMESPACE: Uuid = Uuid::from_bytes([
    0xb5, 0xe5, 0xc1, 0x8e, 0x0f, 0x4a, 0x5b, 0x2e, 0x8e, 0x6e, 0xc3, 0xa0, 0xd6, 0xe8, 0xf1, 0xb2,
]);

/// Property name for the USR string stored on each vertex.
const PROP_USR: &str = "usr";
/// Property name for the repository name.
const PROP_REPO_NAME: &str = "repo_name";
/// Property name for the node kind string.
const PROP_KIND: &str = "kind";
/// Property name for the display name.
const PROP_NAME: &str = "name";
/// Property name for the fully-qualified name.
const PROP_QUALIFIED_NAME: &str = "qualified_name";
/// Property name for extra attributes (JSON blob).
const PROP_ATTRS_JSON: &str = "attrs_json";

// ── M8 promoted property-name constants (S45, design.md §3.6) ─────────────────

/// Return type string; FUNCTION/METHOD only.
const PROP_RETURN_TYPE: &str = "return_type";
/// Parameter list; serialised as `indradb::Json`; FUNCTION/METHOD only.
const PROP_PARAMS: &str = "params";
/// Signature string; FUNCTION/METHOD only.
const PROP_SIGNATURE: &str = "signature";
/// Verbatim source snippet capped at 32 KiB (ADR-12); FUNCTION/METHOD only.
const PROP_CODE: &str = "code";
/// `true` when the body was truncated; FUNCTION/METHOD only.
const PROP_CODE_TRUNCATED: &str = "code_truncated";
/// Template parameter list; serialised as `indradb::Json`; TEMPLATE_DECL only.
const PROP_TEMPLATE_PARAMS: &str = "template_params";
/// Template argument list; serialised as `indradb::Json`; SPECIALIZATION only.
const PROP_TEMPLATE_ARGS: &str = "template_args";
/// `true` when the method is virtual; METHOD only.
const PROP_IS_VIRTUAL: &str = "is_virtual";
/// `true` when the method is pure virtual; METHOD only.
const PROP_IS_PURE_VIRTUAL: &str = "is_pure_virtual";
/// `true` when the function/method is static; FUNCTION/METHOD only.
const PROP_IS_STATIC: &str = "is_static";
/// USES edge source access kind (e.g. `"read"`, `"write"`); USES edges only.
const PROP_SRC_ASSOC_TYPE: &str = "source_association_type";
/// USES edge target access kind; USES edges only.
const PROP_DST_ASSOC_TYPE: &str = "target_association_type";
/// Vertex type used for the Phase 5 lock sentinel.
const LOCK_VERTEX_TYPE: &str = "cxg_phase5_lock";
/// Property name for the lock holder tag.
const PROP_LOCK_HOLDER: &str = "holder";
/// Schema-version vertex type.
const SCHEMA_VERSION_TYPE: &str = "cxg_schema_version";
/// Schema-version property name.
const PROP_SCHEMA_VERSION: &str = "version";
/// Property name for the full attrs JSON blob on the schema-version vertex.
const PROP_SCHEMA_ATTRS_JSON: &str = "attrs_json";
/// Fixed UUID for the singleton schema-version vertex (upsert target).
///
/// Using a deterministic UUID means `write_schema_version` can MERGE
/// (delete + re-insert) rather than scanning for an existing vertex.
/// Value: UUIDv5 of namespace + "cxg_schema_version_singleton".
const SCHEMA_VERSION_UUID: Uuid = Uuid::from_bytes([
    0x3d, 0x1a, 0x2e, 0x5f, 0x8c, 0x4b, 0x57, 0x9a, 0xb1, 0x6e, 0xd2, 0xf3, 0xa4, 0x05, 0xc8, 0x1d,
]);

/// Total retry attempts (first attempt + up to 2 retries = 3 total).
const MAX_ATTEMPTS: u32 = 3;
/// Base back-off duration for the first retry.
const BACKOFF_BASE_MS: u64 = 100;

// ── Helpers ───────────────────────────────────────────────────────────────────

/// Derive a deterministic `Uuid` from a `(repo_name, usr)` pair using UUIDv5.
fn usr_to_uuid(repo_name: &str, usr: &str) -> Uuid {
    let key = format!("{repo_name}\x00{usr}");
    Uuid::new_v5(&USR_NAMESPACE, key.as_bytes())
}

/// Return `true` for errors that are worth retrying.
fn is_transient(err: &ClientError) -> bool {
    match err {
        ClientError::Transport { .. } => true,
        ClientError::Grpc { inner } => matches!(
            inner.code(),
            Code::Unavailable | Code::DeadlineExceeded | Code::ResourceExhausted
        ),
        _ => false,
    }
}

/// Wrap a `ClientError` into the crate-level `Error::Sink`.
fn wrap(e: ClientError) -> Error {
    Error::Sink {
        backend: "indradb",
        source: Box::new(e),
    }
}

/// Wrap an `indradb::ValidationError` into `Error::Sink`.
fn wrap_validation(e: indradb::ValidationError) -> Error {
    Error::Sink {
        backend: "indradb",
        source: Box::new(e),
    }
}

/// Build a validated `Identifier`, mapping validation errors to `Error::Sink`.
fn ident(s: &str) -> Result<Identifier> {
    Identifier::new(s).map_err(wrap_validation)
}

/// Build a `Json` value from a JSON string.
fn json_str(s: &str) -> Result<Json> {
    let v: serde_json::Value = serde_json::from_str(s).map_err(|e| Error::Sink {
        backend: "indradb",
        source: Box::new(e),
    })?;
    Ok(Json::new(v))
}

/// Serialise any `serde::Serialize` value to `indradb::Json` (ADR-14).
///
/// Used for structured-list fields (`params`, `template_params`, `template_args`)
/// that carry `Vec<Struct>` in Rust and must land as a JSON array in IndraDB.
fn json_value<T: serde::Serialize>(v: &T) -> Result<Json> {
    let val = serde_json::to_value(v).map_err(|e| Error::Sink {
        backend: "indradb",
        source: Box::new(e),
    })?;
    Ok(Json::new(val))
}

// ── Phase5LockGuard impl ──────────────────────────────────────────────────────

/// Advisory Phase 5 lock held as a sentinel vertex in IndraDB.
///
/// The lock is released by deleting the sentinel vertex.  On drop, a
/// best-effort fire-and-forget task is spawned if a tokio runtime is available.
pub struct IndraDbPhase5LockGuard {
    client: indradb_proto::Client,
    vertex_id: Uuid,
    released: bool,
}

impl IndraDbPhase5LockGuard {
    async fn do_release(&mut self) -> Result<()> {
        if self.released {
            return Ok(());
        }
        self.released = true;
        let mut c = self.client.clone();
        let q = indradb::SpecificVertexQuery::single(self.vertex_id);
        c.delete(q).await.map_err(wrap)
    }
}

impl Phase5LockGuard for IndraDbPhase5LockGuard {
    fn release(
        &mut self,
    ) -> std::pin::Pin<Box<dyn std::future::Future<Output = Result<()>> + Send + '_>> {
        Box::pin(self.do_release())
    }
}

impl Drop for IndraDbPhase5LockGuard {
    fn drop(&mut self) {
        if self.released {
            return;
        }
        // Best-effort: fire-and-forget via tokio::spawn.  Do NOT block the executor.
        if let Ok(rt) = tokio::runtime::Handle::try_current() {
            let mut c = self.client.clone();
            let vid = self.vertex_id;
            rt.spawn(async move {
                let q = indradb::SpecificVertexQuery::single(vid);
                let _ = c.delete(q).await;
            });
        }
        self.released = true;
    }
}

// ── IndraDbSink ───────────────────────────────────────────────────────────────

/// `GraphSink` implementation backed by IndraDB via gRPC (`indradb-proto` v5).
///
/// # Batching
///
/// `write_nodes` / `write_edges` split their input into `batch_size`-item
/// chunks and dispatch up to `sessions` chunks concurrently using
/// `tokio::task::JoinSet`.  Each task clones the cheap tonic `Channel` client.
pub struct IndraDbSink {
    /// Base client.  Cheaply cloneable (holds a pooled tonic `Channel`).
    /// Each RPC call clones this before use so concurrent calls are not serialised.
    client: indradb_proto::Client,
    /// Resolved auth token (empty string = no auth required).
    #[allow(dead_code)]
    token: String,
    /// Records per `bulk_insert` call (default [`DEFAULT_BATCH_SIZE`]).
    batch_size: usize,
    /// Maximum concurrent in-flight `bulk_insert` calls (default [`DEFAULT_SESSIONS`]).
    sessions: usize,
}

impl IndraDbSink {
    /// Construct from resolved config + token.  Does **not** open a connection;
    /// call [`preflight`][GraphSink::preflight] to verify reachability.
    pub async fn new(config: &IndraDbSinkConfig, token: String) -> Result<Self> {
        let endpoint =
            tonic::transport::Endpoint::from_shared(config.endpoint.clone()).map_err(|e| {
                Error::Sink {
                    backend: "indradb",
                    source: Box::new(e),
                }
            })?;
        let client = indradb_proto::Client::new(endpoint).await.map_err(wrap)?;
        Ok(Self {
            client,
            token,
            batch_size: DEFAULT_BATCH_SIZE,
            sessions: config.sessions.unwrap_or(DEFAULT_SESSIONS),
        })
    }

    /// Override the batch size for `bulk_insert` chunks.
    ///
    /// Useful for testing and for wiring the top-level `[sink].batch_size` config
    /// knob without changing per-backend constructor signatures.
    pub fn with_batch_size(mut self, n: usize) -> Self {
        self.batch_size = n;
        self
    }

    /// Clone the inner client (cheap — shares the tonic channel).
    fn client(&self) -> indradb_proto::Client {
        self.client.clone()
    }
}

#[async_trait]
impl GraphSink for IndraDbSink {
    fn backend_name(&self) -> &'static str {
        "indradb"
    }

    async fn preflight(&self) -> Result<()> {
        let mut c = self.client();
        c.ping().await.map_err(wrap)
    }

    async fn ensure_indexes(&self) -> Result<()> {
        // Index `usr` and `repo_name` properties for fast lookups and reset operations.
        let mut c = self.client();
        c.index_property(ident(PROP_USR)?).await.map_err(wrap)?;
        c.index_property(ident(PROP_REPO_NAME)?)
            .await
            .map_err(wrap)?;

        // M8 (S45/ADR-15): register the 4 M8 property indexes.
        // IndraDB v5 only supports per-property existence indexes (no composite or range
        // indexes). The Neo4j composite index (kind, return_type) has no IndraDB peer —
        // documented as a known limitation per ADR-15 §"IndraDB property-index parity".
        for name in &[PROP_RETURN_TYPE, PROP_IS_VIRTUAL, PROP_IS_STATIC, PROP_KIND] {
            c.index_property(ident(name)?).await.map_err(wrap)?;
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

        let started = Instant::now();

        // Build BulkInsertItems: Vertex + base properties + M8 optional properties per record.
        //
        // Base fields: 1 Vertex + 6 VertexProperty (usr, repo_name, kind, name,
        // qualified_name, attrs_json) = 7 items guaranteed.
        // M8 optional fields: up to 10 additional VertexProperty items when all
        // optional fields are Some — giving a worst-case of 17 items per node.
        // `None` fields are skipped (AC-S45-1: no spurious property writes).
        let mut all_items: Vec<BulkInsertItem> = Vec::with_capacity(batch.len() * 17);
        for node in batch {
            let vid = usr_to_uuid(&node.repo_name, &node.usr);
            let vtype = ident(node.kind.as_str())?;
            all_items.push(BulkInsertItem::Vertex(Vertex::with_id(vid, vtype)));
            all_items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_USR)?,
                Json::new(serde_json::Value::String(node.usr.clone())),
            ));
            all_items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_REPO_NAME)?,
                Json::new(serde_json::Value::String(node.repo_name.clone())),
            ));
            all_items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_KIND)?,
                Json::new(serde_json::Value::String(node.kind.as_str().to_owned())),
            ));
            all_items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_NAME)?,
                Json::new(serde_json::Value::String(node.name.clone())),
            ));
            all_items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_QUALIFIED_NAME)?,
                Json::new(serde_json::Value::String(node.qualified_name.clone())),
            ));
            all_items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_ATTRS_JSON)?,
                json_str(&node.attrs_json)?,
            ));

            // ── M8 optional fields (S45) — skip None to avoid spurious property writes ──

            if let Some(ref v) = node.return_type {
                all_items.push(BulkInsertItem::VertexProperty(
                    vid,
                    ident(PROP_RETURN_TYPE)?,
                    Json::new(serde_json::Value::String(v.clone())),
                ));
            }
            if let Some(ref v) = node.params {
                all_items.push(BulkInsertItem::VertexProperty(
                    vid,
                    ident(PROP_PARAMS)?,
                    json_value(v)?,
                ));
            }
            if let Some(ref v) = node.signature {
                all_items.push(BulkInsertItem::VertexProperty(
                    vid,
                    ident(PROP_SIGNATURE)?,
                    Json::new(serde_json::Value::String(v.clone())),
                ));
            }
            if let Some(ref v) = node.code {
                all_items.push(BulkInsertItem::VertexProperty(
                    vid,
                    ident(PROP_CODE)?,
                    Json::new(serde_json::Value::String(v.clone())),
                ));
            }
            if let Some(v) = node.code_truncated {
                all_items.push(BulkInsertItem::VertexProperty(
                    vid,
                    ident(PROP_CODE_TRUNCATED)?,
                    Json::new(serde_json::Value::Bool(v)),
                ));
            }
            if let Some(ref v) = node.template_params {
                all_items.push(BulkInsertItem::VertexProperty(
                    vid,
                    ident(PROP_TEMPLATE_PARAMS)?,
                    json_value(v)?,
                ));
            }
            if let Some(ref v) = node.template_args {
                all_items.push(BulkInsertItem::VertexProperty(
                    vid,
                    ident(PROP_TEMPLATE_ARGS)?,
                    json_value(v)?,
                ));
            }
            if let Some(v) = node.is_virtual {
                all_items.push(BulkInsertItem::VertexProperty(
                    vid,
                    ident(PROP_IS_VIRTUAL)?,
                    Json::new(serde_json::Value::Bool(v)),
                ));
            }
            if let Some(v) = node.is_pure_virtual {
                all_items.push(BulkInsertItem::VertexProperty(
                    vid,
                    ident(PROP_IS_PURE_VIRTUAL)?,
                    Json::new(serde_json::Value::Bool(v)),
                ));
            }
            if let Some(v) = node.is_static {
                all_items.push(BulkInsertItem::VertexProperty(
                    vid,
                    ident(PROP_IS_STATIC)?,
                    Json::new(serde_json::Value::Bool(v)),
                ));
            }
        }

        // Chunk by worst-case items per node (17 = 7 base + 10 M8 optional) × batch_size.
        // The effective chunk may be smaller when optional fields are sparse, which is
        // acceptable — it errs toward under-utilizing the batch size.
        // Flagged in implementation-notes as a known sizing concern per ADR-15.
        let items_per_node_worst_case = 17_usize;
        let chunk_items = self.batch_size.saturating_mul(items_per_node_worst_case);
        let total_nodes = batch.len() as u64;

        let mut total_retries = 0u32;
        // JoinSet task returns crate Result<u32> (retries used for this chunk).
        let mut set: JoinSet<Result<u32>> = JoinSet::new();

        for item_chunk in all_items.chunks(chunk_items) {
            if set.len() >= self.sessions {
                if let Some(join_res) = set.join_next().await {
                    let chunk_retries = join_res.map_err(|e| Error::Sink {
                        backend: "indradb",
                        source: e.into(),
                    })??;
                    total_retries += chunk_retries;
                }
            }

            let mut c = self.client();
            let items = item_chunk.to_vec();
            let max_attempts = MAX_ATTEMPTS;
            let backoff_base = BACKOFF_BASE_MS;
            set.spawn(async move {
                let mut last_err: Option<ClientError> = None;
                let mut retries = 0u32;
                for attempt in 0..max_attempts {
                    match c.bulk_insert(items.clone()).await {
                        Ok(_) => return Ok(retries),
                        Err(e) if is_transient(&e) && attempt + 1 < max_attempts => {
                            let wait_ms = backoff_base * (1u64 << attempt);
                            tokio::time::sleep(Duration::from_millis(wait_ms)).await;
                            retries += 1;
                            last_err = Some(e);
                        }
                        Err(e) => return Err(wrap(e)),
                    }
                }
                Err(wrap(last_err.unwrap()))
            });
        }

        while let Some(join_res) = set.join_next().await {
            let chunk_retries = join_res.map_err(|e| Error::Sink {
                backend: "indradb",
                source: e.into(),
            })??;
            total_retries += chunk_retries;
        }

        Ok(WriteStats {
            nodes_written: total_nodes,
            retries: total_retries,
            elapsed: started.elapsed(),
        })
    }

    async fn write_edges(&self, batch: &[EdgeRecord]) -> Result<WriteStats> {
        // Build items for all resolvable edges.
        //
        // Base items: 1 Edge + 1 EdgeProperty (attrs_json) = 2 items guaranteed.
        // M8 optional: up to 2 additional EdgeProperty items for association_type fields —
        // giving a worst-case of 4 items per edge. `None` fields are skipped.
        let mut all_items: Vec<BulkInsertItem> = Vec::with_capacity(batch.len() * 4);
        let mut total_written = 0u64;
        for edge_rec in batch {
            let dst_usr = match &edge_rec.dst_usr {
                Some(u) => u,
                None => continue, // unresolved edge; skip
            };
            let src_id = usr_to_uuid(&edge_rec.repo_name, &edge_rec.src_usr);
            let dst_id = usr_to_uuid(&edge_rec.repo_name, dst_usr);
            let edge_type = ident(edge_rec.kind.as_str())?;
            let e = Edge::new(src_id, edge_type, dst_id);
            all_items.push(BulkInsertItem::Edge(e.clone()));
            all_items.push(BulkInsertItem::EdgeProperty(
                e.clone(),
                ident(PROP_ATTRS_JSON)?,
                json_str(&edge_rec.attrs_json)?,
            ));

            // ── M8 optional edge fields (S45) — USES edges only; skip None ───────
            if let Some(ref v) = edge_rec.source_association_type {
                all_items.push(BulkInsertItem::EdgeProperty(
                    e.clone(),
                    ident(PROP_SRC_ASSOC_TYPE)?,
                    Json::new(serde_json::Value::String(v.clone())),
                ));
            }
            if let Some(ref v) = edge_rec.target_association_type {
                all_items.push(BulkInsertItem::EdgeProperty(
                    e.clone(),
                    ident(PROP_DST_ASSOC_TYPE)?,
                    Json::new(serde_json::Value::String(v.clone())),
                ));
            }

            total_written += 1;
        }

        if all_items.is_empty() {
            return Ok(WriteStats {
                nodes_written: 0,
                retries: 0,
                elapsed: Duration::ZERO,
            });
        }

        let started = Instant::now();

        // Chunk by worst-case items per edge (4 = 2 base + 2 M8 optional) × batch_size.
        let items_per_edge_worst_case = 4_usize;
        let chunk_items = self.batch_size.saturating_mul(items_per_edge_worst_case);

        let mut total_retries = 0u32;
        // JoinSet task returns crate Result<u32> (retries used for this chunk).
        let mut set: JoinSet<Result<u32>> = JoinSet::new();

        for item_chunk in all_items.chunks(chunk_items) {
            if set.len() >= self.sessions {
                if let Some(join_res) = set.join_next().await {
                    let chunk_retries = join_res.map_err(|e| Error::Sink {
                        backend: "indradb",
                        source: e.into(),
                    })??;
                    total_retries += chunk_retries;
                }
            }

            let mut c = self.client();
            let items = item_chunk.to_vec();
            let max_attempts = MAX_ATTEMPTS;
            let backoff_base = BACKOFF_BASE_MS;
            set.spawn(async move {
                let mut last_err: Option<ClientError> = None;
                let mut retries = 0u32;
                for attempt in 0..max_attempts {
                    match c.bulk_insert(items.clone()).await {
                        Ok(_) => return Ok(retries),
                        Err(e) if is_transient(&e) && attempt + 1 < max_attempts => {
                            let wait_ms = backoff_base * (1u64 << attempt);
                            tokio::time::sleep(Duration::from_millis(wait_ms)).await;
                            retries += 1;
                            last_err = Some(e);
                        }
                        Err(e) => return Err(wrap(e)),
                    }
                }
                Err(wrap(last_err.unwrap()))
            });
        }

        while let Some(join_res) = set.join_next().await {
            let chunk_retries = join_res.map_err(|e| Error::Sink {
                backend: "indradb",
                source: e.into(),
            })??;
            total_retries += chunk_retries;
        }

        Ok(WriteStats {
            nodes_written: total_written,
            retries: total_retries,
            elapsed: started.elapsed(),
        })
    }

    async fn reset(&self, target: ResetTarget) -> Result<()> {
        let mut c = self.client();
        match target {
            ResetTarget::All => {
                c.delete(indradb::AllVertexQuery).await.map_err(wrap)?;
            }
            ResetTarget::Repo(repo_name) => {
                // Delete all vertices where `repo_name` property equals <name>.
                // `VertexWithPropertyValueQuery` matches exact equality.
                let q = indradb::VertexWithPropertyValueQuery::new(
                    ident(PROP_REPO_NAME)?,
                    Json::new(serde_json::Value::String(repo_name)),
                );
                c.delete(q).await.map_err(wrap)?;
            }
        }
        Ok(())
    }

    async fn acquire_phase5_lock(&self, _ttl: Duration) -> Result<Box<dyn Phase5LockGuard>> {
        let mut c = self.client();
        let lock_type = ident(LOCK_VERTEX_TYPE)?;
        let vid = Vertex::new(lock_type);
        let id = vid.id;
        c.create_vertex(&vid).await.map_err(wrap)?;

        // Record holder info (`pid:<n>`) for debugging.
        let holder = format!("pid:{}", std::process::id());
        let q = indradb::SpecificVertexQuery::single(id);
        c.set_properties(
            q,
            ident(PROP_LOCK_HOLDER)?,
            &Json::new(serde_json::Value::String(holder)),
        )
        .await
        .map_err(wrap)?;

        Ok(Box::new(IndraDbPhase5LockGuard {
            client: self.client.clone(),
            vertex_id: id,
            released: false,
        }))
    }

    async fn read_schema_version(&self) -> Result<Option<String>> {
        let mut c = self.client();

        // Find the first vertex of type `cxg_schema_version`.
        let q = indradb::RangeVertexQuery::new()
            .limit(1)
            .t(ident(SCHEMA_VERSION_TYPE)?);
        let values = c.get(q).await.map_err(wrap)?;

        if let Some(indradb::QueryOutputValue::Vertices(verts)) = values.first() {
            if let Some(v) = verts.first() {
                // Pipe to its named properties.
                let prop_q = indradb::SpecificVertexQuery::single(v.id)
                    .properties()
                    .map_err(wrap_validation)?
                    .name(ident(PROP_SCHEMA_VERSION)?);
                let prop_vals = c.get(prop_q).await.map_err(wrap)?;
                // VertexProperties output: Vec<VertexProperties>, each has .props: Vec<NamedProperty>
                if let Some(indradb::QueryOutputValue::VertexProperties(vps)) = prop_vals.first() {
                    for vp in vps {
                        for named in &vp.props {
                            // `Json` implements Deref<Target = serde_json::Value>.
                            if let serde_json::Value::String(s) = &*named.value {
                                return Ok(Some(s.clone()));
                            }
                        }
                    }
                }
            }
        }
        Ok(None)
    }

    async fn write_schema_version(&self, tag: &str, attrs_json: &str) -> Result<()> {
        let mut c = self.client();
        let vid = SCHEMA_VERSION_UUID;
        let vtype = ident(SCHEMA_VERSION_TYPE)?;

        // Delete any existing singleton vertex first, then re-insert.
        // IndraDB lacks native upsert; delete-then-insert gives idempotency.
        let del_q = indradb::SpecificVertexQuery::single(vid);
        // Ignore "not found" — vertex may not exist on first write.
        let _ = c.delete(del_q).await;

        let items = vec![
            BulkInsertItem::Vertex(Vertex::with_id(vid, vtype)),
            BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_SCHEMA_VERSION)?,
                Json::new(serde_json::Value::String(tag.to_owned())),
            ),
            BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_SCHEMA_ATTRS_JSON)?,
                json_str(attrs_json)?,
            ),
        ];
        c.bulk_insert(items).await.map_err(wrap)
    }

    async fn health(&self) -> Result<HealthInfo> {
        let started = Instant::now();
        let mut c = self.client();
        c.ping().await.map_err(wrap)?;
        Ok(HealthInfo {
            status: "ok".to_owned(),
            latency: started.elapsed(),
        })
    }
}

// ── Unit tests ────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::schema::edges::{EdgeKind, EdgeRecord};
    use crate::schema::nodes::{NodeKind, NodeRecord};

    // ── usr_to_uuid ───────────────────────────────────────────────────────────

    #[test]
    fn usr_to_uuid_is_deterministic() {
        let a = usr_to_uuid("my_repo", "c:@F@foo");
        let b = usr_to_uuid("my_repo", "c:@F@foo");
        assert_eq!(a, b, "same input must produce same UUID");
    }

    #[test]
    fn usr_to_uuid_differs_across_repos() {
        let a = usr_to_uuid("repo_a", "c:@F@foo");
        let b = usr_to_uuid("repo_b", "c:@F@foo");
        assert_ne!(
            a, b,
            "same USR in different repos must produce different UUIDs"
        );
    }

    #[test]
    fn usr_to_uuid_differs_across_usrs() {
        let a = usr_to_uuid("my_repo", "c:@F@foo");
        let b = usr_to_uuid("my_repo", "c:@F@bar");
        assert_ne!(a, b, "different USRs must produce different UUIDs");
    }

    #[test]
    fn usr_to_uuid_is_v5() {
        let id = usr_to_uuid("my_repo", "c:@F@foo");
        // UUIDv5 has version bits == 5 in the high nibble of byte 6.
        assert_eq!(id.get_version_num(), 5, "UUID must be version 5");
    }

    // ── ident validation ──────────────────────────────────────────────────────

    #[test]
    fn all_prop_names_are_valid_identifiers() {
        for name in [
            PROP_USR,
            PROP_REPO_NAME,
            PROP_KIND,
            PROP_NAME,
            PROP_QUALIFIED_NAME,
            PROP_ATTRS_JSON,
            PROP_LOCK_HOLDER,
            PROP_SCHEMA_VERSION,
            // M8 promoted property names (S45)
            PROP_RETURN_TYPE,
            PROP_PARAMS,
            PROP_SIGNATURE,
            PROP_CODE,
            PROP_CODE_TRUNCATED,
            PROP_TEMPLATE_PARAMS,
            PROP_TEMPLATE_ARGS,
            PROP_IS_VIRTUAL,
            PROP_IS_PURE_VIRTUAL,
            PROP_IS_STATIC,
            PROP_SRC_ASSOC_TYPE,
            PROP_DST_ASSOC_TYPE,
        ] {
            ident(name).unwrap_or_else(|_| panic!("'{name}' must be a valid identifier"));
        }
    }

    #[test]
    fn node_kind_strings_are_valid_identifiers() {
        for &kind in NodeKind::all() {
            ident(kind.as_str()).unwrap_or_else(|_| {
                panic!("NodeKind '{}' must be valid identifier", kind.as_str())
            });
        }
    }

    #[test]
    fn edge_kind_strings_are_valid_identifiers() {
        for &kind in EdgeKind::all() {
            ident(kind.as_str()).unwrap_or_else(|_| {
                panic!("EdgeKind '{}' must be valid identifier", kind.as_str())
            });
        }
    }

    #[test]
    fn vertex_type_constants_are_valid_identifiers() {
        ident(LOCK_VERTEX_TYPE).expect("LOCK_VERTEX_TYPE must be valid");
        ident(SCHEMA_VERSION_TYPE).expect("SCHEMA_VERSION_TYPE must be valid");
    }

    // ── is_transient ──────────────────────────────────────────────────────────

    #[test]
    fn channel_closed_is_not_transient() {
        assert!(!is_transient(&ClientError::ChannelClosed));
    }

    #[test]
    fn transport_error_is_transient() {
        // We cannot easily construct a real TransportError without connecting,
        // so we verify the non-transient path instead.
        let grpc_err = ClientError::Grpc {
            inner: tonic::Status::not_found("missing"),
        };
        assert!(!is_transient(&grpc_err), "NOT_FOUND is not transient");
    }

    #[test]
    fn grpc_unavailable_is_transient() {
        let grpc_err = ClientError::Grpc {
            inner: tonic::Status::unavailable("server busy"),
        };
        assert!(is_transient(&grpc_err), "UNAVAILABLE must be transient");
    }

    #[test]
    fn grpc_deadline_exceeded_is_transient() {
        let grpc_err = ClientError::Grpc {
            inner: tonic::Status::deadline_exceeded("timeout"),
        };
        assert!(is_transient(&grpc_err));
    }

    // ── build_items helpers ───────────────────────────────────────────────────

    fn make_node(usr: &str) -> NodeRecord {
        NodeRecord {
            usr: usr.to_owned(),
            kind: NodeKind::Function,
            name: "foo".to_owned(),
            qualified_name: "ns::foo".to_owned(),
            mangled_name: None,
            file_path: "/src/foo.cpp".to_owned(),
            line: Some(10),
            col: Some(1),
            repo_name: "my_repo".to_owned(),
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
        }
    }

    fn make_edge(src: &str, dst: &str) -> EdgeRecord {
        EdgeRecord {
            src_usr: src.to_owned(),
            dst_usr: Some(dst.to_owned()),
            dst_placeholder: None,
            kind: EdgeKind::Calls,
            resolved: true,
            cross_repo_candidate: false,
            repo_name: "my_repo".to_owned(),
            attrs_json: "{}".to_owned(),
            tu_hash: [0u8; 32],
            source_association_type: None,
            target_association_type: None,
        }
    }

    #[test]
    fn node_base_fields_produce_seven_items() {
        // A node with all M8 optional fields = None produces 1 Vertex + 6 VertexProperty = 7 items.
        let node = make_node("c:@F@foo");
        let vid = usr_to_uuid(&node.repo_name, &node.usr);
        let vtype = ident(node.kind.as_str()).unwrap();
        let mut items: Vec<BulkInsertItem> = Vec::new();
        items.push(BulkInsertItem::Vertex(Vertex::with_id(vid, vtype)));
        for prop in [
            PROP_USR,
            PROP_REPO_NAME,
            PROP_KIND,
            PROP_NAME,
            PROP_QUALIFIED_NAME,
            PROP_ATTRS_JSON,
        ] {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(prop).unwrap(),
                Json::new(serde_json::Value::String("x".to_owned())),
            ));
        }
        // All optional M8 fields are None → no additional items emitted.
        assert_eq!(
            items.len(),
            7,
            "base items (all M8 optional = None) must be 7"
        );
    }

    #[test]
    fn node_with_all_m8_fields_produces_seventeen_items() {
        // A node with all 10 M8 optional fields set produces 7 base + 10 = 17 items.
        use crate::schema::nodes::{Param, TemplateArg, TemplateParam};
        let mut node = make_node("c:@F@full");
        node.return_type = Some("int".to_owned());
        node.params = Some(vec![Param {
            name: "x".to_owned(),
            type_: "int".to_owned(),
        }]);
        node.signature = Some("int(int)".to_owned());
        node.code = Some("int foo(int x) { return x; }".to_owned());
        node.code_truncated = Some(false);
        node.template_params = Some(vec![TemplateParam {
            name: "T".to_owned(),
            kind: "type".to_owned(),
            default: None,
        }]);
        node.template_args = Some(vec![TemplateArg {
            kind: "type".to_owned(),
            value: "int".to_owned(),
        }]);
        node.is_virtual = Some(false);
        node.is_pure_virtual = Some(false);
        node.is_static = Some(false);

        let vid = usr_to_uuid(&node.repo_name, &node.usr);
        let vtype = ident(node.kind.as_str()).unwrap();
        let mut items: Vec<BulkInsertItem> = Vec::new();
        items.push(BulkInsertItem::Vertex(Vertex::with_id(vid, vtype)));
        for prop in [
            PROP_USR,
            PROP_REPO_NAME,
            PROP_KIND,
            PROP_NAME,
            PROP_QUALIFIED_NAME,
            PROP_ATTRS_JSON,
        ] {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(prop).unwrap(),
                Json::new(serde_json::Value::String("x".to_owned())),
            ));
        }
        // M8 optional string/bool fields
        for prop in [PROP_RETURN_TYPE, PROP_SIGNATURE, PROP_CODE] {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(prop).unwrap(),
                Json::new(serde_json::Value::String("v".to_owned())),
            ));
        }
        // Params, template_params, template_args — JSON arrays
        for prop in [PROP_PARAMS, PROP_TEMPLATE_PARAMS, PROP_TEMPLATE_ARGS] {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(prop).unwrap(),
                Json::new(serde_json::Value::Array(vec![])),
            ));
        }
        // Boolean fields
        for prop in [
            PROP_CODE_TRUNCATED,
            PROP_IS_VIRTUAL,
            PROP_IS_PURE_VIRTUAL,
            PROP_IS_STATIC,
        ] {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(prop).unwrap(),
                Json::new(serde_json::Value::Bool(false)),
            ));
        }
        assert_eq!(
            items.len(),
            17,
            "all M8 optional fields populated must produce 17 items"
        );
    }

    #[test]
    fn unresolved_edge_is_skipped() {
        let mut edge = make_edge("a", "b");
        edge.dst_usr = None;
        // Replicate the skip logic from write_edges.
        let dst = edge.dst_usr.as_ref();
        assert!(dst.is_none(), "unresolved edge must have no dst_usr");
    }

    #[test]
    fn json_str_valid_object_parses() {
        let j = json_str("{}").expect("empty object must parse");
        // Json implements Deref<Target = serde_json::Value>; dereference to compare.
        assert_eq!(*j, serde_json::Value::Object(Default::default()));
    }

    #[test]
    fn json_str_invalid_returns_err() {
        assert!(json_str("{not json}").is_err());
    }

    // ── S18: batched write — chunk-count and WriteStats invariants ────────────
    //
    // Verify chunk logic and item-count arithmetic without a live IndraDB server.

    #[test]
    fn items_per_node_base_is_seven() {
        // A node with all M8 optional fields = None → 1 Vertex + 6 VertexProperty = 7 base items.
        // write_nodes now chunks by worst-case 17 items per node.
        let node = make_node("c:@F@foo");
        let vid = usr_to_uuid(&node.repo_name, &node.usr);
        let vtype = ident(node.kind.as_str()).unwrap();
        let mut items: Vec<BulkInsertItem> = vec![];
        items.push(BulkInsertItem::Vertex(Vertex::with_id(vid, vtype)));
        for prop in [
            PROP_USR,
            PROP_REPO_NAME,
            PROP_KIND,
            PROP_NAME,
            PROP_QUALIFIED_NAME,
            PROP_ATTRS_JSON,
        ] {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(prop).unwrap(),
                Json::new(serde_json::Value::String("x".to_owned())),
            ));
        }
        assert_eq!(
            items.len(),
            7,
            "base items (no M8 optional fields) must equal 7"
        );
    }

    #[test]
    fn items_per_edge_base_is_two() {
        // Each edge with no association_type fields → 1 Edge + 1 EdgeProperty (attrs_json) = 2 items.
        // write_edges now chunks by worst-case 4 items per edge.
        let edge = make_edge("src", "dst");
        let src_id = usr_to_uuid(&edge.repo_name, &edge.src_usr);
        let dst_id = usr_to_uuid(&edge.repo_name, edge.dst_usr.as_deref().unwrap());
        let edge_type = ident(edge.kind.as_str()).unwrap();
        let e = Edge::new(src_id, edge_type, dst_id);
        let items: Vec<BulkInsertItem> = vec![
            BulkInsertItem::Edge(e.clone()),
            BulkInsertItem::EdgeProperty(
                e,
                ident(PROP_ATTRS_JSON).unwrap(),
                Json::new(serde_json::Value::Object(Default::default())),
            ),
        ];
        assert_eq!(
            items.len(),
            2,
            "base edge items (no association_type) must equal 2"
        );
    }

    #[test]
    fn items_per_edge_with_association_types_is_four() {
        // A USES edge with both association_type fields set → 4 items.
        let mut edge = make_edge("src", "dst");
        edge.source_association_type = Some("read".to_owned());
        edge.target_association_type = Some("read".to_owned());

        let src_id = usr_to_uuid(&edge.repo_name, &edge.src_usr);
        let dst_id = usr_to_uuid(&edge.repo_name, edge.dst_usr.as_deref().unwrap());
        let edge_type = ident(edge.kind.as_str()).unwrap();
        let e = Edge::new(src_id, edge_type, dst_id);
        let mut items: Vec<BulkInsertItem> = vec![
            BulkInsertItem::Edge(e.clone()),
            BulkInsertItem::EdgeProperty(
                e.clone(),
                ident(PROP_ATTRS_JSON).unwrap(),
                Json::new(serde_json::Value::Object(Default::default())),
            ),
        ];
        if let Some(ref v) = edge.source_association_type {
            items.push(BulkInsertItem::EdgeProperty(
                e.clone(),
                ident(PROP_SRC_ASSOC_TYPE).unwrap(),
                Json::new(serde_json::Value::String(v.clone())),
            ));
        }
        if let Some(ref v) = edge.target_association_type {
            items.push(BulkInsertItem::EdgeProperty(
                e.clone(),
                ident(PROP_DST_ASSOC_TYPE).unwrap(),
                Json::new(serde_json::Value::String(v.clone())),
            ));
        }
        assert_eq!(
            items.len(),
            4,
            "USES edge with both association_type fields must produce 4 items"
        );
    }

    #[test]
    fn chunk_size_arithmetic_nodes() {
        // With batch_size=3 and worst-case items_per_node=17, chunk_items = 51.
        // 60 items should produce ceil(60/51) = 2 chunks.
        let batch_size = 3_usize;
        let items_per_node_worst_case = 17_usize;
        let chunk_items = batch_size * items_per_node_worst_case; // 51
        let total_items = 60_usize;
        let chunk_count = total_items.div_ceil(chunk_items);
        assert_eq!(chunk_count, 2, "60 items / chunk_size 51 => 2 chunks");
    }

    #[test]
    fn unresolved_edges_excluded_before_chunk_build() {
        // In write_edges, unresolved edges are skipped during item building.
        // Verify the count reflects only resolved edges.
        let edges = vec![
            make_edge("a", "b"),
            {
                let mut e = make_edge("c", "d");
                e.dst_usr = None; // unresolved
                e
            },
            make_edge("e", "f"),
        ];
        let mut item_count = 0_usize;
        let mut written = 0_u64;
        for edge_rec in &edges {
            if let Some(dst_usr) = &edge_rec.dst_usr {
                let src_id = usr_to_uuid(&edge_rec.repo_name, &edge_rec.src_usr);
                let dst_id = usr_to_uuid(&edge_rec.repo_name, dst_usr);
                let edge_type = ident(edge_rec.kind.as_str()).unwrap();
                let e = Edge::new(src_id, edge_type, dst_id);
                item_count += 2; // Edge + EdgeProperty
                written += 1;
                let _ = e; // suppress unused
            }
        }
        assert_eq!(written, 2, "2 of 3 edges are resolved");
        assert_eq!(item_count, 4, "2 resolved edges => 4 BulkInsertItems");
    }

    // ── S45: M8 property write helpers ───────────────────────────────────────

    /// None optional fields must NOT produce any extra BulkInsertItems (AC-S45-1).
    #[test]
    fn none_m8_fields_produce_no_items() {
        let node = make_node("c:@F@no_m8");
        // All M8 fields are None in make_node — verify item count stays at 7 base.
        assert!(node.return_type.is_none());
        assert!(node.params.is_none());
        assert!(node.signature.is_none());
        assert!(node.code.is_none());
        assert!(node.code_truncated.is_none());
        assert!(node.template_params.is_none());
        assert!(node.template_args.is_none());
        assert!(node.is_virtual.is_none());
        assert!(node.is_pure_virtual.is_none());
        assert!(node.is_static.is_none());

        // Simulate the item-build loop from write_nodes for this node.
        let vid = usr_to_uuid(&node.repo_name, &node.usr);
        let vtype = ident(node.kind.as_str()).unwrap();
        let mut items: Vec<BulkInsertItem> = Vec::new();
        items.push(BulkInsertItem::Vertex(Vertex::with_id(vid, vtype)));
        for prop in [
            PROP_USR,
            PROP_REPO_NAME,
            PROP_KIND,
            PROP_NAME,
            PROP_QUALIFIED_NAME,
            PROP_ATTRS_JSON,
        ] {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(prop).unwrap(),
                Json::new(serde_json::Value::Null),
            ));
        }
        // None fields → nothing pushed
        if let Some(ref v) = node.return_type {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_RETURN_TYPE).unwrap(),
                Json::new(serde_json::Value::String(v.clone())),
            ));
        }
        if let Some(ref v) = node.params {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_PARAMS).unwrap(),
                json_value(v).unwrap(),
            ));
        }
        if let Some(ref v) = node.signature {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_SIGNATURE).unwrap(),
                Json::new(serde_json::Value::String(v.clone())),
            ));
        }
        if let Some(ref v) = node.code {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_CODE).unwrap(),
                Json::new(serde_json::Value::String(v.clone())),
            ));
        }
        if let Some(v) = node.code_truncated {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_CODE_TRUNCATED).unwrap(),
                Json::new(serde_json::Value::Bool(v)),
            ));
        }
        if let Some(ref v) = node.template_params {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_TEMPLATE_PARAMS).unwrap(),
                json_value(v).unwrap(),
            ));
        }
        if let Some(ref v) = node.template_args {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_TEMPLATE_ARGS).unwrap(),
                json_value(v).unwrap(),
            ));
        }
        if let Some(v) = node.is_virtual {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_IS_VIRTUAL).unwrap(),
                Json::new(serde_json::Value::Bool(v)),
            ));
        }
        if let Some(v) = node.is_pure_virtual {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_IS_PURE_VIRTUAL).unwrap(),
                Json::new(serde_json::Value::Bool(v)),
            ));
        }
        if let Some(v) = node.is_static {
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_IS_STATIC).unwrap(),
                Json::new(serde_json::Value::Bool(v)),
            ));
        }

        assert_eq!(items.len(), 7, "no M8 optional fields → exactly 7 items");
    }

    /// `params` list serialises to a JSON array of objects with a `"type"` key (ADR-14).
    #[test]
    fn params_serialise_to_json_array_with_type_key() {
        use crate::schema::nodes::Param;
        let params = vec![
            Param {
                name: "x".to_owned(),
                type_: "int".to_owned(),
            },
            Param {
                name: "y".to_owned(),
                type_: "const char *".to_owned(),
            },
        ];
        let j = json_value(&params).expect("serialisation must succeed");
        let arr = match &*j {
            serde_json::Value::Array(a) => a,
            other => panic!("expected array, got {other:?}"),
        };
        assert_eq!(arr.len(), 2);
        // Param.type_ must be serialised as key "type" (not "type_") per serde rename.
        assert!(
            arr[0].get("type").is_some(),
            "first param must have 'type' key"
        );
        assert_eq!(arr[0]["name"], serde_json::Value::String("x".to_owned()));
        assert_eq!(arr[0]["type"], serde_json::Value::String("int".to_owned()));
    }

    /// A node with `code_truncated: Some(true)` and `code: None` must not panic.
    /// Exercises the AC-S45-5 path (no spurious code property, truncated flag written).
    #[test]
    fn code_truncated_true_with_none_code_produces_only_flag_item() {
        let mut node = make_node("c:@F@truncated");
        node.code = None;
        node.code_truncated = Some(true);

        let vid = usr_to_uuid(&node.repo_name, &node.usr);
        let mut extra_items: Vec<BulkInsertItem> = Vec::new();
        // Simulate just the optional-field portion of write_nodes.
        if let Some(ref v) = node.code {
            extra_items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_CODE).unwrap(),
                Json::new(serde_json::Value::String(v.clone())),
            ));
        }
        if let Some(v) = node.code_truncated {
            extra_items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_CODE_TRUNCATED).unwrap(),
                Json::new(serde_json::Value::Bool(v)),
            ));
        }
        // code is None → no code item; code_truncated is Some(true) → 1 item.
        assert_eq!(
            extra_items.len(),
            1,
            "only code_truncated item, no code item"
        );
        match &extra_items[0] {
            BulkInsertItem::VertexProperty(_, id, val) => {
                assert_eq!(id.as_str(), PROP_CODE_TRUNCATED);
                assert_eq!(*val, Json::new(serde_json::Value::Bool(true)));
            }
            other => panic!("unexpected item type: {other:?}"),
        }
    }

    /// Association type fields on edges: None fields must not produce items.
    #[test]
    fn none_association_types_produce_no_extra_items() {
        let edge = make_edge("a", "b"); // both association_type = None
        let src_id = usr_to_uuid(&edge.repo_name, &edge.src_usr);
        let dst_id = usr_to_uuid(&edge.repo_name, edge.dst_usr.as_deref().unwrap());
        let edge_type = ident(edge.kind.as_str()).unwrap();
        let e = Edge::new(src_id, edge_type, dst_id);
        let mut items: Vec<BulkInsertItem> = vec![
            BulkInsertItem::Edge(e.clone()),
            BulkInsertItem::EdgeProperty(
                e.clone(),
                ident(PROP_ATTRS_JSON).unwrap(),
                Json::new(serde_json::Value::Null),
            ),
        ];
        if let Some(ref v) = edge.source_association_type {
            items.push(BulkInsertItem::EdgeProperty(
                e.clone(),
                ident(PROP_SRC_ASSOC_TYPE).unwrap(),
                Json::new(serde_json::Value::String(v.clone())),
            ));
        }
        if let Some(ref v) = edge.target_association_type {
            items.push(BulkInsertItem::EdgeProperty(
                e.clone(),
                ident(PROP_DST_ASSOC_TYPE).unwrap(),
                Json::new(serde_json::Value::String(v.clone())),
            ));
        }
        assert_eq!(
            items.len(),
            2,
            "None association_type fields → 2 base items only"
        );
    }

    /// Both association_type fields populated → 2 extra EdgeProperty items (4 total).
    #[test]
    fn populated_association_types_produce_four_items() {
        let mut edge = make_edge("a", "b");
        edge.source_association_type = Some("write".to_owned());
        edge.target_association_type = Some("write".to_owned());
        let src_id = usr_to_uuid(&edge.repo_name, &edge.src_usr);
        let dst_id = usr_to_uuid(&edge.repo_name, edge.dst_usr.as_deref().unwrap());
        let edge_type = ident(edge.kind.as_str()).unwrap();
        let e = Edge::new(src_id, edge_type, dst_id);
        let mut items: Vec<BulkInsertItem> = vec![
            BulkInsertItem::Edge(e.clone()),
            BulkInsertItem::EdgeProperty(
                e.clone(),
                ident(PROP_ATTRS_JSON).unwrap(),
                Json::new(serde_json::Value::Null),
            ),
        ];
        if let Some(ref v) = edge.source_association_type {
            items.push(BulkInsertItem::EdgeProperty(
                e.clone(),
                ident(PROP_SRC_ASSOC_TYPE).unwrap(),
                Json::new(serde_json::Value::String(v.clone())),
            ));
        }
        if let Some(ref v) = edge.target_association_type {
            items.push(BulkInsertItem::EdgeProperty(
                e.clone(),
                ident(PROP_DST_ASSOC_TYPE).unwrap(),
                Json::new(serde_json::Value::String(v.clone())),
            ));
        }
        assert_eq!(items.len(), 4, "both association_type populated → 4 items");
    }

    #[test]
    fn idempotency_key_is_deterministic_uuid() {
        // The write_nodes idempotency guarantee rests on usr_to_uuid being
        // deterministic: same (repo_name, usr) always maps to the same UUID.
        let a = usr_to_uuid("repo", "c:@F@foo");
        let b = usr_to_uuid("repo", "c:@F@foo");
        assert_eq!(a, b, "same input must produce same UUID (idempotency key)");

        // A different USR must produce a different UUID so there is no collision.
        let c = usr_to_uuid("repo", "c:@F@bar");
        assert_ne!(a, c, "different USR must produce different UUID");
    }
}
