//! RFC-7807 `application/problem+json` response type.
//!
//! Every error returned by `cxg-daemon` REST handlers is encoded as a
//! [`Problem`].  Handlers return `Result<impl IntoResponse, Problem>`;
//! [`Problem`] implements [`axum::response::IntoResponse`] with
//! `Content-Type: application/problem+json`.
//!
//! AC-M7-7.

use axum::{
    http::{header, HeaderValue, StatusCode},
    response::{IntoResponse, Response},
};
use serde::Serialize;

use crate::error::Error;

// ── Problem struct ─────────────────────────────────────────────────────────────

/// RFC-7807 problem detail object.
#[derive(Debug, Serialize)]
pub struct Problem {
    /// A URI that identifies the problem type.
    #[serde(rename = "type")]
    pub type_uri: String,
    /// A short, human-readable title.
    pub title: String,
    /// HTTP status code (numeric).
    pub status: u16,
    /// Human-readable explanation specific to this occurrence.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub detail: Option<String>,
    /// URI that identifies the specific occurrence (e.g. request path).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub instance: Option<String>,
}

impl Problem {
    /// Construct a new `Problem` with the given status and title.
    pub fn new(status: StatusCode, title: impl Into<String>) -> Self {
        let code = status.as_u16();
        Self {
            type_uri: format!("https://httpstatuses.com/{code}"),
            title: title.into(),
            status: code,
            detail: None,
            instance: None,
        }
    }

    /// Attach a human-readable detail message.
    pub fn detail(mut self, detail: impl Into<String>) -> Self {
        self.detail = Some(detail.into());
        self
    }

    /// Attach an instance URI (typically the request path).
    pub fn instance(mut self, instance: impl Into<String>) -> Self {
        self.instance = Some(instance.into());
        self
    }
}

impl IntoResponse for Problem {
    fn into_response(self) -> Response {
        let status = StatusCode::from_u16(self.status).unwrap_or(StatusCode::INTERNAL_SERVER_ERROR);
        let body = serde_json::to_vec(&self).unwrap_or_else(|_| b"{}".to_vec());
        let mut resp = (status, body).into_response();
        resp.headers_mut().insert(
            header::CONTENT_TYPE,
            HeaderValue::from_static("application/problem+json"),
        );
        resp
    }
}

// ── Error → Problem mapping ────────────────────────────────────────────────────

impl From<Error> for Problem {
    fn from(err: Error) -> Self {
        match &err {
            Error::Api(msg) => {
                Problem::new(StatusCode::BAD_REQUEST, "API error").detail(msg.clone())
            }
            Error::Config { field, detail } => {
                Problem::new(StatusCode::INTERNAL_SERVER_ERROR, "Configuration error")
                    .detail(format!("field `{field}`: {detail}"))
            }
            Error::Workspace(msg) => {
                Problem::new(StatusCode::BAD_REQUEST, "Workspace error").detail(msg.clone())
            }
            Error::Sink { backend, source } => {
                Problem::new(StatusCode::BAD_GATEWAY, "Sink backend error")
                    .detail(format!("{backend}: {source}"))
            }
            _ => Problem::new(StatusCode::INTERNAL_SERVER_ERROR, "Internal server error")
                .detail(err.to_string()),
        }
    }
}

// ── Tests ──────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn problem_serialises_to_rfc7807() {
        let p = Problem::new(StatusCode::BAD_REQUEST, "Invalid input")
            .detail("field `source` is required")
            .instance("/v1/ingest");
        let json = serde_json::to_value(&p).unwrap();
        assert_eq!(json["status"], 400);
        assert_eq!(json["title"], "Invalid input");
        assert!(json["type"].as_str().unwrap().contains("400"));
        assert_eq!(json["detail"], "field `source` is required");
        assert_eq!(json["instance"], "/v1/ingest");
    }

    #[test]
    fn problem_omits_optional_fields_when_absent() {
        let p = Problem::new(StatusCode::NOT_FOUND, "Not found");
        let json = serde_json::to_value(&p).unwrap();
        assert!(json.get("detail").is_none(), "detail must be omitted");
        assert!(json.get("instance").is_none(), "instance must be omitted");
    }

    #[test]
    fn problem_into_response_has_correct_content_type() {
        let p = Problem::new(StatusCode::UNAUTHORIZED, "Unauthorized");
        let resp = p.into_response();
        assert_eq!(resp.status(), StatusCode::UNAUTHORIZED);
        assert_eq!(
            resp.headers().get(header::CONTENT_TYPE).unwrap(),
            "application/problem+json"
        );
    }

    #[test]
    fn error_api_maps_to_400() {
        let err = Error::Api("bad request".to_owned());
        let p = Problem::from(err);
        assert_eq!(p.status, 400);
    }

    #[test]
    fn error_workspace_maps_to_400() {
        let err = Error::Workspace("clone failed".to_owned());
        let p = Problem::from(err);
        assert_eq!(p.status, 400);
    }
}
