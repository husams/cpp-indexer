/// Monotonically-increasing schema version baked into the binary at compile time.
///
/// Per ADR-9: bump this constant in the same PR that changes `nodes.rs` or `edges.rs`.
/// The CI gate (`tests/schema_version_bump.rs`) enforces that structural changes to
/// `nodes.rs`/`edges.rs` are paired with a bump.
///
/// Note: plan.md specified `SCHEMA_VERSION: &str = "1"` but ADR-9 (accepted, governs over plan)
/// specifies `u32 = 1` plus a derived tag string. We follow ADR-9. Deviation recorded in
/// implementation-notes.md.
/// Bumped in S14 (cpp-extensions): added NAMESPACE, TEMPLATE_DECL, SPECIALIZATION, TYPEDEF,
/// ENUM, HEADER node kinds and INCLUDES, OVERRIDES, INSTANTIATES, SPECIALIZES, FRIEND_OF,
/// ADL_CANDIDATE edge kinds.
/// Bumped in S22 (repo-nodes): added REPO node kind and BELONGS_TO_REPO edge kind.
/// Any further change to NodeKind/EdgeKind variants must bump again per ADR-9.
pub const SCHEMA_VERSION: u32 = 3;

/// Human-readable schema tag derived from `SCHEMA_VERSION`.
///
/// ADR-9 references `const_format::concatcp!` to derive this; since `const_format` is not in
/// Cargo.toml, we use a hand-written string literal with a `debug_assert!` to keep them in sync.
pub const SCHEMA_VERSION_TAG: &str = "cxg-schema-v3";

/// Magic key stored in Parquet KV metadata; Phase 3 refuses mismatched-version shards.
pub const PARQUET_MAGIC: &str = "cxg_parquet_v3";

/// Verify at test-time that the tag string is consistent with the integer constant.
///
/// This fires only in debug/test builds; not a runtime overhead.
#[allow(dead_code)]
fn assert_version_tag_consistent() {
    debug_assert_eq!(
        SCHEMA_VERSION_TAG,
        &format!("cxg-schema-v{SCHEMA_VERSION}"),
        "SCHEMA_VERSION_TAG is out of sync with SCHEMA_VERSION — update both together"
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn schema_version_tag_matches_integer() {
        let expected = format!("cxg-schema-v{SCHEMA_VERSION}");
        assert_eq!(
            SCHEMA_VERSION_TAG, expected,
            "SCHEMA_VERSION_TAG must match SCHEMA_VERSION"
        );
    }

    #[test]
    fn parquet_magic_contains_version() {
        assert!(
            PARQUET_MAGIC.contains(&format!("v{SCHEMA_VERSION}")),
            "PARQUET_MAGIC must embed the version string"
        );
    }
}
