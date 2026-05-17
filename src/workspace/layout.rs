//! Clone-path layout: `<workspace_dir>/<repo-name>-<short_sha>/`.
//!
//! Per ADR-6: short SHA is the first 12 characters of the resolved commit SHA.
//! The `<repo-name>` is derived from the last non-empty path segment of the
//! URL (`.git` suffix stripped).
//!
//! AC-M7-12.

use std::path::{Path, PathBuf};

use url::Url;

/// Derive the repository name from a git URL.
///
/// Strips a trailing `.git` suffix if present and returns the last path segment.
/// Falls back to `"repo"` if the URL has no usable path segment.
pub fn repo_name_from_url(raw_url: &str) -> String {
    let parsed = match Url::parse(raw_url) {
        Ok(u) => u,
        Err(_) => return "repo".to_owned(),
    };

    // Walk path segments from the end; skip empty segments.
    let name = parsed
        .path_segments()
        .and_then(|mut segs| segs.rfind(|s| !s.is_empty()))
        .unwrap_or("repo");

    // Strip `.git` suffix.
    name.strip_suffix(".git").unwrap_or(name).to_owned()
}

/// Construct the target clone directory path.
///
/// Returns `<workspace_dir>/<repo-name>-<short_sha>/` where `short_sha` is
/// the first 12 characters of `commit_sha`.
pub fn clone_path(workspace_dir: &Path, repo_name: &str, commit_sha: &str) -> PathBuf {
    let short = &commit_sha[..commit_sha.len().min(12)];
    workspace_dir.join(format!("{repo_name}-{short}"))
}

// ── Tests ──────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn repo_name_strips_git_suffix() {
        assert_eq!(
            repo_name_from_url("https://github.com/org/myrepo.git"),
            "myrepo"
        );
    }

    #[test]
    fn repo_name_no_git_suffix() {
        assert_eq!(
            repo_name_from_url("https://github.com/org/myrepo"),
            "myrepo"
        );
    }

    #[test]
    fn repo_name_trailing_slash() {
        // Path ends with `/` — last non-empty segment is still the repo name.
        assert_eq!(
            repo_name_from_url("https://github.com/org/myrepo/"),
            "myrepo"
        );
    }

    #[test]
    fn repo_name_fallback_for_invalid_url() {
        assert_eq!(repo_name_from_url("not-a-url"), "repo");
    }

    #[test]
    fn clone_path_uses_12_char_sha() {
        let dir = PathBuf::from("/workspace/clones");
        let sha = "abcdef123456789";
        let result = clone_path(&dir, "myrepo", sha);
        assert_eq!(
            result,
            PathBuf::from("/workspace/clones/myrepo-abcdef123456")
        );
    }

    #[test]
    fn clone_path_short_sha_not_truncated() {
        let dir = PathBuf::from("/workspace/clones");
        let sha = "abc123"; // fewer than 12 chars
        let result = clone_path(&dir, "myrepo", sha);
        assert_eq!(result, PathBuf::from("/workspace/clones/myrepo-abc123"));
    }

    #[test]
    fn clone_path_exact_12_chars() {
        let dir = PathBuf::from("/workspace/clones");
        let sha = "abcdef123456"; // exactly 12
        let result = clone_path(&dir, "myrepo", sha);
        assert_eq!(
            result,
            PathBuf::from("/workspace/clones/myrepo-abcdef123456")
        );
    }
}
