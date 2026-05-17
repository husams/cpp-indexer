use std::error::Error as StdError;
use std::fmt;
use std::path::PathBuf;

/// Crate-level error type. Library code uses `thiserror`; bins use `anyhow` in `main`.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),

    #[error("libclang error: {0}")]
    Clang(String),

    #[error("compile_commands.json error in {path}: {message}")]
    CompileCommands { path: PathBuf, message: String },

    #[error("could not auto-detect compile_commands.json; searched:\n{0}")]
    Autodetect(AutodetectPaths),

    #[error("sink backend '{backend}': {source}")]
    Sink {
        backend: &'static str,
        source: Box<dyn StdError + Send + Sync + 'static>,
    },

    #[error("schema error: {0}")]
    Schema(String),

    #[error("cache error: {0}")]
    Cache(String),

    #[error("workspace error: {0}")]
    Workspace(String),

    #[error("API error: {0}")]
    Api(String),
}

/// Newtype for display of searched paths in `Autodetect`.
#[derive(Debug)]
pub struct AutodetectPaths(pub Vec<PathBuf>);

impl fmt::Display for AutodetectPaths {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for (i, p) in self.0.iter().enumerate() {
            if i > 0 {
                writeln!(f)?;
            }
            write!(f, "  {}", p.display())?;
        }
        Ok(())
    }
}

/// Convenience alias so callers can write `Result<T>` instead of `Result<T, Error>`.
pub type Result<T> = std::result::Result<T, Error>;

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    #[test]
    fn autodetect_display_lists_every_path() {
        let paths = vec![
            PathBuf::from("/some/project/build"),
            PathBuf::from("/some/project/out"),
            PathBuf::from("/some/project/cmake-build-debug"),
        ];
        let err = Error::Autodetect(AutodetectPaths(paths.clone()));
        let rendered = err.to_string();

        for p in &paths {
            assert!(
                rendered.contains(p.to_str().unwrap()),
                "expected path {:?} in display output, got: {rendered}",
                p
            );
        }
    }
}
