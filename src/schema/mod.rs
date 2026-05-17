/// CodexGraph schema types: node/edge enums, Arrow round-trip, and schema versioning.
///
/// Stories covered: S02-schema-base-types (AC-M1-2, AC-M1-3, AC-M1-4).
/// ADRs governing this module: ADR-3 (column layout), ADR-9 (version const).
pub mod arrow;
pub mod edges;
pub mod nodes;
pub mod version;

// Re-export the most commonly used types for callers that `use cpp_indexer::schema::*`
// or `use cpp_indexer::schema::{NodeKind, EdgeKind, …}`.
pub use edges::{EdgeKind, EdgeRecord};
pub use nodes::{NodeKind, NodeRecord};
pub use version::{PARQUET_MAGIC, SCHEMA_VERSION, SCHEMA_VERSION_TAG};
