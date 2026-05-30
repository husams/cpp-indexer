//! `cxg-daemon` — REST control plane for the cpp-indexer pipeline.
//!
//! Reads `cxg-daemon.toml` (or the path in `--config`), loads the bearer
//! token from the env var named in `[api].auth_token_env`, then starts an
//! axum HTTP server on `[api].listen` (default `127.0.0.1:7878`).
//!
//! The daemon refuses to start if `[api].auth_token_env` is unset in the
//! environment.  There is no insecure mode (ADR-5 §Auth).
//!
//! ADR-5.

use std::{net::SocketAddr, path::PathBuf, sync::Arc};

use anyhow::{bail, Context};
use clap::Parser;
use tokio::net::TcpListener;

use cpp_indexer::{
    api::{
        auth::BearerToken,
        jobs::{IngestSource, JobMessage, JobPhase, JobQueue, RepoInfo, RepoRegistry},
        routes::{build_router, AppState},
    },
    config::{Config, IndexConfig, RepoConfig, WorkspaceConfig},
    pipeline::{run, PipelineStats, RunOptions},
    sink::factory,
};

// ── CLI ────────────────────────────────────────────────────────────────────────

#[derive(Debug, Parser)]
#[command(name = "cxg-daemon", about = "cpp-indexer REST daemon")]
struct Args {
    /// Path to `cxg-daemon.toml` (default: `./cxg-daemon.toml`).
    #[arg(short, long, default_value = "cxg-daemon.toml")]
    config: String,
}

// ── Entry point ────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // Initialise tracing first so all startup errors are structured.
    cpp_indexer::observability::init_tracing();

    let args = Args::parse();

    // Load and validate config.
    let toml_text = std::fs::read_to_string(&args.config)
        .with_context(|| format!("reading config file `{}`", args.config))?;
    let config = Config::parse(&toml_text)
        .with_context(|| format!("parsing config file `{}`", args.config))?;
    config
        .validate()
        .with_context(|| "config validation failed")?;

    // Daemon requires [api] section.
    let api_cfg = config
        .api
        .as_ref()
        .ok_or_else(|| anyhow::anyhow!("config file must contain an [api] section"))?;

    // Load bearer token from env var (refuse to start if unset).
    let token_value = std::env::var(&api_cfg.auth_token_env).with_context(|| {
        format!(
            "env var `{}` (named in [api].auth_token_env) is not set; \
             refusing to start without a bearer token",
            api_cfg.auth_token_env
        )
    })?;
    if token_value.is_empty() {
        bail!(
            "env var `{}` is set but empty; bearer token must be non-empty",
            api_cfg.auth_token_env
        );
    }
    let token = BearerToken(token_value);

    // Parse listen address.
    let listen_str = api_cfg.listen.as_deref().unwrap_or("127.0.0.1:7878");
    let listen_addr: SocketAddr = listen_str
        .parse()
        .with_context(|| format!("parsing [api].listen address `{listen_str}`"))?;

    // Build the sink from config.
    let sink = factory::create(&config.sink)
        .await
        .with_context(|| "initialising graph sink")?;
    let sink: Arc<dyn cpp_indexer::sink::GraphSink> = sink;

    // Run sink preflight (validates credentials + connectivity).
    sink.preflight()
        .await
        .with_context(|| "sink preflight check failed")?;

    if let Some(workspace_cfg) = config.workspace.as_ref() {
        cpp_indexer::workspace::ensure_workspace_dir(workspace_cfg)
            .with_context(|| "creating workspace directory")?;
    }

    // Build job queue.
    let queue_max = api_cfg.job_queue_max.unwrap_or(64);
    let (queue, mut rx) = JobQueue::new(queue_max);
    let repos = RepoRegistry::new();
    let stage_root = config
        .index
        .as_ref()
        .and_then(|index| index.stage_dir.clone())
        .unwrap_or_else(|| PathBuf::from(".cxg-cache/stage"));

    // Spawn worker loop.
    {
        let queue_worker = queue.clone();
        let repos_worker = repos.clone();
        let sink_worker = Arc::clone(&sink);
        let workspace_cfg = config.workspace.clone();
        let index_cfg = config.index.clone();
        let repo_cfg = config.repo.clone();
        tokio::spawn(async move {
            while let Some(msg) = rx.recv().await {
                queue_worker.mark_running(&msg.job_id);
                queue_worker.update_phase(&msg.job_id, JobPhase::Bootstrap, 0.0);

                let job_id = msg.job_id.clone();
                match run_job(
                    Arc::clone(&sink_worker),
                    workspace_cfg.clone(),
                    index_cfg.clone(),
                    repo_cfg.clone(),
                    msg,
                )
                .await
                {
                    Ok((stats, input_path, repo_name)) => {
                        queue_worker.mark_done_with_counts(
                            &job_id,
                            stats.tu_count.try_into().unwrap_or(u64::MAX),
                            stats.failed_tu_count.try_into().unwrap_or(u64::MAX),
                            stats.nodes_written,
                            stats.edges_written,
                        );
                        repos_worker.upsert(RepoInfo {
                            name: repo_name,
                            path: input_path.to_string_lossy().into_owned(),
                            last_job_id: job_id,
                        });
                    }
                    Err(e) => {
                        queue_worker.mark_failed(&job_id, e.to_string());
                    }
                }
            }
        });
    }

    // Build router.
    let state = AppState {
        queue,
        repos,
        sink,
        listen: listen_addr,
        version: env!("CARGO_PKG_VERSION").to_owned(),
        workspace_cfg: config.workspace.clone(),
        stage_root,
    };
    let router = build_router(state, token);

    // Bind and serve.
    let listener = TcpListener::bind(listen_addr)
        .await
        .with_context(|| format!("binding to {listen_addr}"))?;
    tracing::info!("cxg-daemon listening on {listen_addr}");

    axum::serve(listener, router)
        .await
        .with_context(|| "axum server error")?;

    Ok(())
}

async fn run_job(
    sink: Arc<dyn cpp_indexer::sink::GraphSink>,
    workspace_cfg: Option<WorkspaceConfig>,
    index_cfg: Option<IndexConfig>,
    repo_cfg: Option<RepoConfig>,
    msg: JobMessage,
) -> anyhow::Result<(PipelineStats, PathBuf, String)> {
    if let Some(requested_sink) = msg.options.sink.as_deref() {
        if requested_sink != sink.backend_name() {
            bail!(
                "per-job sink override `{requested_sink}` is not available; daemon is using `{}`",
                sink.backend_name()
            );
        }
    }

    let (input_path, repo_name, compile_commands) = match msg.source {
        IngestSource::Path { path } => {
            let repo_name = repo_name_for_path(&path);
            let compile_commands = repo_cfg.and_then(|cfg| cfg.compile_commands);
            (path, repo_name, compile_commands)
        }
        IngestSource::GitUrl { git_url, git_ref } => {
            let cfg = workspace_cfg
                .ok_or_else(|| anyhow::anyhow!("git_url ingest requires [workspace] config"))?;
            let repo_name = cpp_indexer::workspace::layout::repo_name_from_url(&git_url);
            let git_url_for_task = git_url.clone();
            let git_ref_for_task = git_ref.clone();
            let (_outcome, clone_path) = tokio::task::spawn_blocking(move || {
                cpp_indexer::workspace::ingest_git_url(
                    &cfg,
                    &git_url_for_task,
                    git_ref_for_task.as_deref(),
                )
            })
            .await
            .with_context(|| "workspace clone task failed")?
            .with_context(|| "git workspace ingest failed")?;
            (clone_path, repo_name, None)
        }
    };

    let stage_dir = index_cfg
        .as_ref()
        .and_then(|cfg| cfg.stage_dir.clone())
        .map(|root| root.join(&repo_name));
    let skip_phase2 =
        msg.options.skip_phase2 || index_cfg.as_ref().is_some_and(|cfg| cfg.skip_phase2);
    let skip_system_headers = index_cfg
        .as_ref()
        .map(|cfg| cfg.skip_system_headers)
        .unwrap_or(true);
    let workers = index_cfg.as_ref().and_then(|cfg| cfg.workers);

    let opts = RunOptions {
        input_path: input_path.clone(),
        compile_commands,
        repo_name: repo_name.clone(),
        stage_dir,
        skip_phase2,
        skip_system_headers,
        workers,
        skip_cache: false,
        skip_repo_node: false,
        symbol_db_path: None,
        symbol_cache_size: 100_000,
    };

    let stats = run(sink, opts).await?;
    Ok((stats, input_path, repo_name))
}

fn repo_name_for_path(path: &std::path::Path) -> String {
    let base = if path.is_file() {
        path.parent().unwrap_or(path)
    } else {
        path
    };
    base.file_name()
        .and_then(|name| name.to_str())
        .filter(|name| !name.is_empty())
        .unwrap_or("default")
        .to_owned()
}
