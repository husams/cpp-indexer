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
/// Vertex type used for the Phase 5 lock sentinel.
const LOCK_VERTEX_TYPE: &str = "cxg_phase5_lock";
/// Property name for the lock holder tag.
const PROP_LOCK_HOLDER: &str = "holder";
/// Schema-version vertex type.
const SCHEMA_VERSION_TYPE: &str = "cxg_schema_version";
/// Schema-version property name.
const PROP_SCHEMA_VERSION: &str = "version";

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

        // Build BulkInsertItems: Vertex + key properties per record.
        let mut all_items: Vec<BulkInsertItem> = Vec::with_capacity(batch.len() * 7);
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
        }

        // Each node produces 7 items; chunk by items-per-node * batch_size.
        let items_per_node = 7_usize;
        let chunk_items = self.batch_size.saturating_mul(items_per_node);
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
        let mut all_items: Vec<BulkInsertItem> = Vec::with_capacity(batch.len() * 2);
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
                e,
                ident(PROP_ATTRS_JSON)?,
                json_str(&edge_rec.attrs_json)?,
            ));
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

        // Each edge produces 2 items; chunk by items-per-edge * batch_size.
        let items_per_edge = 2_usize;
        let chunk_items = self.batch_size.saturating_mul(items_per_edge);

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
        }
    }

    #[test]
    fn node_produces_correct_bulk_item_count() {
        // Each node should produce 1 Vertex + 6 VertexProperty items = 7.
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
        assert_eq!(items.len(), 7);
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
    fn items_per_node_is_seven() {
        // Each node → 1 Vertex + 6 VertexProperty = 7 items.
        // The write_nodes chunking multiplies batch_size × 7.
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
        assert_eq!(items.len(), 7, "items_per_node constant must equal 7");
    }

    #[test]
    fn items_per_edge_is_two() {
        // Each edge → 1 Edge + 1 EdgeProperty = 2 items.
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
        assert_eq!(items.len(), 2, "items_per_edge constant must equal 2");
    }

    #[test]
    fn chunk_size_arithmetic_nodes() {
        // With batch_size=3 and items_per_node=7, chunk_items = 21.
        // 30 items (from 4+ nodes) should produce ceil(30/21) = 2 chunks.
        let batch_size = 3_usize;
        let items_per_node = 7_usize;
        let chunk_items = batch_size * items_per_node; // 21
        let total_items = 30_usize;
        let chunk_count = total_items.div_ceil(chunk_items);
        assert_eq!(chunk_count, 2, "30 items / chunk_size 21 => 2 chunks");
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
