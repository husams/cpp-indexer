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
/// Bumped in S26 (macros): added MACRO node kind and EXPANDS_TO edge kind.
/// Bumped in S40 (structured-attrs, M8): promoted 10 node fields + 2 edge fields out of
/// `attrs_json` into native columns on NodeRecord/EdgeRecord/Arrow. No dual-write; full promotion
/// per ADR-11. Pre-v5 graphs are refused at handshake.
/// Bumped in graph-symbol-ids (S3, Story 3): added integer ID fields `symbol_id`, `file_id` on
/// NodeRecord and `src_id`, `dst_id`, `dst_repo_name` on EdgeRecord. Both sinks write integer IDs
/// only (no `usr`/`src_usr`/`dst_usr` strings in the durable graph). Pre-v6 graphs are refused at
/// handshake (adr-2, adr-4).
/// Any further change to NodeKind/EdgeKind variants must bump again per ADR-9.
pub const SCHEMA_VERSION: u32 = 6;

/// Human-readable schema tag derived from `SCHEMA_VERSION`.
///
/// ADR-9 references `const_format::concatcp!` to derive this; since `const_format` is not in
/// Cargo.toml, we use a hand-written string literal with a `debug_assert!` to keep them in sync.
pub const SCHEMA_VERSION_TAG: &str = "cxg-schema-v6";

/// Magic key stored in Parquet KV metadata; Phase 3 refuses mismatched-version shards.
pub const PARQUET_MAGIC: &str = "cxg_parquet_v6";

/// Build the `attrs_json` payload for the `SchemaVersion` node written by Phase 4.
///
/// Per ADR-9: the node carries `version: u32`, `tag: String`, `indexer_commit: String`,
/// `libclang_version: String`, and `wrote_at: ISO8601`.
///
/// `indexer_commit` is supplied by the caller (baked from `build.rs` via
/// `env!("CXG_INDEXER_COMMIT")`); `wrote_at` is the current UTC timestamp.
/// `libclang_version` is obtained from `clang::get_version()` and passed in.
pub fn schema_version_attrs(indexer_commit: &str, libclang_version: &str) -> String {
    let wrote_at = {
        // RFC 3339 / ISO 8601: use SystemTime → seconds since epoch and format manually
        // so we avoid pulling in `chrono` for a single timestamp.
        let secs = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        // Format as YYYY-MM-DDTHH:MM:SSZ using integer arithmetic (UTC only).
        let s = secs;
        let sec = s % 60;
        let min = (s / 60) % 60;
        let hour = (s / 3600) % 24;
        let days = s / 86400; // days since 1970-01-01

        // Gregorian calendar conversion from days since epoch.
        let (year, month, day) = days_to_ymd(days);

        format!("{year:04}-{month:02}-{day:02}T{hour:02}:{min:02}:{sec:02}Z")
    };

    serde_json::json!({
        "version": SCHEMA_VERSION,
        "tag": SCHEMA_VERSION_TAG,
        "indexer_commit": indexer_commit,
        "libclang_version": libclang_version,
        "wrote_at": wrote_at,
    })
    .to_string()
}

/// Convert days since Unix epoch (1970-01-01) to `(year, month, day)` in UTC.
///
/// Uses the Gregorian calendar proleptic algorithm.
fn days_to_ymd(days: u64) -> (u64, u64, u64) {
    // Algorithm: http://howardhinnant.github.io/date_algorithms.html (civil_from_days)
    let z = days + 719_468;
    let era = z / 146_097;
    let doe = z % 146_097;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    (y, m, d)
}

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

    #[test]
    fn schema_version_attrs_is_valid_json() {
        let attrs = schema_version_attrs("abc123", "libclang 18.0");
        let v: serde_json::Value =
            serde_json::from_str(&attrs).expect("schema_version_attrs must produce valid JSON");
        assert_eq!(v["version"], SCHEMA_VERSION);
        assert_eq!(v["tag"], SCHEMA_VERSION_TAG);
        assert_eq!(v["indexer_commit"], "abc123");
        assert_eq!(v["libclang_version"], "libclang 18.0");
        assert!(
            v["wrote_at"].as_str().unwrap().contains('T'),
            "wrote_at must be ISO8601"
        );
    }

    #[test]
    fn schema_version_attrs_embeds_current_version() {
        let attrs = schema_version_attrs("deadbeef", "18.x");
        let v: serde_json::Value = serde_json::from_str(&attrs).unwrap();
        assert_eq!(
            v["version"].as_u64().unwrap() as u32,
            SCHEMA_VERSION,
            "attrs version must match SCHEMA_VERSION constant"
        );
    }

    #[test]
    fn days_to_ymd_epoch_is_1970_01_01() {
        let (y, m, d) = days_to_ymd(0);
        assert_eq!((y, m, d), (1970, 1, 1));
    }

    #[test]
    fn days_to_ymd_known_date() {
        // 2024-01-01 = day 19723 since epoch.
        let (y, m, d) = days_to_ymd(19_723);
        assert_eq!((y, m, d), (2024, 1, 1));
    }
}
