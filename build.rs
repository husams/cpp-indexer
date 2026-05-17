// build.rs — S30 prompt-codegen
//
// Writes `prompt/graph_database/cpp/schema.txt` from the enum tables in
// `src/prompt/codegen.rs`. The generated file is committed; CI fails on drift
// (AC-M6-3).
//
// We `include!` codegen.rs rather than calling into the crate because build.rs
// compiles before the library crate and cannot import it.
//
// The SCHEMA_VERSION_TAG literal is taken from src/schema/version.rs via a
// simple text search rather than `include!`, since that file contains `use`
// items that may reference crate types unavailable during the build script.

use std::path::PathBuf;

// Pull in the generate_schema() function + the constant tables.
// codegen.rs must be self-contained: no `use crate::…`, no derive macros
// that require external crate dependencies the build script doesn't have.
// The `#[cfg(test)]` block inside codegen.rs is inert here (cfg(test) is false
// for build scripts).
include!("src/prompt/codegen.rs");

/// Extract `SCHEMA_VERSION_TAG` from `src/schema/version.rs` by scanning for
/// the literal assignment. We do not `include!` that file because it contains
/// items referencing crate internals.
fn read_schema_version_tag(manifest_dir: &std::path::Path) -> String {
    let path = manifest_dir.join("src/schema/version.rs");
    let src = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("build.rs: cannot read {}: {e}", path.display()));

    // Match: pub const SCHEMA_VERSION_TAG: &str = "cxg-schema-vN";
    for line in src.lines() {
        let trimmed = line.trim();
        if trimmed.starts_with("pub const SCHEMA_VERSION_TAG") {
            if let Some(start) = trimmed.find('"') {
                if let Some(end) = trimmed[start + 1..].find('"') {
                    return trimmed[start + 1..start + 1 + end].to_string();
                }
            }
        }
    }
    panic!(
        "build.rs: could not find SCHEMA_VERSION_TAG in {}",
        path.display()
    );
}

fn main() {
    // Re-run whenever the schema sources or the codegen logic change.
    println!("cargo:rerun-if-changed=src/schema/nodes.rs");
    println!("cargo:rerun-if-changed=src/schema/edges.rs");
    println!("cargo:rerun-if-changed=src/schema/version.rs");
    println!("cargo:rerun-if-changed=src/prompt/codegen.rs");

    let manifest_dir = PathBuf::from(
        std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR must be set by Cargo"),
    );

    let schema_version_tag = read_schema_version_tag(&manifest_dir);

    let schema_text = generate_schema(&schema_version_tag);

    // Write to the tracked file so `git diff --exit-code prompt/` catches drift.
    let out_path = manifest_dir
        .join("prompt")
        .join("graph_database")
        .join("cpp")
        .join("schema.txt");

    // Create directories if they do not yet exist (first run after repo clone).
    std::fs::create_dir_all(out_path.parent().unwrap())
        .unwrap_or_else(|e| panic!("build.rs: cannot create {}: {e}", out_path.display()));

    std::fs::write(&out_path, schema_text)
        .unwrap_or_else(|e| panic!("build.rs: cannot write {}: {e}", out_path.display()));
}
