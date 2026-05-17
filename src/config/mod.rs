//! Configuration schemas for `cxg-index.toml` and `cxg-daemon.toml`.
//!
//! Both files share the same top-level [`Config`] struct. Daemon-specific
//! sections ([`ApiConfig`], [`WorkspaceConfig`]) are optional so that
//! `cxg-index` can load a config that omits them.
//!
//! # Secrets
//!
//! Secret values (passwords, tokens, PATs) are **never** read directly from a
//! TOML field. Instead each sensitive field is an `*_env` string that names
//! an environment variable. Calling the helpers in [`env`] exchanges each
//! `*_env` name for the value read from the environment; an unset variable is
//! a hard error. Any TOML field named `password`, `token`, or `pat` (direct
//! value, not `_env` indirection) is rejected at parse time.

pub mod env;

use serde::Deserialize;
use std::path::PathBuf;

use crate::error::{Error, Result};

// ── Parse-time secret guard ───────────────────────────────────────────────────
//
// Strategy: each public config struct has a corresponding private "raw" struct
// that includes the forbidden fields as `Option<String>`. After serde
// deserialises the raw struct we check whether a forbidden field was supplied
// and return a clear error before constructing the public type.

fn check_forbidden(field: &str, value: Option<String>) -> std::result::Result<(), String> {
    if value.is_some() {
        return Err(format!(
            "field `{field}` must not contain a direct secret value; \
             use `{field}_env` to name an environment variable instead"
        ));
    }
    Ok(())
}

// ── Repo section ──────────────────────────────────────────────────────────────

/// `[repo]` — input-scope configuration (used by `cxg-index`).
#[derive(Debug, Clone)]
pub struct RepoConfig {
    /// Absolute or relative path to the repository root.
    pub path: PathBuf,

    /// Optional explicit path to `compile_commands.json`.
    /// If absent, auto-detection (Phase 0.5) is used.
    pub compile_commands: Option<PathBuf>,

    /// Index only files under this directory (relative to `path`).
    pub scope_dir: Option<PathBuf>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawRepoConfig {
    path: PathBuf,
    compile_commands: Option<PathBuf>,
    scope_dir: Option<PathBuf>,
}

impl From<RawRepoConfig> for RepoConfig {
    fn from(r: RawRepoConfig) -> Self {
        Self {
            path: r.path,
            compile_commands: r.compile_commands,
            scope_dir: r.scope_dir,
        }
    }
}

// ── Index section ─────────────────────────────────────────────────────────────

/// `[index]` — Phase 1 / parallelism settings.
#[derive(Debug, Clone)]
pub struct IndexConfig {
    /// Number of rayon worker threads (default: number of logical CPUs).
    pub workers: Option<usize>,

    /// Skip Phase 2 decoration (default: false).
    pub skip_phase2: bool,

    /// Directory for Parquet staging shards and manifest cache.
    pub stage_dir: Option<PathBuf>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawIndexConfig {
    workers: Option<usize>,
    #[serde(default)]
    skip_phase2: bool,
    stage_dir: Option<PathBuf>,
}

impl From<RawIndexConfig> for IndexConfig {
    fn from(r: RawIndexConfig) -> Self {
        Self {
            workers: r.workers,
            skip_phase2: r.skip_phase2,
            stage_dir: r.stage_dir,
        }
    }
}

// ── Sink section ─────────────────────────────────────────────────────────────

/// `[sink.neo4j]` — Neo4j connection parameters.
#[derive(Debug, Clone)]
pub struct Neo4jSinkConfig {
    /// Bolt URI, e.g. `bolt://localhost:7687`.
    pub uri: String,

    /// Neo4j username.
    pub user: String,

    /// **Env var name** whose value is the Neo4j password. Required.
    pub password_env: String,

    /// Connection pool size (default 16).
    pub sessions: Option<usize>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawNeo4jSinkConfig {
    uri: String,
    user: String,
    password_env: String,
    sessions: Option<usize>,
    /// Forbidden: must use `password_env` instead.
    password: Option<String>,
}

impl RawNeo4jSinkConfig {
    fn into_config(self) -> std::result::Result<Neo4jSinkConfig, String> {
        check_forbidden("password", self.password)?;
        Ok(Neo4jSinkConfig {
            uri: self.uri,
            user: self.user,
            password_env: self.password_env,
            sessions: self.sessions,
        })
    }
}

/// `[sink.indradb]` — IndraDB connection parameters.
#[derive(Debug, Clone)]
pub struct IndraDbSinkConfig {
    /// gRPC endpoint, e.g. `http://localhost:27615`.
    pub endpoint: String,

    /// **Env var name** whose value is the IndraDB auth token. Optional
    /// (public IndraDB instances may not require auth).
    pub token_env: Option<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawIndraDbSinkConfig {
    endpoint: String,
    token_env: Option<String>,
    /// Forbidden: must use `token_env` instead.
    token: Option<String>,
}

impl RawIndraDbSinkConfig {
    fn into_config(self) -> std::result::Result<IndraDbSinkConfig, String> {
        check_forbidden("token", self.token)?;
        Ok(IndraDbSinkConfig {
            endpoint: self.endpoint,
            token_env: self.token_env,
        })
    }
}

/// `[sink]` — top-level sink selector.
#[derive(Debug, Clone)]
pub struct SinkConfig {
    /// Which backend to use: `"neo4j"` or `"indradb"`.
    pub backend: String,

    /// Neo4j parameters (required when `backend = "neo4j"`).
    pub neo4j: Option<Neo4jSinkConfig>,

    /// IndraDB parameters (required when `backend = "indradb"`).
    pub indradb: Option<IndraDbSinkConfig>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawSinkConfig {
    backend: String,
    neo4j: Option<RawNeo4jSinkConfig>,
    indradb: Option<RawIndraDbSinkConfig>,
}

impl RawSinkConfig {
    fn into_config(self) -> std::result::Result<SinkConfig, String> {
        let neo4j = self.neo4j.map(|r| r.into_config()).transpose()?;
        let indradb = self.indradb.map(|r| r.into_config()).transpose()?;
        Ok(SinkConfig {
            backend: self.backend,
            neo4j,
            indradb,
        })
    }
}

// ── API section (daemon only) ─────────────────────────────────────────────────

/// `[api]` — REST control plane settings for `cxg-daemon`.
#[derive(Debug, Clone)]
pub struct ApiConfig {
    /// Bind address (default `"127.0.0.1:7878"`).
    pub listen: Option<String>,

    /// **Env var name** whose value is the bearer token for write endpoints.
    /// If absent, the daemon refuses to start.
    pub auth_token_env: String,

    /// Maximum in-process job queue depth before returning 429 (default 64).
    pub job_queue_max: Option<usize>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawApiConfig {
    listen: Option<String>,
    auth_token_env: String,
    job_queue_max: Option<usize>,
    /// Forbidden: must use `auth_token_env` instead.
    auth_token: Option<String>,
    /// Forbidden: must use `auth_token_env` instead.
    pat: Option<String>,
}

impl RawApiConfig {
    fn into_config(self) -> std::result::Result<ApiConfig, String> {
        check_forbidden("auth_token", self.auth_token)?;
        check_forbidden("pat", self.pat)?;
        Ok(ApiConfig {
            listen: self.listen,
            auth_token_env: self.auth_token_env,
            job_queue_max: self.job_queue_max,
        })
    }
}

// ── Workspace section (daemon only) ──────────────────────────────────────────

/// `[workspace]` — git clone manager settings for `cxg-daemon`.
#[derive(Debug, Clone)]
pub struct WorkspaceConfig {
    /// Directory where clones are placed.
    pub dir: PathBuf,

    /// Host-suffix allowlist for git URLs (e.g. `["github.com"]`).
    pub allowed_hosts: Vec<String>,

    /// **Env var name** whose value is the git PAT. Optional (public repos).
    pub git_credentials_env: Option<String>,

    /// Shallow clone depth (default 1; 0 = full history).
    pub default_clone_depth: Option<u32>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawWorkspaceConfig {
    dir: PathBuf,
    allowed_hosts: Vec<String>,
    git_credentials_env: Option<String>,
    default_clone_depth: Option<u32>,
    /// Forbidden: must use `git_credentials_env` instead.
    pat: Option<String>,
    /// Forbidden: must use `git_credentials_env` instead.
    git_credentials: Option<String>,
}

impl RawWorkspaceConfig {
    fn into_config(self) -> std::result::Result<WorkspaceConfig, String> {
        check_forbidden("pat", self.pat)?;
        check_forbidden("git_credentials", self.git_credentials)?;
        Ok(WorkspaceConfig {
            dir: self.dir,
            allowed_hosts: self.allowed_hosts,
            git_credentials_env: self.git_credentials_env,
            default_clone_depth: self.default_clone_depth,
        })
    }
}

// ── Top-level raw config ──────────────────────────────────────────────────────

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawConfig {
    repo: Option<RawRepoConfig>,
    index: Option<RawIndexConfig>,
    sink: RawSinkConfig,
    api: Option<RawApiConfig>,
    workspace: Option<RawWorkspaceConfig>,
}

// ── Top-level Config ──────────────────────────────────────────────────────────

/// Root configuration struct shared by `cxg-index.toml` and `cxg-daemon.toml`.
///
/// Load with [`Config::parse`] (TOML text). After loading, call
/// [`Config::validate`] to ensure required fields are present for the selected
/// backend.
#[derive(Debug, Clone)]
pub struct Config {
    pub repo: Option<RepoConfig>,
    pub index: Option<IndexConfig>,
    pub sink: SinkConfig,
    pub api: Option<ApiConfig>,
    pub workspace: Option<WorkspaceConfig>,
}

impl Config {
    /// Parse a TOML string into a [`Config`].
    ///
    /// Returns [`Error::Config`] with a human-readable description if:
    /// - a required field is missing,
    /// - a forbidden direct-secret field is present, or
    /// - the TOML is structurally malformed.
    pub fn parse(s: &str) -> Result<Self> {
        let raw: RawConfig = toml::from_str(s).map_err(|e| Error::Config {
            field: field_path_from_toml_error(&e),
            detail: e.to_string(),
        })?;

        let sink = raw.sink.into_config().map_err(|detail| Error::Config {
            field: "sink".to_owned(),
            detail,
        })?;
        let api = raw
            .api
            .map(|a| {
                a.into_config().map_err(|detail| Error::Config {
                    field: "api".to_owned(),
                    detail,
                })
            })
            .transpose()?;
        let workspace = raw
            .workspace
            .map(|w| {
                w.into_config().map_err(|detail| Error::Config {
                    field: "workspace".to_owned(),
                    detail,
                })
            })
            .transpose()?;

        Ok(Config {
            repo: raw.repo.map(Into::into),
            index: raw.index.map(Into::into),
            sink,
            api,
            workspace,
        })
    }

    /// Validate that the config is internally consistent.
    ///
    /// Checks:
    /// - `sink.backend` is one of `"neo4j"` or `"indradb"`.
    /// - The corresponding `sink.neo4j` / `sink.indradb` sub-section is present.
    pub fn validate(&self) -> Result<()> {
        match self.sink.backend.as_str() {
            "neo4j" => {
                if self.sink.neo4j.is_none() {
                    return Err(Error::Config {
                        field: "sink.neo4j".to_owned(),
                        detail: "backend = \"neo4j\" requires a [sink.neo4j] section".to_owned(),
                    });
                }
            }
            "indradb" => {
                if self.sink.indradb.is_none() {
                    return Err(Error::Config {
                        field: "sink.indradb".to_owned(),
                        detail: "backend = \"indradb\" requires a [sink.indradb] section"
                            .to_owned(),
                    });
                }
            }
            other => {
                return Err(Error::Config {
                    field: "sink.backend".to_owned(),
                    detail: format!("unknown backend `{other}`; expected \"neo4j\" or \"indradb\""),
                });
            }
        }
        Ok(())
    }
}

/// Extract a best-effort field path from a `toml::de::Error`.
fn field_path_from_toml_error(e: &toml::de::Error) -> String {
    let msg = e.to_string();
    if let Some(pos) = msg.find("missing field `") {
        let rest = &msg[pos + "missing field `".len()..];
        if let Some(end) = rest.find('`') {
            return rest[..end].to_owned();
        }
    }
    msg.lines().next().unwrap_or("(unknown)").to_owned()
}

// ── Unit tests ────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    // ── (a) Golden TOML parsing ───────────────────────────────────────────────

    #[test]
    fn config_parse_cxg_index_golden() {
        let toml_str = std::fs::read_to_string("tests/fixtures/config/cxg-index-golden.toml")
            .expect("fixture must exist");
        let cfg = Config::parse(&toml_str).expect("golden must parse");
        cfg.validate().expect("golden must validate");

        let repo = cfg.repo.as_ref().expect("repo section present");
        assert_eq!(repo.path, PathBuf::from("/workspace/my-project"));

        let neo4j = cfg.sink.neo4j.as_ref().expect("neo4j section present");
        assert_eq!(neo4j.uri, "bolt://localhost:7687");
        assert_eq!(neo4j.password_env, "NEO4J_PASSWORD");
    }

    #[test]
    fn config_parse_cxg_daemon_golden() {
        let toml_str = std::fs::read_to_string("tests/fixtures/config/cxg-daemon-golden.toml")
            .expect("fixture must exist");
        let cfg = Config::parse(&toml_str).expect("golden must parse");
        cfg.validate().expect("golden must validate");

        let api = cfg.api.as_ref().expect("api section present");
        assert_eq!(api.auth_token_env, "CXG_API_TOKEN");

        let ws = cfg.workspace.as_ref().expect("workspace section present");
        assert!(ws.allowed_hosts.contains(&"github.com".to_owned()));
    }

    // ── (b) Missing required field → typed error naming field path ────────────

    #[test]
    fn config_missing_sink_backend_errors() {
        let toml_str = r#"
[sink]
# backend is intentionally absent
"#;
        let err = Config::parse(toml_str).expect_err("must fail without backend");
        match err {
            Error::Config { field, .. } => {
                assert!(
                    field.contains("backend") || field.contains("sink"),
                    "error must name the missing field; got: {field}"
                );
            }
            other => panic!("expected Error::Config, got {other:?}"),
        }
    }

    #[test]
    fn config_unknown_backend_errors() {
        let toml_str = r#"
[sink]
backend = "postgres"
"#;
        let cfg = Config::parse(toml_str).expect("parses OK");
        let err = cfg
            .validate()
            .expect_err("validation must reject unknown backend");
        match err {
            Error::Config { field, detail } => {
                assert_eq!(field, "sink.backend");
                assert!(detail.contains("postgres"), "detail must name bad value");
            }
            other => panic!("expected Error::Config, got {other:?}"),
        }
    }

    // ── (c) password_env / token_env unset at resolve time → Error::Sink ─────

    #[test]
    fn resolve_neo4j_password_env_missing_errors() {
        let var_name = "__CXG_TEST_NEO4J_PW_MISSING__";
        std::env::remove_var(var_name);

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
        let cfg = Config::parse(&toml_str).expect("parse must succeed");
        let err = env::resolve_neo4j_password(&cfg).expect_err("missing env var must error");
        match err {
            Error::Sink { source, .. } => {
                let msg = source.to_string();
                assert!(
                    msg.contains(var_name),
                    "error must name the env var; got: {msg}"
                );
            }
            other => panic!("expected Error::Sink, got {other:?}"),
        }
    }

    #[test]
    fn resolve_indradb_token_env_missing_errors() {
        let var_name = "__CXG_TEST_INDRA_TOKEN_MISSING__";
        std::env::remove_var(var_name);

        let toml_str = format!(
            r#"
[sink]
backend = "indradb"

[sink.indradb]
endpoint = "http://localhost:27615"
token_env = "{var_name}"
"#
        );
        let cfg = Config::parse(&toml_str).expect("parse must succeed");
        let err = env::resolve_indradb_token(&cfg).expect_err("missing env var must error");
        match err {
            Error::Sink { source, .. } => {
                let msg = source.to_string();
                assert!(
                    msg.contains(var_name),
                    "error must name the env var; got: {msg}"
                );
            }
            other => panic!("expected Error::Sink, got {other:?}"),
        }
    }

    // ── (d) direct password / token / pat field rejected at parse time ────────

    #[test]
    fn config_rejects_direct_password_field() {
        let toml_str = r#"
[sink]
backend = "neo4j"

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"
password = "hunter2"
"#;
        let err = Config::parse(toml_str).expect_err("must reject direct password");
        match err {
            Error::Config { detail, .. } => {
                assert!(
                    detail.contains("password"),
                    "error must mention 'password'; got: {detail}"
                );
                assert!(
                    detail.contains("_env"),
                    "error must suggest *_env alternative; got: {detail}"
                );
            }
            other => panic!("expected Error::Config, got {other:?}"),
        }
    }

    #[test]
    fn config_rejects_direct_token_field() {
        let toml_str = r#"
[sink]
backend = "indradb"

[sink.indradb]
endpoint = "http://localhost:27615"
token_env = "INDRA_TOKEN"
token = "secret-token"
"#;
        let err = Config::parse(toml_str).expect_err("must reject direct token");
        match err {
            Error::Config { detail, .. } => {
                assert!(
                    detail.contains("token"),
                    "error must mention 'token'; got: {detail}"
                );
            }
            other => panic!("expected Error::Config, got {other:?}"),
        }
    }

    #[test]
    fn config_rejects_direct_pat_field_in_api() {
        let toml_str = r#"
[sink]
backend = "neo4j"

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"

[api]
auth_token_env = "CXG_API_TOKEN"
auth_token = "my-secret-token"
"#;
        let err = Config::parse(toml_str).expect_err("must reject direct auth_token");
        match err {
            Error::Config { detail, .. } => {
                assert!(
                    detail.contains("pat") || detail.contains("auth_token"),
                    "error must mention the forbidden field; got: {detail}"
                );
            }
            other => panic!("expected Error::Config, got {other:?}"),
        }
    }
}
