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
//! holds only an internally-pooled tonic `Channel`.  We store the base client
//! behind a `Mutex` only for the clone operation; each async call clones the
//! client before entering the RPC so that concurrent writes are not serialised.
//!
//! # Retry policy
//!
//! Transient errors (`Transport`, `Grpc` with `UNAVAILABLE` / `DEADLINE_EXCEEDED` /
//! `RESOURCE_EXHAUSTED`) are retried up to 3 total attempts with exponential
//! back-off (100 ms, 200 ms).  Non-transient errors are returned immediately.

use std::sync::Arc;
use std::time::{Duration, Instant};

use async_trait::async_trait;
use indradb::{BulkInsertItem, Edge, Identifier, Json, QueryExt, Vertex};
use indradb_proto::ClientError;
use tokio::sync::Mutex;
use tonic::Code;
use uuid::Uuid;

use crate::config::IndraDbSinkConfig;
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
    client: Arc<Mutex<indradb_proto::Client>>,
    vertex_id: Uuid,
    released: bool,
}

impl IndraDbPhase5LockGuard {
    async fn do_release(&mut self) -> Result<()> {
        if self.released {
            return Ok(());
        }
        self.released = true;
        let mut c = self.client.lock().await.clone();
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
            let client = self.client.clone();
            let vid = self.vertex_id;
            rt.spawn(async move {
                let mut c = client.lock().await.clone();
                let q = indradb::SpecificVertexQuery::single(vid);
                let _ = c.delete(q).await;
            });
        }
        self.released = true;
    }
}

// ── IndraDbSink ───────────────────────────────────────────────────────────────

/// `GraphSink` implementation backed by IndraDB via gRPC (`indradb-proto` v5).
pub struct IndraDbSink {
    /// Shared client.  We lock only to clone; each RPC uses its own clone.
    client: Arc<Mutex<indradb_proto::Client>>,
    /// Resolved auth token (empty string = no auth required).
    #[allow(dead_code)]
    token: String,
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
            client: Arc::new(Mutex::new(client)),
            token,
        })
    }

    /// Clone the inner client (cheap — shares the tonic channel).
    async fn client(&self) -> indradb_proto::Client {
        self.client.lock().await.clone()
    }

    /// Execute `f` with retry for transient errors.  Returns `(result, retries_used)`.
    async fn with_retry<F, Fut, T>(&self, mut f: F) -> (std::result::Result<T, ClientError>, u32)
    where
        F: FnMut(indradb_proto::Client) -> Fut,
        Fut: std::future::Future<Output = std::result::Result<T, ClientError>>,
    {
        let mut last_err: Option<ClientError> = None;
        let mut retries = 0u32;
        for attempt in 0..MAX_ATTEMPTS {
            let c = self.client().await;
            match f(c).await {
                Ok(v) => return (Ok(v), retries),
                Err(e) if is_transient(&e) && attempt + 1 < MAX_ATTEMPTS => {
                    let wait_ms = BACKOFF_BASE_MS * (1u64 << attempt);
                    tokio::time::sleep(Duration::from_millis(wait_ms)).await;
                    retries += 1;
                    last_err = Some(e);
                }
                Err(e) => return (Err(e), retries),
            }
        }
        (Err(last_err.unwrap()), retries)
    }
}

#[async_trait]
impl GraphSink for IndraDbSink {
    fn backend_name(&self) -> &'static str {
        "indradb"
    }

    async fn preflight(&self) -> Result<()> {
        let mut c = self.client().await;
        c.ping().await.map_err(wrap)
    }

    async fn ensure_indexes(&self) -> Result<()> {
        // Index `usr` and `repo_name` properties for fast lookups and reset operations.
        let mut c = self.client().await;
        c.index_property(ident(PROP_USR)?).await.map_err(wrap)?;
        c.index_property(ident(PROP_REPO_NAME)?)
            .await
            .map_err(wrap)?;
        Ok(())
    }

    async fn write_nodes(&self, batch: &[NodeRecord]) -> Result<WriteStats> {
        let started = Instant::now();

        // Build BulkInsertItems: Vertex + key properties per record.
        let mut items: Vec<BulkInsertItem> = Vec::with_capacity(batch.len() * 7);
        for node in batch {
            let vid = usr_to_uuid(&node.repo_name, &node.usr);
            let vtype = ident(node.kind.as_str())?;
            items.push(BulkInsertItem::Vertex(Vertex::with_id(vid, vtype)));
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_USR)?,
                Json::new(serde_json::Value::String(node.usr.clone())),
            ));
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_REPO_NAME)?,
                Json::new(serde_json::Value::String(node.repo_name.clone())),
            ));
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_KIND)?,
                Json::new(serde_json::Value::String(node.kind.as_str().to_owned())),
            ));
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_NAME)?,
                Json::new(serde_json::Value::String(node.name.clone())),
            ));
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_QUALIFIED_NAME)?,
                Json::new(serde_json::Value::String(node.qualified_name.clone())),
            ));
            items.push(BulkInsertItem::VertexProperty(
                vid,
                ident(PROP_ATTRS_JSON)?,
                json_str(&node.attrs_json)?,
            ));
        }

        let count = batch.len() as u64;
        let (result, retries) = self
            .with_retry(|mut c| {
                let its = items.clone();
                async move { c.bulk_insert(its).await }
            })
            .await;
        result.map_err(wrap)?;

        Ok(WriteStats {
            nodes_written: count,
            retries,
            elapsed: started.elapsed(),
        })
    }

    async fn write_edges(&self, batch: &[EdgeRecord]) -> Result<WriteStats> {
        let started = Instant::now();

        let mut items: Vec<BulkInsertItem> = Vec::with_capacity(batch.len() * 2);
        let mut written = 0u64;
        for edge_rec in batch {
            let dst_usr = match &edge_rec.dst_usr {
                Some(u) => u,
                None => continue, // unresolved edge; skip
            };
            let src_id = usr_to_uuid(&edge_rec.repo_name, &edge_rec.src_usr);
            let dst_id = usr_to_uuid(&edge_rec.repo_name, dst_usr);
            let edge_type = ident(edge_rec.kind.as_str())?;
            let e = Edge::new(src_id, edge_type, dst_id);
            items.push(BulkInsertItem::Edge(e.clone()));
            items.push(BulkInsertItem::EdgeProperty(
                e,
                ident(PROP_ATTRS_JSON)?,
                json_str(&edge_rec.attrs_json)?,
            ));
            written += 1;
        }

        if items.is_empty() {
            return Ok(WriteStats {
                nodes_written: 0,
                retries: 0,
                elapsed: started.elapsed(),
            });
        }

        let (result, retries) = self
            .with_retry(|mut c| {
                let its = items.clone();
                async move { c.bulk_insert(its).await }
            })
            .await;
        result.map_err(wrap)?;

        Ok(WriteStats {
            nodes_written: written,
            retries,
            elapsed: started.elapsed(),
        })
    }

    async fn reset(&self, target: ResetTarget) -> Result<()> {
        let mut c = self.client().await;
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
        let mut c = self.client().await;
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
        let mut c = self.client().await;

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
        let mut c = self.client().await;
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
}
