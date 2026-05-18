//! Schema drift detection tests (S46, AC-S46-5).
//!
//! # Non-live tests (run always)
//!
//! - `schema_txt_contains_all_promoted_fields`: parses `prompt/graph_database/cpp/schema.txt`
//!   and asserts every promoted field name appears in the file.
//! - `schema_md_contains_all_promoted_fields`: same check against `docs/schema/SCHEMA.md`.
//! - `drift_parser_detects_mutation`: unit test — mutates one name in a synthetic fixture and
//!   verifies the check catches it.
//!
//! # Live tests (opt-in via `CPP_INDEXER_LIVE_NEO4J=1`)
//!
//! - `schema_drift_live_neo4j`: queries `CALL db.propertyKeys()` against the dev Neo4j
//!   instance and asserts every promoted field name is present.
//!
//! Run the live test with:
//! ```bash
//! CPP_INDEXER_LIVE_NEO4J=1 \
//!   NEO4J_URI=bolt://192.168.1.200:7687 \
//!   NEO4J_PASSWORD=<password> \
//!   cargo test --test schema_drift -- --ignored --nocapture
//! ```
//!
//! AC-S46-5: drift check passes when names match, fails when a name is mutated.

use std::collections::BTreeSet;
use std::path::Path;

// ── Canonical field name list ─────────────────────────────────────────────────
//
// Single source of truth for this test.  Names must match the `pub` field names
// in `src/schema/nodes.rs` (NodeRecord) and `src/schema/edges.rs` (EdgeRecord).

/// All 12 M8 promoted field names (10 node + 2 edge).
const PROMOTED_FIELDS: &[&str] = &[
    // Node fields (S40–S42)
    "return_type",
    "params",
    "signature",
    "code",
    "code_truncated",
    "template_params",
    "template_args",
    "is_virtual",
    "is_pure_virtual",
    "is_static",
    // Edge fields (S43)
    "source_association_type",
    "target_association_type",
];

// ── Helpers ───────────────────────────────────────────────────────────────────

fn workspace_root() -> std::path::PathBuf {
    // Cargo sets CARGO_MANIFEST_DIR to the crate root when running tests.
    let manifest_dir =
        std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR must be set by cargo test");
    Path::new(&manifest_dir).to_path_buf()
}

fn live_neo4j_enabled() -> bool {
    std::env::var("CPP_INDEXER_LIVE_NEO4J")
        .map(|v| v == "1")
        .unwrap_or(false)
}

fn neo4j_uri() -> String {
    std::env::var("NEO4J_URI").unwrap_or_else(|_| "bolt://192.168.1.200:7687".to_owned())
}

fn neo4j_password() -> String {
    std::env::var("NEO4J_PASSWORD").unwrap_or_else(|_| "neo4j".to_owned())
}

/// Returns `true` when `c` is an identifier character (`[A-Za-z0-9_]`).
///
/// Used to implement word-boundary detection without a regex dependency.
fn is_ident_char(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_'
}

/// Parse field names from a text file with word-boundary matching.
///
/// A name is considered "found" only when every occurrence in the file is
/// bounded on both sides by a non-identifier character (or start/end of
/// string).  This prevents `is_virtual` from being shadowed by
/// `is_pure_virtual`, `code` by `code_truncated`, or `params` by
/// `template_params`.
///
/// Returns a set of names from `candidates` that were found with a valid
/// word boundary.
fn names_found_in_file(file_content: &str, candidates: &[&str]) -> BTreeSet<String> {
    candidates
        .iter()
        .filter(|&&name| {
            let bytes = file_content.as_bytes();
            let name_bytes = name.as_bytes();
            let name_len = name_bytes.len();

            // Scan every position where `name` could start.
            bytes.windows(name_len).enumerate().any(|(i, window)| {
                if window != name_bytes {
                    return false;
                }
                // Check that the character before (if any) is not an ident char.
                let left_ok =
                    i == 0 || !is_ident_char(file_content[..i].chars().next_back().unwrap());
                // Check that the character after (if any) is not an ident char.
                let end = i + name_len;
                let right_ok = end == bytes.len()
                    || !is_ident_char(file_content[end..].chars().next().unwrap());
                left_ok && right_ok
            })
        })
        .map(|&s| s.to_owned())
        .collect()
}

/// Assert that every name in `expected` appears in `found`; panic with a
/// descriptive message listing missing names if any are absent.
fn assert_no_missing(label: &str, expected: &[&str], found: &BTreeSet<String>) {
    let missing: Vec<&str> = expected
        .iter()
        .copied()
        .filter(|n| !found.contains(*n))
        .collect();
    assert!(
        missing.is_empty(),
        "{label}: missing promoted field names: {missing:?}\n\
         These names must appear in the file so that consumers of the file \
         know about the M8 v5 promoted properties.\n\
         Add or correct the field listing and re-run `cargo test --test schema_drift`."
    );
}

// ── Non-live tests ────────────────────────────────────────────────────────────

/// AC-S46-5 (doc): all 12 promoted field names appear in `schema.txt`.
#[test]
fn schema_txt_contains_all_promoted_fields() {
    let root = workspace_root();
    let path = root.join("prompt/graph_database/cpp/schema.txt");
    let content = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("Cannot read {}: {e}", path.display()));

    let found = names_found_in_file(&content, PROMOTED_FIELDS);
    assert_no_missing("schema.txt", PROMOTED_FIELDS, &found);
}

/// AC-S46-5 (doc): all 12 promoted field names appear in `docs/schema/SCHEMA.md`.
#[test]
fn schema_md_contains_all_promoted_fields() {
    let root = workspace_root();
    let path = root.join("docs/schema/SCHEMA.md");
    let content = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("Cannot read {}: {e}", path.display()));

    let found = names_found_in_file(&content, PROMOTED_FIELDS);
    assert_no_missing("SCHEMA.md", PROMOTED_FIELDS, &found);
}

/// AC-S46-5 (fixture mutation): verify the detector catches a mutated name.
///
/// Two sub-cases:
/// 1. `return_type` replaced with a bogus name — detector must report it missing.
/// 2. `is_virtual` absent while `is_pure_virtual` is still present — verifies that
///    word-boundary matching prevents the shorter name being shadowed by the longer one.
#[test]
fn drift_parser_detects_mutation() {
    // ── Sub-case 1: rename return_type ────────────────────────────────────────
    let mutated_name = "return_typ_MUTATED";
    let synthetic_content: String = PROMOTED_FIELDS
        .iter()
        .map(|&n| {
            if n == "return_type" {
                format!("{mutated_name}\n")
            } else {
                format!("{n}\n")
            }
        })
        .collect();

    let found = names_found_in_file(&synthetic_content, PROMOTED_FIELDS);

    assert!(
        !found.contains("return_type"),
        "detector should NOT find 'return_type' in mutated fixture, but it did"
    );

    let other_expected: Vec<&str> = PROMOTED_FIELDS
        .iter()
        .copied()
        .filter(|&n| n != "return_type")
        .collect();
    for name in &other_expected {
        assert!(
            found.contains(*name),
            "name '{name}' should be found in synthetic fixture but was missing"
        );
    }

    // ── Sub-case 2: is_virtual absent, is_pure_virtual present ───────────────
    // Verifies word-boundary matching: `is_virtual` must NOT be found as a
    // substring of `is_pure_virtual`.
    let no_is_virtual = "is_pure_virtual\nparams\ncode\ntemplate_params\n";
    let found2 = names_found_in_file(no_is_virtual, PROMOTED_FIELDS);

    assert!(
        !found2.contains("is_virtual"),
        "word-boundary check failed: 'is_virtual' must NOT be found as a \
         substring of 'is_pure_virtual', but names_found_in_file reported it present"
    );
    assert!(
        found2.contains("is_pure_virtual"),
        "'is_pure_virtual' should be found in fixture, but was absent"
    );

    // Sub-case 2b: `code` absent, `code_truncated` present — same boundary check.
    let no_code = "code_truncated\nparams\ntemplate_params\n";
    let found3 = names_found_in_file(no_code, PROMOTED_FIELDS);

    assert!(
        !found3.contains("code"),
        "word-boundary check failed: 'code' must NOT be found as a substring \
         of 'code_truncated', but names_found_in_file reported it present"
    );
    assert!(
        found3.contains("code_truncated"),
        "'code_truncated' should be found in fixture, but was absent"
    );

    // Sub-case 2c: `params` absent, `template_params` present.
    let no_params = "template_params\ncode\ncode_truncated\n";
    let found4 = names_found_in_file(no_params, PROMOTED_FIELDS);

    assert!(
        !found4.contains("params"),
        "word-boundary check failed: 'params' must NOT be found as a substring \
         of 'template_params', but names_found_in_file reported it present"
    );
    assert!(
        found4.contains("template_params"),
        "'template_params' should be found in fixture, but was absent"
    );
}

// ── Live test ─────────────────────────────────────────────────────────────────

/// AC-S46-5 (live): query `CALL db.propertyKeys()` and assert all 12 promoted
/// field names are present in the dev Neo4j instance.
///
/// Gated by `CPP_INDEXER_LIVE_NEO4J=1`.  Requires at least one repo to have
/// been indexed with the v5 schema so that property keys are registered.
#[tokio::test]
#[ignore = "requires CPP_INDEXER_LIVE_NEO4J=1 and NEO4J_URI/NEO4J_PASSWORD and a v5-indexed repo"]
async fn schema_drift_live_neo4j() {
    if !live_neo4j_enabled() {
        eprintln!("SKIP: CPP_INDEXER_LIVE_NEO4J not set to 1");
        return;
    }

    let uri = neo4j_uri();
    let password = neo4j_password();
    let pw_env = "CXG_SCHEMA_DRIFT_TEST_NEO4J_PW";
    // SAFETY: test process; single-threaded env-var access for test setup only.
    unsafe {
        std::env::set_var(pw_env, &password);
    }

    let config = cpp_indexer::config::Neo4jSinkConfig {
        uri,
        user: "neo4j".to_owned(),
        password_env: pw_env.to_owned(),
        sessions: Some(4),
    };

    let sink = cpp_indexer::sink::neo4j::Neo4jSink::connect(&config, &password)
        .await
        .expect("connect to dev Neo4j must succeed");

    let graph = sink.graph_handle();

    // `CALL db.propertyKeys()` returns one row per distinct property key stored
    // in the database.  Requires at least one v5-indexed repo to be present.
    let mut stream = graph
        .execute(neo4rs::query("CALL db.propertyKeys() YIELD propertyKey"))
        .await
        .expect("db.propertyKeys() must execute");

    let mut live_keys: BTreeSet<String> = BTreeSet::new();
    while let Some(row) = stream
        .next()
        .await
        .expect("db.propertyKeys stream must not error")
    {
        if let Ok(key) = row.get::<String>("propertyKey") {
            live_keys.insert(key);
        }
    }

    eprintln!("Live Neo4j property keys: {live_keys:?}");

    let missing: Vec<&str> = PROMOTED_FIELDS
        .iter()
        .copied()
        .filter(|n| !live_keys.contains(*n))
        .collect();

    assert!(
        missing.is_empty(),
        "Schema drift detected: the following M8 promoted fields are absent \
         from the live Neo4j property-key registry: {missing:?}\n\
         Ensure at least one v5 repo has been indexed (run the re-index recipe \
         in docs/runbooks/staging-recovery.md §6) and re-run this test."
    );
}
