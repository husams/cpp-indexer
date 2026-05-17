/// Phase 3 / Phase 5 — USR-map resolution and cross-repo edge materialisation.
///
/// Sub-modules:
/// - `per_repo`: single-repo resolver (`HashMap<Usr, NodeMeta>` → `final-edges.parquet`).
/// - `spill`: RocksDB spill-to-disk when the USR map exceeds 8 GiB threshold (S20, AC-M3-12).
/// - `cross_repo`: Phase 5 `EXTERNAL_REF` materialisation (S23, AC-M4-4, AC-M4-6, AC-M6-7).
pub mod cross_repo;
pub mod per_repo;
pub mod spill;
