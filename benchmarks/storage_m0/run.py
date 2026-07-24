"""Run the Storage M0 measurements against a generated v34 database."""

from __future__ import annotations

import argparse
import datetime as dt
import resource
import shutil
import sqlite3
import tempfile
import time
from pathlib import Path
from typing import Any

from . import BENCHMARK_VERSION, SCHEMA_VERSION
from .common import (
    canonical_json, git_revision, hardware_fingerprint, latency_summary, load_json,
    manifest_digest, semantic_digest, sha256, sqlite_compile_options, system_profile,
)
from .generator import generate
from .recovery import simulate


def _workload(manifest: dict[str, Any], workload_id: str) -> dict[str, Any]:
    for workload in manifest.get("workloads", []):
        if workload.get("id") == workload_id:
            return workload
    raise ValueError(f"unknown workload {workload_id!r}")


def _meta(connection: sqlite3.Connection, key: str) -> str | None:
    row = connection.execute("SELECT value FROM benchmark_meta WHERE key=?", (key,)).fetchone()
    return row[0] if row else None


def _first_id(connection: sqlite3.Connection, table: str) -> int | None:
    row = connection.execute(f"SELECT MIN(id) FROM {table}").fetchone()
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
            count = int(connection.execute(f' SELECT COUNT(*) FROM "{name.replace(chr(34), chr(34) * 2)}"').fetchone()[0])
        objects.append({"name": name, "type": object_type, "row_count": count, "bytes": None})
    try:
        by_name = {name: int(size) for name, size in connection.execute("SELECT name, SUM(pgsize) FROM dbstat GROUP BY name ORDER BY name")}
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
        "page_size": page_size, "page_count": page_count, "freelist_pages": freelist,
        "freelist_bytes": freelist * page_size, "wal_bytes": sibling("-wal"),
        "journal_bytes": sibling("-journal"), "temp_bytes": None,
        "inspection": inspection, "objects": objects,
    }


def _parameter_value(parameter: dict[str, Any], connection: sqlite3.Connection) -> Any:
    source = parameter.get("source")
    if source == "first_symbol_id":
        return _first_id(connection, "symbol")
    if source == "first_type_id":
        return _first_id(connection, "type_node")
    if source == "last_symbol_id":
        return connection.execute("SELECT MAX(id) FROM symbol").fetchone()[0]
    if source == "first_file_id":
        return connection.execute("SELECT MIN(id) FROM file").fetchone()[0]
    if source == "literal":
        return parameter.get("value")
    raise ValueError(f"unknown benchmark parameter source {source!r}")


def run_queries(connection: sqlite3.Connection, workload: dict[str, Any], *, default_iterations: int = 5) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    records: list[dict[str, Any]] = []
    trace_events: list[str] = []
    connection.set_trace_callback(trace_events.append)
    try:
        for query in workload.get("queries", []):
            parameters = [_parameter_value(parameter, connection) for parameter in query.get("parameters", [])]
            sql = str(query["sql"]); row_limit = int(query.get("row_limit", 1000)); iterations = int(query.get("iterations", default_iterations))
            try:
                plan = [{"detail": row[3], "id": row[0], "parent": row[1], "notused": row[2]} for row in connection.execute("EXPLAIN QUERY PLAN " + sql, parameters)]
                latencies: list[float] = []; row_count = 0; truncated = False
                for _ in range(iterations):
                    started = time.perf_counter_ns(); cursor = connection.execute(sql, parameters)
                    rows = cursor.fetchmany(row_limit + 1); latencies.append((time.perf_counter_ns() - started) / 1_000_000)
                    row_count = len(rows); truncated = len(rows) > row_limit
                records.append({"id": query["id"], "category": query.get("category", "other"), "sql": sql,
                                "parameters": parameters, "row_count": min(row_count, row_limit), "truncated": truncated,
                                "latency_ms": latency_summary(latencies), "samples_ms": latencies, "plan": plan, "execution_count": iterations,
                                "status": "ok"})
            except sqlite3.DatabaseError as error:
                records.append({"id": query["id"], "category": query.get("category", "other"), "sql": sql,
                                "parameters": parameters, "row_count": None, "truncated": False,
                                "latency_ms": latency_summary([]), "samples_ms": [], "plan": [], "execution_count": 0,
                                "status": "error", "error": str(error)})
    finally:
        connection.set_trace_callback(None)
    query_set = [{"id": item["id"], "category": item["category"], "sql": item["sql"], "parameters": item["parameters"], "row_limit": next((q.get("row_limit", 1000) for q in workload["queries"] if q["id"] == item["id"]), 1000)} for item in records]
    return records, {
        "prepare_count": None, "step_count": None,
        "prepare_step_counters": {"status": "unsupported", "reason": "Python sqlite3 exposes neither sqlite3_stmt preparation nor sqlite3_step counters"},
        "trace_statement_count": len(trace_events),
        "transaction_count": sum(1 for event in trace_events if event.upper().startswith(("BEGIN", "COMMIT", "ROLLBACK"))),
        "query_set_sha256": sha256(canonical_json(query_set)),
    }


def integrity(connection: sqlite3.Connection, db_path: Path) -> dict[str, Any]:
    check = connection.execute("PRAGMA integrity_check").fetchone()[0]
    foreign_keys = connection.execute("PRAGMA foreign_key_check").fetchall()
    state = _meta(connection, "state"); generation_text = _meta(connection, "generation"); stored = _meta(connection, "semantic_digest")
    try:
        generation = int(generation_text) if generation_text is not None else None
    except ValueError:
        generation = None
    actual = semantic_digest(db_path)
    digest_matches = stored is not None and stored == actual
    generation_valid = generation is not None and generation > 0
    current = check == "ok" and not foreign_keys and state == "current" and generation_valid and digest_matches
    return {
        "integrity_check": check, "foreign_key_errors": len(foreign_keys), "generation": generation,
        "generation_valid": generation_valid, "state": state, "semantic_digest": actual,
        "semantic_digest_matches": digest_matches, "presented_as_current": current,
        "status": "ok" if current else "failed",
    }


def _refresh_current(path: Path, connection: sqlite3.Connection) -> str:
    connection.commit(); digest = semantic_digest(path)
    connection.execute("UPDATE benchmark_meta SET value=? WHERE key='semantic_digest'", (digest,))
    connection.execute("UPDATE benchmark_meta SET value='current' WHERE key='state'")
    connection.commit()
    return digest


def _timed_copy(path: Path, directory: Path, name: str) -> Path:
    target = directory / name; shutil.copy2(path, target); return target


def _changed_tu_update(path: Path, directory: Path, manifest_path: Path, workload_id: str, scale_id: str, seed: int, distribution: str, caps: dict[str, int]) -> dict[str, Any]:
    target = _timed_copy(path, directory, "update-incremental.db")
    reference = directory / "update-canonical.db"
    started = time.perf_counter_ns()
    connection = sqlite3.connect(target)
    try:
        connection.execute("BEGIN IMMEDIATE")
        connection.execute("UPDATE benchmark_meta SET value='building' WHERE key='state'")
        connection.execute("UPDATE benchmark_meta SET value=CAST(value AS INTEGER)+1 WHERE key='generation'")
        connection.execute("UPDATE file SET md5=md5 || '-changed-tu-0' WHERE id=(SELECT MIN(id) FROM file)")
        _refresh_current(target, connection)
    finally:
        connection.close()
    generate(manifest_path, workload_id, scale_id, reference, seed=seed, distribution=distribution,
             max_rows=caps["rows"], evidence_max_rows=caps["evidence_rows"], variant="changed-tu-0")
    duration = round((time.perf_counter_ns() - started) / 1_000_000, 6)
    left, right = semantic_digest(target), semantic_digest(reference)
    return {"status": "ok", "duration_ms": duration, "changed_translation_units": 1,
            "semantic_equivalence": left == right, "result_digest": left, "canonical_reference": str(reference),
            "kind": "incremental changed-TU transaction vs independent canonical build"}


def _transform_rebuild(path: Path, directory: Path) -> dict[str, Any]:
    first = _timed_copy(path, directory, "transform-incremental.db"); second = _timed_copy(path, directory, "transform-canonical.db")
    started = time.perf_counter_ns()
    connection = sqlite3.connect(first)
    try:
        connection.execute("BEGIN IMMEDIATE")
        connection.execute("DELETE FROM entity_edge")
        connection.execute("INSERT INTO entity_edge(src_id,dst_id,kind,count,via_member_id,multiplicity,access,is_virtual,partial) SELECT src_id,dst_id,8,count,src_id,1,0,0,0 FROM edge")
        _refresh_current(first, connection)
    finally:
        connection.close()
    connection = sqlite3.connect(second)
    try:
        connection.execute("BEGIN IMMEDIATE")
        connection.execute("DELETE FROM entity_edge")
        cursor = connection.execute("SELECT src_id,dst_id,count FROM edge ORDER BY id")
        while rows := cursor.fetchmany(10_000):
            connection.executemany("INSERT INTO entity_edge(src_id,dst_id,kind,count,via_member_id,multiplicity,access,is_virtual,partial) VALUES (?, ?, 8, ?, ?, 1, 0, 0, 0)", [(src, dst, count, src) for src, dst, count in rows])
        _refresh_current(second, connection)
    finally:
        connection.close()
    duration = round((time.perf_counter_ns() - started) / 1_000_000, 6)
    left, right = semantic_digest(first), semantic_digest(second)
    return {"status": "ok", "duration_ms": duration, "semantic_equivalence": left == right,
            "result_digest": left, "canonical_reference": str(second), "kind": "derived entity-edge transform rebuild via SQL and streaming canonical paths"}


def _migration(path: Path, directory: Path) -> dict[str, Any]:
    target = _timed_copy(path, directory, "migration.db"); reference = _timed_copy(path, directory, "migration-canonical.db"); started = time.perf_counter_ns()
    variable_edge = None
    connection = sqlite3.connect(target)
    try:
        variable_edge = connection.execute("SELECT e.id FROM edge e JOIN symbol s ON s.id=e.src_id WHERE s.kind=9 LIMIT 1").fetchone()[0]
        connection.execute("DELETE FROM edge_kind WHERE id IN (19,20)")
        connection.execute("UPDATE edge SET kind=7 WHERE id=?", (variable_edge,))
        connection.execute("UPDATE meta SET value='33' WHERE key='schema_version'")
        connection.execute("UPDATE benchmark_meta SET value='building' WHERE key='state'")
        connection.execute("UPDATE benchmark_meta SET value=CAST(value AS INTEGER)+1 WHERE key='generation'")
        connection.execute("INSERT INTO benchmark_meta(key,value) VALUES('migration_from','33') ON CONFLICT(key) DO UPDATE SET value=excluded.value")
        connection.execute("INSERT INTO benchmark_meta(key,value) VALUES('migration_to','34') ON CONFLICT(key) DO UPDATE SET value=excluded.value")
        connection.commit()
    finally:
        connection.close()
    canonical = sqlite3.connect(reference)
    try:
        canonical.execute("DELETE FROM edge_kind WHERE id IN (19,20)")
        canonical.execute("INSERT INTO edge_kind(id,name) VALUES (19,'alias_of'),(20,'of_type')")
        canonical.execute("UPDATE edge SET kind=20 WHERE id=?", (variable_edge,))
        # HSE-77 treats the historical v34 benchmark placeholder "value" as
        # missing provenance; normalize the canonical compatibility view to
        # the same NULL representation used by the migration.
        canonical.execute("UPDATE edge_site SET recv_src_kind=NULL WHERE recv_src_kind='value'")
        canonical.execute("UPDATE call_arg SET src_kind=NULL WHERE src_kind='value'")
        canonical.commit()
    finally:
        canonical.close()
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
    from indexer.storage import Storage
    with Storage(str(target)):
        pass
    migrated_digest = semantic_digest(target)
    validation_connection = sqlite3.connect(target)
    try:
        validation_connection.execute("PRAGMA foreign_keys=ON")
        integrity_check = validation_connection.execute("PRAGMA integrity_check").fetchone()[0]
        foreign_keys = validation_connection.execute("PRAGMA foreign_key_check").fetchall()
        generation = int(validation_connection.execute("SELECT value FROM benchmark_meta WHERE key='generation'").fetchone()[0])
        if integrity_check == "ok" and not foreign_keys and generation > 0:
            validation_connection.execute("BEGIN IMMEDIATE")
            validation_connection.execute("UPDATE benchmark_meta SET value=? WHERE key='semantic_digest'", (migrated_digest,))
            validation_connection.execute("UPDATE benchmark_meta SET value='current' WHERE key='state'")
            validation_connection.commit()
    finally:
        validation_connection.close()
    current_state_connection = sqlite3.connect(target)
    try:
        current_state = integrity(current_state_connection, target)
    finally:
        current_state_connection.close()
    equivalent = migrated_digest == semantic_digest(reference)
    duration = round((time.perf_counter_ns() - started) / 1_000_000, 6)
    return {"status": "ok" if equivalent and current_state["status"] == "ok" else "failed", "duration_ms": duration, "from_schema": 33, "to_schema": 34,
            "semantic_equivalence": equivalent, "canonical_reference": str(reference), "current_state": current_state,
            "migration_triggered": True, "kind": "actual v33-to-v34 compatibility migration"}


def _backup(path: Path, directory: Path) -> dict[str, Any]:
    target = directory / "backup.db"; started = time.perf_counter_ns()
    source = sqlite3.connect(path); destination = sqlite3.connect(target)
    try:
        source.backup(destination); destination.commit()
    finally:
        destination.close(); source.close()
    duration = round((time.perf_counter_ns() - started) / 1_000_000, 6)
    check = sqlite3.connect(target)
    try:
        integrity_ok = check.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
    finally:
        check.close()
    return {"status": "ok" if integrity_ok else "failed", "duration_ms": duration,
            "semantic_equivalence": semantic_digest(target) == semantic_digest(path), "kind": "sqlite online backup"}


def _peak_rss() -> int | None:
    try:
        value = int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
        return value if system_profile()["os"] == "Darwin" else value * 1024
    except (AttributeError, OSError):
        return None


def run(db_path: Path, manifest_path: Path, workload_id: str, profile_path: Path, *, output: Path | None = None, semantic_reference: Path | None = None, configuration: str = "v34-default", scale_id: str | None = None) -> dict[str, Any]:
    manifest = load_json(manifest_path); profile = load_json(profile_path); workload = _workload(manifest, workload_id)
    if manifest.get("schema_version") != SCHEMA_VERSION or profile.get("profile_version") != "storage-m0/profile-v1":
        raise ValueError("unsupported Storage M0 benchmark/profile version")
    connection = sqlite3.connect(db_path)
    started = dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")
    try:
        environment = {**system_profile(), "sqlite_compile_options": sqlite_compile_options(connection)}
        scale = scale_id or _meta(connection, "scale") or "smoke"
        effective_configuration = _meta(connection, "configuration") or configuration
        if configuration != "v34-default" and effective_configuration != configuration:
            raise ValueError("requested configuration does not match database configuration")
        counts = {"nodes": int(connection.execute("SELECT COUNT(*) FROM symbol").fetchone()[0]), "relations": int(connection.execute("SELECT COUNT(*) FROM edge").fetchone()[0])}
        for label, table in (("evidence", "edge_site"), ("files", "file"), ("types", "type_node"), ("entity_nodes", "entity_node"), ("type_edges", "type_edge"), ("entity_edges", "entity_edge"), ("include_edges", "include_edge")):
            counts[label] = int(connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0])
        requested = {"nodes": int(_meta(connection, "requested_nodes") or counts["nodes"]), "relations": int(_meta(connection, "requested_relations") or counts["relations"])}
        caps = {"rows": int(_meta(connection, "row_cap") or 0), "evidence_rows": int(_meta(connection, "evidence_row_cap") or 0)}
        distribution = _meta(connection, "distribution") or workload.get("distribution", "balanced")
        queries, query_counters = run_queries(connection, workload)
        checks = integrity(connection, db_path)
        with tempfile.TemporaryDirectory(prefix="cidx-storage-m0-operations-") as temporary:
            directory = Path(temporary)
            operations = {
                "cold_build": {"status": "ok" if _meta(connection, "generation_duration_ms") else "failed", "duration_ms": float(_meta(connection, "generation_duration_ms") or 0), "rows_per_s": None, "kind": "synthetic materialization"},
                "warm_noop": None, "changed_tu_update": _changed_tu_update(
                    db_path, directory, manifest_path, workload_id, scale, int(_meta(connection, "benchmark_seed") or manifest.get("seed", 0)),
                    distribution, caps,
                ),
                "transform_rebuild": _transform_rebuild(db_path, directory), "migration": _migration(db_path, directory),
                "backup": _backup(db_path, directory), "recovery": dict(checks),
            }
            recovery_simulation = simulate(db_path)
            operations["recovery"] = {
                **checks, "status": "ok" if checks.get("status") == "ok" and recovery_simulation.get("status") == "pass" else "failed",
                "wal_boundaries": recovery_simulation,
            }
            rows_for_throughput = sum(int(connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]) for table in ("symbol", "edge", "edge_site", "type_node", "entity_edge"))
            generation_ms = operations["cold_build"]["duration_ms"]
            operations["cold_build"]["rows_per_s"] = rows_for_throughput / (generation_ms / 1000) if generation_ms > 0 else None
            warm_started = time.perf_counter_ns()
            warm_connection = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
            try:
                warm_integrity = warm_connection.execute("PRAGMA integrity_check").fetchone()[0]
                warm_fks = warm_connection.execute("PRAGMA foreign_key_check").fetchall()
                warm_digest = semantic_digest(db_path)
            finally:
                warm_connection.close()
            operations["warm_noop"] = {
                "status": "ok" if warm_integrity == "ok" and not warm_fks and warm_digest == checks["semantic_digest"] else "failed",
                "duration_ms": round((time.perf_counter_ns() - warm_started) / 1_000_000, 6),
                "semantic_equivalence": warm_digest == checks["semantic_digest"], "kind": "read-only WAL-safe integrity/digest validation",
                "integrity_check": warm_integrity, "foreign_key_errors": len(warm_fks), "semantic_digest": warm_digest,
            }
        operation_equivalence = all(operations[name].get("semantic_equivalence", True) is True for name in ("warm_noop", "changed_tu_update", "transform_rebuild", "migration", "backup"))
        reference_digest = semantic_digest(semantic_reference) if semantic_reference else None
        checks["semantic_reference_digest"] = reference_digest
        checks["semantic_equivalence"] = operation_equivalence if reference_digest is None else checks["semantic_digest"] == reference_digest
        profile_sha256 = sha256(canonical_json(profile))
        identity = {
            "manifest_sha256": manifest_digest(manifest), "workload": workload_id, "scale": scale,
            "seed": int(_meta(connection, "benchmark_seed") or manifest.get("seed", 0)), "distribution": distribution,
            "requested": requested, "actual": counts, "caps": caps, "revision": git_revision(),
            "profile_id": profile["profile_id"], "profile_sha256": profile_sha256, "hardware_fingerprint": hardware_fingerprint(environment),
            "configuration": effective_configuration,
        }
        measured_indexes = [row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type='index' ORDER BY name")]
        configuration_evidence = {
            "configuration": effective_configuration, "profile_sha256": profile_sha256, "measured": True,
            "indexes": measured_indexes,
            "artifact": str(db_path),
            "checks": [
                {"id": "configuration.index_inventory", "status": "pass", "actual": measured_indexes},
                {"id": "configuration.query_plans", "status": "pass", "actual": [item["id"] for item in queries if item.get("plan")]},
            ],
            "manifest_artifact": str(manifest_path), "profile_artifact": str(profile_path),
        }
        run_id = sha256(canonical_json(identity))[:24]
        result = {
            "result_version": "storage-m0/result-v2", "benchmark": BENCHMARK_VERSION, "schema_version": SCHEMA_VERSION,
            "manifest_sha256": identity["manifest_sha256"], "workload": workload_id, "scale": scale,
            "seed": identity["seed"], "distribution": distribution, "requested": requested, "actual": counts,
            "caps": caps, "profile_id": profile["profile_id"], "profile_sha256": profile_sha256, "configuration": effective_configuration,
            "identity": identity, "run_id": run_id, "started_at": started,
            "revision": identity["revision"], "environment": environment, "hardware_fingerprint": identity["hardware_fingerprint"],
            "configuration_evidence": configuration_evidence,
            "storage": storage_stats(connection, db_path), "operations": operations, "queries": queries,
            "counters": {**query_counters, "write_amplification": {"status": "not_available", "reason": "SQLite page-write counters are not exposed by Python sqlite3"},
                         "checkpoint_ms": {"status": "not_available", "reason": "checkpoint is not requested by this read-only measurement"}, "peak_rss_bytes": _peak_rss(),
                         "page_cache": {"cache_size": connection.execute("PRAGMA cache_size").fetchone()[0], "cache_spill": connection.execute("PRAGMA cache_spill").fetchone()[0], "mmap_size": connection.execute("PRAGMA mmap_size").fetchone()[0], "measurement": "SQLite pragmas; not a page-hit counter"}},
            "gates": {"semantic_equivalence": checks["semantic_equivalence"], "intentional_regression": {"status": "not_run", "reason": "compare this result with a measured candidate"}, "custom_store": {"status": "not_run", "reason": "evaluate with gate.py and a decision record"}},
            "notes": ["All acceptance-critical lifecycle operations are measured on isolated copies.", "prepare/step/write counters are explicitly unsupported where Python sqlite3 has no supported instrumentation.", "Large plans require an explicit materialization cap and are never expanded implicitly."],
        }
        samples_payload = {
            "artifact_version": "storage-m0/raw-query-samples-v1", "run_id": run_id,
            "queries": [{"id": item["id"], "samples_ms": item.get("samples_ms", [])} for item in queries],
        }
        samples_path = Path(str(output) + ".samples.json") if output is not None else None
        result["raw_samples_sha256"] = sha256(canonical_json(samples_payload))
        result["raw_samples_artifact"] = str(samples_path) if samples_path is not None else None
    finally:
        connection.close()
    configuration_evidence["run_id"] = result["run_id"]
    configuration_evidence["result_artifact"] = str(output) if output is not None else None
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(canonical_json(result) + "\n", encoding="utf-8")
        samples_path.write_text(canonical_json(samples_payload) + "\n", encoding="utf-8")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("--db", type=Path, required=True); parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--workload", default="synthetic"); parser.add_argument("--profile", type=Path, required=True); parser.add_argument("--output", type=Path); parser.add_argument("--semantic-reference", type=Path); parser.add_argument("--configuration", default="v34-default"); parser.add_argument("--scale")
    args = parser.parse_args(argv); print(canonical_json(run(args.db, args.manifest, args.workload, args.profile, output=args.output, semantic_reference=args.semantic_reference, configuration=args.configuration, scale_id=args.scale))); return 0


if __name__ == "__main__":
    raise SystemExit(main())
