//! Per-repo SQLite symbol/file map with write-through LRU cache.
//!
//! # Design
//! - Single write `Connection` behind `std::sync::Mutex`; WAL + NORMAL sync.
//! - Two tables: `symbols(id INTEGER PRIMARY KEY, usr TEXT NOT NULL UNIQUE)` and
//!   `files(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE)`.
//! - Bidirectional write-through LRU: SQLite first, then cache; eviction never loses data.
//! - `cache_size == 0` disables the cache; every lookup hits SQLite.
//! - A `ReadOnlyHandle` provides cross-repo / read-path resolution via a separate read-only
//!   connection; it exposes `resolve_symbol`, `resolve_file`, `lookup_symbol`, `lookup_file`.

use std::path::Path;
#[cfg(test)]
use std::sync::atomic::{AtomicU64, Ordering};
#[cfg(test)]
use std::sync::Arc;
use std::sync::Mutex;

use lru::LruCache;
use rusqlite::{Connection, OpenFlags, OptionalExtension};

use crate::error::{Error, Result};

// ── bidirectional LRU (usr↔id, path↔id) ──────────────────────────────────────

/// Holds forward (`text → id`) and reverse (`id → text`) LRU caches of the same
/// capacity.  A capacity of 1 means at most one entry is retained at a time.
struct BiLru {
    fwd: LruCache<String, i64>,
    rev: LruCache<i64, String>,
}

impl BiLru {
    fn new(cap: usize) -> Self {
        use std::num::NonZeroUsize;
        let n = NonZeroUsize::new(cap.max(1)).expect("cap >= 1");
        Self {
            fwd: LruCache::new(n),
            rev: LruCache::new(n),
        }
    }

    /// Promote / insert a forward entry and mirror it in the reverse direction.
    /// If capacity is 1, the second distinct insert evicts the first from both caches.
    fn put(&mut self, text: &str, id: i64) {
        // Insert forward; if it evicts an old entry, remove the corresponding reverse entry.
        if let Some((evicted_text, evicted_id)) = self.fwd.push(text.to_owned(), id) {
            // Only remove reverse entry if the id matches (idempotent).
            if self.rev.peek(&evicted_id) == Some(&evicted_text) {
                self.rev.pop(&evicted_id);
            }
        }
        // Insert reverse; same mirroring.
        if let Some((evicted_id, evicted_text)) = self.rev.push(id, text.to_owned()) {
            if self
                .fwd
                .peek(&evicted_text)
                .is_some_and(|i| *i == evicted_id)
            {
                self.fwd.pop(&evicted_text);
            }
        }
    }

    fn get_fwd(&mut self, text: &str) -> Option<i64> {
        self.fwd.get(text).copied()
    }

    fn get_rev(&mut self, id: i64) -> Option<&str> {
        self.rev.get(&id).map(String::as_str)
    }
}

// ── inner writer state ─────────────────────────────────────────────────────────

struct Inner {
    conn: Connection,
    sym_cache: Option<BiLru>,
    file_cache: Option<BiLru>,
    /// Number of SQLite SELECT queries executed (for test observability).
    #[cfg(test)]
    query_count: Arc<AtomicU64>,
}

impl Inner {
    fn open_write(path: &Path, cache_size: usize) -> Result<Self> {
        let conn = Connection::open(path)?;
        Self::configure(&conn)?;
        Ok(Self {
            conn,
            sym_cache: (cache_size > 0).then(|| BiLru::new(cache_size)),
            file_cache: (cache_size > 0).then(|| BiLru::new(cache_size)),
            #[cfg(test)]
            query_count: Arc::new(AtomicU64::new(0)),
        })
    }

    fn configure(conn: &Connection) -> Result<()> {
        conn.execute_batch(
            "PRAGMA journal_mode=WAL;
             PRAGMA synchronous=NORMAL;
             CREATE TABLE IF NOT EXISTS symbols (
                 id  INTEGER PRIMARY KEY,
                 usr TEXT NOT NULL UNIQUE
             );
             CREATE TABLE IF NOT EXISTS files (
                 id   INTEGER PRIMARY KEY,
                 path TEXT NOT NULL UNIQUE
             );",
        )?;
        Ok(())
    }

    fn get_or_insert_symbol(&mut self, usr: &str) -> Result<i64> {
        // Check cache first.
        if let Some(c) = &mut self.sym_cache {
            if let Some(id) = c.get_fwd(usr) {
                return Ok(id);
            }
        }
        // SQLite get-or-insert.
        self.conn.execute(
            "INSERT INTO symbols (usr) VALUES (?1) ON CONFLICT(usr) DO NOTHING",
            [usr],
        )?;
        #[cfg(test)]
        self.query_count.fetch_add(1, Ordering::Relaxed);
        let id: i64 = self
            .conn
            .query_row("SELECT id FROM symbols WHERE usr = ?1", [usr], |r| r.get(0))?;
        if let Some(c) = &mut self.sym_cache {
            c.put(usr, id);
        }
        Ok(id)
    }

    fn get_or_insert_file(&mut self, path: &str) -> Result<i64> {
        if let Some(c) = &mut self.file_cache {
            if let Some(id) = c.get_fwd(path) {
                return Ok(id);
            }
        }
        self.conn.execute(
            "INSERT INTO files (path) VALUES (?1) ON CONFLICT(path) DO NOTHING",
            [path],
        )?;
        #[cfg(test)]
        self.query_count.fetch_add(1, Ordering::Relaxed);
        let id: i64 = self
            .conn
            .query_row("SELECT id FROM files WHERE path = ?1", [path], |r| r.get(0))?;
        if let Some(c) = &mut self.file_cache {
            c.put(path, id);
        }
        Ok(id)
    }

    fn resolve_symbol(&mut self, id: i64) -> Result<Option<String>> {
        if let Some(c) = &mut self.sym_cache {
            if let Some(s) = c.get_rev(id) {
                return Ok(Some(s.to_owned()));
            }
        }
        #[cfg(test)]
        self.query_count.fetch_add(1, Ordering::Relaxed);
        let opt: Option<String> = self
            .conn
            .query_row("SELECT usr FROM symbols WHERE id = ?1", [id], |r| r.get(0))
            .optional()?;
        if let (Some(ref s), Some(c)) = (&opt, &mut self.sym_cache) {
            c.put(s, id);
        }
        Ok(opt)
    }

    fn resolve_file(&mut self, id: i64) -> Result<Option<String>> {
        if let Some(c) = &mut self.file_cache {
            if let Some(s) = c.get_rev(id) {
                return Ok(Some(s.to_owned()));
            }
        }
        #[cfg(test)]
        self.query_count.fetch_add(1, Ordering::Relaxed);
        let opt: Option<String> = self
            .conn
            .query_row("SELECT path FROM files WHERE id = ?1", [id], |r| r.get(0))
            .optional()?;
        if let (Some(ref s), Some(c)) = (&opt, &mut self.file_cache) {
            c.put(s, id);
        }
        Ok(opt)
    }
}

// ── SymbolAllocator (public write handle) ─────────────────────────────────────

/// Thread-safe write handle: allocates integer IDs for USRs and file paths.
///
/// Internally wraps a single SQLite connection behind a `Mutex` so multiple
/// rayon threads can call `get_or_insert_*` concurrently without races.
pub struct SymbolAllocator {
    inner: Mutex<Inner>,
    /// Exposed for test query-count probing only.
    #[cfg(test)]
    pub query_count: Arc<AtomicU64>,
}

impl SymbolAllocator {
    /// Open (or create) the SQLite database at `path` with the given `cache_size`.
    /// `cache_size == 0` disables the LRU; every allocation hits SQLite directly.
    pub fn open(path: &Path, cache_size: usize) -> Result<Self> {
        let inner = Inner::open_write(path, cache_size)?;
        #[cfg(test)]
        let qc = Arc::clone(&inner.query_count);
        Ok(Self {
            inner: Mutex::new(inner),
            #[cfg(test)]
            query_count: qc,
        })
    }

    /// Return the existing integer ID for `usr`, or allocate a new one.
    pub fn get_or_insert_symbol(&self, usr: &str) -> Result<i64> {
        self.inner
            .lock()
            .map_err(|_| Error::Schema("SymbolAllocator mutex poisoned".into()))?
            .get_or_insert_symbol(usr)
    }

    /// Return the existing integer ID for `path`, or allocate a new one.
    pub fn get_or_insert_file(&self, path: &str) -> Result<i64> {
        self.inner
            .lock()
            .map_err(|_| Error::Schema("SymbolAllocator mutex poisoned".into()))?
            .get_or_insert_file(path)
    }

    /// Resolve an integer symbol ID back to its USR string, or `None` if unknown.
    pub fn resolve_symbol(&self, id: i64) -> Result<Option<String>> {
        self.inner
            .lock()
            .map_err(|_| Error::Schema("SymbolAllocator mutex poisoned".into()))?
            .resolve_symbol(id)
    }

    /// Resolve an integer file ID back to its path string, or `None` if unknown.
    pub fn resolve_file(&self, id: i64) -> Result<Option<String>> {
        self.inner
            .lock()
            .map_err(|_| Error::Schema("SymbolAllocator mutex poisoned".into()))?
            .resolve_file(id)
    }

    /// Open a read-only resolver handle for another (or the same) repo's database.
    ///
    /// This is the cross-story API contract: Stories 3 and 4 use this to resolve
    /// `dst_usr → dst_id` and `id → usr/path` against an arbitrary repo's SQLite db.
    pub fn open_readonly(path: &Path, cache_size: usize) -> Result<ReadOnlyHandle> {
        ReadOnlyHandle::open(path, cache_size)
    }
}

// ── ReadOnlyHandle (cross-repo resolver, Stories 3 & 4) ───────────────────────

/// Read-only resolver handle: opens a repo's `cxg-symbols.db` with
/// `SQLITE_OPEN_READ_ONLY` and provides bidirectional lookups.
///
/// Thread-safe via an internal `Mutex`; may be cloned or wrapped in `Arc`.
pub struct ReadOnlyHandle {
    inner: Mutex<ReadOnlyInner>,
}

struct ReadOnlyInner {
    conn: Connection,
    sym_cache: Option<BiLru>,
    file_cache: Option<BiLru>,
}

impl ReadOnlyHandle {
    fn open(path: &Path, cache_size: usize) -> Result<Self> {
        let conn = Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_ONLY)?;
        Ok(Self {
            inner: Mutex::new(ReadOnlyInner {
                conn,
                sym_cache: (cache_size > 0).then(|| BiLru::new(cache_size)),
                file_cache: (cache_size > 0).then(|| BiLru::new(cache_size)),
            }),
        })
    }

    /// Look up `usr → id`.  Returns `None` if the USR is not in the database.
    pub fn lookup_symbol(&self, usr: &str) -> Result<Option<i64>> {
        let mut g = self
            .inner
            .lock()
            .map_err(|_| Error::Schema("ReadOnlyHandle mutex poisoned".into()))?;
        if let Some(c) = &mut g.sym_cache {
            if let Some(id) = c.get_fwd(usr) {
                return Ok(Some(id));
            }
        }
        let opt: Option<i64> = g
            .conn
            .query_row("SELECT id FROM symbols WHERE usr = ?1", [usr], |r| r.get(0))
            .optional()?;
        if let (Some(id), Some(c)) = (opt, &mut g.sym_cache) {
            c.put(usr, id);
        }
        Ok(opt)
    }

    /// Resolve `id → usr`.  Returns `None` if the ID is not in the database.
    pub fn resolve_symbol(&self, id: i64) -> Result<Option<String>> {
        let mut g = self
            .inner
            .lock()
            .map_err(|_| Error::Schema("ReadOnlyHandle mutex poisoned".into()))?;
        if let Some(c) = &mut g.sym_cache {
            if let Some(s) = c.get_rev(id) {
                return Ok(Some(s.to_owned()));
            }
        }
        let opt: Option<String> = g
            .conn
            .query_row("SELECT usr FROM symbols WHERE id = ?1", [id], |r| r.get(0))
            .optional()?;
        if let (Some(ref s), Some(c)) = (&opt, &mut g.sym_cache) {
            c.put(s, id);
        }
        Ok(opt)
    }

    /// Look up `path → id`.
    pub fn lookup_file(&self, path: &str) -> Result<Option<i64>> {
        let mut g = self
            .inner
            .lock()
            .map_err(|_| Error::Schema("ReadOnlyHandle mutex poisoned".into()))?;
        if let Some(c) = &mut g.file_cache {
            if let Some(id) = c.get_fwd(path) {
                return Ok(Some(id));
            }
        }
        let opt: Option<i64> = g
            .conn
            .query_row("SELECT id FROM files WHERE path = ?1", [path], |r| r.get(0))
            .optional()?;
        if let (Some(id), Some(c)) = (opt, &mut g.file_cache) {
            c.put(path, id);
        }
        Ok(opt)
    }

    /// Resolve `id → path`.
    pub fn resolve_file(&self, id: i64) -> Result<Option<String>> {
        let mut g = self
            .inner
            .lock()
            .map_err(|_| Error::Schema("ReadOnlyHandle mutex poisoned".into()))?;
        if let Some(c) = &mut g.file_cache {
            if let Some(s) = c.get_rev(id) {
                return Ok(Some(s.to_owned()));
            }
        }
        let opt: Option<String> = g
            .conn
            .query_row("SELECT path FROM files WHERE id = ?1", [id], |r| r.get(0))
            .optional()?;
        if let (Some(ref s), Some(c)) = (&opt, &mut g.file_cache) {
            c.put(s, id);
        }
        Ok(opt)
    }
}

// ── tests ──────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::Ordering;
    use std::sync::Arc;
    use tempfile::NamedTempFile;

    fn temp_alloc(cache_size: usize) -> (SymbolAllocator, NamedTempFile) {
        let f = NamedTempFile::new().unwrap();
        let a = SymbolAllocator::open(f.path(), cache_size).unwrap();
        (a, f)
    }

    // S7-SC-01: insert new USR → new id; row exists
    #[test]
    fn sc01_new_usr_gets_new_id() {
        let (a, _f) = temp_alloc(0);
        let id = a.get_or_insert_symbol("c:@F@foo").unwrap();
        assert!(id > 0);
        // Row exists
        assert_eq!(a.resolve_symbol(id).unwrap(), Some("c:@F@foo".to_owned()));
    }

    // S7-SC-02: dup USR → same id, no new row
    #[test]
    fn sc02_dup_usr_same_id() {
        let (a, _f) = temp_alloc(0);
        let id1 = a.get_or_insert_symbol("c:@F@foo").unwrap();
        let id2 = a.get_or_insert_symbol("c:@F@foo").unwrap();
        assert_eq!(id1, id2);
        // Only one row
        let count: i64 = a
            .inner
            .lock()
            .unwrap()
            .conn
            .query_row("SELECT COUNT(*) FROM symbols", [], |r| r.get(0))
            .unwrap();
        assert_eq!(count, 1);
    }

    // S7-SC-03: insert new path → new id; row exists
    #[test]
    fn sc03_new_path_gets_new_id() {
        let (a, _f) = temp_alloc(0);
        let id = a.get_or_insert_file("/src/foo.cpp").unwrap();
        assert!(id > 0);
        assert_eq!(a.resolve_file(id).unwrap(), Some("/src/foo.cpp".to_owned()));
    }

    // S7-SC-04: dup path → same id
    #[test]
    fn sc04_dup_path_same_id() {
        let (a, _f) = temp_alloc(0);
        let id1 = a.get_or_insert_file("/src/foo.cpp").unwrap();
        let id2 = a.get_or_insert_file("/src/foo.cpp").unwrap();
        assert_eq!(id1, id2);
    }

    // S7-SC-05: persistence across reopen
    #[test]
    fn sc05_persistence_across_reopen() {
        let f = NamedTempFile::new().unwrap();
        let id = {
            let a = SymbolAllocator::open(f.path(), 0).unwrap();
            a.get_or_insert_symbol("c:@F@bar").unwrap()
        };
        let a2 = SymbolAllocator::open(f.path(), 0).unwrap();
        let id2 = a2.get_or_insert_symbol("c:@F@bar").unwrap();
        assert_eq!(id, id2);
    }

    // S7-SC-06: cache hit does not increment query counter
    #[test]
    fn sc06_cache_hit_no_query() {
        let (a, _f) = temp_alloc(100);
        a.get_or_insert_symbol("c:@F@foo").unwrap(); // miss → SQLite
        let before = a.query_count.load(Ordering::Relaxed);
        a.get_or_insert_symbol("c:@F@foo").unwrap(); // hit → no query
        let after = a.query_count.load(Ordering::Relaxed);
        assert_eq!(before, after, "cache hit should not increment query_count");
    }

    // S7-SC-07: miss → query and populate cache
    #[test]
    fn sc07_miss_queries_and_populates() {
        let (a, _f) = temp_alloc(100);
        let before = a.query_count.load(Ordering::Relaxed);
        a.get_or_insert_symbol("c:@F@newone").unwrap();
        let after = a.query_count.load(Ordering::Relaxed);
        assert!(after > before, "cache miss should increment query_count");
        // Second call: cache hit
        let before2 = a.query_count.load(Ordering::Relaxed);
        a.get_or_insert_symbol("c:@F@newone").unwrap();
        assert_eq!(
            a.query_count.load(Ordering::Relaxed),
            before2,
            "second call should be a cache hit"
        );
    }

    // S7-SC-08: LRU eviction — cache_size=1, second distinct insert evicts the first
    #[test]
    fn sc08_lru_eviction() {
        let (a, _f) = temp_alloc(1);
        let id_foo = a.get_or_insert_symbol("c:@F@foo").unwrap();
        a.get_or_insert_symbol("c:@F@bar").unwrap(); // evicts foo from cache

        // After eviction, resolving foo must still work (via SQLite) — write-through guarantee
        let resolved = a.resolve_symbol(id_foo).unwrap();
        assert_eq!(resolved, Some("c:@F@foo".to_owned()));
    }

    // S7-SC-09: cache_size=0 disables cache
    #[test]
    fn sc09_cache_disabled() {
        let (a, _f) = temp_alloc(0);
        // Every call hits SQLite — query count increments on every get_or_insert
        a.get_or_insert_symbol("c:@F@a").unwrap();
        a.get_or_insert_symbol("c:@F@a").unwrap();
        // Both calls should have incremented the query counter (no cache)
        assert!(
            a.query_count.load(Ordering::Relaxed) >= 2,
            "with cache disabled every call should hit SQLite"
        );
    }

    // S7-SC-10: reverse resolve id → usr
    #[test]
    fn sc10_resolve_symbol_by_id() {
        let (a, _f) = temp_alloc(0);
        let id = a.get_or_insert_symbol("c:@F@omega").unwrap();
        assert_eq!(a.resolve_symbol(id).unwrap(), Some("c:@F@omega".to_owned()));
        assert_eq!(a.resolve_symbol(9999).unwrap(), None);
    }

    // S7-SC-11: reverse resolve id → path
    #[test]
    fn sc11_resolve_file_by_id() {
        let (a, _f) = temp_alloc(0);
        let id = a.get_or_insert_file("/src/lib.h").unwrap();
        assert_eq!(a.resolve_file(id).unwrap(), Some("/src/lib.h".to_owned()));
        assert_eq!(a.resolve_file(9999).unwrap(), None);
    }

    // S2-SC-08: concurrent same-usr → exactly one id, both callers equal, no dup rows
    #[test]
    fn sc08_concurrent_same_usr() {
        let f = NamedTempFile::new().unwrap();
        let a = Arc::new(SymbolAllocator::open(f.path(), 0).unwrap());
        let results: Vec<i64> = std::thread::scope(|s| {
            let handles: Vec<_> = (0..8)
                .map(|_| {
                    let a = Arc::clone(&a);
                    s.spawn(move || a.get_or_insert_symbol("c:@F@concurrent").unwrap())
                })
                .collect();
            handles.into_iter().map(|h| h.join().unwrap()).collect()
        });
        let first = results[0];
        assert!(
            results.iter().all(|&id| id == first),
            "all threads must get same id"
        );
        let count: i64 = a
            .inner
            .lock()
            .unwrap()
            .conn
            .query_row("SELECT COUNT(*) FROM symbols", [], |r| r.get(0))
            .unwrap();
        assert_eq!(count, 1, "exactly one row in symbols table");
    }

    // S2-SC-09: concurrent distinct usrs → distinct ids
    #[test]
    fn sc09_concurrent_distinct_usrs() {
        let f = NamedTempFile::new().unwrap();
        let a = Arc::new(SymbolAllocator::open(f.path(), 0).unwrap());
        let mut results: Vec<i64> = std::thread::scope(|s| {
            let handles: Vec<_> = (0..8)
                .map(|i| {
                    let a = Arc::clone(&a);
                    let usr = format!("c:@F@sym{i}");
                    s.spawn(move || a.get_or_insert_symbol(&usr).unwrap())
                })
                .collect();
            handles.into_iter().map(|h| h.join().unwrap()).collect()
        });
        results.sort_unstable();
        results.dedup();
        assert_eq!(results.len(), 8, "8 distinct USRs must get 8 distinct IDs");
    }

    // S2-SC-10: N≥8 threads, no deadlock/panic
    #[test]
    fn sc10_no_deadlock_n_threads() {
        let f = NamedTempFile::new().unwrap();
        let a = Arc::new(SymbolAllocator::open(f.path(), 1000).unwrap());
        std::thread::scope(|s| {
            for i in 0..16 {
                let a = Arc::clone(&a);
                s.spawn(move || {
                    for j in 0..50u64 {
                        let usr = format!("c:@F@sym_{i}_{j}");
                        a.get_or_insert_symbol(&usr).unwrap();
                    }
                });
            }
        });
    }

    // Cross-story API contract: ReadOnlyHandle
    #[test]
    fn read_only_handle_lookup_and_resolve() {
        let f = NamedTempFile::new().unwrap();
        {
            let a = SymbolAllocator::open(f.path(), 0).unwrap();
            a.get_or_insert_symbol("c:@F@alpha").unwrap();
            a.get_or_insert_file("/src/alpha.cpp").unwrap();
        }
        let h = SymbolAllocator::open_readonly(f.path(), 100).unwrap();
        let id = h
            .lookup_symbol("c:@F@alpha")
            .unwrap()
            .expect("should find it");
        assert_eq!(h.resolve_symbol(id).unwrap(), Some("c:@F@alpha".to_owned()));
        let fid = h
            .lookup_file("/src/alpha.cpp")
            .unwrap()
            .expect("should find it");
        assert_eq!(
            h.resolve_file(fid).unwrap(),
            Some("/src/alpha.cpp".to_owned())
        );
        // Unknown
        assert_eq!(h.lookup_symbol("c:@F@unknown").unwrap(), None);
        assert_eq!(h.resolve_symbol(9999).unwrap(), None);
    }
}
