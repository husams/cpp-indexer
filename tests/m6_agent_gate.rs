//! S32-m6-agent-gate: schema-version handshake contract test.
//!
//! AC covered: AC-M6-8 (stable schema contract cpp-mcp can consume),
//!             AC-M6-9 (NL eval fixture present and well-formed).
//!
//! These are automated contract tests that do NOT require a live agent or DB.
//! The manual ≥8/10 NL-answer gate is recorded separately in test-report.md
//! by the QA engineer.
//!
//! # What we verify
//!
//! 1. `prompt/graph_database/cpp/schema.txt` contains a machine-readable
//!    `# schema-version: <tag>` header on line 3, and that tag matches the
//!    `SCHEMA_VERSION_TAG` constant in this binary.  This is the exact line
//!    cpp-mcp parses at startup (per ADR-1) to perform the version handshake.
//!
//! 2. `prompt/graph_database/cpp/example.txt` exists and is non-empty.
//!    cpp-mcp vendors both files from a tagged cpp-indexer release.
//!
//! 3. `tests/integration/m6_nl_eval.json` is well-formed JSON, contains
//!    exactly 10 entries, and each entry carries the required fields:
//!    `id`, `question`, `expected_fragments`.

use cpp_indexer::schema::version::SCHEMA_VERSION_TAG;
use serde_json::Value;
use std::fs;
use std::path::PathBuf;

fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

// ── Contract 1: schema.txt version header ────────────────────────────────────

/// `schema.txt` line 3 must contain a machine-readable `# schema-version:`
/// header whose tag matches `SCHEMA_VERSION_TAG`.
///
/// This is the exact line cpp-mcp parses on startup to verify it is consuming
/// the correct schema generation (ADR-1 §5).
#[test]
fn schema_txt_version_header_matches_binary_constant() {
    let schema_path = repo_root().join("prompt/graph_database/cpp/schema.txt");
    let content = fs::read_to_string(&schema_path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", schema_path.display()));

    // Line 3 (0-indexed line 2) must be the version comment.
    let lines: Vec<&str> = content.lines().collect();
    assert!(
        lines.len() >= 3,
        "schema.txt must have at least 3 lines; got {}",
        lines.len()
    );

    let version_line = lines[2];
    let expected = format!("# schema-version: {SCHEMA_VERSION_TAG}");
    assert_eq!(
        version_line, expected,
        "schema.txt line 3 must be the machine-readable version header \
         that cpp-mcp parses at startup.\n\
         expected: {expected}\n\
         actual:   {version_line}"
    );
}

/// `schema.txt` must not be empty and must contain both node-kind and
/// edge-kind sections (sanity check that the generator ran).
#[test]
fn schema_txt_is_non_empty_and_well_structured() {
    let schema_path = repo_root().join("prompt/graph_database/cpp/schema.txt");
    let content = fs::read_to_string(&schema_path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", schema_path.display()));

    assert!(!content.trim().is_empty(), "schema.txt must not be empty");
    assert!(
        content.contains("## Node kinds"),
        "schema.txt must contain '## Node kinds' section"
    );
    assert!(
        content.contains("## Edge kinds"),
        "schema.txt must contain '## Edge kinds' section"
    );
}

// ── Contract 2: example.txt present ──────────────────────────────────────────

/// `example.txt` must exist and be non-empty.  cpp-mcp vendors this file
/// alongside `schema.txt` (ADR-1 §2).
#[test]
fn example_txt_exists_and_is_non_empty() {
    let example_path = repo_root().join("prompt/graph_database/cpp/example.txt");
    let content = fs::read_to_string(&example_path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", example_path.display()));

    assert!(
        !content.trim().is_empty(),
        "example.txt must not be empty; it is vendored by cpp-mcp alongside schema.txt"
    );
}

// ── Contract 3: m6_nl_eval.json fixture is well-formed ───────────────────────

/// `m6_nl_eval.json` must parse as a JSON array of exactly 10 entries, each
/// containing `id`, `question`, and `expected_fragments`.
///
/// This is the QA fixture for the manual ≥8/10 gate (AC-M6-9).
#[test]
fn m6_nl_eval_json_is_well_formed() {
    let eval_path = repo_root().join("tests/integration/m6_nl_eval.json");
    let content = fs::read_to_string(&eval_path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", eval_path.display()));

    let parsed: Value = serde_json::from_str(&content)
        .unwrap_or_else(|e| panic!("m6_nl_eval.json is not valid JSON: {e}"));

    let entries = parsed
        .as_array()
        .expect("m6_nl_eval.json must be a JSON array at the top level");

    assert_eq!(
        entries.len(),
        10,
        "m6_nl_eval.json must contain exactly 10 questions (AC-M6-9 requires 8/10 pass rate); \
         got {}",
        entries.len()
    );

    for (i, entry) in entries.iter().enumerate() {
        let obj = entry
            .as_object()
            .unwrap_or_else(|| panic!("entry {i} must be a JSON object; got: {entry}"));

        assert!(
            obj.contains_key("id"),
            "entry {i} is missing required field 'id'"
        );
        assert!(
            obj.contains_key("question"),
            "entry {i} is missing required field 'question'"
        );

        let fragments = obj
            .get("expected_fragments")
            .unwrap_or_else(|| panic!("entry {i} is missing required field 'expected_fragments'"));

        let frags_arr = fragments
            .as_array()
            .unwrap_or_else(|| panic!("entry {i}: 'expected_fragments' must be an array"));

        assert!(
            !frags_arr.is_empty(),
            "entry {i}: 'expected_fragments' must have at least one fragment"
        );

        // Each fragment must be a non-empty string.
        for (j, frag) in frags_arr.iter().enumerate() {
            let s = frag
                .as_str()
                .unwrap_or_else(|| panic!("entry {i} fragment {j} must be a string"));
            assert!(
                !s.is_empty(),
                "entry {i} fragment {j} must not be an empty string"
            );
        }
    }
}

/// Q1 in the NL eval fixture must target `llvm::Value` inheritance — the
/// canonical AC-M6-8 smoke question.
#[test]
fn m6_nl_eval_q01_targets_llvm_value_inheritance() {
    let eval_path = repo_root().join("tests/integration/m6_nl_eval.json");
    let content = fs::read_to_string(&eval_path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", eval_path.display()));

    let entries: Vec<Value> = serde_json::from_str(&content).expect("valid JSON");
    let q01 = &entries[0];

    let question = q01["question"]
        .as_str()
        .expect("Q01 question must be a string");
    assert!(
        question.to_lowercase().contains("inherit") || question.to_lowercase().contains("subclass"),
        "Q01 must ask about inheritance/subclasses (AC-M6-8 canonical smoke test); \
         got: {question}"
    );
    assert!(
        question.contains("llvm::Value") || question.contains("Value"),
        "Q01 must reference llvm::Value (AC-M6-8); got: {question}"
    );

    // Expected fragments must include at least one real LLVM subclass.
    let frags = q01["expected_fragments"]
        .as_array()
        .expect("expected_fragments is an array");
    let known_subclasses = ["User", "Argument", "BasicBlock", "InlineAsm"];
    let has_known = frags
        .iter()
        .filter_map(|f| f.as_str())
        .any(|f| known_subclasses.contains(&f));
    assert!(
        has_known,
        "Q01 expected_fragments must include at least one known llvm::Value subclass \
         (User, Argument, BasicBlock, InlineAsm); got: {frags:?}"
    );
}
