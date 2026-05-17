# Operator Runbook: Observability and Dashboard

**Audience:** Operators  
**Last updated:** 2026-05-17

---

## Prometheus Metrics Endpoint

`cxg-daemon` exposes Prometheus metrics at `GET /metrics` (no authentication
required):

```bash
curl -s http://127.0.0.1:7878/metrics
```

---

## Available Metrics

| Metric | Type | Description |
|--------|------|-------------|
| `cxg_nodes_total` | Counter | Total graph nodes written across all ingest runs |
| `cxg_edges_total` | Counter | Total graph edges written across all ingest runs |
| `cxg_libclang_errors_total` | Counter | TUs that failed or panicked in Phase 1 (libclang parse) |
| `cxg_cache_hit_ratio` | Gauge | Ratio of TUs served from manifest cache (0.0–1.0) |
| `cxg_queue_depth` | Gauge | Number of jobs currently queued |
| `cxg_nodes_per_second` | Gauge | Rolling nodes/s throughput |
| `cxg_edges_per_second` | Gauge | Rolling edges/s throughput |

---

## Key Alert Thresholds

| Condition | Expression | Action |
|-----------|-----------|--------|
| High parse error rate | `cxg_libclang_errors_total / cxg_nodes_total > 0.01` | Check compile_commands.json; see escalation below |
| Queue backed up | `cxg_queue_depth > 32` | Check whether pipeline is stalled; inspect daemon logs |
| No throughput | `cxg_nodes_per_second == 0` for > 5 min during active job | Job may be stalled; check logs for Phase 1 panics |

---

## Grafana Dashboard

The following panel queries work against the cpp-indexer metrics. Import them
into any Grafana instance pointed at your Prometheus server.

### Ingest throughput

```
rate(cxg_nodes_total[5m])
rate(cxg_edges_total[5m])
```

### Parse error rate

```
rate(cxg_libclang_errors_total[5m]) / rate(cxg_nodes_total[5m])
```

Threshold line: `0.01` (1 %).

### Cache hit ratio

```
cxg_cache_hit_ratio
```

A high ratio (> 0.8) on incremental re-indexes is expected. A ratio of 0.0
on a hot repo indicates cache invalidation (libclang version bump or schema
version change).

### Queue depth

```
cxg_queue_depth
```

---

## Scrape Configuration (Prometheus)

Add to `prometheus.yml`:

```yaml
scrape_configs:
  - job_name: cxg_daemon
    static_configs:
      - targets: ["localhost:7878"]
    metrics_path: /metrics
    scrape_interval: 15s
```

---

## Log-Based Observability

Progress lines are emitted to stderr at least once every 5 seconds during
active ingestion. Example:

```
INFO cpp_indexer::pipeline  phase=1 tu=142/500 nodes=12400 edges=48100
```

Filter with:

```bash
journalctl -u cxg-daemon -f | grep "cpp_indexer::pipeline"
```

---

## Escalation: High libclang Error Rate

If `cxg_libclang_errors_total / cxg_nodes_total > 0.01`:

1. Check daemon logs for the specific TU that failed:
   ```bash
   journalctl -u cxg-daemon | grep "Phase 1 parallel: hard libclang failure"
   ```

2. Verify `compile_commands.json` contains the failing file and its compiler
   flags are correct.

3. Ensure `LIBCLANG_PATH` points to libclang 18 — see the
   [libclang-setup runbook](libclang-setup.md).

4. If `catch_unwind` is firing (panic-wrapped TUs), the TU is skipped and
   counted in `cxg_libclang_errors_total` but does not crash the daemon. Confirm
   the rest of the index is healthy before deciding whether to escalate.
