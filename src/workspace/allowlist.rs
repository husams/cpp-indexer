//! Host-suffix allowlist for git URLs.
//!
//! Only HTTPS URLs are accepted (SSH is deferred to v2 per ADR-6/Q6).
//! Host matching is case-insensitive suffix-based: `foo.github.com` matches
//! the allowlist entry `github.com`.
//!
//! AC-M7-14.

use url::Url;

/// Return `true` if `raw_url` is an HTTPS URL whose host suffix-matches an
/// entry in `allowed_hosts` (case-insensitive).
///
/// Returns `false` for:
/// - non-HTTPS schemes (including SSH, git://, file://),
/// - URLs that fail to parse,
/// - URLs whose host does not suffix-match any entry.
pub fn is_allowed(raw_url: &str, allowed_hosts: &[String]) -> bool {
    let parsed = match Url::parse(raw_url) {
        Ok(u) => u,
        Err(_) => return false,
    };

    // HTTPS only for v1.
    if parsed.scheme() != "https" {
        return false;
    }

    let host = match parsed.host_str() {
        Some(h) => h.to_ascii_lowercase(),
        None => return false,
    };

    for entry in allowed_hosts {
        let entry_lc = entry.to_ascii_lowercase();
        // Exact match or subdomain suffix: `foo.github.com` matches `github.com`.
        if host == entry_lc || host.ends_with(&format!(".{entry_lc}")) {
            return true;
        }
    }
    false
}

// ── Tests ──────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn hosts(s: &[&str]) -> Vec<String> {
        s.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn exact_host_match() {
        assert!(is_allowed(
            "https://github.com/org/repo",
            &hosts(&["github.com"])
        ));
    }

    #[test]
    fn subdomain_suffix_match() {
        assert!(is_allowed(
            "https://sub.github.com/org/repo",
            &hosts(&["github.com"])
        ));
    }

    #[test]
    fn case_insensitive_host() {
        assert!(is_allowed(
            "https://GITHUB.COM/org/repo",
            &hosts(&["github.com"])
        ));
    }

    #[test]
    fn case_insensitive_allowlist_entry() {
        assert!(is_allowed(
            "https://github.com/org/repo",
            &hosts(&["GitHub.com"])
        ));
    }

    #[test]
    fn disallowed_host_returns_false() {
        assert!(!is_allowed(
            "https://evil.example.com/repo",
            &hosts(&["github.com"])
        ));
    }

    #[test]
    fn partial_suffix_does_not_match() {
        // `notgithub.com` should NOT match allowlist entry `github.com`.
        assert!(!is_allowed(
            "https://notgithub.com/repo",
            &hosts(&["github.com"])
        ));
    }

    #[test]
    fn http_scheme_rejected() {
        assert!(!is_allowed(
            "http://github.com/org/repo",
            &hosts(&["github.com"])
        ));
    }

    #[test]
    fn ssh_scheme_rejected() {
        assert!(!is_allowed(
            "ssh://git@github.com/org/repo",
            &hosts(&["github.com"])
        ));
    }

    #[test]
    fn git_scheme_rejected() {
        assert!(!is_allowed(
            "git://github.com/org/repo",
            &hosts(&["github.com"])
        ));
    }

    #[test]
    fn file_scheme_rejected() {
        assert!(!is_allowed("file:///tmp/bare.git", &hosts(&["github.com"])));
    }

    #[test]
    fn invalid_url_returns_false() {
        assert!(!is_allowed("not a url at all", &hosts(&["github.com"])));
    }

    #[test]
    fn empty_allowlist_rejects_all() {
        assert!(!is_allowed("https://github.com/org/repo", &[]));
    }

    #[test]
    fn multiple_entries_first_match_wins() {
        let allowed = hosts(&["gitlab.senussi.me", "github.com"]);
        assert!(is_allowed("https://gitlab.senussi.me/proj/repo", &allowed));
        assert!(is_allowed("https://github.com/org/repo", &allowed));
    }
}
