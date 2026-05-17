//! `cxg-index` — index a C++ repository into a graph database.
//!
//! Usage: `cxg-index [OPTIONS] <PATH>`
//!
//! `PATH` may be a source file, a directory, or a repository root. When no
//! `--compile-commands` flag is provided, `cxg-index` auto-detects
//! `compile_commands.json` by walking upward from `PATH` (Phase 0.5).

use std::path::PathBuf;
use std::sync::Arc;

use anyhow::Context as _;
use clap::Parser;
use tracing::info;

use cpp_indexer::pipeline::{run, RunOptions};
use cpp_indexer::sink::factory;
use cpp_indexer::visit::modules_cpp20;

// ---------------------------------------------------------------------------
// CLI definition
// ---------------------------------------------------------------------------

/// Index a C++ repository into a graph database.
#[derive(Debug, Parser)]
#[command(name = "cxg-index", about, disable_version_flag = true)]
struct Cli {
    /// Print version information (including C++20 module capability) and exit.
    #[arg(short = 'V', long = "version", action = clap::ArgAction::SetTrue)]
    print_version: bool,

    /// Path to the file, directory, or repository root to index.
    #[arg(value_name = "PATH", required = false)]
    input_path: Option<PathBuf>,

    /// Path to `compile_commands.json`. Auto-detected when absent.
    #[arg(long, value_name = "PATH")]
    compile_commands: Option<PathBuf>,

    /// Repository name embedded in every node/edge record.
    #[arg(long, default_value = "default")]
    repo_name: String,

    /// Staging directory for Parquet shards. A temporary directory is used when absent.
    #[arg(long, value_name = "DIR")]
    stage_dir: Option<PathBuf>,

    /// Skip Phase 2 decoration pass.
    #[arg(long)]
    skip_phase2: bool,

    /// Include nodes and edges from system headers (default: excluded).
    #[arg(long)]
    include_system_headers: bool,

    /// Sink backend: `neo4j` or `indradb`.
    #[arg(long, default_value = "neo4j")]
    backend: String,

    /// Neo4j/IndraDB URI (e.g. `bolt://localhost:7687` or `http://localhost:27615`).
    #[arg(long)]
    db_uri: Option<String>,

    /// Neo4j username (default `neo4j`).
    #[arg(long, default_value = "neo4j")]
    neo4j_user: String,

    /// Env var whose value is the Neo4j password (default `NEO4J_PASSWORD`).
    #[arg(long, default_value = "NEO4J_PASSWORD")]
    neo4j_password_env: String,

    /// Env var whose value is the IndraDB auth token (optional).
    #[arg(long)]
    indradb_token_env: Option<String>,

    /// Path to a `cxg-index.toml` config file. CLI flags override file settings.
    #[arg(long, value_name = "FILE")]
    config: Option<PathBuf>,
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // Initialise tracing with RUST_LOG env filter; default to INFO.
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info")),
        )
        .with_writer(std::io::stderr)
        .init();

    let cli = Cli::parse();

    // Handle --version: print version + C++20 module capability note, then exit.
    if cli.print_version {
        println!("cxg-index {}", env!("CARGO_PKG_VERSION"));
        if let Some(note) = modules_cpp20::capability_version_note() {
            println!("{note}");
        }
        return Ok(());
    }

    // Build a SinkConfig from CLI flags before moving fields out of cli.
    let sink_config = build_sink_config(&cli)?;
    let backend_name_hint = cli.backend.clone();

    // input_path is required when not printing version.
    let input_path = cli
        .input_path
        .ok_or_else(|| anyhow::anyhow!("PATH argument is required; run with --help for usage"))?;

    let sink = factory::create(&sink_config)
        .await
        .with_context(|| format!("failed to connect to {backend_name_hint} sink"))?;

    info!(
        "cxg-index: starting pipeline for {:?} → sink '{}'",
        input_path,
        sink.backend_name()
    );

    let opts = RunOptions {
        input_path,
        compile_commands: cli.compile_commands,
        repo_name: cli.repo_name,
        stage_dir: cli.stage_dir,
        skip_phase2: cli.skip_phase2,
        skip_system_headers: !cli.include_system_headers,
        skip_cache: false,
        skip_repo_node: false,
    };

    let stats = run(Arc::clone(&sink), opts)
        .await
        .context("pipeline failed")?;

    eprintln!(
        "cxg-index: done — {} TUs | {} partial | {} nodes | {} edges",
        stats.tu_count, stats.partial_tu_count, stats.nodes_written, stats.edges_written
    );

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
