//! glibc allocator tuning (Issue 0002 Bug 1).
//!
//! libclang allocates its translation-unit ASTs through the system C allocator
//! (glibc on Linux), **not** through Rust's `GlobalAlloc`.  With one rayon
//! worker per CPU each doing large transient allocations, glibc opens up to
//! `n_cpu × 8` per-thread arenas and retains freed chunks per-arena rather than
//! returning them to the OS.  The result is monotonic RSS growth during Phase 1
//! even though individual ASTs are freed promptly.  Setting
//! `MALLOC_ARENA_MAX=2` plateaus RSS — the proof that this is arena retention,
//! not a leak.
//!
//! This module provides safe wrappers around the two glibc tunables that fix
//! it in-process:
//!
//! * [`set_arena_max`] — `mallopt(M_ARENA_MAX, n)`, called as the *first*
//!   statement in `main()` (before any worker spawns or libclang loads).
//! * [`trim`] — `malloc_trim(0)`, called on a cadence from the Phase 1 driver
//!   to return freed top-of-arena pages to the OS and counter residual creep.
//!
//! Both are glibc/Linux-specific.  On every other platform (notably macOS,
//! which uses a different allocator that does not exhibit this arena-retention
//! behaviour) they compile to harmless no-ops, so the rest of the pipeline is
//! unconditionally portable.

/// Cap the maximum number of glibc malloc arenas at `n`.
///
/// Equivalent to the `MALLOC_ARENA_MAX=<n>` environment variable, but applied
/// in-process via `mallopt(M_ARENA_MAX, n)` so the operator does not need to
/// set an env var.  `n == 0` is treated as "leave the glibc default untouched"
/// (the A/B escape hatch — see `--malloc-arena-max 0`).
///
/// MUST be called before any worker thread spawns or libclang loads/allocates;
/// arenas already opened before the cap is applied are not retroactively
/// collapsed.  Calling it as the very first statement in `main()` guarantees
/// correct ordering.
///
/// On non-Linux targets this is a no-op and always returns `true`.
///
/// Returns `true` if the tunable was applied successfully (or skipped because
/// `n == 0` / non-Linux), `false` if `mallopt` reported failure.
#[cfg(target_os = "linux")]
pub fn set_arena_max(n: usize) -> bool {
    if n == 0 {
        // 0 = leave glibc default; A/B escape hatch.
        return true;
    }
    // SAFETY: `mallopt` is a thread-safe glibc entry point that takes two
    // `c_int` arguments and returns a `c_int` status.  `M_ARENA_MAX` is the
    // documented tunable for the arena cap.  We clamp `n` into the `c_int`
    // range so the FFI argument is always valid; no Rust memory is touched.
    let value = i32::try_from(n).unwrap_or(i32::MAX);
    let rc = unsafe { libc::mallopt(libc::M_ARENA_MAX, value) };
    rc == 1
}

/// Return freed top-of-arena pages to the operating system.
///
/// Wraps `malloc_trim(0)`.  Cheap when there is nothing to return (glibc makes
/// it a near-no-op in that case), so it is safe to call on a periodic cadence
/// from the Phase 1 hot path to counter residual RSS creep.
///
/// On non-Linux targets this is a no-op and always returns `false`.
///
/// Returns `true` if memory was actually returned to the OS.
#[cfg(target_os = "linux")]
pub fn trim() -> bool {
    // SAFETY: `malloc_trim` is a thread-safe glibc entry point taking a single
    // `size_t` pad argument and returning a `c_int` (1 if memory was released,
    // 0 otherwise).  Passing `0` requests trimming all reclaimable arenas.  No
    // Rust memory is touched.
    let rc = unsafe { libc::malloc_trim(0) };
    rc == 1
}

/// No-op stub on non-glibc platforms (macOS, Windows, *BSD, …).
///
/// The glibc per-thread arena-retention bug this guards against is specific to
/// glibc; other allocators do not exhibit it, so capping arenas is unnecessary
/// and the underlying `mallopt`/`M_ARENA_MAX` symbols may not exist.
#[cfg(not(target_os = "linux"))]
pub fn set_arena_max(_n: usize) -> bool {
    true
}

/// No-op stub on non-glibc platforms (see [`set_arena_max`]).
#[cfg(not(target_os = "linux"))]
pub fn trim() -> bool {
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn set_arena_max_zero_is_noop_ok() {
        // 0 means "leave default"; must succeed on every platform.
        assert!(set_arena_max(0));
    }

    #[test]
    fn set_arena_max_returns_without_panic() {
        // On Linux this applies mallopt(M_ARENA_MAX, 2); on other platforms it
        // is a no-op returning true.  Either way it must not panic.
        let _ = set_arena_max(2);
    }

    #[test]
    fn trim_returns_without_panic() {
        // malloc_trim(0) on Linux; no-op false elsewhere.  Must not panic.
        let _ = trim();
    }

    #[cfg(not(target_os = "linux"))]
    #[test]
    fn non_linux_stubs_are_noops() {
        assert!(set_arena_max(2), "set_arena_max is a no-op true off-Linux");
        assert!(!trim(), "trim is a no-op false off-Linux");
    }
}
