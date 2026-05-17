//! Bearer-token authentication middleware for `cxg-daemon`.
//!
//! Writes (`POST` to `/v1/*`) require an `Authorization: Bearer <token>` header
//! whose value equals the configured token (constant-time compare).  Reads and
//! `/metrics` bypass auth (AC-M7-6, AC-M7-18).
//!
//! ADR-5 §Auth.

use axum::{
    extract::Request,
    http::{Method, StatusCode},
    middleware::Next,
    response::IntoResponse,
};

use super::problem::Problem;

/// Token loaded from the environment at daemon startup.
///
/// Stored in [`AppState`][super::routes::AppState] and passed to the middleware
/// via `axum::middleware::from_fn_with_state`.
#[derive(Clone, Debug)]
pub struct BearerToken(pub String);

/// Middleware: require bearer token on POST requests to `/v1/*`.
///
/// GET requests and any non-`/v1/` path (e.g. `/metrics`) pass through
/// unconditionally.
pub async fn bearer_auth_writes_only(
    axum::extract::State(token): axum::extract::State<BearerToken>,
    req: Request,
    next: Next,
) -> impl IntoResponse {
    let method = req.method();
    let path = req.uri().path();

    // Only enforce auth on mutating methods to /v1/* paths.
    let requires_auth =
        (method == Method::POST || method == Method::DELETE) && path.starts_with("/v1/");

    if requires_auth {
        match extract_bearer(req.headers()) {
            Some(provided) if constant_time_eq(provided.as_bytes(), token.0.as_bytes()) => {
                // Auth OK — proceed.
                next.run(req).await.into_response()
            }
            Some(_) => {
                // Wrong token.
                Problem::new(StatusCode::UNAUTHORIZED, "Unauthorized")
                    .detail("invalid bearer token")
                    .instance(path)
                    .into_response()
            }
            None => {
                // Missing header.
                Problem::new(StatusCode::UNAUTHORIZED, "Unauthorized")
                    .detail("missing Authorization: Bearer header")
                    .instance(path)
                    .into_response()
            }
        }
    } else {
        next.run(req).await.into_response()
    }
}

/// Extract the bearer token string from the `Authorization` header.
fn extract_bearer(headers: &axum::http::HeaderMap) -> Option<&str> {
    let value = headers
        .get(axum::http::header::AUTHORIZATION)?
        .to_str()
        .ok()?;
    value.strip_prefix("Bearer ")
}

/// Constant-time byte-slice comparison.
///
/// Returns `true` iff `a` and `b` have the same length and the same contents.
/// This prevents timing attacks that could reveal token length.
fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff: u8 = 0;
    for (x, y) in a.iter().zip(b.iter()) {
        diff |= x ^ y;
    }
    diff == 0
}

// ── Tests ──────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn constant_time_eq_equal() {
        assert!(constant_time_eq(b"my-secret-token", b"my-secret-token"));
    }

    #[test]
    fn constant_time_eq_different_content() {
        assert!(!constant_time_eq(b"token-a", b"token-b"));
    }

    #[test]
    fn constant_time_eq_different_length() {
        assert!(!constant_time_eq(b"short", b"longer-token"));
    }

    #[test]
    fn extract_bearer_parses_header() {
        let mut headers = axum::http::HeaderMap::new();
        headers.insert(
            axum::http::header::AUTHORIZATION,
            axum::http::HeaderValue::from_static("Bearer my-token"),
        );
        assert_eq!(extract_bearer(&headers), Some("my-token"));
    }

    #[test]
    fn extract_bearer_returns_none_for_missing() {
        let headers = axum::http::HeaderMap::new();
        assert!(extract_bearer(&headers).is_none());
    }

    #[test]
    fn extract_bearer_returns_none_for_basic_auth() {
        let mut headers = axum::http::HeaderMap::new();
        headers.insert(
            axum::http::header::AUTHORIZATION,
            axum::http::HeaderValue::from_static("Basic dXNlcjpwYXNz"),
        );
        assert!(extract_bearer(&headers).is_none());
    }
}
