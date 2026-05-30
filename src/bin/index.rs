//! `cxg-index` — index a C++ repository into a graph database.
//!
//! Usage: `cxg-index [OPTIONS] <PATH>`
//!
//! `PATH` may be a source file, a directory, or a repository root. When no
//! `--compile-commands` flag is provided, `cxg-index` auto-detects
//! `compile_commands.json` by walking upward from `PATH` (Phase 0.5).

use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::Arc;

use anyhow::Context as _;
use clap::Parser;
use tracing::info;

use cpp_indexer::config::Config;
use cpp_indexer::pipeline::{run, RunOptions};
use cpp_indexer::sink::factory;
use cpp_indexer::visit::modules_cpp20;

// ---------------------------------------------------------------------------
// --fail-on-tu-error flag type
// ---------------------------------------------------------------------------

/// Controls when a non-zero failed-TU fraction causes exit code 2.
#[derive(Debug, Clone, Copy)]
enum FailOnTuError {
    /// Never exit 2 due to failed TUs; the flag is a no-op.
    Never,
    /// Exit 2 when `failed / total >= ratio`. Invariant: `0.0 <= ratio <= 1.0`.
    Ratio(f64),
}

impl std::str::FromStr for FailOnTuError {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        if s.eq_ignore_ascii_case("never") {
            return Ok(Self::Never);
        }
        let r: f64 = s
            .parse()
            .map_err(|_| format!("expected a number in [0.0, 1.0] or `never`, got `{s}`"))?;
        if r.is_nan() || !(0.0..=1.0).contains(&r) {
            return Err(format!("ratio must be in [0.0, 1.0], got `{r}`"));
        }
        Ok(Self::Ratio(r))
    }
}

impl Default for FailOnTuError {
    fn default() -> Self {
        Self::Ratio(1.0)
    }
}

impl FailOnTuError {
    /// Return the process exit code based on the failed / total TU counts.
    ///
    /// - `Never` → always `0`.
    /// - `failed == 0` → `0` (no failures means success regardless of ratio).
    /// - `failed / total >= ratio` → `2`; otherwise `0`.
    fn exit_code(&self, failed: usize, total: usize) -> u8 {
        match self {
            FailOnTuError::Never => 0,
            FailOnTuError::Ratio(r) => {
                if failed == 0 {
                    return 0;
                }
                // total >= 1 because failed >= 1 implies total >= 1
                let ratio = failed as f64 / total as f64;
                if ratio >= *r {
                    2
                } else {
                    0
                }
            }
        }
    }
}

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
    #[arg(long)]
    backend: Option<String>,

    /// Neo4j/IndraDB URI (e.g. `bolt://localhost:7687` or `http://localhost:27615`).
    #[arg(long)]
    db_uri: Option<String>,

    /// Neo4j username (default `neo4j`).
    #[arg(long)]
    neo4j_user: Option<String>,

    /// Env var whose value is the Neo4j password (default `NEO4J_PASSWORD`).
    #[arg(long)]
    neo4j_password_env: Option<String>,

    /// Env var whose value is the IndraDB auth token (optional).
    #[arg(long)]
    indradb_token_env: Option<String>,

    /// LRU cache capacity for the per-repo symbol/file allocator.
    ///
    /// Overrides `[index].symbol_cache_size` in the config file and the
    /// `CXG_SYMBOL_CACHE_SIZE` env var.  Set to `0` to disable the cache.
    #[arg(long, value_name = "N")]
    symbol_cache_size: Option<usize>,

    /// Explicit path for the per-repo SQLite symbol map (`cxg-symbols.db`).
    ///
    /// Overrides `[index].symbol_db_path` in the config file and the
    /// `CXG_SYMBOL_DB_PATH` env var.  When absent the default
    /// `<stage_dir>/cxg-symbols.db` is used at the allocator use site.
    #[arg(long, value_name = "PATH")]
    symbol_db_path: Option<PathBuf>,

    /// Path to a `cxg-index.toml` config file. CLI flags override file settings.
    #[arg(long, value_name = "FILE")]
    config: Option<PathBuf>,

    /// Exit with code 2 when the fraction of failed translation units meets or
    /// exceeds RATIO. `never` disables the check. Default: 1.0 (exit 2 only
    /// when every TU fails).
    #[arg(
        long = "fail-on-tu-error",
        value_name = "RATIO|never",
        default_value = "1.0"
    )]
    fail_on_tu_error: FailOnTuError,
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

#[tokio::main]
async fn main() -> anyhow::Result<ExitCode> {
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
        return Ok(ExitCode::SUCCESS);
    }

    let file_config = load_config(cli.config.as_ref())?;

    // Build a SinkConfig from config + CLI flags before moving fields out of cli.
    let sink_config = build_sink_config(&cli, file_config.as_ref())?;
    let backend_name_hint = sink_config.backend.clone();

    let input_path = cli
        .input_path
        .clone()
        .or_else(|| {
            file_config
                .as_ref()
                .and_then(|cfg| cfg.repo.as_ref().map(|repo| repo.path.clone()))
        })
        .ok_or_else(|| anyhow::anyhow!("PATH argument is required; run with --help for usage"))?;
    let compile_commands = cli.compile_commands.clone().or_else(|| {
        file_config.as_ref().and_then(|cfg| {
            cfg.repo
                .as_ref()
                .and_then(|repo| repo.compile_commands.clone())
        })
    });
    let stage_dir = cli.stage_dir.clone().or_else(|| {
        file_config
            .as_ref()
            .and_then(|cfg| cfg.index.as_ref().and_then(|index| index.stage_dir.clone()))
    });
    let skip_phase2 = cli.skip_phase2
        || file_config
            .as_ref()
            .and_then(|cfg| cfg.index.as_ref().map(|index| index.skip_phase2))
            .unwrap_or(false);
    let skip_system_headers = if cli.include_system_headers {
        false
    } else {
        file_config
            .as_ref()
            .and_then(|cfg| cfg.index.as_ref().map(|index| index.skip_system_headers))
            .unwrap_or(true)
    };
    let workers = file_config
        .as_ref()
        .and_then(|cfg| cfg.index.as_ref().and_then(|index| index.workers));

    // Resolve symbol_cache_size and symbol_db_path: CLI > env > file > default
    // (S3-SC-01..06).  The shared helpers in config::mod live under --lib so
    // tests in config/mod.rs cover the precedence logic directly.
    let env_cache_size = cpp_indexer::config::env::resolve_symbol_cache_size()
        .context("reading CXG_SYMBOL_CACHE_SIZE")?;
    let env_db_path = cpp_indexer::config::env::resolve_symbol_db_path();
    let file_cache_size = file_config
        .as_ref()
        .and_then(|cfg| cfg.index.as_ref())
        .map(|idx| idx.symbol_cache_size);
    let file_db_path = file_config
        .as_ref()
        .and_then(|cfg| cfg.index.as_ref())
        .and_then(|idx| idx.symbol_db_path.clone());

    let symbol_cache_size = cpp_indexer::config::resolve_symbol_cache_size(
        cli.symbol_cache_size,
        env_cache_size,
        file_cache_size,
    );
    let symbol_db_path = cpp_indexer::config::resolve_symbol_db_path(
        cli.symbol_db_path.clone(),
        env_db_path,
        file_db_path,
    );

    // These values are threaded into the pipeline in Story 3 (SymbolAllocator
    // construction in parallel.rs).  Held here so they compile and are testable.
    let _ = (symbol_cache_size, symbol_db_path);

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
        compile_commands,
        repo_name: cli.repo_name,
        stage_dir,
        skip_phase2,
        skip_system_headers,
        workers,
        skip_cache: false,
        skip_repo_node: false,
        symbol_db_path: None,
        symbol_cache_size: 100_000,
    };

    let stats = run(Arc::clone(&sink), opts)
        .await
        .context("pipeline failed")?;

    eprintln!("{}", stats.closing_summary());

    let code = cli
        .fail_on_tu_error
        .exit_code(stats.failed_tu_count, stats.tu_count);
    Ok(ExitCode::from(code))
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn load_config(path: Option<&PathBuf>) -> anyhow::Result<Option<Config>> {
    let Some(path) = path else {
        return Ok(None);
    };
    let text = std::fs::read_to_string(path)
        .with_context(|| format!("reading config file `{}`", path.display()))?;
    let config = Config::parse(&text)
        .with_context(|| format!("parsing config file `{}`", path.display()))?;
    config
        .validate()
        .with_context(|| format!("validating config file `{}`", path.display()))?;
    Ok(Some(config))
}

fn build_sink_config(
    cli: &Cli,
    file_config: Option<&Config>,
) -> anyhow::Result<cpp_indexer::config::SinkConfig> {
    use cpp_indexer::config::{IndraDbSinkConfig, Neo4jSinkConfig, SinkConfig};

    let backend = cli
        .backend
        .clone()
        .or_else(|| file_config.map(|cfg| cfg.sink.backend.clone()))
        .unwrap_or_else(|| "neo4j".to_owned());
    let batch_size = file_config.and_then(|cfg| cfg.sink.batch_size);

    match backend.as_str() {
        "neo4j" => {
            let configured = file_config.and_then(|cfg| cfg.sink.neo4j.as_ref());
            let uri = cli
                .db_uri
                .clone()
                .or_else(|| configured.map(|cfg| cfg.uri.clone()))
                .unwrap_or_else(|| "bolt://localhost:7687".to_owned());
            Ok(SinkConfig {
                backend: "neo4j".to_owned(),
                batch_size,
                neo4j: Some(Neo4jSinkConfig {
                    uri,
                    user: cli
                        .neo4j_user
                        .clone()
                        .or_else(|| configured.map(|cfg| cfg.user.clone()))
                        .unwrap_or_else(|| "neo4j".to_owned()),
                    password_env: cli
                        .neo4j_password_env
                        .clone()
                        .or_else(|| configured.map(|cfg| cfg.password_env.clone()))
                        .unwrap_or_else(|| "NEO4J_PASSWORD".to_owned()),
                    sessions: configured.and_then(|cfg| cfg.sessions),
                }),
                indradb: None,
            })
        }
        "indradb" => {
            let configured = file_config.and_then(|cfg| cfg.sink.indradb.as_ref());
            let uri = cli
                .db_uri
                .clone()
                .or_else(|| configured.map(|cfg| cfg.endpoint.clone()))
                .unwrap_or_else(|| "http://localhost:27615".to_owned());
            Ok(SinkConfig {
                backend: "indradb".to_owned(),
                batch_size,
                neo4j: None,
                indradb: Some(IndraDbSinkConfig {
                    endpoint: uri,
                    token_env: cli
                        .indradb_token_env
                        .clone()
                        .or_else(|| configured.and_then(|cfg| cfg.token_env.clone())),
                    sessions: configured.and_then(|cfg| cfg.sessions),
                }),
            })
        }
        other => anyhow::bail!("unknown backend `{other}`; expected \"neo4j\" or \"indradb\""),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cli_defaults() -> Cli {
        Cli {
            print_version: false,
            input_path: None,
            compile_commands: None,
            repo_name: "default".to_owned(),
            stage_dir: None,
            skip_phase2: false,
            include_system_headers: false,
            backend: None,
            db_uri: None,
            neo4j_user: None,
            neo4j_password_env: None,
            indradb_token_env: None,
            symbol_cache_size: None,
            symbol_db_path: None,
            config: None,
            fail_on_tu_error: FailOnTuError::default(),
        }
    }

    // -----------------------------------------------------------------------
    // FailOnTuError::from_str round-trip and invalid-input cases
    // -----------------------------------------------------------------------

    #[test]
    fn from_str_never_case_insensitive() {
        let variants = ["never", "Never", "NEVER", "nEvEr"];
        for v in variants {
            let parsed: FailOnTuError = v
                .parse()
                .unwrap_or_else(|e| panic!("expected `never` to parse, got error for `{v}`: {e}"));
            assert!(
                matches!(parsed, FailOnTuError::Never),
                "expected Never for `{v}`"
            );
        }
    }

    #[test]
    fn from_str_ratio_valid_boundaries() {
        for (s, expected) in [("0.0", 0.0_f64), ("1.0", 1.0), ("0.5", 0.5), ("0.25", 0.25)] {
            match s.parse::<FailOnTuError>().unwrap() {
                FailOnTuError::Ratio(r) => {
                    assert!(
                        (r - expected).abs() < f64::EPSILON,
                        "ratio mismatch for `{s}`: got {r}"
                    );
                }
                other => panic!("expected Ratio for `{s}`, got {other:?}"),
            }
        }
    }

    #[test]
    fn from_str_invalid_above_1() {
        let err = "1.5".parse::<FailOnTuError>().unwrap_err();
        assert!(
            err.contains("ratio must be in"),
            "error should mention ratio range; got: {err}"
        );
    }

    #[test]
    fn from_str_invalid_below_0() {
        let err = "-0.1".parse::<FailOnTuError>().unwrap_err();
        assert!(
            err.contains("ratio must be in"),
            "error should mention ratio range; got: {err}"
        );
    }

    #[test]
    fn from_str_invalid_non_numeric() {
        let err = "garbage".parse::<FailOnTuError>().unwrap_err();
        assert!(
            err.contains("expected a number"),
            "error should describe expected format; got: {err}"
        );
    }

    #[test]
    fn from_str_invalid_nan() {
        // "NaN" is a valid f64 parse target but must be rejected.
        let err = "NaN".parse::<FailOnTuError>().unwrap_err();
        assert!(
            !err.is_empty(),
            "NaN should produce a non-empty error; got: {err}"
        );
    }

    // -----------------------------------------------------------------------
    // FailOnTuError::exit_code logic (unit table)
    // -----------------------------------------------------------------------

    #[test]
    fn exit_code_ratio_1_all_fail() {
        // ratio 1.0, all fail → 5/5 = 1.0 >= 1.0 → exit 2
        assert_eq!(FailOnTuError::Ratio(1.0).exit_code(5, 5), 2);
    }

    #[test]
    fn exit_code_ratio_1_partial_fail() {
        // ratio 1.0, partial fail → 2/5 = 0.4 < 1.0 → exit 0
        assert_eq!(FailOnTuError::Ratio(1.0).exit_code(2, 5), 0);
    }

    #[test]
    fn exit_code_ratio_0_any_fail() {
        // ratio 0.0, 1 failure → 1/5 = 0.2 >= 0.0 → exit 2
        assert_eq!(FailOnTuError::Ratio(0.0).exit_code(1, 5), 2);
    }

    #[test]
    fn exit_code_ratio_0_no_fail() {
        // ratio 0.0, 0 failures → failed == 0 short-circuit → exit 0
        assert_eq!(FailOnTuError::Ratio(0.0).exit_code(0, 5), 0);
    }

    #[test]
    fn exit_code_never_all_fail() {
        // Never variant → always exit 0
        assert_eq!(FailOnTuError::Never.exit_code(5, 5), 0);
    }

    #[test]
    fn exit_code_never_no_fail() {
        assert_eq!(FailOnTuError::Never.exit_code(0, 5), 0);
    }

    #[test]
    fn exit_code_zero_tu_zero_fail() {
        // 0 TUs, 0 failures → exit 0 (0/0 edge case; resolved by failed==0 short-circuit)
        assert_eq!(FailOnTuError::Ratio(0.0).exit_code(0, 0), 0);
        assert_eq!(FailOnTuError::Ratio(1.0).exit_code(0, 0), 0);
    }

    #[test]
    fn exit_code_boundary_exactly_met() {
        // ratio 0.5, 2 of 4 fail → 0.5 >= 0.5 → exit 2
        assert_eq!(FailOnTuError::Ratio(0.5).exit_code(2, 4), 2);
    }

    #[test]
    fn exit_code_boundary_just_below() {
        // ratio 0.5, 1 of 4 fail → 0.25 < 0.5 → exit 0
        assert_eq!(FailOnTuError::Ratio(0.5).exit_code(1, 4), 0);
    }

    #[test]
    fn default_is_ratio_1() {
        assert!(
            matches!(FailOnTuError::default(), FailOnTuError::Ratio(r) if (r - 1.0).abs() < f64::EPSILON)
        );
    }

    #[test]
    fn config_file_values_feed_sink_config() {
        let text = std::fs::read_to_string("tests/fixtures/config/cxg-index-golden.toml").unwrap();
        let config = Config::parse(&text).unwrap();
        let sink = build_sink_config(&cli_defaults(), Some(&config)).unwrap();
        let neo4j = sink.neo4j.unwrap();
        assert_eq!(sink.backend, "neo4j");
        assert_eq!(neo4j.uri, "bolt://localhost:7687");
        assert_eq!(neo4j.user, "neo4j");
        assert_eq!(neo4j.password_env, "NEO4J_PASSWORD");
        assert_eq!(neo4j.sessions, Some(16));
    }

    #[test]
    fn cli_flags_override_config_sink_values() {
        let text = std::fs::read_to_string("tests/fixtures/config/cxg-index-golden.toml").unwrap();
        let config = Config::parse(&text).unwrap();
        let mut cli = cli_defaults();
        cli.db_uri = Some("bolt://override:7687".to_owned());
        cli.neo4j_user = Some("override-user".to_owned());
        cli.neo4j_password_env = Some("OVERRIDE_PASSWORD".to_owned());

        let sink = build_sink_config(&cli, Some(&config)).unwrap();
        let neo4j = sink.neo4j.unwrap();
        assert_eq!(neo4j.uri, "bolt://override:7687");
        assert_eq!(neo4j.user, "override-user");
        assert_eq!(neo4j.password_env, "OVERRIDE_PASSWORD");
    }
}
