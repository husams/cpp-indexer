//! REPO node metadata collected from the local git repository.
//!
//! Story: S22-repo-nodes (AC-M4-1).
//!
//! [`collect`] reads the git HEAD commit SHA and date from the repository that
//! contains `repo_root`, using `git2`.  It does not require a network connection
//! or any remote.
//!
//! The resulting [`RepoMeta`] is consumed by `pipeline::run` to build the REPO
//! `NodeRecord` and the `BELONGS_TO_REPO` edge batch.

use std::path::{Path, PathBuf};

use crate::error::{Error, Result};

// ── Public types ──────────────────────────────────────────────────────────────

/// Metadata collected from the local git repository for the REPO node.
///
/// All string fields are non-empty; `collect` returns an error if git2 cannot
/// provide any of them.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RepoMeta {
    /// Repository name — taken directly from `RunOptions.repo_name`.
    pub name: String,

    /// Absolute path to the repository root (where `.git` lives).
    pub root_path: PathBuf,

    /// Full 40-hex-character SHA of the HEAD commit.
    pub commit_sha: String,

    /// ISO-8601 date string (`YYYY-MM-DD`) of the HEAD commit author date.
    pub commit_date: String,
}

// ── Public function ───────────────────────────────────────────────────────────

/// Collect REPO-node metadata by opening the git repository that contains
/// `repo_root`.
///
/// `git2::Repository::discover` is used so that `repo_root` does not have to
/// be the exact `.git` parent; it walks upward until it finds a git repository.
///
/// # Errors
/// - [`Error::Bootstrap`] if no git repository is found under `repo_root`.
/// - [`Error::Bootstrap`] if HEAD is unborn (no commits yet).
/// - [`Error::Bootstrap`] if the commit author timestamp is out of range for
///   `chrono`.
pub fn collect(repo_name: &str, repo_root: &Path) -> Result<RepoMeta> {
    let git_repo = git2::Repository::discover(repo_root).map_err(|e| {
        Error::Bootstrap(format!(
            "repo_meta: cannot discover git repository at {}: {e}",
            repo_root.display()
        ))
    })?;

    let workdir = git_repo
        .workdir()
        .unwrap_or_else(|| git_repo.path())
        .to_path_buf();

    let head = git_repo.head().map_err(|e| {
        Error::Bootstrap(format!(
            "repo_meta: cannot read HEAD in {}: {e}",
            workdir.display()
        ))
    })?;

    let commit = head.peel_to_commit().map_err(|e| {
        Error::Bootstrap(format!(
            "repo_meta: HEAD is not a commit in {}: {e}",
            workdir.display()
        ))
    })?;

    let sha = commit.id().to_string();

    // git2 exposes the author time as seconds-since-epoch + timezone offset.
    let time = commit.author().when();
    let commit_date = epoch_seconds_to_date(time.seconds()).ok_or_else(|| {
        Error::Bootstrap(format!(
            "repo_meta: commit timestamp {} is out of range in {}",
            time.seconds(),
            workdir.display()
        ))
    })?;

    Ok(RepoMeta {
        name: repo_name.to_owned(),
        root_path: workdir,
        commit_sha: sha,
        commit_date,
    })
}

// ── Private helpers ───────────────────────────────────────────────────────────

/// Convert a Unix timestamp (seconds since epoch) to an ISO-8601 date string
/// (`YYYY-MM-DD`).  Returns `None` if the timestamp is outside the representable
/// range for `i64` calendar arithmetic.
fn epoch_seconds_to_date(epoch_secs: i64) -> Option<String> {
    // We avoid pulling in `chrono` or `time` as extra dependencies; instead we
    // use a simple Julian-day calculation that covers the full i64 range.
    //
    // Algorithm: Gregorian calendar from Julian Day Number.
    //   See https://www.researchgate.net/publication/316558298 (Richards 2013).

    // Offset from Unix epoch (1970-01-01) to Julian Day Number.
    const UNIX_EPOCH_JDN: i64 = 2_440_588;
    const SECS_PER_DAY: i64 = 86_400;

    let day_number = epoch_secs.div_euclid(SECS_PER_DAY);
    let jdn = day_number.checked_add(UNIX_EPOCH_JDN)?;

    // Richards (2013) algorithm — integers only.
    let f = jdn + 1_401 + (((4 * jdn + 274_277) / 146_097) * 3) / 4 - 38;
    let e = 4 * f + 3;
    let g = e % 1_461 / 4;
    let h = 5 * g + 2;

    let day = (h % 153) / 5 + 1;
    let month = (h / 153 + 2) % 12 + 1;
    let year = e / 1_461 - 4_716 + (14 - month) / 12;

    // Sanity-check the result is within a reasonable range.
    if !(1970..=9999).contains(&year) {
        return None;
    }

    Some(format!("{year:04}-{month:02}-{day:02}"))
}

// ── Unit tests ─────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn epoch_zero_is_1970_01_01() {
        assert_eq!(epoch_seconds_to_date(0), Some("1970-01-01".to_owned()));
    }

    #[test]
    fn known_date_2024_05_17() {
        // 2024-05-17 00:00:00 UTC → 1715904000
        assert_eq!(
            epoch_seconds_to_date(1_715_904_000),
            Some("2024-05-17".to_owned())
        );
    }

    #[test]
    fn known_date_2026_01_01() {
        // 2026-01-01 00:00:00 UTC → 1767225600
        assert_eq!(
            epoch_seconds_to_date(1_767_225_600),
            Some("2026-01-01".to_owned())
        );
    }

    #[test]
    fn collect_reads_real_repo() {
        // Run this test only when there is a git repo at the project root.
        // `cargo nextest run` runs from the project root, which is a git repo.
        let root = std::env::current_dir().expect("cwd must be available");
        let meta = match collect("test-repo", &root) {
            Ok(m) => m,
            Err(e) => {
                eprintln!("Skipping collect_reads_real_repo: {e}");
                return;
            }
        };

        assert_eq!(meta.name, "test-repo");
        assert!(!meta.commit_sha.is_empty(), "commit_sha must be non-empty");
        assert_eq!(meta.commit_sha.len(), 40, "commit_sha must be 40 hex chars");
        assert!(
            meta.commit_date.len() == 10,
            "commit_date must be YYYY-MM-DD"
        );
        let parts: Vec<&str> = meta.commit_date.split('-').collect();
        assert_eq!(parts.len(), 3, "commit_date must have 3 parts");
    }

    #[test]
    fn repo_meta_test_with_init() {
        // Create a fresh git repo in a temp dir and verify collect() works.
        let dir = tempfile::tempdir().expect("tempdir");
        let repo = git2::Repository::init(dir.path()).expect("git init");

        // Create an initial commit so HEAD exists.
        let sig = git2::Signature::new(
            "Test",
            "test@example.com",
            &git2::Time::new(1_715_904_000, 0),
        )
        .expect("signature");
        let tree_oid = {
            let mut index = repo.index().expect("index");
            index.write_tree().expect("write tree")
        };
        let tree = repo.find_tree(tree_oid).expect("find tree");
        repo.commit(Some("HEAD"), &sig, &sig, "initial commit", &tree, &[])
            .expect("commit");

        let meta = collect("my-lib", dir.path()).expect("collect must succeed");

        assert_eq!(meta.name, "my-lib");
        assert_eq!(meta.commit_sha.len(), 40);
        assert_eq!(meta.commit_date, "2024-05-17", "date must match epoch");
    }
}
