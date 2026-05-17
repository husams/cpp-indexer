//! REST control plane for `cxg-daemon`.
//!
//! Sub-modules:
//! - [`auth`]    — bearer-token middleware (AC-M7-6)
//! - [`jobs`]    — in-process job queue + state machine (AC-M7-1, AC-M7-2)
//! - [`problem`] — RFC-7807 `application/problem+json` (AC-M7-7)
//! - [`reset`]   — POST /v1/reset with confirm_token (AC-M7-9..11)
//! - [`routes`]  — axum route definitions (AC-M7-1..8)
//!
//! The `metrics` module (AC-M7-17..19) lands with S35.
//!
//! ADR-5.

pub mod auth;
pub mod jobs;
pub mod problem;
pub mod reset;
pub mod routes;
