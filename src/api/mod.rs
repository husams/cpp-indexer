//! REST control plane for `cxg-daemon`.
//!
//! Sub-modules:
//! - [`auth`]    — bearer-token middleware (AC-M7-6)
//! - [`jobs`]    — in-process job queue + state machine (AC-M7-1, AC-M7-2)
//! - [`problem`] — RFC-7807 `application/problem+json` (AC-M7-7)
//! - [`routes`]  — axum route definitions (AC-M7-1..8)
//!
//! The `metrics` module (AC-M7-17..19) and `GET /v1/reset` (AC-M7-9..11)
//! are implemented in S35 and S34 respectively.
//!
//! ADR-5.

pub mod auth;
pub mod jobs;
pub mod problem;
pub mod routes;
