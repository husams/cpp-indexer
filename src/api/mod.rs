//! REST control plane for `cxg-daemon`.
//!
//! Sub-modules:
//! - [`auth`]    — bearer-token middleware (AC-M7-6)
//! - [`jobs`]    — in-process job queue + state machine (AC-M7-1, AC-M7-2)
//! - [`metrics`] — Prometheus exposition (AC-M7-17..19)
//! - [`problem`] — RFC-7807 `application/problem+json` (AC-M7-7)
//! - [`reset`]   — POST /v1/reset with confirm_token (AC-M7-9..11)
//! - [`routes`]  — axum route definitions (AC-M7-1..8)
//!
//! ADR-5.

pub mod auth;
pub mod jobs;
pub mod metrics;
pub mod problem;
pub mod reset;
pub mod routes;
