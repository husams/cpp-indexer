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

/// Default LRU cache size for the symbol/file allocator (OQ-2, adr-3).
///
/// Set to `0` to disable the cache entirely.
pub const DEFAULT_SYMBOL_CACHE_SIZE: usize = 100_000;

/// Default glibc `M_ARENA_MAX` cap applied at process start (Issue 0002 Bug 1b).
///
/// Caps glibc malloc arenas process-wide (including libclang's transient AST
/// heap) so per-thread arena retention does not balloon RSS during Phase 1.
/// `0` leaves the glibc default untouched (the A/B escape hatch).
pub const DEFAULT_MALLOC_ARENA_MAX: usize = 2;

/// Default `malloc_trim` cadence in parses-per-worker (Issue 0002 Bug 1c).
///
/// `0` disables periodic trimming.
pub const DEFAULT_TRIM_INTERVAL: u32 = 64;

/// Default per-thread `clang::Index` recycle interval in parses (Bug 1d).
///
/// `0` disables recycling (the `Index` is reused for the whole run).
pub const DEFAULT_INDEX_RECYCLE_INTERVAL: u32 = 256;

/// `[index]` — Phase 1 / parallelism settings.
#[derive(Debug, Clone)]
pub struct IndexConfig {
    /// Number of rayon worker threads (default: number of logical CPUs).
    pub workers: Option<usize>,

    /// Skip Phase 2 decoration (default: false).
    pub skip_phase2: bool,

    /// When `true`, nodes and edges whose source location falls inside a system
    /// header (under `/usr/include/`, compiler-internal paths, or any path
    /// passed via `-isystem`) are excluded from Parquet output (AC-M2-14).
    /// Default: `true`.
    pub skip_system_headers: bool,

    /// Directory for Parquet staging shards and manifest cache.
    pub stage_dir: Option<PathBuf>,

    /// LRU cache capacity for the per-repo symbol/file allocator.
    ///
    /// Default: [`DEFAULT_SYMBOL_CACHE_SIZE`] (100 000).  Set to `0` to
    /// disable the cache (every lookup hits SQLite).
    pub symbol_cache_size: usize,

    /// Explicit path for the per-repo SQLite symbol map (`cxg-symbols.db`).
    ///
    /// `None` means "resolve to `<stage_dir>/cxg-symbols.db` at the use
    /// site".  Carrying it as `Option<PathBuf>` here preserves the
    /// distinction between "operator supplied a path" and "use the default".
    pub symbol_db_path: Option<PathBuf>,

    /// glibc `M_ARENA_MAX` cap applied at process start (Issue 0002 Bug 1b).
    ///
    /// Default [`DEFAULT_MALLOC_ARENA_MAX`] (2).  `0` leaves the glibc default
    /// untouched (A/B escape hatch).  No effect off Linux.
    pub malloc_arena_max: usize,

    /// `malloc_trim` cadence in parses-per-worker (Issue 0002 Bug 1c).
    ///
    /// Default [`DEFAULT_TRIM_INTERVAL`] (64).  `0` disables periodic trimming.
    pub trim_interval: u32,

    /// Per-thread `clang::Index` recycle interval in parses (Bug 1d).
    ///
    /// Default [`DEFAULT_INDEX_RECYCLE_INTERVAL`] (256).  `0` disables recycling.
    pub index_recycle_interval: u32,

    /// When `true`, skip Phases 0–3 and run Phase 4 against an existing
    /// `--stage-dir` (Issue 0002 Bug 2 resume).  Default `false`.
    pub write_only: bool,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawIndexConfig {
    workers: Option<usize>,
    #[serde(default)]
    skip_phase2: bool,
    #[serde(default = "default_true")]
    skip_system_headers: bool,
    stage_dir: Option<PathBuf>,
    #[serde(default = "default_symbol_cache_size")]
    symbol_cache_size: usize,
    symbol_db_path: Option<PathBuf>,
    #[serde(default = "default_malloc_arena_max")]
    malloc_arena_max: usize,
    #[serde(default = "default_trim_interval")]
    trim_interval: u32,
    #[serde(default = "default_index_recycle_interval")]
    index_recycle_interval: u32,
    #[serde(default)]
    write_only: bool,
}

fn default_true() -> bool {
    true
}

fn default_symbol_cache_size() -> usize {
    DEFAULT_SYMBOL_CACHE_SIZE
}

fn default_malloc_arena_max() -> usize {
    DEFAULT_MALLOC_ARENA_MAX
}

fn default_trim_interval() -> u32 {
    DEFAULT_TRIM_INTERVAL
}

fn default_index_recycle_interval() -> u32 {
    DEFAULT_INDEX_RECYCLE_INTERVAL
}

impl From<RawIndexConfig> for IndexConfig {
    fn from(r: RawIndexConfig) -> Self {
        Self {
            workers: r.workers,
            skip_phase2: r.skip_phase2,
            skip_system_headers: r.skip_system_headers,
            stage_dir: r.stage_dir,
            symbol_cache_size: r.symbol_cache_size,
            symbol_db_path: r.symbol_db_path,
            malloc_arena_max: r.malloc_arena_max,
            trim_interval: r.trim_interval,
            index_recycle_interval: r.index_recycle_interval,
            write_only: r.write_only,
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

    /// Maximum number of concurrent in-flight gRPC write calls (default 16).
    ///
    /// Each call sends one `batch_size` chunk. Setting this higher than the
    /// IndraDB server's thread-pool size yields diminishing returns.
    pub sessions: Option<usize>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawIndraDbSinkConfig {
    endpoint: String,
    token_env: Option<String>,
    sessions: Option<usize>,
    /// Forbidden: must use `token_env` instead.
    token: Option<String>,
}

impl RawIndraDbSinkConfig {
    fn into_config(self) -> std::result::Result<IndraDbSinkConfig, String> {
        check_forbidden("token", self.token)?;
        Ok(IndraDbSinkConfig {
            endpoint: self.endpoint,
            token_env: self.token_env,
            sessions: self.sessions,
        })
    }
}

/// Default batch size for chunked sink writes (S18: AC-M3-4, AC-M3-5).
pub const DEFAULT_BATCH_SIZE: usize = 10_000;

/// Default number of concurrent in-flight write sessions (S18: ADR-2 §Concurrency).
pub const DEFAULT_SESSIONS: usize = 16;

/// Default byte budget for the Phase 4 streaming write buffer (Issue 0002 Bug 2).
///
/// Phase 4 flushes the in-flight node/edge buffer to the sink when *either* the
/// row count reaches `batch_size` *or* the accumulated record bytes reach this
/// budget — bounding peak RSS even when a buffer is full of large `code`
/// snippets (≤32 KiB each).  `0` disables the byte budget (row-only flushing,
/// restoring pre-Issue-0002 behaviour for A/B).
pub const DEFAULT_WRITE_BUFFER_BYTES: usize = 64 * 1024 * 1024;

/// `[sink]` — top-level sink selector.
#[derive(Debug, Clone)]
pub struct SinkConfig {
    /// Which backend to use: `"neo4j"` or `"indradb"`.
    pub backend: String,

    /// Maximum records per individual write call to the database.
    ///
    /// Larger batches amortise round-trip overhead; smaller batches reduce
    /// peak memory per chunk. Default: [`DEFAULT_BATCH_SIZE`] (10 000).
    pub batch_size: Option<usize>,

    /// Byte budget for the Phase 4 streaming write buffer (Issue 0002 Bug 2).
    ///
    /// `None` falls back to [`DEFAULT_WRITE_BUFFER_BYTES`] (~64 MiB).  `Some(0)`
    /// disables the byte budget (row-only flushing).
    pub write_buffer_bytes: Option<usize>,

    /// Neo4j parameters (required when `backend = "neo4j"`).
    pub neo4j: Option<Neo4jSinkConfig>,

    /// IndraDB parameters (required when `backend = "indradb"`).
    pub indradb: Option<IndraDbSinkConfig>,
}

impl SinkConfig {
    /// Resolved batch size, falling back to [`DEFAULT_BATCH_SIZE`].
    pub fn resolved_batch_size(&self) -> usize {
        self.batch_size.unwrap_or(DEFAULT_BATCH_SIZE)
    }

    /// Resolved write-buffer byte budget, falling back to
    /// [`DEFAULT_WRITE_BUFFER_BYTES`].  `0` means "no byte budget".
    pub fn resolved_write_buffer_bytes(&self) -> usize {
        self.write_buffer_bytes
            .unwrap_or(DEFAULT_WRITE_BUFFER_BYTES)
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RawSinkConfig {
    backend: String,
    batch_size: Option<usize>,
    write_buffer_bytes: Option<usize>,
    neo4j: Option<RawNeo4jSinkConfig>,
    indradb: Option<RawIndraDbSinkConfig>,
}

impl RawSinkConfig {
    fn into_config(self) -> std::result::Result<SinkConfig, String> {
        let neo4j = self.neo4j.map(|r| r.into_config()).transpose()?;
        let indradb = self.indradb.map(|r| r.into_config()).transpose()?;
        Ok(SinkConfig {
            backend: self.backend,
            batch_size: self.batch_size,
            write_buffer_bytes: self.write_buffer_bytes,
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

// ── Symbol config resolution helpers ─────────────────────────────────────────

/// Resolve the effective `symbol_cache_size` from the precedence chain
/// `cli_override > env_override > file_value > default`.
///
/// Callers pass `None` for any layer that is absent. The resolved value is
/// always `Some`; it equals [`DEFAULT_SYMBOL_CACHE_SIZE`] when every layer is
/// `None`.
pub fn resolve_symbol_cache_size(
    cli_override: Option<usize>,
    env_override: Option<usize>,
    file_value: Option<usize>,
) -> usize {
    cli_override
        .or(env_override)
        .or(file_value)
        .unwrap_or(DEFAULT_SYMBOL_CACHE_SIZE)
}

/// Resolve the effective `symbol_db_path` from the precedence chain
/// `cli_override > env_override > file_value`.
///
/// Returns `None` when all layers are absent; the caller is responsible for
/// resolving `None` to `<stage_dir>/cxg-symbols.db` at the allocator use site.
pub fn resolve_symbol_db_path(
    cli_override: Option<PathBuf>,
    env_override: Option<PathBuf>,
    file_value: Option<PathBuf>,
) -> Option<PathBuf> {
    cli_override.or(env_override).or(file_value)
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

    // ── (e) symbol_cache_size + symbol_db_path config surface ────────────────

    // ── resolution helpers: precedence table ──────────────────────────────────

    #[test]
    fn resolve_cache_size_all_none_returns_default() {
        assert_eq!(
            resolve_symbol_cache_size(None, None, None),
            DEFAULT_SYMBOL_CACHE_SIZE
        );
    }

    #[test]
    fn resolve_cache_size_file_value_used_when_no_override() {
        assert_eq!(resolve_symbol_cache_size(None, None, Some(42)), 42);
    }

    #[test]
    fn resolve_cache_size_env_overrides_file() {
        assert_eq!(resolve_symbol_cache_size(None, Some(99), Some(42)), 99);
    }

    #[test]
    fn resolve_cache_size_cli_overrides_env_and_file() {
        assert_eq!(resolve_symbol_cache_size(Some(1), Some(99), Some(42)), 1);
    }

    #[test]
    fn resolve_cache_size_zero_is_preserved() {
        // S3-SC-05: cache_size=0 must survive every surface.
        assert_eq!(resolve_symbol_cache_size(Some(0), None, None), 0);
        assert_eq!(resolve_symbol_cache_size(None, Some(0), None), 0);
        assert_eq!(resolve_symbol_cache_size(None, None, Some(0)), 0);
    }

    #[test]
    fn resolve_db_path_all_none_returns_none() {
        assert!(resolve_symbol_db_path(None, None, None).is_none());
    }

    #[test]
    fn resolve_db_path_cli_overrides_all() {
        let cli = Some(PathBuf::from("/cli/path.db"));
        let env = Some(PathBuf::from("/env/path.db"));
        let file = Some(PathBuf::from("/file/path.db"));
        assert_eq!(
            resolve_symbol_db_path(cli, env, file),
            Some(PathBuf::from("/cli/path.db"))
        );
    }

    #[test]
    fn resolve_db_path_env_overrides_file() {
        let env = Some(PathBuf::from("/env/path.db"));
        let file = Some(PathBuf::from("/file/path.db"));
        assert_eq!(
            resolve_symbol_db_path(None, env, file),
            Some(PathBuf::from("/env/path.db"))
        );
    }

    #[test]
    fn config_symbol_cache_size_default() {
        let toml_str = r#"
[sink]
backend = "neo4j"

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"
"#;
        let cfg = Config::parse(toml_str).expect("must parse");
        // No [index] section → index is None; resolved default is DEFAULT_SYMBOL_CACHE_SIZE.
        let cache_size = cfg
            .index
            .as_ref()
            .map(|i| i.symbol_cache_size)
            .unwrap_or(DEFAULT_SYMBOL_CACHE_SIZE);
        assert_eq!(cache_size, DEFAULT_SYMBOL_CACHE_SIZE);
    }

    #[test]
    fn config_symbol_cache_size_explicit() {
        let toml_str = r#"
[sink]
backend = "neo4j"

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"

[index]
symbol_cache_size = 0
"#;
        let cfg = Config::parse(toml_str).expect("must parse");
        let idx = cfg.index.as_ref().expect("[index] present");
        assert_eq!(idx.symbol_cache_size, 0, "0 must not be coerced to default");
    }

    #[test]
    fn config_symbol_db_path_default_is_none() {
        let toml_str = r#"
[sink]
backend = "neo4j"

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"

[index]
symbol_cache_size = 500
"#;
        let cfg = Config::parse(toml_str).expect("must parse");
        let idx = cfg.index.as_ref().expect("[index] present");
        assert!(idx.symbol_db_path.is_none(), "no path set → None");
    }

    #[test]
    fn config_symbol_db_path_explicit() {
        let toml_str = r#"
[sink]
backend = "neo4j"

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"

[index]
symbol_db_path = "/tmp/my-symbols.db"
"#;
        let cfg = Config::parse(toml_str).expect("must parse");
        let idx = cfg.index.as_ref().expect("[index] present");
        assert_eq!(
            idx.symbol_db_path.as_ref().expect("path set"),
            &PathBuf::from("/tmp/my-symbols.db")
        );
    }

    // ── Issue 0002 config knobs ───────────────────────────────────────────

    #[test]
    fn config_memory_knobs_default_when_index_section_omitted() {
        let toml_str = r#"
[sink]
backend = "neo4j"

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"
"#;
        let cfg = Config::parse(toml_str).expect("must parse");
        // No [index] section → the caller falls back to the DEFAULT_* consts.
        assert!(cfg.index.is_none());
        assert_eq!(DEFAULT_MALLOC_ARENA_MAX, 2);
        assert_eq!(DEFAULT_TRIM_INTERVAL, 64);
        assert_eq!(DEFAULT_INDEX_RECYCLE_INTERVAL, 256);
    }

    #[test]
    fn config_memory_knobs_default_within_index_section() {
        // An [index] section that omits the knobs must still default them.
        let toml_str = r#"
[sink]
backend = "neo4j"

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"

[index]
workers = 4
"#;
        let cfg = Config::parse(toml_str).expect("must parse");
        let idx = cfg.index.as_ref().expect("[index] present");
        assert_eq!(idx.malloc_arena_max, DEFAULT_MALLOC_ARENA_MAX);
        assert_eq!(idx.trim_interval, DEFAULT_TRIM_INTERVAL);
        assert_eq!(idx.index_recycle_interval, DEFAULT_INDEX_RECYCLE_INTERVAL);
        assert!(!idx.write_only, "write_only defaults to false");
    }

    #[test]
    fn config_memory_knobs_explicit_override() {
        let toml_str = r#"
[sink]
backend = "neo4j"

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"

[index]
malloc_arena_max = 0
trim_interval = 16
index_recycle_interval = 1024
write_only = true
"#;
        let cfg = Config::parse(toml_str).expect("must parse");
        let idx = cfg.index.as_ref().expect("[index] present");
        assert_eq!(idx.malloc_arena_max, 0, "0 (uncapped) must survive");
        assert_eq!(idx.trim_interval, 16);
        assert_eq!(idx.index_recycle_interval, 1024);
        assert!(idx.write_only);
    }

    #[test]
    fn config_write_buffer_bytes_default_and_override() {
        let base = r#"
[sink]
backend = "neo4j"

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"
"#;
        let cfg = Config::parse(base).expect("must parse");
        assert!(cfg.sink.write_buffer_bytes.is_none());
        assert_eq!(
            cfg.sink.resolved_write_buffer_bytes(),
            DEFAULT_WRITE_BUFFER_BYTES
        );

        // `write_buffer_bytes` lives under [sink] (above the [sink.neo4j] table).
        let with_value = r#"
[sink]
backend = "neo4j"
write_buffer_bytes = 1048576

[sink.neo4j]
uri = "bolt://localhost:7687"
user = "neo4j"
password_env = "NEO4J_PASSWORD"
"#;
        let cfg = Config::parse(with_value).expect("must parse");
        assert_eq!(cfg.sink.write_buffer_bytes, Some(1_048_576));
        assert_eq!(cfg.sink.resolved_write_buffer_bytes(), 1_048_576);
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
