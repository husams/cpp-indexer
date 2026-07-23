"""Qualify the explicit CIDX SQLite profile against a schema-v34 database.

The command is intentionally standard-library-only so it can run beside the
repository's Python oracle and on a clean measurement host. It reports plans
and bytes rather than treating one machine's latency as a universal target.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sqlite3
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any


SCHEMA_VERSION = 34
REQUIRED_RELATIONS = {
    "edge",
    "def_edge",
    "entity_edge",
    "possible_call",
    "type_edge",
    "include_edge",
    "edge_site",
    "include_site",
}


def canonical(value: Any) -> str:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def digest(value: Any) -> str:
    return hashlib.sha256(canonical(value).encode()).hexdigest()


def qident(value: str) -> str:
    return '"' + value.replace('"', '""') + '"'


def connect(path: Path, *, read_only: bool, journal_mode: str | None = None) -> sqlite3.Connection:
    if read_only:
        connection = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    else:
        connection = sqlite3.connect(path)
    connection.execute("PRAGMA busy_timeout = 5000")
    connection.execute("PRAGMA foreign_keys = ON")
    if read_only:
        connection.execute("PRAGMA query_only = ON")
    else:
        if journal_mode is not None:
            connection.execute(f"PRAGMA journal_mode = {journal_mode}").fetchone()
        connection.execute("PRAGMA synchronous = FULL")
    return connection


def pragma(connection: sqlite3.Connection, name: str) -> Any:
    return connection.execute(f"PRAGMA {name}").fetchone()[0]


def file_facts(path: Path) -> dict[str, Any]:
    facts: dict[str, Any] = {}
    for suffix in ("", "-wal", "-shm", "-journal"):
        candidate = Path(str(path) + suffix)
        if candidate.exists():
            stat = candidate.stat()
            facts[suffix or "database"] = {
                "bytes": stat.st_size,
                "mtime_ns": stat.st_mtime_ns,
                "sha256": hashlib.sha256(candidate.read_bytes()).hexdigest(),
            }
    return facts


def catalog_identity(connection: sqlite3.Connection) -> str:
    catalog = connection.execute(
        "SELECT type, name, tbl_name, COALESCE(sql, '') "
        "FROM sqlite_master WHERE type IN ('table', 'index', 'trigger', 'view') "
        "ORDER BY type, name"
    ).fetchall()
    meta = connection.execute(
        "SELECT key, value FROM meta ORDER BY key"
    ).fetchall() if _has_table(connection, "meta") else []
    return digest({"catalog": catalog, "meta": meta})


def fact_set_identity(connection: sqlite3.Connection) -> str:
    tables = [row[0] for row in connection.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
    )]
    counts = []
    for table in tables:
        counts.append((table, connection.execute(f"SELECT COUNT(*) FROM {qident(table)}").fetchone()[0]))
    return digest(counts)


def _has_table(connection: sqlite3.Connection, table: str) -> bool:
    return connection.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (table,)
    ).fetchone() is not None


def storage_objects(connection: sqlite3.Connection) -> dict[str, Any]:
    result: dict[str, Any] = {}
    try:
        rows = connection.execute(
            "SELECT name, path, pageno, pagetype, ncell, payload "
            "FROM dbstat ORDER BY name, pageno"
        ).fetchall()
    except sqlite3.DatabaseError:
        return {"inspection": "page_count", "objects": []}
    objects: dict[str, dict[str, int]] = {}
    for name, _path, _page, _kind, cells, payload in rows:
        item = objects.setdefault(name, {"pages": 0, "cells": 0, "payload_bytes": 0})
        item["pages"] += 1
        item["cells"] += cells
        item["payload_bytes"] += payload
    result["inspection"] = "dbstat"
    result["objects"] = [
        {"name": name, **objects[name]} for name in sorted(objects)
    ]
    return result


def connection_profile(connection: sqlite3.Connection) -> dict[str, Any]:
    return {
        "foreign_keys": pragma(connection, "foreign_keys"),
        "query_only": pragma(connection, "query_only"),
        "busy_timeout_ms": pragma(connection, "busy_timeout"),
        "journal_mode": pragma(connection, "journal_mode"),
        "synchronous": pragma(connection, "synchronous"),
        "temp_store": pragma(connection, "temp_store"),
        "cache_size": pragma(connection, "cache_size"),
        "mmap_size": pragma(connection, "mmap_size"),
        "page_size": pragma(connection, "page_size"),
        "auto_vacuum": pragma(connection, "auto_vacuum"),
    }


def query_cases(connection: sqlite3.Connection) -> list[dict[str, Any]]:
    symbol_id = connection.execute("SELECT id FROM symbol ORDER BY id LIMIT 1").fetchone()
    file_id = connection.execute("SELECT id FROM file ORDER BY id LIMIT 1").fetchone()
    definition_id = connection.execute("SELECT id FROM definition ORDER BY id LIMIT 1").fetchone()
    type_id = connection.execute("SELECT id FROM type_node ORDER BY id LIMIT 1").fetchone()
    qual_name = connection.execute(
        "SELECT qual_name FROM symbol WHERE qual_name IS NOT NULL ORDER BY qual_name LIMIT 1"
    ).fetchone()
    first_symbol = symbol_id[0] if symbol_id else -1
    first_file = file_id[0] if file_id else -1
    first_definition = definition_id[0] if definition_id else -1
    first_type = type_id[0] if type_id else -1
    prefix = (qual_name[0].split("::", 1)[0] + "%") if qual_name else "%"
    return [
        {"id": "exact_identity", "sql": "SELECT id, usr, spelling, qual_name FROM symbol WHERE id = ?", "params": [first_symbol], "expected": None, "limit": 10},
        {"id": "name_prefix", "sql": "SELECT id, usr, qual_name FROM symbol WHERE qual_name LIKE ? ORDER BY qual_name LIMIT 100", "params": [prefix], "expected": "idx_symbol_qual_nc", "limit": 100},
        {"id": "outgoing_one_hop", "sql": "SELECT dst_id, kind, count FROM edge WHERE src_id = ? ORDER BY dst_id", "params": [first_symbol], "expected": "sqlite_autoindex_edge_1", "limit": 100},
        {"id": "incoming_one_hop", "sql": "SELECT src_id, kind, count FROM edge WHERE dst_id = ? ORDER BY src_id", "params": [first_symbol], "expected": "idx_edge_dst", "limit": 100},
        {"id": "definition_outgoing", "sql": "SELECT dst_id, kind, count FROM def_edge WHERE src_def_id = ? ORDER BY dst_id", "params": [first_definition], "expected": "sqlite_autoindex_def_edge_1", "limit": 100},
        {"id": "definition_incoming", "sql": "SELECT src_def_id, kind, count FROM def_edge WHERE dst_id = ? ORDER BY src_def_id", "params": [first_symbol], "expected": "idx_def_edge_dst", "limit": 100},
        {"id": "possible_call_outgoing", "sql": "SELECT dst_def_id, count FROM possible_call WHERE src_def_id = ? ORDER BY dst_def_id", "params": [first_definition], "expected": "sqlite_autoindex_possible_call_1", "limit": 100},
        {"id": "possible_call_incoming", "sql": "SELECT src_def_id, count FROM possible_call WHERE dst_def_id = ? ORDER BY src_def_id", "params": [first_definition], "expected": "idx_possible_call_dst", "limit": 100},
        {"id": "bounded_paths", "sql": "WITH RECURSIVE walk(id, depth) AS (SELECT ?, 0 UNION ALL SELECT edge.dst_id, walk.depth + 1 FROM walk JOIN edge ON edge.src_id = walk.id WHERE walk.depth < 3) SELECT id, depth FROM walk ORDER BY depth, id", "params": [first_symbol], "expected": "sqlite_autoindex_edge_1", "limit": 1000},
        {"id": "references_sites", "sql": "SELECT edge_site.edge_id, edge_site.file_id, edge_site.line, edge_site.col FROM edge_site JOIN edge ON edge.id = edge_site.edge_id WHERE edge.src_id = ? ORDER BY edge_site.file_id, edge_site.line, edge_site.col", "params": [first_symbol], "expected": "idx_edge_src", "limit": 1000},
        {"id": "type_closure", "sql": "WITH RECURSIVE closure(id) AS (SELECT ? UNION SELECT type_edge.dst_id FROM closure JOIN type_edge ON type_edge.src_id = closure.id) SELECT id FROM closure ORDER BY id", "params": [first_type], "expected": "PRIMARY KEY", "limit": 1000},
        {"id": "entity_graph", "sql": "SELECT dst_id, kind, count FROM entity_edge WHERE src_id = ? ORDER BY dst_id", "params": [first_symbol], "expected": "idx_entity_edge_identity", "limit": 1000},
        {"id": "include_graph", "sql": "SELECT dst_file_id, dst_path, count FROM include_edge WHERE src_file_id = ? ORDER BY dst_path", "params": [first_file], "expected": "sqlite_autoindex_include_edge_1", "limit": 1000},
        {"id": "include_graph_reverse", "sql": "SELECT src_file_id, dst_path, count FROM include_edge WHERE dst_file_id = ? ORDER BY src_file_id", "params": [first_file], "expected": "idx_include_edge_dst", "limit": 1000},
    ]


def query_evidence(connection: sqlite3.Connection, cases: list[dict[str, Any]], iterations: int) -> list[dict[str, Any]]:
    evidence = []
    for case in cases:
        plan = connection.execute("EXPLAIN QUERY PLAN " + case["sql"], case["params"]).fetchall()
        plan_text = [" ".join(str(value) for value in row) for row in plan]
        samples = []
        rows_seen = 0
        truncated = False
        error = None
        try:
            for _ in range(iterations):
                started = time.perf_counter_ns()
                cursor = connection.execute(case["sql"], case["params"])
                rows = cursor.fetchmany(case["limit"] + 1)
                elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
                samples.append(elapsed_ms)
                rows_seen = len(rows[: case["limit"]])
                truncated = len(rows) > case["limit"]
        except sqlite3.DatabaseError as exc:
            error = str(exc)
        expected = case["expected"]
        indexed = expected is None or any(expected.lower() in line.lower() for line in plan_text)
        status = "ok" if error is None and indexed else "error"
        evidence.append({
            "id": case["id"],
            "sql": case["sql"],
            "parameters": case["params"],
            "row_count": rows_seen,
            "truncated": truncated,
            "latency_ms": {
                "count": len(samples),
                "min": round(min(samples), 6) if samples else None,
                "p50": round(statistics.median(samples), 6) if samples else None,
                "max": round(max(samples), 6) if samples else None,
            },
            "plan": plan_text,
            "expected_index": expected,
            "indexed": indexed,
            "status": status,
            "error": error,
        })
    return evidence


def run_layout(path: Path, iterations: int) -> dict[str, Any]:
    connection = connect(path, read_only=True)
    try:
        version = connection.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
        cases = query_cases(connection)
        return {
            "schema_version": int(version[0]) if version else None,
            "profile": connection_profile(connection),
            "storage": {"database": file_facts(path), **storage_objects(connection)},
            "catalog_identity": catalog_identity(connection),
            "fact_set_identity": fact_set_identity(connection),
            "queries": query_evidence(connection, cases, iterations),
        }
    finally:
        connection.close()


def copy_for_profile(source: Path, directory: Path, mode: str) -> Path:
    target = directory / f"{mode}.db"
    shutil.copy2(source, target)
    return target


def run_runtime_profile(source: Path, mode: str, iterations: int) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="cidx-storage-m1-") as temporary:
        path = copy_for_profile(source, Path(temporary), mode)
        connection = connect(path, read_only=False, journal_mode=mode)
        try:
            started = time.perf_counter_ns()
            for _ in range(iterations):
                connection.execute("BEGIN IMMEDIATE")
                connection.execute("UPDATE meta SET value=value WHERE key='schema_version'")
                connection.commit()
            write_ms = (time.perf_counter_ns() - started) / 1_000_000
            checkpoint = None
            if mode == "WAL":
                checkpoint = connection.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
            return {
                "mode": mode,
                "profile": connection_profile(connection),
                "writes": iterations,
                "write_total_ms": round(write_ms, 6),
                "checkpoint": checkpoint,
                "files": file_facts(path),
            }
        finally:
            connection.close()


def run_read_only_check(source: Path) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="cidx-storage-m1-ro-") as temporary:
        path = copy_for_profile(source, Path(temporary), "readonly")
        before = file_facts(path)
        connection = connect(path, read_only=True)
        try:
            connection.execute("SELECT COUNT(*) FROM symbol").fetchone()
            state = connection_profile(connection)
        finally:
            connection.close()
        after = file_facts(path)
        return {"status": "pass" if before == after else "fail", "before": before, "after": after, "profile": state}


def run_backup_check(source: Path) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="cidx-storage-m1-backup-") as temporary:
        directory = Path(temporary)
        target = directory / "backup.db"
        source_connection = connect(source, read_only=True)
        try:
            target_connection = sqlite3.connect(target)
            try:
                source_connection.backup(target_connection)
                target_connection.commit()
            finally:
                target_connection.close()
            restored = connect(target, read_only=True)
            try:
                integrity = restored.execute("PRAGMA integrity_check").fetchone()[0]
                foreign_keys = restored.execute("PRAGMA foreign_key_check").fetchall()
                source_identity = (catalog_identity(source_connection), fact_set_identity(source_connection))
                target_identity = (catalog_identity(restored), fact_set_identity(restored))
                version = restored.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
            finally:
                restored.close()
        finally:
            source_connection.close()
        return {
            "status": "pass" if integrity == "ok" and not foreign_keys and source_identity == target_identity and int(version[0]) == SCHEMA_VERSION else "fail",
            "integrity": integrity,
            "foreign_key_errors": foreign_keys,
            "identity": {"source": source_identity, "restored": target_identity},
            "schema_version": int(version[0]) if version else None,
        }


def crash_child(path: Path, phase: str) -> int:
    connection = connect(path, read_only=False, journal_mode="DELETE")
    try:
        connection.execute("BEGIN IMMEDIATE")
        if phase == "migration":
            connection.execute("CREATE TABLE migration_probe(value INTEGER)")
        else:
            connection.execute("UPDATE meta SET value=value WHERE key='schema_version'")
        if phase == "after_commit_before_checkpoint":
            connection.commit()
        os.kill(os.getpid(), 9)
    finally:
        connection.close()
    return 0


def run_recovery_check(source: Path) -> dict[str, Any]:
    phases = ["after_write_before_commit", "migration", "after_commit_before_checkpoint"]
    results = []
    for phase in phases:
        with tempfile.TemporaryDirectory(prefix="cidx-storage-m1-recovery-") as temporary:
            path = copy_for_profile(source, Path(temporary), phase)
            completed = subprocess.run(
                [sys.executable, __file__, "--crash-child", "--db", str(path), "--phase", phase],
                capture_output=True,
                text=True,
            )
            connection = connect(path, read_only=True)
            try:
                integrity = connection.execute("PRAGMA integrity_check").fetchone()[0]
                version = connection.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
                probe = _has_table(connection, "migration_probe")
            finally:
                connection.close()
            passed = completed.returncode == -9 and integrity == "ok" and int(version[0]) == SCHEMA_VERSION and not probe
            results.append({"phase": phase, "status": "pass" if passed else "fail", "child_exit": completed.returncode, "integrity": integrity, "migration_probe_present": probe})
    return {"status": "pass" if all(item["status"] == "pass" for item in results) else "fail", "cases": results}


def qualify(source: Path, profile_path: Path, iterations: int) -> dict[str, Any]:
    profile = json.loads(profile_path.read_text())
    strategies = {item["table"] for item in profile["relation_strategies"]}
    missing = sorted(REQUIRED_RELATIONS - strategies)
    layout = run_layout(source, iterations)
    runtime = [run_runtime_profile(source, mode, iterations) for mode in ("DELETE", "WAL")]
    read_only = run_read_only_check(source)
    backup = run_backup_check(source)
    recovery = run_recovery_check(source)
    query_failures = [item["id"] for item in layout["queries"] if item["status"] != "ok"]
    return {
        "result_version": "storage-m1/result-v1",
        "benchmark": "storage-m1/v1",
        "schema_version": layout["schema_version"],
        "source": str(source),
        "profile_version": profile["profile_version"],
        "profile_id": "storage-m1-runtime",
        "layout": layout,
        "runtime_comparison": runtime,
        "read_only": read_only,
        "backup_restore": backup,
        "recovery": recovery,
        "gates": {
            "relation_strategy_catalog": "pass" if not missing else "fail",
            "query_plans": "pass" if not query_failures else "fail",
            "read_only_non_mutating": read_only["status"],
            "backup_restore_identity": backup["status"],
            "interruption_recovery": recovery["status"],
            "wal_decision": "qualification_only",
        },
        "missing_relation_strategies": missing,
        "query_failures": query_failures,
        "notes": [
            "DELETE is the shipped runtime profile; WAL is measured on the same copied database and workload.",
            "A benchmark result is host-specific and must not be treated as a universal SLO.",
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", type=Path)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--iterations", type=int, default=3)
    parser.add_argument("--crash-child", action="store_true")
    parser.add_argument("--phase", default="after_write_before_commit")
    args = parser.parse_args(argv)
    if args.crash_child:
        if args.db is None:
            raise SystemExit("--db is required for --crash-child")
        return crash_child(args.db, args.phase)
    if args.db is None or args.profile is None or args.output is None:
        raise SystemExit("--db, --profile, and --output are required")
    result = qualify(args.db, args.profile, max(1, args.iterations))
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"output": str(args.output), "gates": result["gates"]}, indent=2, sort_keys=True))
    return 0 if all(value in {"pass", "qualification_only"} for value in result["gates"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
