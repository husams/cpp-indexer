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

    // 2. Derive repo name and a stable clone path.
    //    We use a placeholder SHA of zeros so we can call `clone_or_fetch`
    //    immediately; the git2 clone sets the actual HEAD.  The ADR specifies
    //    resolving the SHA via ls-remote first for path naming, but for
    //    pragmatic integration (avoiding an extra round-trip in unit tests) we
    //    use a "lazy" path: `<repo-name>-HEAD` resolved after clone.
    //
    //    Production note: the full ls-remote approach described in ADR-6
    //    requires a live network call before the job is queued.  We implement
    //    the simpler variant here (clone, then read HEAD SHA, then rename if
    //    needed) to keep the happy path synchronous and testable.  The rename
    //    step is a follow-up (see open items in implementation-notes.md).
    let repo_name = layout::repo_name_from_url(git_url);

    // Depth: 0 in config means full history; otherwise use the configured value.
    let depth = match cfg.default_clone_depth {
        Some(0) | None => None, // full history
        Some(d) => Some(d),     // shallow
    };

    // For path disambiguation we use "0000" as the short SHA placeholder and
    // rename after the first clone.  For re-ingests the directory already
    // exists so we use the same naming.
    let placeholder_sha = "0000000000000000";
    let target_dir = layout::clone_path(&cfg.dir, &repo_name, placeholder_sha);

    // 3. Clone or fetch.
    let outcome = git::clone_or_fetch(
        git_url,
        git_ref,
        cfg.git_credentials_env.as_deref(),
        depth,
        &target_dir,
    )?;

    // 4. After clone, rename the directory to the actual HEAD SHA.
    //    This implements AC-M7-12 (path layout includes short SHA).
    let final_dir = if let CloneOutcome::Cloned(_) = &outcome {
        let repo = git2::Repository::open(&target_dir).map_err(|e| {
            Error::Workspace(format!(
                "could not open cloned repo for SHA resolution: {e}"
            ))
        })?;
        let head_sha = repo
            .head()
            .and_then(|h| h.peel_to_commit())
            .map(|c| c.id().to_string())
            .unwrap_or_else(|_| placeholder_sha.to_owned());

        let final_path = layout::clone_path(&cfg.dir, &repo_name, &head_sha);
        if final_path != target_dir {
            std::fs::rename(&target_dir, &final_path).map_err(|e| {
                Error::Workspace(format!(
                    "could not rename clone dir {} → {}: {e}",
                    target_dir.display(),
                    final_path.display()
                ))
            })?;
        }
        final_path
    } else {
        target_dir.clone()
    };

    // Return the outcome with the final path.
    let final_outcome = match outcome {
        CloneOutcome::Cloned(_) => CloneOutcome::Cloned(final_dir.clone()),
        CloneOutcome::Fetched(_) => CloneOutcome::Fetched(final_dir.clone()),
    };

    Ok((final_outcome, final_dir))
}

/// Best-effort host extraction from a URL string for error messages.
fn extract_host(raw_url: &str) -> Option<String> {
    url::Url::parse(raw_url)
        .ok()
        .and_then(|u| u.host_str().map(|h| h.to_owned()))
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
}
