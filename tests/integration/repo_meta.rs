//! Integration tests for S22 — REPO node + BELONGS_TO_REPO edges.
//!
//! AC covered: AC-M4-1, AC-M4-2, AC-M4-3 (attribution only; enforce in S23).
//!
//! The test:
//!   1. Creates a temporary git repository with one commit.
//!   2. Runs the pipeline (MockSink) against the M1 fixture with an explicit
//!      `compile_commands.json` path, but with `input_path` pointing to the
//!      temp git repo root so `repo_meta::collect` can discover the git repo.
//!   3. Asserts that:
//!      - Exactly one REPO node exists in the write batch.
//!      - The REPO node's `attrs_json` contains non-empty `name`, `root_path`,
//!        `commit_sha`, `commit_date`, and `sink` fields.
//!      - Every non-REPO node has exactly one BELONGS_TO_REPO edge pointing to
//!        the REPO node's USR.

use std::collections::{HashMap, HashSet};
use std::path::Path;
use std::sync::Arc;

use tempfile::TempDir;

use cpp_indexer::pipeline::{run, RunOptions};
use cpp_indexer::schema::edges::EdgeKind;
use cpp_indexer::schema::nodes::NodeKind;
use cpp_indexer::sink::mock::{MockCall, MockSink};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Path to the M1 5-file fixture (has a `compile_commands.json`).
fn m1_fixture_dir() -> std::path::PathBuf {
    let manifest = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    manifest.join("tests/fixtures/m1_5file")
}

/// Create a minimal git repo in `dir` with one commit at the given epoch.
fn init_git_repo_with_commit(dir: &Path, epoch_secs: i64) {
    let repo = git2::Repository::init(dir).expect("git init");
    let sig = git2::Signature::new("Test", "test@example.com", &git2::Time::new(epoch_secs, 0))
        .expect("signature");
    let tree_oid = {
        let mut index = repo.index().expect("index");
        index.write_tree().expect("write tree")
    };
    let tree = repo.find_tree(tree_oid).expect("find tree");
    repo.commit(Some("HEAD"), &sig, &sig, "initial commit", &tree, &[])
        .expect("commit");
}

fn copy_fixture_into_repo(fixture: &Path, repo_dir: &Path) {
    for entry in std::fs::read_dir(fixture).expect("fixture read_dir") {
        let entry = entry.expect("fixture entry");
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let name = path.file_name().unwrap().to_string_lossy();
        if name == "compile_commands.json" || name == "golden_graph.json" {
            continue;
        }
        std::fs::copy(&path, repo_dir.join(name.as_ref())).expect("copy fixture file");
    }

    let repo = repo_dir.canonicalize().expect("repo canonicalize");
    let entries: Vec<_> = ["shapes.cpp", "utils.cpp", "broken.cpp"]
        .into_iter()
        .map(|name| {
            let file = repo.join(name);
            serde_json::json!({
                "directory": repo,
                "file": file,
                "arguments": [
                    "clang++",
                    "-std=c++14",
                    format!("-I{}", repo.display()),
                    file.to_string_lossy()
                ]
            })
        })
        .collect();
    std::fs::write(
        repo_dir.join("compile_commands.json"),
        serde_json::to_vec_pretty(&entries).unwrap(),
    )
    .expect("write compile_commands");
}

// ---------------------------------------------------------------------------
// AC-M4-1: REPO node exists with required attributes
// ---------------------------------------------------------------------------

#[tokio::test]
async fn repo_node_has_required_attrs() {
    let fixture = m1_fixture_dir();
    if !fixture.exists() {
        eprintln!("Skipping repo_node_has_required_attrs: m1_5file fixture absent");
        return;
    }

    let git_tmp = TempDir::new().expect("tempdir");
    copy_fixture_into_repo(&fixture, git_tmp.path());
    // 2026-05-17 00:00:00 UTC
    init_git_repo_with_commit(git_tmp.path(), 1_747_440_000);

    let stage_tmp = TempDir::new().expect("tempdir");
    let sink = Arc::new(MockSink::default());

    let opts = RunOptions {
        // input_path points to the git repo so repo_meta::collect can discover it.
        input_path: git_tmp.path().to_path_buf(),
        compile_commands: Some(git_tmp.path().join("compile_commands.json")),
        repo_name: "test-repo".to_owned(),
        stage_dir: Some(stage_tmp.path().to_path_buf()),
        skip_phase2: true,
        skip_system_headers: true,
        workers: None,
        skip_cache: true,
        skip_repo_node: false,
        symbol_db_path: None,
        symbol_cache_size: 100_000,
    };

    run(
        Arc::clone(&sink) as Arc<dyn cpp_indexer::sink::GraphSink>,
        opts,
    )
    .await
    .expect("pipeline must succeed");

    let calls = sink.calls();

    // Collect all node batches into a flat list.
    let all_nodes: Vec<_> = calls
        .iter()
        .filter_map(|c| {
            if let MockCall::WriteNodes(batch) = c {
                Some(batch.clone())
            } else {
                None
            }
        })
        .flatten()
        .collect();

    // ── AC-M4-1: exactly one REPO node ──────────────────────────────────────
    let repo_nodes: Vec<_> = all_nodes
        .iter()
        .filter(|n| n.kind == NodeKind::Repo)
        .collect();

    assert_eq!(
        repo_nodes.len(),
        1,
        "exactly one REPO node must be written; got {}",
        repo_nodes.len()
    );

    let repo_node = repo_nodes[0];
    let attrs: serde_json::Value =
        serde_json::from_str(&repo_node.attrs_json).expect("attrs_json must be valid JSON");

    // name
    assert_eq!(
        repo_node.name, "test-repo",
        "REPO node name must equal repo_name"
    );

    // root_path — non-empty string
    let root_path = attrs["root_path"].as_str().unwrap_or("");
    assert!(
        !root_path.is_empty(),
        "REPO attrs_json.root_path must be non-empty"
    );

    // commit_sha — 40 hex chars
    let commit_sha = attrs["commit_sha"].as_str().unwrap_or("");
    assert_eq!(
        commit_sha.len(),
        40,
        "REPO attrs_json.commit_sha must be 40 hex chars; got {:?}",
        commit_sha
    );

    // commit_date — YYYY-MM-DD format
    let commit_date = attrs["commit_date"].as_str().unwrap_or("");
    assert_eq!(
        commit_date.len(),
        10,
        "REPO attrs_json.commit_date must be YYYY-MM-DD; got {:?}",
        commit_date
    );
    assert!(
        commit_date.contains('-'),
        "commit_date must contain dashes; got {:?}",
        commit_date
    );

    // sink — non-empty backend name
    let sink_attr = attrs["sink"].as_str().unwrap_or("");
    assert!(
        !sink_attr.is_empty(),
        "REPO attrs_json.sink must be non-empty"
    );
}

// ---------------------------------------------------------------------------
// AC-M4-2: every non-REPO node has exactly one BELONGS_TO_REPO edge
// ---------------------------------------------------------------------------

#[tokio::test]
async fn every_node_has_belongs_to_repo_edge() {
    let fixture = m1_fixture_dir();
    if !fixture.exists() {
        eprintln!("Skipping every_node_has_belongs_to_repo_edge: m1_5file fixture absent");
        return;
    }

    let git_tmp = TempDir::new().expect("tempdir");
    copy_fixture_into_repo(&fixture, git_tmp.path());
    init_git_repo_with_commit(git_tmp.path(), 1_747_440_000);

    let stage_tmp = TempDir::new().expect("tempdir");
    let sink = Arc::new(MockSink::default());

    let opts = RunOptions {
        input_path: git_tmp.path().to_path_buf(),
        compile_commands: Some(git_tmp.path().join("compile_commands.json")),
        repo_name: "test-repo".to_owned(),
        stage_dir: Some(stage_tmp.path().to_path_buf()),
        skip_phase2: true,
        skip_system_headers: true,
        workers: None,
        skip_cache: true,
        skip_repo_node: false,
        symbol_db_path: None,
        symbol_cache_size: 100_000,
    };

    run(
        Arc::clone(&sink) as Arc<dyn cpp_indexer::sink::GraphSink>,
        opts,
    )
    .await
    .expect("pipeline must succeed");

    let calls = sink.calls();

    let all_nodes: Vec<_> = calls
        .iter()
        .filter_map(|c| {
            if let MockCall::WriteNodes(batch) = c {
                Some(batch.clone())
            } else {
                None
            }
        })
        .flatten()
        .collect();

    let all_edges: Vec<_> = calls
        .iter()
        .filter_map(|c| {
            if let MockCall::WriteEdges(batch) = c {
                Some(batch.clone())
            } else {
                None
            }
        })
        .flatten()
        .collect();

    // Collect the REPO node USR.
    let repo_usrs: HashSet<String> = all_nodes
        .iter()
        .filter(|n| n.kind == NodeKind::Repo)
        .map(|n| n.usr.clone())
        .collect();
    assert_eq!(repo_usrs.len(), 1, "exactly one REPO USR expected");
    let repo_usr = repo_usrs.into_iter().next().unwrap();

    // Build a map: node_usr → count of BELONGS_TO_REPO edges to repo_usr.
    let mut belongs_count: HashMap<String, usize> = HashMap::new();
    for edge in &all_edges {
        if edge.kind == EdgeKind::BelongsToRepo {
            assert_eq!(
                edge.dst_usr.as_deref(),
                Some(repo_usr.as_str()),
                "BELONGS_TO_REPO edge must point to the REPO node"
            );
            *belongs_count.entry(edge.src_usr.clone()).or_insert(0) += 1;
        }
    }

    // Every non-REPO node must have exactly one BELONGS_TO_REPO edge.
    let non_repo_nodes: Vec<_> = all_nodes
        .iter()
        .filter(|n| n.kind != NodeKind::Repo)
        .collect();

    assert!(
        !non_repo_nodes.is_empty(),
        "pipeline must emit at least one non-REPO node"
    );

    for node in &non_repo_nodes {
        let count = belongs_count.get(&node.usr).copied().unwrap_or(0);
        assert_eq!(
            count, 1,
            "node {:?} (kind={:?}) must have exactly one BELONGS_TO_REPO edge; got {}",
            node.usr, node.kind, count
        );
    }
}
