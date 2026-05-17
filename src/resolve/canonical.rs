//! USR canonicalisation for system headers and vendored copies — ADR-4.
//!
//! Story: S24-syshdr-canonicalisation (AC-M4-7, AC-M4-8).
//!
//! ## Rule evaluation order (declaration order, first match wins)
//!
//! 1. **Config override list** (`canonical_overrides`) — user-supplied prefix rules
//!    take highest precedence, evaluated before any static rule.
//! 2. **Vendored prefix** — path contains `third_party/<pkg>/` or `vendor/<pkg>/`
//!    segment → `repo:vendored:<pkg>`.
//! 3. **System-header prefixes** — static table, evaluated top-to-bottom:
//!    - libstdc++ (Linux): `/usr/include/c++/`, `/usr/include/x86_64-linux-gnu/c++/`,
//!      `/usr/lib/llvm-*/include/c++/` → `system:libstdc++`
//!    - libc++ (macOS): `/Library/Developer/CommandLineTools/usr/include/c++/` → `system:libcxx`
//!    - clang builtins: `lib/clang/*/include/` → `system:clang-builtins`
//!    - libc (catch-all): `/usr/include/` → `system:libc`
//! 4. **No match** → `None` (caller keeps the original repo attribution).
//!
//! ## Naming convention
//!
//! The `RepoRef` strings returned use the forms defined in ADR-4:
//! - `system:<lib>` for compiler / OS-supplied headers.
//! - `repo:vendored:<pkg>` for vendored copies.
//! - User-supplied overrides may use any string.

/// A resolved canonical REPO name for a symbol's source path.
///
/// `system:<lib>` | `repo:vendored:<pkg>` | user override value.
pub type RepoRef = String;

/// A single override rule: if `file_path` starts with `path_prefix`, use `repo_ref`.
///
/// Override rules take precedence over all static rules (ADR-4 §Rule set, rule 3).
#[derive(Debug, Clone)]
pub struct CanonicalOverride {
    /// Path prefix to match (e.g. `"/opt/vendor/boost/"`).
    pub path_prefix: String,
    /// The canonical repo ref to assign (e.g. `"repo:boost"`).
    pub repo_ref: String,
}

/// Determine the canonical REPO ref for a symbol defined at `file_path`.
///
/// Returns `Some(RepoRef)` when a rule matches, or `None` when no rule applies
/// and the caller should keep the original repo attribution.
///
/// `overrides` is evaluated first (ADR-4 §Rule set, rule 3 — config priority).
/// The static system-header table follows in declaration order.
///
/// # Arguments
///
/// * `file_path` — absolute path to the defining source file (from `NodeRecord.file_path`).
/// * `overrides` — ordered list of user-supplied prefix→repo_ref rules; may be empty.
pub fn canonical_repo(file_path: &str, overrides: &[CanonicalOverride]) -> Option<RepoRef> {
    // ── Rule 1: config override list ──────────────────────────────────────────
    for rule in overrides {
        if file_path.starts_with(rule.path_prefix.as_str()) {
            return Some(rule.repo_ref.clone());
        }
    }

    // ── Rule 2: vendored prefix ───────────────────────────────────────────────
    //
    // Match path segments: `third_party/<pkg>/` or `vendor/<pkg>/`.
    // We search for the segment separator so that a bare prefix like "/vendor/"
    // is never matched unless followed by a package name and a slash.
    if let Some(pkg) = extract_vendored_pkg(file_path, "third_party/") {
        return Some(format!("repo:vendored:{pkg}"));
    }
    if let Some(pkg) = extract_vendored_pkg(file_path, "vendor/") {
        return Some(format!("repo:vendored:{pkg}"));
    }

    // ── Rule 3a: libstdc++ (Linux variants) ──────────────────────────────────
    //
    // Must be checked before the plain `/usr/include/` catch-all.
    const LIBSTDCXX_PREFIXES: &[&str] = &[
        "/usr/include/c++/",
        "/usr/include/x86_64-linux-gnu/c++/",
        // LLVM-packaged libstdc++ (e.g. /usr/lib/llvm-18/include/c++/)
    ];
    for prefix in LIBSTDCXX_PREFIXES {
        if file_path.starts_with(prefix) {
            return Some("system:libstdc++".to_owned());
        }
    }
    // LLVM-versioned path: matches `/usr/lib/llvm-<N>/include/c++/`
    if is_llvm_libstdcxx(file_path) {
        return Some("system:libstdc++".to_owned());
    }

    // ── Rule 3b: libc++ (macOS) ───────────────────────────────────────────────
    if file_path.starts_with("/Library/Developer/CommandLineTools/usr/include/c++/")
        || file_path.starts_with("/Applications/Xcode.app/Contents/Developer/Toolchains/")
            && file_path.contains("/usr/include/c++/")
    {
        return Some("system:libcxx".to_owned());
    }

    // ── Rule 3c: clang builtins ───────────────────────────────────────────────
    //
    // Matches `lib/clang/<version>/include/` anywhere in the path (ADR-4).
    if is_clang_builtin(file_path) {
        return Some("system:clang-builtins".to_owned());
    }

    // ── Rule 3d: libc catch-all ───────────────────────────────────────────────
    if file_path.starts_with("/usr/include/") {
        return Some("system:libc".to_owned());
    }

    None
}

// ── Private helpers ───────────────────────────────────────────────────────────

/// Extract the package name immediately following `segment` inside `path`.
///
/// Returns `Some("pkg")` when `path` contains `/<segment>pkg/` (or starts
/// with `<segment>pkg/`), otherwise `None`.
fn extract_vendored_pkg<'a>(path: &'a str, segment: &str) -> Option<&'a str> {
    // Find the segment inside the path, preceded by a '/' or at the start.
    let mut search_from = 0;
    loop {
        let pos = path[search_from..].find(segment)?;
        let abs_pos = search_from + pos;

        // The segment must appear right after a path separator (or at path start).
        let preceded_by_sep = abs_pos == 0 || path.as_bytes().get(abs_pos - 1) == Some(&b'/');
        if !preceded_by_sep {
            search_from = abs_pos + segment.len();
            continue;
        }

        // Everything after the segment until the next '/' is the package name.
        let after = &path[abs_pos + segment.len()..];
        if after.is_empty() {
            return None;
        }
        let pkg_end = after.find('/').unwrap_or(after.len());
        let pkg = &after[..pkg_end];
        if pkg.is_empty() {
            return None;
        }
        return Some(pkg);
    }
}

/// Returns `true` when `path` matches `/usr/lib/llvm-<N>/include/c++/…`.
fn is_llvm_libstdcxx(path: &str) -> bool {
    // Fast prefix guard.
    if !path.starts_with("/usr/lib/llvm-") {
        return false;
    }
    // After the LLVM version, expect `/include/c++/`.
    let rest = &path["/usr/lib/llvm-".len()..];
    // Rest: `<version>/include/c++/…`
    if let Some(slash) = rest.find('/') {
        let after_version = &rest[slash..]; // starts with '/'
        return after_version.starts_with("/include/c++/");
    }
    false
}

/// Returns `true` when `path` contains a `lib/clang/<version>/include/` segment.
fn is_clang_builtin(path: &str) -> bool {
    // We look for the literal substring; the leading '/' may or may not be present
    // depending on whether the path is absolute or compiler-internal.
    let marker = "lib/clang/";
    let mut search_from = 0;
    while let Some(pos) = path[search_from..].find(marker) {
        let abs = search_from + pos;
        let rest = &path[abs + marker.len()..];
        // rest: `<version>/include/…`
        if let Some(slash) = rest.find('/') {
            let after_ver = &rest[slash..];
            if after_ver.starts_with("/include/") {
                return true;
            }
        }
        search_from = abs + marker.len();
    }
    false
}

// ── Unit tests ─────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    // ── system:libstdc++ ──────────────────────────────────────────────────────

    #[test]
    fn usr_under_usr_include_cxx_is_libstdcxx() {
        assert_eq!(
            canonical_repo("/usr/include/c++/12/vector", &[]),
            Some("system:libstdc++".to_owned())
        );
    }

    #[test]
    fn usr_under_x86_64_gnu_cxx_is_libstdcxx() {
        assert_eq!(
            canonical_repo(
                "/usr/include/x86_64-linux-gnu/c++/12/bits/stl_vector.h",
                &[]
            ),
            Some("system:libstdc++".to_owned())
        );
    }

    #[test]
    fn usr_under_llvm_include_cxx_is_libstdcxx() {
        assert_eq!(
            canonical_repo("/usr/lib/llvm-18/include/c++/v1/string", &[]),
            Some("system:libstdc++".to_owned())
        );
    }

    // ── system:libcxx (macOS) ─────────────────────────────────────────────────

    #[test]
    fn usr_under_commandlinetools_cxx_is_libcxx() {
        assert_eq!(
            canonical_repo(
                "/Library/Developer/CommandLineTools/usr/include/c++/v1/vector",
                &[]
            ),
            Some("system:libcxx".to_owned())
        );
    }

    // ── system:clang-builtins ─────────────────────────────────────────────────

    #[test]
    fn usr_under_clang_internal_is_clang_builtins() {
        assert_eq!(
            canonical_repo("/usr/lib/llvm-18/lib/clang/18/include/stdint.h", &[]),
            Some("system:clang-builtins".to_owned())
        );
    }

    #[test]
    fn compiler_internal_path_is_clang_builtins() {
        // Typical path emitted by clang on Linux.
        assert_eq!(
            canonical_repo("/usr/local/lib/clang/17.0.6/include/float.h", &[]),
            Some("system:clang-builtins".to_owned())
        );
    }

    // ── system:libc ───────────────────────────────────────────────────────────

    #[test]
    fn usr_under_usr_include_is_libc() {
        assert_eq!(
            canonical_repo("/usr/include/stdio.h", &[]),
            Some("system:libc".to_owned())
        );
    }

    #[test]
    fn usr_under_usr_include_sys_is_libc() {
        assert_eq!(
            canonical_repo("/usr/include/sys/types.h", &[]),
            Some("system:libc".to_owned())
        );
    }

    // ── repo:vendored:<pkg> ───────────────────────────────────────────────────

    #[test]
    fn third_party_pkg_is_vendored() {
        assert_eq!(
            canonical_repo("/home/user/myrepo/third_party/abseil/absl/base/base.h", &[]),
            Some("repo:vendored:abseil".to_owned())
        );
    }

    #[test]
    fn vendor_pkg_is_vendored() {
        assert_eq!(
            canonical_repo("/workspace/project/vendor/fmt/include/fmt/format.h", &[]),
            Some("repo:vendored:fmt".to_owned())
        );
    }

    #[test]
    fn vendor_bare_no_match() {
        // Path ends at "vendor/" with no pkg name — must not match.
        assert_eq!(canonical_repo("/project/vendor/", &[]), None);
    }

    // ── Config override list takes precedence ─────────────────────────────────

    #[test]
    fn override_list_takes_precedence_over_system_rule() {
        let overrides = vec![CanonicalOverride {
            path_prefix: "/usr/include/c++/".to_owned(),
            repo_ref: "repo:boost".to_owned(),
        }];
        // Would normally match system:libstdc++, but override wins.
        assert_eq!(
            canonical_repo("/usr/include/c++/12/vector", &overrides),
            Some("repo:boost".to_owned())
        );
    }

    #[test]
    fn override_list_takes_precedence_over_vendored_rule() {
        let overrides = vec![CanonicalOverride {
            path_prefix: "/opt/myrepo/third_party/abseil/".to_owned(),
            repo_ref: "repo:abseil-upstream".to_owned(),
        }];
        assert_eq!(
            canonical_repo(
                "/opt/myrepo/third_party/abseil/absl/strings/string_view.h",
                &overrides
            ),
            Some("repo:abseil-upstream".to_owned())
        );
    }

    #[test]
    fn first_matching_override_wins() {
        let overrides = vec![
            CanonicalOverride {
                path_prefix: "/usr/include/".to_owned(),
                repo_ref: "repo:first".to_owned(),
            },
            CanonicalOverride {
                path_prefix: "/usr/include/stdio.h".to_owned(),
                repo_ref: "repo:second".to_owned(),
            },
        ];
        assert_eq!(
            canonical_repo("/usr/include/stdio.h", &overrides),
            Some("repo:first".to_owned())
        );
    }

    // ── No match ──────────────────────────────────────────────────────────────

    #[test]
    fn user_project_path_returns_none() {
        assert_eq!(
            canonical_repo("/home/user/myproject/src/main.cpp", &[]),
            None
        );
    }

    #[test]
    fn empty_path_returns_none() {
        assert_eq!(canonical_repo("", &[]), None);
    }
}
