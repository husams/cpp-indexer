"""Run the Storage M0 measurements against a generated v34 database."""

from __future__ import annotations

import argparse
import datetime as dt
import resource
import sqlite3
import time
from pathlib import Path
from typing import Any

from . import BENCHMARK_VERSION, SCHEMA_VERSION
from .common import (
    canonical_json,
    git_revision,
    latency_summary,
    load_json,
    manifest_digest,
    semantic_digest,
    sqlite_compile_options,
    system_profile,
)


def _quote(identifier: str) -> str:
    return '"' + identifier.replace('"', '""') + '"'


def _workload(manifest: dict[str, Any], workload_id: str) -> dict[str, Any]:
    for workload in manifest.get("workloads", []):
        if workload.get("id") == workload_id:
            return workload
    raise ValueError(f"unknown workload {workload_id!r}")


def _first_symbol_id(connection: sqlite3.Connection) -> int | None:
    row = connection.execute("SELECT MIN(id) FROM symbol").fetchone()
    return None if row is None else row[0]


def storage_stats(connection: sqlite3.Connection, db_path: Path) -> dict[str, Any]:
    page_size = int(connection.execute("PRAGMA page_size").fetchone()[0])
    page_count = int(connection.execute("PRAGMA page_count").fetchone()[0])
    freelist = int(connection.execute("PRAGMA freelist_count").fetchone()[0])
    objects: list[dict[str, Any]] = []
    for name, object_type in connection.execute(
        "SELECT name, type FROM sqlite_master WHERE type IN ('table', 'index') "
        "AND name NOT LIKE 'sqlite_%' ORDER BY type, name"
    ):
        count = None
        if object_type == "table":
            count = int(connection.execute(f"SELECT COUNT(*) FROM {_quote(name)}").fetchone()[0])
        objects.append({"name": name, "type": object_type, "row_count": count, "bytes": None})
    try:
        by_name = {
            name: int(size)
            for name, size in connection.execute(
                "SELECT name, SUM(pgsize) FROM dbstat GROUP BY name ORDER BY name"
            )
        }
        for item in objects:
            item["bytes"] = by_name.get(item["name"], 0)
        inspection = "dbstat"
    except sqlite3.DatabaseError:
        inspection = "page_count"
    def sibling(suffix: str) -> int:
        path = Path(str(db_path) + suffix)
        return path.stat().st_size if path.exists() else 0
    return {
        "database_bytes": db_path.stat().st_size if db_path.exists() else 0,
        "page_size": page_size,
        "page_count": page_count,
        "freelist_pages": freelist,
        "freelist_bytes": freelist * page_size,
        "wal_bytes": sibling("-wal"),
        "journal_bytes": sibling("-journal"),
        "temp_bytes": None,
        "inspection": inspection,
        "objects": objects,
    }


def _parameter_value(parameter: dict[str, Any], connection: sqlite3.Connection) -> Any:
    source = parameter.get("source")
    if source == "first_symbol_id":
        return _first_symbol_id(connection)
    if source == "last_symbol_id":
        return connection.execute("SELECT MAX(id) FROM symbol").fetchone()[0]
    if source == "first_file_id":
        return connection.execute("SELECT MIN(id) FROM file").fetchone()[0]
    if source == "literal":
        return parameter.get("value")
    raise ValueError(f"unknown benchmark parameter source {source!r}")


def run_queries(
    connection: sqlite3.Connection,
    workload: dict[str, Any],
    *,
    default_iterations: int = 5,
) -> tuple[list[dict[str, Any]], dict[str, int]]:
    records: list[dict[str, Any]] = []
    trace_events: list[str] = []
    connection.set_trace_callback(trace_events.append)
    for query in workload.get("queries", []):
        parameters = [
            _parameter_value(parameter, connection)
            for parameter in query.get("parameters", [])
        ]
        sql = str(query["sql"])
        row_limit = int(query.get("row_limit", 1000))
        iterations = int(query.get("iterations", default_iterations))
        try:
            plan = [
                {"detail": row[3], "id": row[0], "parent": row[1], "notused": row[2]}
                for row in connection.execute("EXPLAIN QUERY PLAN " + sql, parameters)
            ]
            latencies: list[float] = []
            row_count = 0
            truncated = False
            for _ in range(iterations):
                started = time.perf_counter_ns()
                cursor = connection.execute(sql, parameters)
                rows = cursor.fetchmany(row_limit + 1)
                elapsed = (time.perf_counter_ns() - started) / 1_000_000
                latencies.append(elapsed)
                row_count = len(rows)
                truncated = len(rows) > row_limit
            records.append({
                "id": query["id"],
                "category": query.get("category", "other"),
                "sql": sql,
                "parameters": parameters,
                "row_count": min(row_count, row_limit),
                "truncated": truncated,
                "latency_ms": latency_summary(latencies),
                "plan": plan,
                "status": "ok",
            })
        except sqlite3.DatabaseError as error:
            records.append({
                "id": query["id"],
                "category": query.get("category", "other"),
                "sql": sql,
                "parameters": parameters,
                "row_count": None,
                "truncated": False,
                "latency_ms": latency_summary([]),
                "plan": [],
                "status": "error",
                "error": str(error),
            })
    connection.set_trace_callback(None)
    return records, {
        "prepare_count": len(trace_events),
        "step_count": sum(
            int(item["row_count"] or 0) for item in records if item["status"] == "ok"
        ),
        "transaction_count": sum(
            1 for event in trace_events if event.upper().startswith(("BEGIN", "COMMIT", "ROLLBACK"))
        ),
    }


def integrity(connection: sqlite3.Connection, db_path: Path) -> dict[str, Any]:
    check = connection.execute("PRAGMA integrity_check").fetchone()[0]
    foreign_keys = connection.execute("PRAGMA foreign_key_check").fetchall()
    state_row = connection.execute(
        "SELECT value FROM benchmark_meta WHERE key='state'"
    ).fetchone()
    generation_row = connection.execute(
        "SELECT value FROM benchmark_meta WHERE key='generation'"
    ).fetchone()
    stored_digest_row = connection.execute(
        "SELECT value FROM benchmark_meta WHERE key='semantic_digest'"
    ).fetchone()
    actual_digest = semantic_digest(db_path)
    return {
        "integrity_check": check,
        "foreign_key_errors": len(foreign_keys),
        "generation": int(generation_row[0]) if generation_row else None,
        "state": state_row[0] if state_row else None,
        "semantic_digest": actual_digest,
        "semantic_digest_matches": stored_digest_row is not None and stored_digest_row[0] == actual_digest,
        "presented_as_current": state_row is not None and state_row[0] == "current",
        "status": "ok" if check == "ok" and not foreign_keys and state_row and state_row[0] == "current" else "failed",
    }


def _not_run(reason: str) -> dict[str, Any]:
    return {"status": "not_run", "reason": reason}


def run(
    db_path: Path,
    manifest_path: Path,
    workload_id: str,
    profile_path: Path,
    *,
    output: Path | None = None,
    semantic_reference: Path | None = None,
    configuration: str = "v34-default",
) -> dict[str, Any]:
    manifest = load_json(manifest_path)
    profile = load_json(profile_path)
    workload = _workload(manifest, workload_id)
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("manifest is not for SQLite schema v34")
    if profile.get("profile_version") != "storage-m0/profile-v1":
        raise ValueError("unsupported Storage M0 profile version")
    connection = sqlite3.connect(db_path)
    started = dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")
    try:
        stats = storage_stats(connection, db_path)
        queries, query_counters = run_queries(connection, workload)
        checks = integrity(connection, db_path)
        reference_digest = semantic_digest(semantic_reference) if semantic_reference else None
        if reference_digest is not None:
            checks["semantic_reference_digest"] = reference_digest
            checks["semantic_equivalence"] = checks["semantic_digest"] == reference_digest
        else:
            checks["semantic_equivalence"] = None
        try:
            peak_rss = int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
            if system_profile()["os"] != "Darwin":
                peak_rss *= 1024
        except (AttributeError, OSError):
            peak_rss = None
        generation_duration = connection.execute(
            "SELECT value FROM benchmark_meta WHERE key='generation_duration_ms'"
        ).fetchone()
        generation_ms = float(generation_duration[0]) if generation_duration else None
        generated_rows = sum(
            int(connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0])
            for table in ("symbol", "edge", "edge_site")
        )
        build_throughput = (
            generated_rows / (generation_ms / 1000)
            if generation_ms and generation_ms > 0 else None
        )
        result = {
            "result_version": "storage-m0/result-v1",
            "benchmark": BENCHMARK_VERSION,
            "schema_version": SCHEMA_VERSION,
            "manifest_sha256": manifest_digest(manifest),
            "workload": workload_id,
            "profile_id": profile["profile_id"],
            "configuration": configuration,
            "run_id": manifest_digest(manifest)[:16] + "-" + workload_id,
            "started_at": started,
            "revision": git_revision(),
            "environment": {
                **system_profile(),
                "sqlite_compile_options": sqlite_compile_options(connection),
            },
            "storage": stats,
            "operations": {
                "cold_build": {
                    "status": "ok" if generation_duration else "not_run",
                    "duration_ms": generation_ms,
                    "rows_per_s": build_throughput,
                    "kind": "synthetic materialization" if generation_duration else None,
                },
                "warm_noop": _not_run("requires a cidx incremental-index adapter"),
                "changed_tu_update": _not_run("requires a corpus adapter and changed source file"),
                "transform_rebuild": _not_run("requires a transform adapter"),
                "migration": _not_run("run cidx db migrate against an older fixture"),
                "backup": _not_run("use sqlite backup adapter in the recovery phase"),
                "recovery": checks,
            },
            "queries": queries,
            "counters": {
                **query_counters,
                "write_amplification": None,
                "checkpoint_ms": None,
                "peak_rss_bytes": peak_rss,
                "page_cache": {
                    "cache_size": connection.execute("PRAGMA cache_size").fetchone()[0],
                    "cache_spill": connection.execute("PRAGMA cache_spill").fetchone()[0],
                    "mmap_size": connection.execute("PRAGMA mmap_size").fetchone()[0],
                    "measurement": "sqlite pragmas; not a page-hit counter",
                },
            },
            "gates": {
                "semantic_equivalence": checks["semantic_equivalence"],
                "intentional_regression": _not_run("compare this result with --candidate-bad-config"),
                "custom_store": _not_run("evaluate with gate.py and a decision record"),
            },
            "notes": [
                "The result is a measured v34 SQLite baseline; not_run fields are explicit gaps, not zeroes.",
                "Large manifest scales require an explicit materialization cap and are never expanded implicitly.",
            ],
        }
    finally:
        connection.close()
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(canonical_json(result) + "\n", encoding="utf-8")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--workload", default="synthetic")
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--semantic-reference", type=Path)
    parser.add_argument("--configuration", default="v34-default")
    args = parser.parse_args(argv)
    result = run(
        args.db, args.manifest, args.workload, args.profile,
        output=args.output, semantic_reference=args.semantic_reference,
        configuration=args.configuration,
    )
    print(canonical_json(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
