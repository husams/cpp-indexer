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

use std::{net::SocketAddr, sync::Arc};

use anyhow::{bail, Context};
use clap::Parser;
use tokio::net::TcpListener;

use cpp_indexer::{
    api::{
        auth::BearerToken,
        jobs::{JobQueue, RepoRegistry},
        routes::{build_router, AppState},
    },
    config::Config,
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

    // Build job queue.
    let queue_max = api_cfg.job_queue_max.unwrap_or(64);
    let (queue, mut rx) = JobQueue::new(queue_max);
    let repos = RepoRegistry::new();

    // Spawn worker loop.
    {
        let queue_worker = queue.clone();
        tokio::spawn(async move {
            while let Some(msg) = rx.recv().await {
                queue_worker.mark_running(&msg.job_id);
                // TODO(S36/S37): wire pipeline::run here.
                // For now, mark done immediately (placeholder).
                queue_worker.mark_done(&msg.job_id);
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
