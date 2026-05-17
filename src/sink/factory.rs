//! Sink factory — runtime dispatch from `[sink].backend` to a `dyn GraphSink`.
//!
//! S08 provides the trait + `MockSink` impl.  S12 adds the `IndraDbSink` impl.
//! The Neo4j sink arrives in a later story (S11).  Until that lands, `create`
//! returns `Error::Config` for `"neo4j"` so that the factory contract is
//! testable today without a running Neo4j instance.

use std::sync::Arc;

use crate::config::{env as cfg_env, SinkConfig};
use crate::error::{Error, Result};
use crate::sink::indradb::IndraDbSink;
use crate::sink::GraphSink;

/// Create a `Arc<dyn GraphSink>` from the resolved sink configuration.
///
/// # Errors
///
/// - Returns `Error::Config` for an unknown `backend` string.
/// - Returns `Error::Config` for `"neo4j"` until that story-milestone implementation is merged.
/// - Returns `Error::Config` when the required `[sink.indradb]` section is missing.
/// - Returns `Error::Sink` when the IndraDB endpoint cannot be reached.
pub async fn create(config: &SinkConfig) -> Result<Arc<dyn GraphSink>> {
    match config.backend.as_str() {
        "neo4j" => Err(Error::Config {
            field: "sink.backend".to_owned(),
            detail: "neo4j sink is not yet implemented (target: S11)".to_owned(),
        }),
        "indradb" => {
            let indradb_cfg = config.indradb.as_ref().ok_or_else(|| Error::Config {
                field: "sink.indradb".to_owned(),
                detail: "backend = \"indradb\" requires a [sink.indradb] section".to_owned(),
            })?;
            // Resolve optional token (empty string when token_env is absent).
            let token = cfg_env::resolve_indradb_token_opt(config)?;
            let sink = IndraDbSink::new(indradb_cfg, token).await?;
            Ok(Arc::new(sink))
        }
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

    #[tokio::test]
    async fn neo4j_backend_returns_config_error() {
        // `Arc<dyn GraphSink>` does not implement `Debug`, so use `match` instead of
        // `expect_err` (which requires `T: Debug` for its panic message).
        match create(&sink_config("neo4j")).await {
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

    #[tokio::test]
    async fn indradb_missing_section_returns_config_error() {
        // When `backend = "indradb"` but [sink.indradb] section is absent, we get
        // Error::Config naming the missing section.
        match create(&sink_config("indradb")).await {
            Ok(_) => panic!("indradb without config section must error"),
            Err(Error::Config { field, detail }) => {
                assert!(
                    field.contains("indradb"),
                    "field must name the missing section; got: {field}"
                );
                assert!(
                    detail.contains("indradb"),
                    "detail must mention indradb; got: {detail}"
                );
            }
            Err(other) => panic!("expected Error::Config, got {other:?}"),
        }
    }

    #[tokio::test]
    async fn unknown_backend_returns_config_error() {
        match create(&sink_config("postgres")).await {
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
