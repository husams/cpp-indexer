//! Source-code capture limits (ADR-12).
//!
//! `MAX_CODE_BYTES` is the maximum byte length of a function/method body that
//! is stored inline on a node record.  When the raw source range exceeds this
//! limit the `code` field is set to `None` and `code_truncated` to `Some(true)`.
//!
//! AC-S41-5: source range ≤ 32 768 bytes → `code = Some(…)`, `code_truncated = Some(false)`.
//! AC-S41-6: source range > 32 768 bytes → `code = None`,    `code_truncated = Some(true)`.

/// Maximum byte length of a code snippet stored inline (32 KiB).
pub const MAX_CODE_BYTES: usize = 32 * 1024;

/// Apply the 32 KiB cap to a source snippet.
///
/// Returns `(Some(src.to_owned()), false)` when `src.len() <= MAX_CODE_BYTES`,
/// or `(None, true)` otherwise, matching the ADR-12 contract.
///
/// The boolean component is the *truncated* flag: callers should write it to
/// `code_truncated` wrapped in `Some(…)` for FUNCTION/METHOD nodes.
pub fn clip_code(src: &str) -> (Option<String>, bool) {
    if src.len() <= MAX_CODE_BYTES {
        (Some(src.to_owned()), false)
    } else {
        (None, true)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clip_code_within_limit_returns_some_false() {
        let s = "a".repeat(MAX_CODE_BYTES);
        let (code, truncated) = clip_code(&s);
        assert_eq!(code, Some(s));
        assert!(!truncated);
    }

    #[test]
    fn clip_code_exactly_at_limit_returns_some_false() {
        // Exactly 32 768 bytes fits (AC-S41-5: ≤ limit).
        let s = "x".repeat(MAX_CODE_BYTES);
        let (code, truncated) = clip_code(&s);
        assert!(code.is_some());
        assert!(
            !truncated,
            "exactly at limit must not be flagged as truncated"
        );
    }

    #[test]
    fn clip_code_one_over_limit_returns_none_true() {
        // 32 769 bytes must be discarded (AC-S41-6: > limit).
        let s = "x".repeat(MAX_CODE_BYTES + 1);
        let (code, truncated) = clip_code(&s);
        assert!(code.is_none(), "oversized snippet must yield None for code");
        assert!(truncated, "oversized snippet must set truncated = true");
    }

    #[test]
    fn clip_code_empty_returns_some_false() {
        let (code, truncated) = clip_code("");
        assert_eq!(code, Some(String::new()));
        assert!(!truncated);
    }
}
