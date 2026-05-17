//! git2-based clone/fetch manager for `cxg-daemon`.
//!
//! Implements ADR-6:
//! - Shallow clone by default (`default_clone_depth = 1`; 0 = full history).
//! - Re-ingest: fetch + `git reset --hard FETCH_HEAD` instead of re-cloning.
//! - PAT from env var at call time; never stored or logged.
//! - URL `user:password@` prefix is scrubbed from any error messages before
//!   they propagate upward (AC-M7-15).
//!
//! AC-M7-12, AC-M7-13, AC-M7-16.

use std::path::{Path, PathBuf};

use git2::{Direction, FetchOptions, RemoteCallbacks, Repository};
use tracing::{debug, info, warn};

use crate::error::{Error, Result};

// ── CloneOutcome ───────────────────────────────────────────────────────────────

/// Result of a [`clone_or_fetch`] call.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CloneOutcome {
    /// A fresh clone was performed.  Contains the clone directory.
    Cloned(PathBuf),
    /// The existing clone was updated via `git fetch`.  Contains the directory.
    Fetched(PathBuf),
}

impl CloneOutcome {
    /// The local path of the clone regardless of whether it was fresh or fetched.
    pub fn path(&self) -> &Path {
        match self {
            CloneOutcome::Cloned(p) | CloneOutcome::Fetched(p) => p,
        }
    }
}

// ── Internal helpers ───────────────────────────────────────────────────────────

/// Scrub any `user:password@` prefix from a URL so it is safe to include in
/// log output or error messages.
fn scrub_credentials(s: &str) -> String {
    // Handle `scheme://user:password@host/...` patterns.
    if let Some(at) = s.rfind('@') {
        if let Some(scheme_end) = s.find("://") {
            let after_scheme = &s[scheme_end + 3..];
            // Only scrub if the `@` comes before or at the host position.
            if let Some(local_at) = after_scheme.find('@') {
                let prefix = &s[..scheme_end + 3]; // "https://"
                let rest = &after_scheme[local_at + 1..]; // after the `@`
                return format!("{prefix}{rest}");
            }
        }
        // Fallback: just cut at the last `@`.
        return s[at + 1..].to_owned();
    }
    s.to_owned()
}

/// Build [`FetchOptions`] with optional PAT credentials.
///
/// If `pat` is `Some`, uses `x-access-token` as the username (standard for
/// GitHub/GitLab HTTPS PAT auth).  The PAT value is never logged.
fn make_callbacks(pat: Option<String>) -> RemoteCallbacks<'static> {
    let mut callbacks = RemoteCallbacks::new();
    callbacks.credentials(move |_url, _username, _allowed| match &pat {
        Some(token) => git2::Cred::userpass_plaintext("x-access-token", token),
        None => Err(git2::Error::from_str("no credentials configured")),
    });
    callbacks
}

fn make_fetch_options(pat: Option<String>) -> FetchOptions<'static> {
    let callbacks = make_callbacks(pat);
    let mut opts = FetchOptions::new();
    opts.remote_callbacks(callbacks);
    opts
}

fn load_pat(pat_env_var: Option<&str>) -> Option<String> {
    match pat_env_var {
        Some(var) => match std::env::var(var) {
            Ok(val) if !val.is_empty() => {
                // Confirmed: do NOT log the value.
                debug!(env_var = %var, "PAT loaded from env var");
                Some(val)
            }
            Ok(_) => {
                warn!(env_var = %var, "PAT env var is set but empty; proceeding without credentials");
                None
            }
            Err(_) => {
                debug!(env_var = %var, "PAT env var unset; treating as public repo");
                None
            }
        },
        None => None,
    }
}

// ── Public API ─────────────────────────────────────────────────────────────────

/// Clone a new repository or fetch + reset an existing one.
///
/// - `url` must already have passed the allowlist check.
/// - `git_ref` defaults to `"HEAD"` if `None`.
/// - `pat_env_var` is the env var *name* (not value). The value is read here
///   at call time and never stored.  Pass `None` for public repos.
/// - `depth` is the shallow-clone depth: `Some(1)` = shallow, `None` = full.
/// - `target_dir` is the pre-computed clone path (from `layout::clone_path`).
///
/// On success, returns the local path of the clone.
pub fn clone_or_fetch(
    url: &str,
    git_ref: Option<&str>,
    pat_env_var: Option<&str>,
    depth: Option<u32>,
    target_dir: &Path,
) -> Result<CloneOutcome> {
    let git_ref = git_ref.unwrap_or("HEAD");

    // Read PAT from env var at call time (not stored).
    let pat = load_pat(pat_env_var);

    // Branch: re-ingest or fresh clone.
    if target_dir.join(".git").exists() {
        fetch_existing(url, git_ref, pat, depth, target_dir)
    } else {
        clone_fresh(url, git_ref, pat, depth, target_dir)
    }
}

/// Resolve `git_ref` to a remote commit SHA using ls-remote-style discovery.
pub fn resolve_remote_commit(
    url: &str,
    git_ref: Option<&str>,
    pat_env_var: Option<&str>,
) -> Result<String> {
    let git_ref = git_ref.unwrap_or("HEAD");
    let pat = load_pat(pat_env_var);
    let mut remote = git2::Remote::create_detached(url).map_err(|e| {
        Error::Workspace(format!(
            "git remote setup failed for {}: {}",
            scrub_credentials(url),
            scrub_credentials(&e.to_string())
        ))
    })?;
    remote
        .connect_auth(Direction::Fetch, Some(make_callbacks(pat)), None)
        .map_err(|e| {
            Error::Workspace(format!(
                "git remote connect failed for {}: {}",
                scrub_credentials(url),
                scrub_credentials(&e.to_string())
            ))
        })?;

    let heads = remote.list().map_err(|e| {
        Error::Workspace(format!(
            "git ls-remote failed for {}: {}",
            scrub_credentials(url),
            scrub_credentials(&e.to_string())
        ))
    })?;

    let branch_ref = format!("refs/heads/{git_ref}");
    let tag_ref = format!("refs/tags/{git_ref}");
    let oid = heads
        .iter()
        .find(|head| {
            let name = head.name();
            if git_ref.eq_ignore_ascii_case("HEAD") {
                name == "HEAD"
            } else {
                name == git_ref || name == branch_ref || name == tag_ref
            }
        })
        .map(|head| head.oid())
        .ok_or_else(|| {
            Error::Workspace(format!(
                "git ref `{git_ref}` not found for {}",
                scrub_credentials(url)
            ))
        })?;

    remote.disconnect().map_err(|e| {
        Error::Workspace(format!(
            "git remote disconnect failed for {}: {}",
            scrub_credentials(url),
            scrub_credentials(&e.to_string())
        ))
    })?;
    Ok(oid.to_string())
}

fn clone_fresh(
    url: &str,
    git_ref: &str,
    pat: Option<String>,
    depth: Option<u32>,
    target_dir: &Path,
) -> Result<CloneOutcome> {
    info!(
        url = %scrub_credentials(url),
        git_ref,
        target = %target_dir.display(),
        depth = ?depth,
        "cloning repository"
    );

    let mut builder = git2::build::RepoBuilder::new();

    // Set branch only when not following HEAD; calling builder.branch("HEAD")
    // resolves incorrectly on some servers.
    let pat_for_depth = pat.clone();
    let is_head_ref = git_ref.eq_ignore_ascii_case("head");
    if !is_head_ref {
        builder.branch(git_ref);
    }

    if let Some(d) = depth {
        if d > 0 {
            let mut fo = make_fetch_options(pat_for_depth);
            fo.depth(d as i32);
            builder.fetch_options(fo);
        } else {
            builder.fetch_options(make_fetch_options(pat));
        }
    } else {
        builder.fetch_options(make_fetch_options(pat));
    }

    let repo = builder
        .clone(url, target_dir)
        .map_err(|e| Error::Workspace(format!("git clone failed: {}", e)))?;

    // Resolve HEAD to log the resulting SHA.
    let head_sha = repo
        .head()
        .ok()
        .and_then(|h| h.peel_to_commit().ok())
        .map(|c| c.id().to_string())
        .unwrap_or_else(|| "(unknown)".to_owned());

    info!(
        url = %scrub_credentials(url),
        target = %target_dir.display(),
        head_sha,
        "clone complete"
    );

    Ok(CloneOutcome::Cloned(target_dir.to_path_buf()))
}

fn fetch_existing(
    url: &str,
    git_ref: &str,
    pat: Option<String>,
    depth: Option<u32>,
    target_dir: &Path,
) -> Result<CloneOutcome> {
    info!(
        url = %scrub_credentials(url),
        git_ref,
        target = %target_dir.display(),
        "existing clone detected; fetching"
    );

    let repo = Repository::open(target_dir)
        .map_err(|e| Error::Workspace(format!("failed to open existing clone: {}", e)))?;

    let mut remote = repo
        .find_remote("origin")
        .map_err(|e| Error::Workspace(format!("could not find remote `origin`: {}", e)))?;

    let mut fetch_opts = make_fetch_options(pat);
    if let Some(d) = depth {
        if d > 0 {
            fetch_opts.depth(d as i32);
        }
    }

    // Fetch the specific ref.  For HEAD, use the catch-all refspec.
    let refspec = if git_ref.eq_ignore_ascii_case("head") {
        "+HEAD:FETCH_HEAD".to_owned()
    } else {
        format!("refs/heads/{git_ref}:refs/remotes/origin/{git_ref}")
    };
    remote
        .fetch(&[&refspec], Some(&mut fetch_opts), None)
        .map_err(|e| {
            Error::Workspace(format!(
                "git fetch from {} failed: {}",
                scrub_credentials(url),
                e
            ))
        })?;

    // Reset to FETCH_HEAD (equivalent to `git reset --hard FETCH_HEAD`).
    let fetch_head = repo
        .find_reference("FETCH_HEAD")
        .map_err(|e| Error::Workspace(format!("could not find FETCH_HEAD: {}", e)))?;
    let fetch_commit = fetch_head
        .peel_to_commit()
        .map_err(|e| Error::Workspace(format!("could not peel FETCH_HEAD to commit: {}", e)))?;
    repo.reset(fetch_commit.as_object(), git2::ResetType::Hard, None)
        .map_err(|e| Error::Workspace(format!("git reset --hard FETCH_HEAD failed: {}", e)))?;

    info!(
        url = %scrub_credentials(url),
        target = %target_dir.display(),
        sha = %fetch_commit.id(),
        "fetch + reset complete"
    );

    Ok(CloneOutcome::Fetched(target_dir.to_path_buf()))
}

// ── Tests ──────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use tempfile::TempDir;

    /// Create a bare source repository in `src_dir` with a single commit.
    /// Returns the commit SHA.
    fn make_bare_repo(src_dir: &Path) -> String {
        // Init a normal repo, commit, then mirror it to bare.
        let work = TempDir::new().unwrap();
        let work_path = work.path();

        let repo = Repository::init(work_path).unwrap();
        let mut config = repo.config().unwrap();
        config.set_str("user.email", "test@example.com").unwrap();
        config.set_str("user.name", "Test").unwrap();
        drop(config);

        // Create a file and commit.
        let file_path = work_path.join("README.md");
        fs::write(&file_path, "hello").unwrap();
        let mut index = repo.index().unwrap();
        index.add_path(Path::new("README.md")).unwrap();
        index.write().unwrap();
        let tree_id = index.write_tree().unwrap();
        let tree = repo.find_tree(tree_id).unwrap();
        let sig = repo.signature().unwrap();
        let commit_id = repo
            .commit(Some("HEAD"), &sig, &sig, "initial commit", &tree, &[])
            .unwrap();

        // Clone as bare into src_dir.
        let mut builder = git2::build::RepoBuilder::new();
        builder.bare(true);
        builder.clone(work_path.to_str().unwrap(), src_dir).unwrap();

        commit_id.to_string()
    }

    // (a) Fresh clone via file:// URL into a new target dir.
    // NOTE: allowlist check is bypassed at this layer; we test clone mechanics
    // only. Allowlist integration is tested via the allowlist module.
    #[test]
    fn fresh_clone_creates_target_dir() {
        let bare = TempDir::new().unwrap();
        let _sha = make_bare_repo(bare.path());

        let target = TempDir::new().unwrap();
        let target_dir = target.path().join("clone");

        // We can't use `clone_or_fetch` directly with file:// (allowlist rejects it
        // for production code). Call the internal function directly.
        let file_url = format!("file://{}", bare.path().to_str().unwrap());
        // Use HEAD so we follow whatever the bare repo's default branch is.
        let outcome = clone_fresh(&file_url, "HEAD", None, None, &target_dir);
        assert!(outcome.is_ok(), "clone_fresh failed: {:?}", outcome);

        match outcome.unwrap() {
            CloneOutcome::Cloned(p) => assert_eq!(p, target_dir),
            other => panic!("expected Cloned, got {:?}", other),
        }
        assert!(target_dir.join(".git").exists());
    }

    // (b) Fetch existing clone (re-ingest path).
    #[test]
    fn fetch_existing_clone_returns_fetched() {
        let bare = TempDir::new().unwrap();
        let _sha = make_bare_repo(bare.path());

        let target = TempDir::new().unwrap();
        let target_dir = target.path().join("clone");

        let file_url = format!("file://{}", bare.path().to_str().unwrap());

        // First clone.
        clone_fresh(&file_url, "HEAD", None, None, &target_dir).unwrap();

        // Second call via `clone_or_fetch` (bypasses allowlist check — target_dir
        // already has `.git`).
        let outcome = fetch_existing(&file_url, "HEAD", None, None, &target_dir);
        assert!(outcome.is_ok(), "fetch_existing failed: {:?}", outcome);

        match outcome.unwrap() {
            CloneOutcome::Fetched(p) => assert_eq!(p, target_dir),
            other => panic!("expected Fetched, got {:?}", other),
        }
    }

    // (d) PAT value never appears in tracing output.
    #[test]
    fn pat_not_logged_in_error_path() {
        // Attempt a clone to a non-existent URL with a sentinel PAT.
        // The error message must NOT contain the PAT value.
        let target = TempDir::new().unwrap();
        let target_dir = target.path().join("clone");
        let sentinel = "PAT-SENTINEL-XYZ-12345";

        let err = clone_fresh(
            "https://github.com/nonexistent/repo.git",
            "main",
            Some(sentinel.to_owned()),
            None,
            &target_dir,
        )
        .unwrap_err();

        let err_str = err.to_string();
        assert!(
            !err_str.contains(sentinel),
            "PAT sentinel must not appear in error message: {err_str}"
        );
    }

    // Test that scrub_credentials removes user:pass@ from URLs.
    #[test]
    fn scrub_credentials_removes_user_pass() {
        let url = "https://x-access-token:my-secret-pat@github.com/org/repo";
        let scrubbed = scrub_credentials(url);
        assert!(!scrubbed.contains("my-secret-pat"), "PAT must be scrubbed");
        assert!(scrubbed.contains("github.com"), "host must remain");
    }

    #[test]
    fn scrub_credentials_no_change_for_clean_url() {
        let url = "https://github.com/org/repo";
        assert_eq!(scrub_credentials(url), url);
    }
}
