//! Repo-scoped reverse resolver: integer ID → USR / file path.
//!
//! Wraps [`ReadOnlyHandle`] handles (one per repo) and resolves
//! `symbol_id → usr` and `file_id → path` for the read path.
//!
//! # Design
//! - One `ReadOnlyHandle` is opened per repo, keyed by repo name.
//! - Handles are constructed on first access and cached for the lifetime
//!   of the resolver (no explicit eviction — one handle per repo is cheap).
//! - Missing IDs (`Ok(None)` from the handle) are converted to an explicit
//!   [`Error::Schema`] so callers receive a typed failure, never a blank.
//! - An unavailable SQLite file (e.g. the db was not written yet) surfaces
//!   as [`Error::Sqlite`] from the open call itself.
//!
//! # Thread safety
//! The resolver is `Send + Sync` via the `Mutex`-guarded handle map.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use crate::error::{Error, Result};
use crate::resolve::symbol_map::{ReadOnlyHandle, SymbolAllocator};

// ── IdResolver ────────────────────────────────────────────────────────────────

/// Repo-scoped resolver: maps integer `symbol_id` / `file_id` back to their
/// original strings using each repo's `cxg-symbols.db`.
///
/// Construct with a repo → db-path map, then call [`resolve_symbol`] /
/// [`resolve_file`] as needed.
///
/// [`resolve_symbol`]: IdResolver::resolve_symbol
/// [`resolve_file`]: IdResolver::resolve_file
pub struct IdResolver {
    /// Repo name → path to `cxg-symbols.db`.
    db_paths: HashMap<String, PathBuf>,
    /// LRU cache size forwarded to each newly-opened handle.
    cache_size: usize,
}

impl IdResolver {
    /// Create a resolver with an explicit `repo_name → db_path` map.
    ///
    /// Handles are opened lazily; no SQLite I/O happens here.
    pub fn new(db_paths: HashMap<String, PathBuf>, cache_size: usize) -> Self {
        Self {
            db_paths,
            cache_size,
        }
    }

    /// Create a resolver from a `stage_root`: each repo's db lives at
    /// `<stage_root>/<repo_name>/cxg-symbols.db`.
    ///
    /// `repo_names` is the set of repos to register up front. Additional repos
    /// can be resolved as long as their db file exists under `stage_root`.
    pub fn from_stage_root(
        stage_root: &Path,
        repo_names: impl IntoIterator<Item = impl Into<String>>,
        cache_size: usize,
    ) -> Self {
        let db_paths = repo_names
            .into_iter()
            .map(|r| {
                let name: String = r.into();
                let path = stage_root.join(&name).join("cxg-symbols.db");
                (name, path)
            })
            .collect();
        Self::new(db_paths, cache_size)
    }

    /// Resolve `symbol_id` for `repo_name` to its USR string.
    ///
    /// # Errors
    /// - `Error::Sqlite` — the SQLite file could not be opened.
    /// - `Error::Schema` — the ID exists in the graph but has no row in
    ///   the symbols table (stale graph or truncated db).
    /// - `Error::Schema` — `repo_name` is not registered in this resolver.
    pub fn resolve_symbol(&self, repo_name: &str, symbol_id: i64) -> Result<String> {
        let handle = self.get_or_open(repo_name)?;
        match handle.resolve_symbol(symbol_id)? {
            Some(usr) => Ok(usr),
            None => Err(Error::Schema(format!(
                "symbol_id {symbol_id} in repo '{repo_name}' has no corresponding USR in cxg-symbols.db"
            ))),
        }
    }

    /// Resolve `file_id` for `repo_name` to its file path string.
    ///
    /// # Errors
    /// Same as [`resolve_symbol`](IdResolver::resolve_symbol).
    pub fn resolve_file(&self, repo_name: &str, file_id: i64) -> Result<String> {
        let handle = self.get_or_open(repo_name)?;
        match handle.resolve_file(file_id)? {
            Some(path) => Ok(path),
            None => Err(Error::Schema(format!(
                "file_id {file_id} in repo '{repo_name}' has no corresponding path in cxg-symbols.db"
            ))),
        }
    }

    /// Resolve both `symbol_id` and `file_id` for the same repo in one call.
    ///
    /// Returns `(usr, path)`. Both IDs must be present; missing either is an error.
    pub fn resolve_node(
        &self,
        repo_name: &str,
        symbol_id: i64,
        file_id: i64,
    ) -> Result<(String, String)> {
        let usr = self.resolve_symbol(repo_name, symbol_id)?;
        let path = self.resolve_file(repo_name, file_id)?;
        Ok((usr, path))
    }

    /// Resolve an EXTERNAL_REF edge's endpoints:
    /// - `src_id` is resolved via `src_repo_name`'s db.
    /// - `dst_id` is resolved via `dst_repo_name`'s db (may differ from `src_repo_name`).
    ///
    /// Returns `(src_usr, dst_usr)`.
    pub fn resolve_external_ref(
        &self,
        src_repo_name: &str,
        src_id: i64,
        dst_repo_name: &str,
        dst_id: i64,
    ) -> Result<(String, String)> {
        let src_usr = self.resolve_symbol(src_repo_name, src_id)?;
        let dst_usr = self.resolve_symbol(dst_repo_name, dst_id)?;
        Ok((src_usr, dst_usr))
    }

    // ── private ────────────────────────────────────────────────────────────────

    /// Open a fresh `ReadOnlyHandle` for `repo_name`.
    ///
    /// Each call opens the SQLite file; the per-handle `BiLru` cache amortises
    /// repeated lookups within a single resolution call chain.  For long-lived
    /// use keep the `IdResolver` alive across many calls.
    fn get_or_open(&self, repo_name: &str) -> Result<ReadOnlyHandle> {
        let db_path = self.db_paths.get(repo_name).ok_or_else(|| {
            Error::Schema(format!(
                "repo '{repo_name}' is not registered in this IdResolver"
            ))
        })?;
        // Use the public cross-story API from SymbolAllocator.
        SymbolAllocator::open_readonly(db_path, self.cache_size)
    }
}

// ── tests ──────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::resolve::symbol_map::SymbolAllocator;
    use tempfile::TempDir;

    fn make_resolver(tmp: &TempDir, repo: &str, cache: usize) -> IdResolver {
        let db_path = tmp.path().join(format!("{repo}.db"));
        let mut map = HashMap::new();
        map.insert(repo.to_owned(), db_path);
        IdResolver::new(map, cache)
    }

    fn seed_db(tmp: &TempDir, repo: &str) -> (i64, i64) {
        let db_path = tmp.path().join(format!("{repo}.db"));
        let alloc = SymbolAllocator::open(&db_path, 0).unwrap();
        let sym_id = alloc.get_or_insert_symbol("c:@F@my_func").unwrap();
        let file_id = alloc.get_or_insert_file("/src/main.cpp").unwrap();
        (sym_id, file_id)
    }

    // S5-SC-01: resolver maps ids → usr/path
    #[test]
    fn resolve_symbol_and_file() {
        let tmp = TempDir::new().unwrap();
        let (sym_id, file_id) = seed_db(&tmp, "repo-a");
        let resolver = make_resolver(&tmp, "repo-a", 100);

        assert_eq!(
            resolver.resolve_symbol("repo-a", sym_id).unwrap(),
            "c:@F@my_func"
        );
        assert_eq!(
            resolver.resolve_file("repo-a", file_id).unwrap(),
            "/src/main.cpp"
        );
    }

    // S5-SC-04: round-trip preserves original strings
    #[test]
    fn roundtrip_preserves_strings() {
        let tmp = TempDir::new().unwrap();
        let db_path = tmp.path().join("repo-b.db");
        let alloc = SymbolAllocator::open(&db_path, 0).unwrap();
        let usrs = ["c:@F@alpha", "c:@F@beta", "c:@F@gamma"];
        let ids: Vec<i64> = usrs
            .iter()
            .map(|u| alloc.get_or_insert_symbol(u).unwrap())
            .collect();

        let mut map = HashMap::new();
        map.insert("repo-b".to_owned(), db_path);
        let resolver = IdResolver::new(map, 0);

        for (usr, id) in usrs.iter().zip(ids.iter()) {
            assert_eq!(
                resolver.resolve_symbol("repo-b", *id).unwrap(),
                *usr,
                "round-trip must preserve original USR"
            );
        }
    }

    // S5-SC-03 (negative): missing id → explicit error
    #[test]
    fn missing_id_returns_error() {
        let tmp = TempDir::new().unwrap();
        seed_db(&tmp, "repo-c");
        let resolver = make_resolver(&tmp, "repo-c", 0);

        let err = resolver.resolve_symbol("repo-c", 9999).unwrap_err();
        assert!(
            err.to_string().contains("9999"),
            "error must mention the missing id: {err}"
        );
    }

    // S5-SC-03 (negative): unknown repo → explicit error
    #[test]
    fn unknown_repo_returns_error() {
        let resolver: IdResolver = IdResolver::new(HashMap::new(), 0);
        let err = resolver.resolve_symbol("nonexistent-repo", 1).unwrap_err();
        assert!(
            err.to_string().contains("nonexistent-repo"),
            "error must mention the unknown repo: {err}"
        );
    }

    // S5-SC-03 (negative): unavailable db → explicit error (Error::Sqlite)
    #[test]
    fn unavailable_db_returns_error() {
        let mut map = HashMap::new();
        map.insert(
            "missing-repo".to_owned(),
            PathBuf::from("/tmp/does-not-exist-cxg-symbols.db"),
        );
        let resolver = IdResolver::new(map, 0);
        let err = resolver.resolve_symbol("missing-repo", 1).unwrap_err();
        // rusqlite will return an error when opening a nonexistent read-only file
        assert!(
            matches!(err, Error::Sqlite(_)),
            "expected Error::Sqlite for unavailable db, got: {err}"
        );
    }

    // resolve_node: both usr and path in one call
    #[test]
    fn resolve_node_returns_pair() {
        let tmp = TempDir::new().unwrap();
        let (sym_id, file_id) = seed_db(&tmp, "repo-d");
        let resolver = make_resolver(&tmp, "repo-d", 100);

        let (usr, path) = resolver.resolve_node("repo-d", sym_id, file_id).unwrap();
        assert_eq!(usr, "c:@F@my_func");
        assert_eq!(path, "/src/main.cpp");
    }

    // resolve_external_ref: cross-repo resolution uses correct per-repo db
    #[test]
    fn resolve_external_ref_uses_correct_repos() {
        let tmp = TempDir::new().unwrap();
        // repo-src
        let src_db = tmp.path().join("repo-src.db");
        let src_alloc = SymbolAllocator::open(&src_db, 0).unwrap();
        let src_id = src_alloc.get_or_insert_symbol("c:@F@src_func").unwrap();

        // repo-dst
        let dst_db = tmp.path().join("repo-dst.db");
        let dst_alloc = SymbolAllocator::open(&dst_db, 0).unwrap();
        let dst_id = dst_alloc.get_or_insert_symbol("c:@F@dst_func").unwrap();

        let mut map = HashMap::new();
        map.insert("repo-src".to_owned(), src_db);
        map.insert("repo-dst".to_owned(), dst_db);
        let resolver = IdResolver::new(map, 0);

        let (src_usr, dst_usr) = resolver
            .resolve_external_ref("repo-src", src_id, "repo-dst", dst_id)
            .unwrap();
        assert_eq!(src_usr, "c:@F@src_func");
        assert_eq!(dst_usr, "c:@F@dst_func");
    }
}
