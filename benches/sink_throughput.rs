//! Micro-benchmark: sink write throughput using `MockSink`.
//!
//! Story: S18-batched-sink-writes (AC-M3-4, AC-M3-5).
//! ADR: ADR-2 §Concurrency.
//!
//! # What this measures
//!
//! This bench measures the overhead of the S18 batching loop itself (chunking,
//! `JoinSet` dispatch, `WriteStats` aggregation) against the `MockSink`, which
//! has zero network cost.  It does **NOT** measure Neo4j or IndraDB throughput,
//! which requires a live server.  Use it to catch regressions in the loop
//! itself (allocation hot spots, lock contention on the mock's `Mutex`).
//!
//! # Running
//!
//! ```bash
//! BENCH=1 cargo bench --bench sink_throughput
//! ```
//!
//! The `BENCH=1` gate prevents accidental execution during `cargo test`.

#![allow(clippy::unwrap_used)]

use std::hint::black_box;
use std::sync::Arc;
use std::time::Instant;

use cpp_indexer::schema::edges::{EdgeKind, EdgeRecord};
use cpp_indexer::schema::nodes::{NodeKind, NodeRecord};
use cpp_indexer::sink::mock::MockSink;
use cpp_indexer::sink::GraphSink;

// ── Fixture builders ──────────────────────────────────────────────────────────

fn make_nodes(n: usize) -> Vec<NodeRecord> {
    (0..n)
        .map(|i| NodeRecord {
            usr: format!("c:@F@fn_{i}"),
            kind: NodeKind::Function,
            name: format!("fn_{i}"),
            qualified_name: format!("ns::fn_{i}"),
            mangled_name: None,
            file_path: format!("/src/file_{}.cpp", i % 100),
            line: Some((i % 500) as u32),
            col: Some(1),
            repo_name: "bench-repo".to_owned(),
            attrs_json: "{}".to_owned(),
            partial: false,
            phase: 1,
            tu_hash: [0u8; 32],
            return_type: None,
            params: None,
            signature: None,
            code: None,
            code_truncated: None,
            template_params: None,
            template_args: None,
            is_virtual: None,
            is_pure_virtual: None,
            is_static: None,
            symbol_id: i as i64 + 1,
            file_id: (i % 100) as i64 + 1,
        })
        .collect()
}

fn make_edges(n: usize) -> Vec<EdgeRecord> {
    (0..n)
        .map(|i| EdgeRecord {
            src_usr: format!("c:@F@fn_{i}"),
            dst_usr: Some(format!("c:@F@fn_{}", (i + 1) % n.max(1))),
            dst_placeholder: None,
            kind: EdgeKind::Calls,
            resolved: true,
            cross_repo_candidate: false,
            repo_name: "bench-repo".to_owned(),
            attrs_json: "{}".to_owned(),
            tu_hash: [0u8; 32],
            source_association_type: None,
            target_association_type: None,
            src_id: i as i64 + 1,
            dst_id: Some((i + 1) as i64 % n.max(1) as i64 + 1),
            dst_repo_name: "bench-repo".to_owned(),
        })
        .collect()
}

// ── Bench harness (manual; no criterion dependency) ───────────────────────────

fn bench_write_nodes(sink: Arc<dyn GraphSink>, nodes: &[NodeRecord], label: &str) {
    let rt = tokio::runtime::Runtime::new().expect("tokio runtime");
    let iters = 5_u32;
    let mut total_ns = 0_u128;

    for _ in 0..iters {
        let s = Arc::clone(&sink);
        let n = black_box(nodes);
        let t0 = Instant::now();
        rt.block_on(async move { s.write_nodes(n).await.unwrap() });
        total_ns += t0.elapsed().as_nanos();
    }

    let avg_ms = total_ns / u128::from(iters) / 1_000_000;
    println!(
        "bench write_nodes [{label}] {n} records — avg {avg_ms} ms over {iters} iters",
        n = nodes.len()
    );
}

fn bench_write_edges(sink: Arc<dyn GraphSink>, edges: &[EdgeRecord], label: &str) {
    let rt = tokio::runtime::Runtime::new().expect("tokio runtime");
    let iters = 5_u32;
    let mut total_ns = 0_u128;

    for _ in 0..iters {
        let s = Arc::clone(&sink);
        let e = black_box(edges);
        let t0 = Instant::now();
        rt.block_on(async move { s.write_edges(e).await.unwrap() });
        total_ns += t0.elapsed().as_nanos();
    }

    let avg_ms = total_ns / u128::from(iters) / 1_000_000;
    println!(
        "bench write_edges [{label}] {n} records — avg {avg_ms} ms over {iters} iters",
        n = edges.len()
    );
}

fn main() {
    // Gate: skip unless BENCH=1 is set.
    if std::env::var("BENCH").as_deref() != Ok("1") {
        println!("sink_throughput bench skipped (set BENCH=1 to run)");
        return;
    }

    let sink: Arc<dyn GraphSink> = Arc::new(MockSink::default());

    // Small batch — exercises the single-chunk path.
    let small_nodes = make_nodes(100);
    let small_edges = make_edges(100);
    bench_write_nodes(Arc::clone(&sink), &small_nodes, "100-nodes");
    bench_write_edges(Arc::clone(&sink), &small_edges, "100-edges");

    // Large batch — exercises the multi-chunk concurrent path.
    let large_nodes = make_nodes(50_000);
    let large_edges = make_edges(50_000);
    bench_write_nodes(Arc::clone(&sink), &large_nodes, "50k-nodes");
    bench_write_edges(Arc::clone(&sink), &large_edges, "50k-edges");

    // Very large batch — stress the chunking loop.
    let huge_nodes = make_nodes(200_000);
    bench_write_nodes(Arc::clone(&sink), &huge_nodes, "200k-nodes");
}
