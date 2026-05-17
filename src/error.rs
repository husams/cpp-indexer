//! Crate-wide error type.

/// Crate-wide result alias.
pub type Result<T> = std::result::Result<T, Error>;

/// All errors produced by the `cpp_indexer` library.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// I/O error.
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),

    /// Configuration parse or validation error.
    #[error("config error at `{field}`: {detail}")]
    Config { field: String, detail: String },

    /// Sink (DB backend) error.
    #[error("sink error (backend={backend}): {source}")]
    Sink {
        backend: &'static str,
        source: Box<dyn std::error::Error + Send + Sync>,
    },

    /// libclang error.
    #[error("libclang error: {0}")]
    Clang(String),

    /// compile_commands.json parse or I/O error.
    #[error("compile_commands error: {0}")]
    CompileCommands(String),

    /// Auto-detect error: lists every directory probed.
    #[error(
        "compile_commands.json not found; searched: {}",
        searched.iter().map(|p| p.display().to_string()).collect::<Vec<_>>().join(", ")
    )]
    Autodetect { searched: Vec<std::path::PathBuf> },

    /// Parquet schema or staging error.
    #[error("schema error: {0}")]
    Schema(String),

    /// Manifest cache error.
    #[error("cache error: {0}")]
    Cache(String),

    /// git2 workspace error.
    #[error("workspace error: {0}")]
    Workspace(String),

    /// API / REST control plane error.
    #[error("api error: {0}")]
    Api(String),
}
