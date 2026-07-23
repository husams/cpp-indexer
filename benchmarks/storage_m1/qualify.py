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
import threading
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
REVERSE_QUERY_IDS = {
    "incoming_one_hop",
    "definition_incoming",
    "possible_call_incoming",
    "type_closure_reverse",
    "entity_graph_reverse",
    "include_graph_reverse",
}


def canonical(value: Any) -> str:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def digest(value: Any) -> str:
    return hashlib.sha256(canonical(value).encode()).hexdigest()


def stable_value(value: Any) -> Any:
    if isinstance(value, bytes):
        return {"blob_hex": value.hex()}
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    return str(value)


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


def table_facts(connection: sqlite3.Connection, table: str) -> dict[str, Any]:
    columns = [row[1] for row in connection.execute(f"PRAGMA table_info({qident(table)})")]
    rows = connection.execute(f"SELECT * FROM {qident(table)}").fetchall()
    encoded = [
        [stable_value(value) for value in row]
        for row in rows
    ]
    encoded.sort(key=canonical)
    return {"columns": columns, "rows": encoded}


def user_table_names(connection: sqlite3.Connection) -> list[str]:
    return [row[0] for row in connection.execute(
        "SELECT name FROM sqlite_master "
        "WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
    )]


def workspace_identity(connection: sqlite3.Connection) -> str:
    workspace_tables = {
        table: table_facts(connection, table)
        for table in ("repository", "clone", "component", "directory", "file")
        if _has_table(connection, table)
    }
    return digest(workspace_tables)


def fact_set_identity(connection: sqlite3.Connection) -> str:
    facts = {
        table: table_facts(connection, table)
        for table in user_table_names(connection)
    }
    return digest({
        "workspace_identity": workspace_identity(connection),
        "facts": facts,
    })


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
    def first(sql: str, default: int = -1) -> int:
        row = connection.execute(sql).fetchone()
        return int(row[0]) if row else default

    symbol_id = first("SELECT id FROM symbol ORDER BY id LIMIT 1")
    file_id = first("SELECT id FROM file ORDER BY id LIMIT 1")
    definition_id = first("SELECT id FROM definition ORDER BY id LIMIT 1")
    type_id = first("SELECT id FROM type_node ORDER BY id LIMIT 1")
    qual_name = connection.execute(
        "SELECT qual_name FROM symbol WHERE qual_name IS NOT NULL ORDER BY qual_name LIMIT 1"
    ).fetchone()
    edge_src = first("SELECT src_id FROM edge ORDER BY src_id, dst_id LIMIT 1")
    edge_dst = first("SELECT dst_id FROM edge ORDER BY dst_id, src_id LIMIT 1")
    edge_site_id = first("SELECT edge_id FROM edge_site ORDER BY edge_id LIMIT 1")
    def_src = first("SELECT src_def_id FROM def_edge ORDER BY src_def_id, dst_id LIMIT 1")
    def_dst = first("SELECT dst_id FROM def_edge ORDER BY dst_id, src_def_id LIMIT 1")
    call_src = first("SELECT src_def_id FROM possible_call ORDER BY src_def_id, dst_def_id LIMIT 1")
    call_dst = first("SELECT dst_def_id FROM possible_call ORDER BY dst_def_id, src_def_id LIMIT 1")
    type_src = first("SELECT src_id FROM type_edge ORDER BY src_id, kind, position LIMIT 1")
    type_dst = first("SELECT dst_id FROM type_edge ORDER BY dst_id, src_id LIMIT 1")
    entity_src = first("SELECT src_id FROM entity_edge ORDER BY src_id, dst_id LIMIT 1")
    entity_dst = first("SELECT dst_id FROM entity_edge ORDER BY dst_id, src_id LIMIT 1")
    include_src = first("SELECT src_file_id FROM include_edge ORDER BY src_file_id, dst_path LIMIT 1")
    include_dst = first("SELECT dst_file_id FROM include_edge WHERE dst_file_id IS NOT NULL ORDER BY dst_file_id, src_file_id LIMIT 1")
    include_site_edge = first("SELECT edge_id FROM include_site ORDER BY edge_id, begin_offset LIMIT 1")
    prefix = (qual_name[0].split("::", 1)[0] + "%") if qual_name else "%"
    return [
        {"id": "exact_identity", "sql": "SELECT id, usr, spelling, qual_name FROM symbol WHERE id = ?", "params": [symbol_id], "expected": None, "limit": 10, "require_rows": True},
        {"id": "name_prefix", "sql": "SELECT id, usr, qual_name FROM symbol WHERE qual_name LIKE ? ORDER BY qual_name LIMIT 100", "params": [prefix], "expected": "idx_symbol_qual_nc", "limit": 100},
        {"id": "outgoing_one_hop", "sql": "SELECT dst_id, kind, count FROM edge WHERE src_id = ? ORDER BY dst_id", "params": [edge_src], "strategy_table": "edge", "strategy_direction": "forward", "limit": 100, "require_rows": True},
        {"id": "incoming_one_hop", "sql": "SELECT src_id, kind, count FROM edge WHERE dst_id = ? ORDER BY src_id", "params": [edge_dst], "strategy_table": "edge", "strategy_direction": "reverse", "limit": 100, "require_rows": True},
        {"id": "definition_outgoing", "sql": "SELECT dst_id, kind, count FROM def_edge WHERE src_def_id = ? ORDER BY dst_id", "params": [def_src], "strategy_table": "def_edge", "strategy_direction": "forward", "limit": 100, "require_rows": True},
        {"id": "definition_incoming", "sql": "SELECT src_def_id, kind, count FROM def_edge WHERE dst_id = ? ORDER BY src_def_id", "params": [def_dst], "strategy_table": "def_edge", "strategy_direction": "reverse", "limit": 100, "require_rows": True},
        {"id": "possible_call_outgoing", "sql": "SELECT dst_def_id, count FROM possible_call WHERE src_def_id = ? ORDER BY dst_def_id", "params": [call_src], "strategy_table": "possible_call", "strategy_direction": "forward", "limit": 100, "require_rows": True},
        {"id": "possible_call_incoming", "sql": "SELECT src_def_id, count FROM possible_call WHERE dst_def_id = ? ORDER BY src_def_id", "params": [call_dst], "strategy_table": "possible_call", "strategy_direction": "reverse", "limit": 100, "require_rows": True},
        {"id": "bounded_paths", "sql": "WITH RECURSIVE walk(id, depth) AS (SELECT ?, 0 UNION ALL SELECT edge.dst_id, walk.depth + 1 FROM walk JOIN edge ON edge.src_id = walk.id WHERE walk.depth < 3) SELECT id, depth FROM walk ORDER BY depth, id", "params": [edge_src], "strategy_table": "edge", "strategy_direction": "forward", "limit": 1000, "require_rows": True},
        {"id": "references_sites", "sql": "SELECT edge_id, file_id, line, col FROM edge_site WHERE edge_id = ? ORDER BY file_id, line, col", "params": [edge_site_id], "strategy_table": "edge_site", "strategy_direction": "forward", "limit": 1000, "require_rows": True},
        {"id": "type_closure_forward", "sql": "WITH RECURSIVE closure(id) AS (SELECT ? UNION SELECT type_edge.dst_id FROM closure JOIN type_edge ON type_edge.src_id = closure.id) SELECT id FROM closure ORDER BY id", "params": [type_src], "strategy_table": "type_edge", "strategy_direction": "forward", "limit": 1000, "require_rows": True},
        {"id": "type_closure_reverse", "sql": "SELECT src_id, kind, position FROM type_edge WHERE dst_id = ? ORDER BY src_id, kind, position", "params": [type_dst], "strategy_table": "type_edge", "strategy_direction": "reverse", "limit": 1000, "require_rows": True},
        {"id": "entity_graph_forward", "sql": "SELECT dst_id, kind, count FROM entity_edge WHERE src_id = ? ORDER BY dst_id", "params": [entity_src], "strategy_table": "entity_edge", "strategy_direction": "forward", "limit": 1000, "require_rows": True},
        {"id": "entity_graph_reverse", "sql": "SELECT src_id, kind, count FROM entity_edge WHERE dst_id = ? ORDER BY src_id", "params": [entity_dst], "strategy_table": "entity_edge", "strategy_direction": "reverse", "limit": 1000, "require_rows": True},
        {"id": "include_graph", "sql": "SELECT dst_file_id, dst_path, count FROM include_edge WHERE src_file_id = ? ORDER BY dst_path", "params": [include_src], "strategy_table": "include_edge", "strategy_direction": "forward", "limit": 1000, "require_rows": True},
        {"id": "include_graph_reverse", "sql": "SELECT src_file_id, dst_path, count FROM include_edge WHERE dst_file_id = ? ORDER BY src_file_id", "params": [include_dst], "strategy_table": "include_edge", "strategy_direction": "reverse", "limit": 1000, "require_rows": True},
        {"id": "include_sites", "sql": "SELECT edge_id, begin_offset, end_offset FROM include_site WHERE edge_id = ? ORDER BY begin_offset", "params": [include_site_edge], "strategy_table": "include_site", "strategy_direction": "forward", "limit": 1000, "require_rows": True},
    ]


def query_evidence(
    connection: sqlite3.Connection,
    cases: list[dict[str, Any]],
    iterations: int,
    strategy_expectations: dict[str, str] | None = None,
) -> list[dict[str, Any]]:
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
        expected = case.get("expected")
        if expected is None and strategy_expectations is not None:
            expected = strategy_expectations.get(case["id"])
        if case.get("strategy_table") and expected is None:
            error = "missing declared strategy expectation"
        expected_text = expected.lower() if expected is not None else None
        indexed = expected is None or any(
            expected_text in line.lower()
            or (
                expected_text.startswith("primary key")
                and "primary key" in line.lower()
            )
            for line in plan_text
        )
        required_rows = case.get("require_rows", True)
        has_evidence = rows_seen > 0 or not required_rows
        status = "ok" if error is None and indexed and has_evidence else "error"
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
            "strategy_table": case.get("strategy_table"),
            "strategy_direction": case.get("strategy_direction"),
            "indexed": indexed,
            "required_rows": required_rows,
            "has_evidence": has_evidence,
            "status": status,
            "error": error,
        })
    return evidence


def strategy_expectations(relation_strategies: list[dict[str, Any]]) -> dict[str, str]:
    expectations = {}
    for strategy in relation_strategies:
        for query_id in strategy.get("query_ids", []):
            direction = query_id in REVERSE_QUERY_IDS
            expectations[query_id] = strategy.get("reverse" if direction else "forward")
    return expectations


def case_strategy_expectations(
    relation_strategies: list[dict[str, Any]],
    cases: list[dict[str, Any]],
) -> dict[str, str]:
    by_table = {strategy["table"]: strategy for strategy in relation_strategies}
    expectations = strategy_expectations(relation_strategies)
    for case in cases:
        table = case.get("strategy_table")
        if table is None:
            continue
        strategy = by_table.get(table)
        if strategy is not None:
            expectations[case["id"]] = strategy.get(case["strategy_direction"])
    return expectations


def prepare_representative_corpus(
    source: Path,
    directory: Path,
) -> tuple[Path, dict[str, Any]]:
    target = copy_for_profile(source, directory, "representative")
    connection = connect(target, read_only=False)
    try:
        before = int(connection.execute("SELECT COUNT(*) FROM possible_call").fetchone()[0])
        if before:
            return target, {
                "source_rows": before,
                "overlay_rows": 0,
                "rows_used": before,
                "status": "source_corpus",
            }
        candidate = connection.execute(
            "SELECT symbol_id, COUNT(*) AS definition_count "
            "FROM definition GROUP BY symbol_id "
            "HAVING definition_count >= 2 ORDER BY symbol_id LIMIT 1"
        ).fetchone()
        if candidate is None:
            raise RuntimeError(
                "possible_call requires a representative multi-definition corpus"
            )
        definitions = [
            int(row[0])
            for row in connection.execute(
                "SELECT id FROM definition WHERE symbol_id = ? ORDER BY id",
                (candidate[0],),
            )
        ]
        source_definition = definitions[0]
        targets = definitions[1:]
        connection.execute("BEGIN IMMEDIATE")
        connection.executemany(
            "INSERT INTO possible_call(src_def_id, dst_def_id, count) VALUES (?, ?, 1)",
            [(source_definition, target) for target in targets],
        )
        connection.commit()
        return target, {
            "source_rows": before,
            "overlay_rows": len(targets),
            "rows_used": len(targets),
            "status": "deterministic_multi_definition_overlay",
            "symbol_id": int(candidate[0]),
            "source_definition": source_definition,
            "target_definitions": targets,
        }
    finally:
        connection.close()


def run_layout(
    path: Path,
    iterations: int,
    relation_strategies: list[dict[str, Any]],
) -> dict[str, Any]:
    connection = connect(path, read_only=True)
    try:
        version = connection.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
        resolved_row = connection.execute(
            "SELECT value FROM meta WHERE key='graph_resolved_at'"
        ).fetchone()
        cases = query_cases(connection)
        queries = query_evidence(
            connection,
            cases,
            iterations,
            case_strategy_expectations(relation_strategies, cases),
        )
        return {
            "schema_version": int(version[0]) if version else None,
            "graph_resolved_at": resolved_row[0] if resolved_row else None,
            "resolved": bool(resolved_row and resolved_row[0]),
            "profile": connection_profile(connection),
            "storage": {"database": file_facts(path), **storage_objects(connection)},
            "catalog_identity": catalog_identity(connection),
            "workspace_identity": workspace_identity(connection),
            "fact_set_identity": fact_set_identity(connection),
            "queries": queries,
        }
    finally:
        connection.close()


def copy_for_profile(source: Path, directory: Path, mode: str) -> Path:
    target = directory / f"{mode}.db"
    shutil.copy2(source, target)
    return target


def run_identity_sensitivity_check(source: Path) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="cidx-storage-m1-identity-") as temporary:
        path = copy_for_profile(source, Path(temporary), "identity")
        before_connection = connect(path, read_only=True)
        try:
            before = fact_set_identity(before_connection)
            workspace_before = workspace_identity(before_connection)
            before_count = before_connection.execute("SELECT COUNT(*) FROM symbol").fetchone()[0]
        finally:
            before_connection.close()
        connection = connect(path, read_only=False)
        try:
            connection.execute("BEGIN IMMEDIATE")
            connection.execute(
                "UPDATE symbol SET spelling = spelling || ? WHERE id = "
                "(SELECT id FROM symbol ORDER BY id LIMIT 1)",
                ("__qualify_content_change__",),
            )
            connection.commit()
        finally:
            connection.close()
        after_connection = connect(path, read_only=True)
        try:
            after = fact_set_identity(after_connection)
            workspace_after = workspace_identity(after_connection)
            after_count = after_connection.execute("SELECT COUNT(*) FROM symbol").fetchone()[0]
        finally:
            after_connection.close()
        passed = (
            before_count == after_count
            and workspace_before == workspace_after
            and before != after
        )
        return {
            "status": "pass" if passed else "fail",
            "same_count": before_count == after_count,
            "workspace_unchanged": workspace_before == workspace_after,
            "content_changed_identity": before != after,
        }


def run_strategy_negative_checks(
    source: Path,
    relation_strategies: list[dict[str, Any]],
    iterations: int,
) -> dict[str, Any]:
    variants: list[tuple[str, list[dict[str, Any]], set[str]]] = []
    bogus = [dict(strategy) for strategy in relation_strategies]
    bogus_edge = next(strategy for strategy in bogus if strategy["table"] == "edge")
    bogus_edge["forward"] = "definitely_not_an_index"
    variants.append(("bogus_forward", bogus, {"outgoing_one_hop", "bounded_paths"}))

    swapped = [dict(strategy) for strategy in relation_strategies]
    swapped_edge = next(strategy for strategy in swapped if strategy["table"] == "edge")
    swapped_edge["forward"], swapped_edge["reverse"] = (
        swapped_edge["reverse"],
        swapped_edge["forward"],
    )
    variants.append(("swapped_edge_directions", swapped, {"outgoing_one_hop", "incoming_one_hop"}))

    results = []
    for name, variant, expected_failures in variants:
        connection = connect(source, read_only=True)
        try:
            cases = query_cases(connection)
            queries = query_evidence(
                connection,
                cases,
                iterations,
                case_strategy_expectations(variant, cases),
            )
        finally:
            connection.close()
        failures = {
            item["id"] for item in queries if item["status"] != "ok"
        }
        detected = expected_failures <= failures
        results.append({
            "name": name,
            "expected_failures": sorted(expected_failures),
            "observed_failures": sorted(failures),
            "status": "pass" if detected else "fail",
        })
    return {
        "status": "pass" if all(item["status"] == "pass" for item in results) else "fail",
        "cases": results,
    }


def validate_profile_partition(profile: dict[str, Any]) -> dict[str, Any]:
    strategies = {item.get("table") for item in profile.get("relation_strategies", [])}
    deferred = profile.get("deferred_relations", [])
    deferred_tables = {item.get("table") for item in deferred}
    missing_required = sorted(REQUIRED_RELATIONS - strategies - deferred_tables)
    unexpected_strategies = sorted(strategies - REQUIRED_RELATIONS)
    unknown_deferred = sorted(deferred_tables - REQUIRED_RELATIONS)
    overlap = sorted(strategies & deferred_tables)
    malformed_deferred = [
        item for item in deferred
        if not item.get("table") or not item.get("owner") or not item.get("reason")
    ]
    return {
        "status": "pass" if not (
            missing_required
            or unexpected_strategies
            or unknown_deferred
            or overlap
            or malformed_deferred
        ) else "fail",
        "required_relations": sorted(REQUIRED_RELATIONS),
        "strategy_tables": sorted(strategies),
        "deferred_tables": sorted(deferred_tables),
        "missing_required": missing_required,
        "unexpected_strategies": unexpected_strategies,
        "unknown_deferred": unknown_deferred,
        "strategy_deferred_overlap": overlap,
        "malformed_deferred": malformed_deferred,
    }


def run_partition_negative_checks(profile: dict[str, Any]) -> dict[str, Any]:
    variants: list[tuple[str, dict[str, Any]]] = []

    missing = dict(profile)
    missing["relation_strategies"] = [
        item for item in profile["relation_strategies"]
        if item.get("table") != "possible_call"
    ]
    variants.append(("missing_required_possible_call", missing))

    unknown = dict(profile)
    unknown["deferred_relations"] = [{
        "table": "unknown_relation",
        "owner": "HSE-78",
        "reason": "test mutation",
    }]
    variants.append(("unknown_deferred_relation", unknown))

    altered = dict(profile)
    altered["deferred_relations"] = [{
        "table": "possible_call",
        "owner": "wrong-owner",
        "reason": "altered required deferral",
    }]
    variants.append(("altered_required_deferral", altered))

    results = []
    for name, variant in variants:
        validation = validate_profile_partition(variant)
        results.append({
            "name": name,
            "status": "pass" if validation["status"] == "fail" else "fail",
            "validation": validation,
        })
    return {
        "status": "pass" if all(item["status"] == "pass" for item in results) else "fail",
        "cases": results,
    }


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
        writer = connect(path, read_only=False, journal_mode="WAL")
        try:
            writer.execute("BEGIN IMMEDIATE")
            writer.execute(
                "INSERT OR REPLACE INTO meta(key, value) VALUES('__qualify_wal_probe__', '1')"
            )
            writer.commit()
            before = file_facts(path)
            connection = connect(path, read_only=True)
            try:
                connection.execute("SELECT COUNT(*) FROM symbol").fetchone()
                state = connection_profile(connection)
            finally:
                connection.close()
            after = file_facts(path)
            new_sidecars = sorted(set(after) - set(before))
            preexisting_sidecar_mutation = before.get("-shm") != after.get("-shm")
            persistent_unchanged = all(
                after.get(key) == before.get(key)
                for key in ("database", "-wal")
                if key in before
            )
            return {
                "status": "pass" if not new_sidecars and persistent_unchanged else "fail",
                "before": before,
                "after": after,
                "new_sidecars": new_sidecars,
                "preexisting_sidecar_mutation": preexisting_sidecar_mutation,
                "preexisting_wal": "-wal" in before and "-shm" in before,
                "contract": "persistent database/WAL bytes are immutable; pre-existing WAL -shm lock-state mutation is permitted",
                "sidecar_mutation_allowed": preexisting_sidecar_mutation,
                "profile": state,
            }
        finally:
            writer.close()


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
                source_identity = (
                    catalog_identity(source_connection),
                    workspace_identity(source_connection),
                    fact_set_identity(source_connection),
                )
                target_identity = (
                    catalog_identity(restored),
                    workspace_identity(restored),
                    fact_set_identity(restored),
                )
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


def prepare_recovery_copy(path: Path) -> None:
    connection = connect(path, read_only=False, journal_mode="DELETE")
    try:
        connection.executescript(
            "CREATE TABLE IF NOT EXISTS __qualify_index_stage "
            "(id INTEGER PRIMARY KEY, usr TEXT NOT NULL, qual_name TEXT); "
            "CREATE TABLE IF NOT EXISTS __qualify_entity_snapshot "
            "AS SELECT * FROM entity_edge WHERE 0; "
            "CREATE TABLE IF NOT EXISTS __qualify_migration_legacy "
            "(id INTEGER PRIMARY KEY, payload TEXT NOT NULL);"
        )
        connection.execute("DELETE FROM __qualify_index_stage")
        connection.execute(
            "INSERT INTO __qualify_index_stage(id, usr, qual_name) "
            "SELECT id, usr, qual_name FROM symbol ORDER BY id"
        )
        connection.execute("DELETE FROM __qualify_entity_snapshot")
        connection.execute(
            "INSERT INTO __qualify_entity_snapshot SELECT * FROM entity_edge"
        )
        connection.execute(
            "INSERT OR REPLACE INTO __qualify_migration_legacy(id, payload) "
            "SELECT id, COALESCE(spelling, '') FROM symbol ORDER BY id"
        )
        connection.commit()
    finally:
        connection.close()


def kill_on_progress(connection: sqlite3.Connection, calls: int = 25) -> None:
    state = {"calls": 0}

    def progress() -> int:
        state["calls"] += 1
        if state["calls"] >= calls:
            os.kill(os.getpid(), 9)
        return 0

    connection.set_progress_handler(progress, 1)


def crash_child(path: Path, phase: str) -> int:
    connection = connect(path, read_only=False, journal_mode="DELETE")
    try:
        if phase == "wal_checkpoint":
            connection.execute("PRAGMA journal_mode = WAL")
            connection.execute("BEGIN IMMEDIATE")
            connection.execute(
                "INSERT OR REPLACE INTO meta(key, value) "
                "VALUES('__qualify_checkpoint_probe__', '1')"
            )
            connection.commit()
            blocker = sqlite3.connect(path)
            blocker.execute("BEGIN")
            blocker.execute("SELECT COUNT(*) FROM symbol").fetchone()
            timer = threading.Timer(0.05, lambda: os.kill(os.getpid(), 9))
            timer.start()
            connection.execute("PRAGMA wal_checkpoint(TRUNCATE)")
            os.kill(os.getpid(), 9)
        elif phase == "maintenance":
            kill_on_progress(connection)
            connection.execute("ANALYZE")
        else:
            connection.execute("BEGIN IMMEDIATE")
            if phase == "indexing":
                connection.execute("DELETE FROM __qualify_index_stage")
                kill_on_progress(connection)
                connection.execute(
                    "INSERT INTO __qualify_index_stage(id, usr, qual_name) "
                    "SELECT id, usr, qual_name FROM symbol ORDER BY id"
                )
            elif phase == "named_transform":
                kill_on_progress(connection)
                connection.execute("DELETE FROM entity_edge")
                connection.execute(
                    "INSERT INTO entity_edge SELECT * FROM __qualify_entity_snapshot"
                )
            elif phase == "migration":
                connection.execute(
                    "ALTER TABLE __qualify_migration_legacy "
                    "ADD COLUMN migrated_marker INTEGER DEFAULT 0"
                )
                kill_on_progress(connection)
                connection.execute(
                    "UPDATE __qualify_migration_legacy SET migrated_marker = 1"
                )
            else:
                raise ValueError(f"unknown recovery phase: {phase}")
    finally:
        connection.close()
    return 0


def run_recovery_check(source: Path) -> dict[str, Any]:
    phases = ["indexing", "named_transform", "migration", "wal_checkpoint", "maintenance"]
    allowed_states = {
        "indexing": {"current"},
        "named_transform": {"current"},
        "migration": {"current"},
        "wal_checkpoint": {"current", "stale-but-valid"},
        "maintenance": {"current", "stale-but-valid"},
    }
    results = []
    for phase in phases:
        with tempfile.TemporaryDirectory(prefix="cidx-storage-m1-recovery-") as temporary:
            path = copy_for_profile(source, Path(temporary), phase)
            prepare_recovery_copy(path)
            before_connection = connect(path, read_only=True)
            try:
                before_identity = (
                    catalog_identity(before_connection),
                    workspace_identity(before_connection),
                    fact_set_identity(before_connection),
                )
            finally:
                before_connection.close()
            completed = subprocess.run(
                [sys.executable, __file__, "--crash-child", "--db", str(path), "--phase", phase],
                capture_output=True,
                text=True,
            )
            connection = connect(path, read_only=True)
            try:
                integrity = connection.execute("PRAGMA integrity_check").fetchone()[0]
                foreign_keys = connection.execute("PRAGMA foreign_key_check").fetchall()
                version = connection.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
                after_identity = (
                    catalog_identity(connection),
                    workspace_identity(connection),
                    fact_set_identity(connection),
                )
            finally:
                connection.close()
            semantic_equivalent = before_identity == after_identity
            valid = (
                integrity == "ok"
                and not foreign_keys
                and version is not None
                and int(version[0]) == SCHEMA_VERSION
            )
            state = "current" if semantic_equivalent else "stale-but-valid"
            passed = completed.returncode == -9 and valid and state in allowed_states[phase]
            results.append({
                "phase": phase,
                "status": "pass" if passed else "fail",
                "child_exit": completed.returncode,
                "integrity": integrity,
                "foreign_key_errors": foreign_keys,
                "semantic_equivalent": semantic_equivalent,
                "state": state,
                "allowed_states": sorted(allowed_states[phase]),
            })
    return {"status": "pass" if all(item["status"] == "pass" for item in results) else "fail", "cases": results}


def qualify(source: Path, profile_path: Path, iterations: int) -> dict[str, Any]:
    profile = json.loads(profile_path.read_text())
    relation_strategies = profile["relation_strategies"]
    partition = validate_profile_partition(profile)
    partition_negative_checks = run_partition_negative_checks(profile)
    strategies = {item["table"] for item in relation_strategies}
    missing = sorted(REQUIRED_RELATIONS - strategies)
    with tempfile.TemporaryDirectory(prefix="cidx-storage-m1-representative-") as temporary:
        representative_source, representative_corpus = prepare_representative_corpus(
            source, Path(temporary)
        )
        layout = run_layout(representative_source, iterations, relation_strategies)
        strategy_negative_checks = run_strategy_negative_checks(
            representative_source, relation_strategies, iterations
        )
    query_by_id = {item["id"]: item for item in layout["queries"]}
    declared_query_ids = sorted({
        query_id
        for strategy in relation_strategies
        for query_id in strategy.get("query_ids", [])
    })
    missing_query_ids = sorted(set(declared_query_ids) - set(query_by_id))
    declared_supporting_ids = set(profile.get("supporting_query_ids", []))
    unexpected_query_ids = sorted(
        set(query_by_id) - set(declared_query_ids) - declared_supporting_ids
    )
    strategy_validation = []
    for strategy in relation_strategies:
        bindings = []
        for query_id in strategy.get("query_ids", []):
            query = query_by_id.get(query_id)
            direction = query.get("strategy_direction") if query else (
                "reverse" if query_id in REVERSE_QUERY_IDS else "forward"
            )
            bindings.append({
                "query_id": query_id,
                "direction": direction,
                "binding_matches": bool(
                    query
                    and query.get("strategy_table") == strategy["table"]
                    and query.get("strategy_direction") == direction
                ),
                "plan_bound": bool(
                    query
                    and query.get("expected_index") == strategy.get(direction)
                    and query.get("indexed")
                    and query.get("status") == "ok"
                ),
            })
        strategy_validation.append({
            "table": strategy["table"],
            "forward": strategy.get("forward"),
            "reverse": strategy.get("reverse"),
            "query_ids": strategy.get("query_ids", []),
            "query_ids_present": all(item["query_id"] in query_by_id for item in bindings),
            "bindings": bindings,
        })
    runtime = [run_runtime_profile(source, mode, iterations) for mode in ("DELETE", "WAL")]
    read_only = run_read_only_check(source)
    backup = run_backup_check(source)
    recovery = run_recovery_check(source)
    identity_sensitivity = run_identity_sensitivity_check(source)
    query_failures = [item["id"] for item in layout["queries"] if item["status"] != "ok"]
    representative_failures = [
        item["id"]
        for item in layout["queries"]
        if item.get("require_rows", True) and not item["has_evidence"]
    ]
    corpus_status = (
        "pass"
        if layout["resolved"] and not representative_failures
        else "fail"
    )
    strategy_status = (
        "pass"
        if not missing
        and not missing_query_ids
        and not unexpected_query_ids
        and partition["status"] == "pass"
        and partition_negative_checks["status"] == "pass"
        and all(
            item["query_ids_present"]
            and all(binding["binding_matches"] and binding["plan_bound"] for binding in item["bindings"])
            for item in strategy_validation
        )
        else "fail"
    )
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
        "identity_sensitivity": identity_sensitivity,
        "gates": {
            "relation_strategy_catalog": strategy_status,
            "representative_corpus": corpus_status,
            "query_plans": "pass" if not query_failures and not missing_query_ids else "fail",
            "read_only_persistent_non_mutating": read_only["status"],
            "backup_restore_identity": backup["status"],
            "interruption_recovery": recovery["status"],
            "fact_identity_sensitivity": identity_sensitivity["status"],
            "strategy_negative_checks": strategy_negative_checks["status"],
            "partition_negative_checks": partition_negative_checks["status"],
            "wal_decision": "qualification_only",
        },
        "missing_relation_strategies": missing,
        "declared_query_ids": declared_query_ids,
        "missing_query_ids": missing_query_ids,
        "unexpected_query_ids": unexpected_query_ids,
        "strategy_validation": strategy_validation,
        "strategy_negative_checks": strategy_negative_checks,
        "profile_partition": partition,
        "partition_negative_checks": partition_negative_checks,
        "representative_corpus": representative_corpus,
        "representative_failures": representative_failures,
        "query_failures": query_failures,
        "notes": [
            "DELETE is the shipped runtime profile; WAL is measured on the same copied database and workload.",
            "A benchmark result is host-specific and must not be treated as a universal SLO.",
            "possible_call is required; an empty canonical relation is qualified through a deterministic temporary multi-definition overlay whose basis and row count are recorded.",
            "The profile's required/deferred relation partition is exact; missing, unknown, or altered partition entries fail negative probes.",
            "A pre-existing WAL -shm lock-state change is permitted; persistent database and WAL bytes must remain unchanged.",
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
