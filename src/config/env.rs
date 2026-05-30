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

/// Resolve the IndraDB token from a [`SinkConfig`][super::SinkConfig] directly.
///
/// Returns `Ok("")` (empty string) when no `token_env` is configured.
/// Returns `Ok(token)` when the env var is set.
///
/// # Errors
///
/// Returns [`Error::Sink`] if `token_env` is configured but the env var is not set.
pub fn resolve_indradb_token_opt(config: &super::SinkConfig) -> Result<String> {
    let indradb = config.indradb.as_ref().ok_or_else(|| Error::Config {
        field: "sink.indradb".to_owned(),
        detail: "backend = \"indradb\" but [sink.indradb] section is missing".to_owned(),
    })?;
    match &indradb.token_env {
        None => Ok(String::new()),
        Some(var_name) => env::var(var_name).map_err(|_| Error::Sink {
            backend: "indradb",
            source: Box::new(EnvVarMissing {
                var_name: var_name.clone(),
            }),
        }),
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

/// Read `CXG_SYMBOL_CACHE_SIZE` from the environment, if set.
///
/// Returns `Ok(None)` when the variable is absent (caller keeps the file/default value).
/// Returns `Ok(Some(n))` when the variable is present and parses as `usize`.
///
/// # Errors
///
/// Returns [`Error::Config`] when the variable is set but cannot be parsed as a non-negative
/// integer.
pub fn resolve_symbol_cache_size() -> crate::error::Result<Option<usize>> {
    match env::var("CXG_SYMBOL_CACHE_SIZE") {
        Err(_) => Ok(None),
        Ok(val) => {
            let n: usize = val.parse().map_err(|_| crate::error::Error::Config {
                field: "CXG_SYMBOL_CACHE_SIZE".to_owned(),
                detail: format!(
                    "env var `CXG_SYMBOL_CACHE_SIZE` must be a non-negative integer, got `{val}`"
                ),
            })?;
            Ok(Some(n))
        }
    }
}

/// Read `CXG_SYMBOL_DB_PATH` from the environment, if set.
///
/// Returns `Ok(None)` when the variable is absent (caller keeps the file/default value).
/// Returns `Ok(Some(path))` when the variable is present (any non-empty string is accepted as a
/// path — validation at use site).
pub fn resolve_symbol_db_path() -> Option<std::path::PathBuf> {
    env::var("CXG_SYMBOL_DB_PATH")
        .ok()
        .map(std::path::PathBuf::from)
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

    // ── Symbol env helpers ────────────────────────────────────────────────────
    //
    // CXG_SYMBOL_CACHE_SIZE / CXG_SYMBOL_DB_PATH are fixed env-var names, so
    // concurrent tests that set/remove them race.  Guard each block with a
    // process-wide mutex so they run serially even under `cargo test`'s default
    // thread pool.

    static ENV_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    #[test]
    fn resolve_symbol_cache_size_unset_returns_none() {
        let _g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        std::env::remove_var("CXG_SYMBOL_CACHE_SIZE");
        let result = resolve_symbol_cache_size().expect("unset → Ok(None)");
        assert!(result.is_none());
    }

    #[test]
    fn resolve_symbol_cache_size_set_valid() {
        let _g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        std::env::set_var("CXG_SYMBOL_CACHE_SIZE", "50000");
        let result = resolve_symbol_cache_size().expect("valid integer must parse");
        assert_eq!(result, Some(50_000usize));
        std::env::remove_var("CXG_SYMBOL_CACHE_SIZE");
    }

    #[test]
    fn resolve_symbol_cache_size_set_zero() {
        let _g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        std::env::set_var("CXG_SYMBOL_CACHE_SIZE", "0");
        let result = resolve_symbol_cache_size().expect("0 is valid");
        assert_eq!(result, Some(0usize), "0 must survive (disables cache)");
        std::env::remove_var("CXG_SYMBOL_CACHE_SIZE");
    }

    #[test]
    fn resolve_symbol_cache_size_set_invalid() {
        let _g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        std::env::set_var("CXG_SYMBOL_CACHE_SIZE", "not_a_number");
        let err = resolve_symbol_cache_size().expect_err("bad value must error");
        match err {
            crate::error::Error::Config { field, detail } => {
                assert!(
                    field.contains("CXG_SYMBOL_CACHE_SIZE"),
                    "field must name the env var; got: {field}"
                );
                assert!(
                    detail.contains("not_a_number"),
                    "detail must echo the bad value; got: {detail}"
                );
            }
            other => panic!("expected Error::Config, got {other:?}"),
        }
        std::env::remove_var("CXG_SYMBOL_CACHE_SIZE");
    }

    #[test]
    fn resolve_symbol_db_path_unset_returns_none() {
        let _g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        std::env::remove_var("CXG_SYMBOL_DB_PATH");
        assert!(resolve_symbol_db_path().is_none());
    }

    #[test]
    fn resolve_symbol_db_path_set() {
        let _g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        std::env::set_var("CXG_SYMBOL_DB_PATH", "/tmp/override.db");
        let path = resolve_symbol_db_path().expect("set var must return Some");
        assert_eq!(path, std::path::PathBuf::from("/tmp/override.db"));
        std::env::remove_var("CXG_SYMBOL_DB_PATH");
    }

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
