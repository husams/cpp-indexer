//! Environment-variable resolution for config secrets.
//!
//! Each `*_env` field in the config holds the **name** of an environment
//! variable; these functions read the value from the live environment at
//! startup time (never cached between calls). An unset variable is a hard
//! [`Error::Sink`] — the caller must refuse to start.
//!
//! No secret value is ever logged or included in error messages beyond naming
//! the environment variable that was missing.

use std::env;

use crate::error::{Error, Result};

/// Error type for a missing environment variable.
#[derive(Debug, thiserror::Error)]
#[error("environment variable `{var_name}` is not set")]
pub struct EnvVarMissing {
    pub var_name: String,
}

/// Read the Neo4j password from the env var named in `config.sink.neo4j.password_env`.
///
/// # Errors
///
/// Returns [`Error::Sink`] (backend `"neo4j"`) if:
/// - `sink.backend` is not `"neo4j"`.
/// - `sink.neo4j` section is absent.
/// - The env var named by `password_env` is not set.
pub fn resolve_neo4j_password(config: &super::Config) -> Result<String> {
    let neo4j = config.sink.neo4j.as_ref().ok_or_else(|| Error::Config {
        field: "sink.neo4j".to_owned(),
        detail: "backend = \"neo4j\" but [sink.neo4j] section is missing".to_owned(),
    })?;

    let var_name = &neo4j.password_env;
    env::var(var_name).map_err(|_| Error::Sink {
        backend: "neo4j",
        source: Box::new(EnvVarMissing {
            var_name: var_name.clone(),
        }),
    })
}

/// Read the IndraDB auth token from the env var named in
/// `config.sink.indradb.token_env`, if present.
///
/// Returns `Ok(None)` when `token_env` is not set in config (public instance).
/// Returns `Ok(Some(token))` when the env var is present and set.
///
/// # Errors
///
/// Returns [`Error::Sink`] (backend `"indradb"`) if:
/// - `sink.indradb` section is absent.
/// - `token_env` is configured but the env var is not set.
pub fn resolve_indradb_token(config: &super::Config) -> Result<Option<String>> {
    let indradb = config.sink.indradb.as_ref().ok_or_else(|| Error::Config {
        field: "sink.indradb".to_owned(),
        detail: "backend = \"indradb\" but [sink.indradb] section is missing".to_owned(),
    })?;

    match &indradb.token_env {
        None => Ok(None),
        Some(var_name) => {
            let value = env::var(var_name).map_err(|_| Error::Sink {
                backend: "indradb",
                source: Box::new(EnvVarMissing {
                    var_name: var_name.clone(),
                }),
            })?;
            Ok(Some(value))
        }
    }
}

/// Read the API bearer token from the env var named in `config.api.auth_token_env`.
///
/// # Errors
///
/// Returns [`Error::Config`] if `config.api` is absent (daemon should always
/// have this). Returns [`Error::Sink`] (backend `"api"`) if the env var is not
/// set — this causes the daemon to refuse startup.
pub fn resolve_api_token(config: &super::Config) -> Result<String> {
    let api = config.api.as_ref().ok_or_else(|| Error::Config {
        field: "api".to_owned(),
        detail: "[api] section is required for cxg-daemon".to_owned(),
    })?;

    let var_name = &api.auth_token_env;
    env::var(var_name).map_err(|_| Error::Sink {
        backend: "api",
        source: Box::new(EnvVarMissing {
            var_name: var_name.clone(),
        }),
    })
}

/// Read the git PAT from the env var named in
/// `config.workspace.git_credentials_env`, if present.
///
/// Returns `Ok(None)` when `git_credentials_env` is not configured.
///
/// # Errors
///
/// Returns [`Error::Sink`] (backend `"workspace"`) if `git_credentials_env` is
/// configured but the env var is not set.
pub fn resolve_git_pat(config: &super::Config) -> Result<Option<String>> {
    let ws = match &config.workspace {
        None => return Ok(None),
        Some(ws) => ws,
    };

    match &ws.git_credentials_env {
        None => Ok(None),
        Some(var_name) => {
            let value = env::var(var_name).map_err(|_| Error::Sink {
                backend: "workspace",
                source: Box::new(EnvVarMissing {
                    var_name: var_name.clone(),
                }),
            })?;
            Ok(Some(value))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::Config;

    fn neo4j_config_with_env(var_name: &str) -> Config {
        let toml_str = format!(
            r#"
[sink]
backend = "neo4j"

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "{var_name}"
"#
        );
        Config::parse(&toml_str).expect("parse must succeed")
    }

    fn indradb_config_with_env(var_name: &str) -> Config {
        let toml_str = format!(
            r#"
[sink]
backend = "indradb"

[sink.indradb]
endpoint = "http://localhost:27615"
token_env = "{var_name}"
"#
        );
        Config::parse(&toml_str).expect("parse must succeed")
    }

    #[test]
    fn resolve_neo4j_password_set() {
        let var = "__CXG_TEST_NEO4J_PW_SET__";
        std::env::set_var(var, "correct-horse-battery-staple");
        let cfg = neo4j_config_with_env(var);
        let pw = resolve_neo4j_password(&cfg).expect("set env var must resolve");
        assert_eq!(pw, "correct-horse-battery-staple");
        std::env::remove_var(var);
    }

    #[test]
    fn resolve_neo4j_password_unset() {
        let var = "__CXG_TEST_NEO4J_PW_UNSET__";
        std::env::remove_var(var);
        let cfg = neo4j_config_with_env(var);
        let err = resolve_neo4j_password(&cfg).expect_err("unset must error");
        match err {
            Error::Sink { backend, source } => {
                assert_eq!(backend, "neo4j");
                assert!(source.to_string().contains(var));
            }
            other => panic!("expected Error::Sink, got {other:?}"),
        }
    }

    #[test]
    fn resolve_indradb_token_set() {
        let var = "__CXG_TEST_INDRA_TOKEN_SET__";
        std::env::set_var(var, "my-indradb-token");
        let cfg = indradb_config_with_env(var);
        let tok = resolve_indradb_token(&cfg)
            .expect("set env var must resolve")
            .expect("Some when token_env is configured");
        assert_eq!(tok, "my-indradb-token");
        std::env::remove_var(var);
    }

    #[test]
    fn resolve_indradb_token_none_when_no_env_configured() {
        let toml_str = r#"
[sink]
backend = "indradb"

[sink.indradb]
endpoint = "http://localhost:27615"
"#;
        let cfg = Config::parse(toml_str).expect("parse must succeed");
        let tok = resolve_indradb_token(&cfg).expect("no token_env = Ok(None)");
        assert!(tok.is_none());
    }
}
