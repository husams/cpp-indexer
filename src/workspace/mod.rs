//! Workspace git2 clone manager for `cxg-daemon`.
//!
//! Entry point: [`ingest_git_url`].  Performs:
//! 1. Allowlist check → `Error::Workspace` if rejected (caller converts to 403).
//! 2. Optional ls-remote SHA resolution for path layout.
//! 3. Clone-path derivation via [`layout`].
//! 4. Clone or fetch via [`git`].
//!
//! ADR-6.  AC-M7-12..16.

pub mod allowlist;
pub mod git;
pub mod layout;

pub use git::CloneOutcome;

use std::path::PathBuf;

use crate::{
    config::WorkspaceConfig,
    error::{Error, Result},
};

/// Ensure `[workspace].dir` exists with restricted permissions.
///
/// Called once at daemon startup.  No-op if the directory already exists.
pub fn ensure_workspace_dir(cfg: &WorkspaceConfig) -> Result<()> {
    let dir = &cfg.dir;
    if dir.exists() {
        return Ok(());
    }
    std::fs::create_dir_all(dir)?;

    // Set `0700` permissions on Unix.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(dir, std::fs::Permissions::from_mode(0o700))?;
    }

    Ok(())
}

/// Top-level git-URL ingest entry point.
///
/// Validates the URL against the allowlist, derives the clone path, then
/// performs a fresh clone or a fetch (re-ingest).  Returns the local clone
/// path on success.
///
/// Callers must convert the returned path into an [`IngestSource::Path`] job
/// and enqueue it.
///
/// # Errors
/// - `Error::Workspace("host not allowed: …")` if allowlist rejects the URL.
/// - `Error::Workspace(…)` for git2 failures (PAT scrubbed from messages).
pub fn ingest_git_url(
    cfg: &WorkspaceConfig,
    git_url: &str,
    git_ref: Option<&str>,
) -> Result<(CloneOutcome, PathBuf)> {
    // 1. Allowlist check (before any network call).
    if !allowlist::is_allowed(git_url, &cfg.allowed_hosts) {
        let host = extract_host(git_url).unwrap_or_else(|| git_url.to_owned());
        return Err(Error::Workspace(format!("host not allowed: {host}")));
    }

    // 2. Derive repo name and resolve the target commit before choosing the
    //    clone path. This keeps re-ingest stable: the same URL/ref maps to the
    //    same `<repo>-<shortsha>` directory and `clone_or_fetch` can fetch.
    let repo_name = layout::repo_name_from_url(git_url);
    let commit_sha =
        git::resolve_remote_commit(git_url, git_ref, cfg.git_credentials_env.as_deref())?;

    // Depth: 0 in config means full history; otherwise use the configured value.
    let depth = clone_depth(cfg.default_clone_depth);

    let target_dir = layout::clone_path(&cfg.dir, &repo_name, &commit_sha);

    // 3. Clone or fetch.
    let outcome = git::clone_or_fetch(
        git_url,
        git_ref,
        cfg.git_credentials_env.as_deref(),
        depth,
        &target_dir,
    )?;

    Ok((outcome, target_dir))
}

/// Best-effort host extraction from a URL string for error messages.
fn extract_host(raw_url: &str) -> Option<String> {
    url::Url::parse(raw_url)
        .ok()
        .and_then(|u| u.host_str().map(|h| h.to_owned()))
}

fn clone_depth(configured: Option<u32>) -> Option<u32> {
    match configured {
        Some(0) => None,
        Some(d) => Some(d),
        None => Some(1),
    }
}

// ── Tests ──────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;
    use tempfile::TempDir;

    fn make_workspace_config(dir: &Path, allowed_hosts: &[&str]) -> WorkspaceConfig {
        WorkspaceConfig {
            dir: dir.to_path_buf(),
            allowed_hosts: allowed_hosts.iter().map(|s| s.to_string()).collect(),
            git_credentials_env: None,
            default_clone_depth: Some(1),
        }
    }

    // Disallowed host → Error::Workspace("host not allowed: …")
    #[test]
    fn disallowed_host_returns_workspace_error() {
        let ws_dir = TempDir::new().unwrap();
        let cfg = make_workspace_config(ws_dir.path(), &["github.com"]);

        let err = ingest_git_url(&cfg, "https://evil.example.com/repo", None).unwrap_err();
        match err {
            Error::Workspace(msg) => {
                assert!(msg.contains("host not allowed"), "unexpected msg: {msg}");
                assert!(
                    msg.contains("evil.example.com"),
                    "must name the host: {msg}"
                );
            }
            other => panic!("expected Error::Workspace, got {other:?}"),
        }
    }

    #[test]
    fn missing_clone_depth_defaults_to_shallow_one() {
        assert_eq!(clone_depth(None), Some(1));
        assert_eq!(clone_depth(Some(1)), Some(1));
        assert_eq!(clone_depth(Some(0)), None);
    }
}
