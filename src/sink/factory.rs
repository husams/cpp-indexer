//! Sink factory — runtime dispatch from `[sink].backend` to a `dyn GraphSink`.
//!
//! Both Neo4j and IndraDB are real implementations. `MockSink` is available
//! for unit-test use elsewhere but not selectable via runtime config.

use std::sync::Arc;

use crate::config::{self, env as cfg_env, SinkConfig};
use crate::error::{Error, Result};
use crate::sink::indradb::IndraDbSink;
use crate::sink::neo4j::Neo4jSink;
use crate::sink::GraphSink;

/// Create an `Arc<dyn GraphSink>` from the resolved sink configuration.
///
/// # Errors
///
/// - Returns `Error::Config` if a required `[sink.<backend>]` sub-section is missing.
/// - Returns `Error::Sink { backend: "neo4j", .. }` if the Neo4j password env var is
///   unset or the Bolt connection fails.
/// - Returns `Error::Sink { backend: "indradb", .. }` if the IndraDB endpoint
///   cannot be reached.
/// - Returns `Error::Config` for any unknown `backend` string.
pub async fn create(config: &SinkConfig) -> Result<Arc<dyn GraphSink>> {
    match config.backend.as_str() {
        "neo4j" => {
            let neo4j_cfg = config.neo4j.as_ref().ok_or_else(|| Error::Config {
                field: "sink.neo4j".to_owned(),
                detail: "backend = \"neo4j\" requires a [sink.neo4j] section".to_owned(),
            })?;
            let temp_cfg = crate::config::Config {
                repo: None,
                index: None,
                sink: config.clone(),
                api: None,
                workspace: None,
            };
            let password = config::env::resolve_neo4j_password(&temp_cfg)?;
            let sink = Neo4jSink::connect(neo4j_cfg, &password).await?;
            Ok(Arc::new(sink))
        }
        "indradb" => {
            let indradb_cfg = config.indradb.as_ref().ok_or_else(|| Error::Config {
                field: "sink.indradb".to_owned(),
                detail: "backend = \"indradb\" requires a [sink.indradb] section".to_owned(),
            })?;
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
    use crate::config::{Neo4jSinkConfig, SinkConfig};

    fn neo4j_sink_config(password_env: &str) -> SinkConfig {
        SinkConfig {
            backend: "neo4j".to_owned(),
            neo4j: Some(Neo4jSinkConfig {
                uri: "bolt://localhost:7687".to_owned(),
                user: "neo4j".to_owned(),
                password_env: password_env.to_owned(),
                sessions: None,
            }),
            indradb: None,
        }
    }

    fn bare_sink_config(backend: &str) -> SinkConfig {
        SinkConfig {
            backend: backend.to_owned(),
            neo4j: None,
            indradb: None,
        }
    }

    #[tokio::test]
    async fn neo4j_missing_subsection_returns_config_error() {
        match create(&bare_sink_config("neo4j")).await {
            Ok(_) => panic!("must error when [sink.neo4j] is absent"),
            Err(Error::Config { field, .. }) => {
                assert!(field.contains("neo4j"), "field must name neo4j; got: {field}");
            }
            Err(other) => panic!("expected Error::Config, got {other:?}"),
        }
    }

    #[tokio::test]
    async fn neo4j_unset_password_env_returns_sink_error() {
        let var = "__CXG_TEST_FACTORY_PW_UNSET__";
        std::env::remove_var(var);
        let cfg = neo4j_sink_config(var);
        match create(&cfg).await {
            Ok(_) => panic!("must error when password env var is unset"),
            Err(Error::Sink { backend, source }) => {
                assert_eq!(backend, "neo4j");
                assert!(
                    source.to_string().contains(var),
                    "source must name the missing var; got: {source}"
                );
            }
            Err(Error::Config { .. }) => {
                // Acceptable: env resolver may surface Config error
            }
            Err(other) => panic!("expected Error::Sink or Config, got {other:?}"),
        }
    }

    #[tokio::test]
    async fn indradb_missing_section_returns_config_error() {
        match create(&bare_sink_config("indradb")).await {
            Ok(_) => panic!("indradb without config section must error"),
            Err(Error::Config { field, detail }) => {
                assert!(field.contains("indradb"), "field must name indradb; got: {field}");
                assert!(detail.contains("indradb"), "detail must mention indradb; got: {detail}");
            }
            Err(other) => panic!("expected Error::Config, got {other:?}"),
        }
    }

    #[tokio::test]
    async fn unknown_backend_returns_config_error() {
        match create(&bare_sink_config("postgres")).await {
            Ok(_) => panic!("unknown backend must error"),
            Err(Error::Config { field, detail }) => {
                assert_eq!(field, "sink.backend");
                assert!(detail.contains("postgres"), "detail must name bad value; got: {detail}");
            }
            Err(other) => panic!("expected Error::Config, got {other:?}"),
        }
    }
}
