//! Process-memory tuning helpers (Issue 0002).
//!
//! Currently houses the glibc allocator-arena controls used to bound Phase 1
//! RSS growth.  See [`glibc`] for the platform-gated details.

pub mod glibc;
