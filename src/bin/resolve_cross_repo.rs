//! `cxg-resolve-cross-repo` — materialise Phase 5 `EXTERNAL_REF` edges.
//!
//! ## Usage
//!
//! ```text
//! cxg-resolve-cross-repo [OPTIONS] <STAGE_DIR>...
//! ```
//!
//! Each `STAGE_DIR` is a directory produced by `cxg-index` Phase 3 for one
//! repository (contains `final-edges.parquet` and `worker-*/nodes-*.parquet`).
//!
//! ## Required environment variables
//!
//! - For Neo4j backend: `NEO4J_PASSWORD` (or the name given by `--neo4j-password-env`).
//! - For IndraDB backend: optionally set via `--indradb-token-env`.
//!
//! ## Exit codes
//!
//! - `0` — success.
//! - `1` — error (message on stderr).

use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use anyhow::Context as _;
use clap::Parser;
use tracing::info;

use cpp_indexer::resolve::cross_repo::{run as phase5_run, Phase5Options};
use cpp_indexer::sink::factory;

// ---------------------------------------------------------------------------
// CLI definition
// ---------------------------------------------------------------------------

/// Materialise EXTERNAL_REF edges between indexed repositories (Phase 5).
#[derive(Debug, Parser)]
#[command(name = "cxg-resolve-cross-repo", version, about)]
struct Cli {
    /// Stage directories produced by cxg-index Phase 3 for each indexed repo.
    ///
    /// Each directory must contain `final-edges.parquet` and
    /// `worker-*/nodes-*.parquet` files.
    #[arg(value_name = "STAGE_DIR", required = true, num_args = 1..)]
    stage_dirs: Vec<PathBuf>,

    /// Sink backend: `neo4j` or `indradb`.
    #[arg(long, default_value = "neo4j")]
    backend: String,

    /// Database URI (e.g. `bolt://localhost:7687` or `http://localhost:27615`).
    #[arg(long)]
    db_uri: Option<String>,

    /// Neo4j username.
    #[arg(long, default_value = "neo4j")]
    neo4j_user: String,

    /// Env var whose value is the Neo4j password (default `NEO4J_PASSWORD`).
    #[arg(long, default_value = "NEO4J_PASSWORD")]
    neo4j_password_env: String,

    /// Env var whose value is the IndraDB auth token (optional).
    #[arg(long)]
    indradb_token_env: Option<String>,

    /// Phase 5 advisory lock TTL in seconds (default 600).
    #[arg(long, default_value = "600")]
    lock_ttl_secs: u64,

    /// Maximum records per write_edges call (default 10000).
    #[arg(long, default_value = "10000")]
    batch_size: usize,
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info")),
        )
        .with_writer(std::io::stderr)
        .init();

    let cli = Cli::parse();

    // Validate stage dirs exist.
    for dir in &cli.stage_dirs {
        if !dir.is_dir() {
            anyhow::bail!("stage_dir does not exist or is not a directory: {:?}", dir);
        }
    }

    // Build sink config from CLI flags.
    let sink_config = build_sink_config(&cli)?;
    let sink = factory::create(&sink_config)
        .await
        .with_context(|| format!("failed to connect to {} sink", cli.backend))?;

    info!(
        "cxg-resolve-cross-repo: {} stage dir(s) → sink '{}'",
        cli.stage_dirs.len(),
        sink.backend_name()
    );

    let opts = Phase5Options {
        stage_dirs: cli.stage_dirs,
        lock_ttl: Duration::from_secs(cli.lock_ttl_secs),
        batch_size: cli.batch_size,
        ..Default::default()
    };

    let stats = phase5_run(Arc::clone(&sink), opts)
        .await
        .context("Phase 5 cross-repo resolution failed")?;

    eprintln!("cxg-resolve-cross-repo: done — {stats}");

    Ok(())
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn build_sink_config(cli: &Cli) -> anyhow::Result<cpp_indexer::config::SinkConfig> {
    use cpp_indexer::config::{IndraDbSinkConfig, Neo4jSinkConfig, SinkConfig};

    match cli.backend.as_str() {
        "neo4j" => {
            let uri = cli
                .db_uri
                .clone()
                .unwrap_or_else(|| "bolt://localhost:7687".to_owned());
            Ok(SinkConfig {
                backend: "neo4j".to_owned(),
                batch_size: None,
                write_buffer_bytes: None,
                neo4j: Some(Neo4jSinkConfig {
                    uri,
                    user: cli.neo4j_user.clone(),
                    password_env: cli.neo4j_password_env.clone(),
                    sessions: None,
                }),
                indradb: None,
            })
        }
        "indradb" => {
            let uri = cli
                .db_uri
                .clone()
                .unwrap_or_else(|| "http://localhost:27615".to_owned());
            Ok(SinkConfig {
                backend: "indradb".to_owned(),
                batch_size: None,
                write_buffer_bytes: None,
                neo4j: None,
                indradb: Some(IndraDbSinkConfig {
                    endpoint: uri,
                    token_env: cli.indradb_token_env.clone(),
                    sessions: None,
                }),
            })
        }
        other => anyhow::bail!("unknown backend `{other}`; expected \"neo4j\" or \"indradb\""),
    }
}
