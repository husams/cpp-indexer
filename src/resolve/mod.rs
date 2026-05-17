/// Phase 3 — in-memory USR-map resolution.
///
/// Sub-modules:
/// - `per_repo`: single-repo resolver (`HashMap<Usr, NodeMeta>` → `final-edges.parquet`).
/// - `spill`: RocksDB spill-to-disk when the USR map exceeds 8 GiB threshold (S20, AC-M3-12).
/// - `cross_repo` (future S??): Phase 5 `EXTERNAL_REF` materialisation.
pub mod per_repo;
pub mod spill;
