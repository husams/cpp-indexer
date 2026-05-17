//! Sink factory — runtime dispatch from `[sink].backend` to a `dyn GraphSink`.
//!
//! S08 provides only the trait + `MockSink` impl; concrete Neo4j and IndraDB
//! implementations arrive in later milestones.  Until those stories land,
//! `create` returns `Error::Config` for `"neo4j"` and `"indradb"` so that the
//! factory contract is testable today without a running database.

use std::sync::Arc;

use crate::config::SinkConfig;
use crate::error::{Error, Result};
use crate::sink::GraphSink;

/// Create a `Arc<dyn GraphSink>` from the resolved sink configuration.
///
/// # Errors
///
/// - Returns `Error::Config` for an unknown `backend` string.
/// - Returns `Error::Config` with a "not yet implemented" detail for `"neo4j"`
///   and `"indradb"` until their story-milestone implementations are merged.
pub fn create(config: &SinkConfig) -> Result<Arc<dyn GraphSink>> {
    match config.backend.as_str() {
        "neo4j" => Err(Error::Config {
            field: "sink.backend".to_owned(),
            detail: "neo4j sink is not yet implemented (target: M3)".to_owned(),
        }),
        "indradb" => Err(Error::Config {
            field: "sink.backend".to_owned(),
            detail: "indradb sink is not yet implemented (target: M4)".to_owned(),
        }),
        other => Err(Error::Config {
            field: "sink.backend".to_owned(),
            detail: format!("unknown backend `{other}`; expected \"neo4j\" or \"indradb\""),
        }),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::SinkConfig;

    fn sink_config(backend: &str) -> SinkConfig {
        SinkConfig {
            backend: backend.to_owned(),
            neo4j: None,
            indradb: None,
        }
    }

    #[test]
    fn neo4j_backend_returns_config_error() {
        // `Arc<dyn GraphSink>` does not implement `Debug`, so use `match` instead of
        // `expect_err` (which requires `T: Debug` for its panic message).
        match create(&sink_config("neo4j")) {
            Ok(_) => panic!("neo4j must error before impl lands"),
            Err(Error::Config { field, detail }) => {
                assert_eq!(field, "sink.backend");
                assert!(
                    detail.contains("neo4j"),
                    "detail must name backend; got: {detail}"
                );
            }
            Err(other) => panic!("expected Error::Config, got {other:?}"),
        }
    }

    #[test]
    fn indradb_backend_returns_config_error() {
        match create(&sink_config("indradb")) {
            Ok(_) => panic!("indradb must error before impl lands"),
            Err(Error::Config { field, detail }) => {
                assert_eq!(field, "sink.backend");
                assert!(
                    detail.contains("indradb"),
                    "detail must name backend; got: {detail}"
                );
            }
            Err(other) => panic!("expected Error::Config, got {other:?}"),
        }
    }

    #[test]
    fn unknown_backend_returns_config_error() {
        match create(&sink_config("postgres")) {
            Ok(_) => panic!("unknown backend must error"),
            Err(Error::Config { field, detail }) => {
                assert_eq!(field, "sink.backend");
                assert!(
                    detail.contains("postgres"),
                    "detail must name bad value; got: {detail}"
                );
            }
            Err(other) => panic!("expected Error::Config, got {other:?}"),
        }
    }
}
