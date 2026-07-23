"""Generate deterministic SQLite v34 benchmark databases.

The generator intentionally writes the checked-in cidx schema, not a parallel
benchmark-only schema. Large plans are represented in manifests and are only
materialized when explicitly requested.
"""

from __future__ import annotations

import argparse
import json
import sqlite3
import time
from pathlib import Path
from typing import Any

from .common import (
    SCHEMA_VERSION,
    canonical_json,
    load_cidx_schema,
    load_json,
    manifest_digest,
    semantic_digest,
)


def _workload(manifest: dict[str, Any], workload_id: str) -> dict[str, Any]:
    for workload in manifest.get("workloads", []):
        if workload.get("id") == workload_id:
            return workload
    raise ValueError(f"unknown workload {workload_id!r}")


def _scale(workload: dict[str, Any], scale_id: str) -> dict[str, int]:
    scales = workload.get("scales", {})
    if scale_id not in scales:
        raise ValueError(f"unknown scale {scale_id!r}")
    value = scales[scale_id]
    if not isinstance(value, dict):
        raise ValueError(f"scale {scale_id!r} must be an object")
    return {key: int(number) for key, number in value.items()}


def _topology(connection: sqlite3.Connection, workload: dict[str, Any]) -> list[int]:
    topology = workload.get("topology", {})
    repositories = int(topology.get("repositories", 1))
    components = int(topology.get("components_per_repository", 1))
    files = int(topology.get("translation_units_per_component", 1))
    file_ids: list[int] = []
    for repository in range(repositories):
        connection.execute(
            "INSERT INTO repository(name, kind, remote_url) VALUES (?, 'repo', ?)",
            (f"repo-{repository:04d}", f"https://example.invalid/repo-{repository:04d}"),
        )
        repository_id = connection.execute("SELECT last_insert_rowid()").fetchone()[0]
        connection.execute(
            "INSERT INTO clone(repository_id, path, label) VALUES (?, ?, 'benchmark')",
            (repository_id, f"/storage-m0/repo-{repository:04d}"),
        )
        clone_id = connection.execute("SELECT last_insert_rowid()").fetchone()[0]
        connection.execute(
            "UPDATE repository SET active_clone_id=? WHERE id=?", (clone_id, repository_id)
        )
        for component in range(components):
            connection.execute(
                "INSERT INTO component(name, path, kind, repository_id) VALUES (?, ?, 'repo', ?)",
                (f"component-{repository:04d}-{component:04d}", f"component-{component:04d}", repository_id),
            )
            component_id = connection.execute("SELECT last_insert_rowid()").fetchone()[0]
            connection.execute(
                "INSERT INTO directory(component_id, path) VALUES (?, '')", (component_id,)
            )
            directory_id = connection.execute("SELECT last_insert_rowid()").fetchone()[0]
            for tu in range(files):
                connection.execute(
                    "INSERT INTO file(directory_id, name, md5, compile_options, driver) "
                    "VALUES (?, ?, ?, ?, ?)",
                    (
                        directory_id,
                        f"tu-{tu:04d}.cpp",
                        f"benchmark-{repository}-{component}-{tu:04d}",
                        json.dumps(["-std=c++23"], separators=(",", ":")),
                        "c++",
                    ),
                )
                file_ids.append(connection.execute("SELECT last_insert_rowid()").fetchone()[0])
    return file_ids


def _relation_pair(index: int, nodes: int, distribution: str) -> tuple[int, int]:
    if nodes < 2:
        return 0, 0
    if distribution == "high-fan-in":
        return index % nodes, (index // nodes) % max(1, nodes // 32)
    if distribution == "high-fan-out":
        fan = max(1, nodes // 32)
        return index % fan, (index // fan + index % fan + 1) % nodes
    if distribution == "long-chain":
        return index % nodes, (index + 1) % nodes
    if distribution == "cyclic":
        return index % nodes, (index + 1 + (index // nodes)) % nodes
    if distribution == "diamond":
        layer = index % 4
        base = (index // 4) % max(1, nodes // 4)
        return (base + layer) % nodes, (base + layer + 1) % nodes
    if distribution == "skewed":
        source = (index * index + 7 * index) % nodes
        return source, (index * 31 + 3) % nodes
    # balanced and the explicit fallback keep the mapping easy to reproduce.
    source = index % nodes
    return source, (index // nodes * 17 + source * 3 + 1) % nodes


def _insert_symbols(
    connection: sqlite3.Connection,
    nodes: int,
    file_ids: list[int],
    kind_ids: dict[str, int],
    chunk_size: int = 10_000,
) -> int:
    kinds = ["class", "struct", "function", "variable", "typedef"]
    first_id: int | None = None
    for start in range(0, nodes, chunk_size):
        rows = []
        for index in range(start, min(nodes, start + chunk_size)):
            kind = kinds[index % len(kinds)]
            file_id = file_ids[index % len(file_ids)]
            rows.append(
                (
                    f"benchmark:node:{index:012d}",
                    f"Node{index:012d}",
                    f"benchmark::Node{index:012d}",
                    kind_ids[kind],
                    file_id,
                    (index % 100_000) + 1,
                    1,
                    1,
                    1,
                    1,
                    1,
                )
            )
        connection.executemany(
            "INSERT INTO symbol(usr, spelling, qual_name, kind, file_id, line, col, "
            "is_definition, resolved, is_static, is_instantiation) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            rows,
        )
        if first_id is None:
            first_id = connection.execute("SELECT last_insert_rowid()").fetchone()[0] - len(rows) + 1
    if first_id is None:
        raise ValueError("nodes must be positive")
    return first_id


def _insert_edges(
    connection: sqlite3.Connection,
    relations: int,
    nodes: int,
    first_symbol_id: int,
    distribution: str,
    chunk_size: int = 10_000,
) -> int:
    for start in range(0, relations, chunk_size):
        rows = []
        for index in range(start, min(relations, start + chunk_size)):
            source, target = _relation_pair(index, nodes, distribution)
            rows.append((first_symbol_id + source, first_symbol_id + target, 1, 1, None, None))
        connection.executemany(
            "INSERT OR IGNORE INTO edge(src_id, dst_id, kind, count, base_access, is_virtual) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            rows,
        )
    return connection.execute("SELECT COUNT(*) FROM edge").fetchone()[0]


def _insert_evidence(
    connection: sqlite3.Connection,
    edges: int,
    file_ids: list[int],
    multiplier: int,
    max_rows: int,
) -> int:
    target = min(edges * multiplier, max_rows)
    edge_ids = [row[0] for row in connection.execute("SELECT id FROM edge ORDER BY id LIMIT ?", (edges,))]
    rows = []
    for index in range(target):
        edge_id = edge_ids[index % len(edge_ids)]
        site = index // len(edge_ids)
        rows.append((edge_id, file_ids[index % len(file_ids)], site + 1, (index % 80) + 1, 0, None))
        if len(rows) >= 10_000:
            connection.executemany(
                "INSERT OR IGNORE INTO edge_site(edge_id, file_id, line, col, conditional, args_sig) "
                "VALUES (?, ?, ?, ?, ?, ?)", rows
            )
            rows.clear()
    if rows:
        connection.executemany(
            "INSERT OR IGNORE INTO edge_site(edge_id, file_id, line, col, conditional, args_sig) "
            "VALUES (?, ?, ?, ?, ?, ?)", rows
        )
    return connection.execute("SELECT COUNT(*) FROM edge_site").fetchone()[0]


def generate(
    manifest_path: Path,
    workload_id: str,
    scale_id: str,
    output: Path,
    *,
    seed: int | None = None,
    distribution: str | None = None,
    max_rows: int = 1_000_000,
    evidence_max_rows: int = 2_000_000,
) -> dict[str, Any]:
    manifest = load_json(manifest_path)
    if manifest.get("manifest_version") != "storage-m0/manifest-v1":
        raise ValueError("unsupported Storage M0 manifest version")
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("manifest is not for SQLite schema v34")
    workload = _workload(manifest, workload_id)
    scale = _scale(workload, scale_id)
    requested_nodes = int(scale["nodes"])
    requested_relations = int(scale["relations"])
    actual_nodes = min(requested_nodes, max_rows)
    actual_relations = min(requested_relations, max_rows)
    evidence_multiplier = int(scale.get("evidence_multiplier", 3))
    actual_distribution = distribution or str(workload.get("distribution", "balanced"))
    if actual_distribution not in {item["id"] for item in manifest.get("distributions", [])}:
        raise ValueError(f"unknown distribution {actual_distribution!r}")
    actual_seed = int(manifest.get("seed", 0) if seed is None else seed)
    if actual_nodes < 1 or actual_relations < 1:
        raise ValueError("nodes and relations must be positive")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()
    schema, kind_ids = load_cidx_schema()
    connection = sqlite3.connect(output)
    started_ns = time.perf_counter_ns()
    try:
        connection.executescript(schema)
        connection.executescript(
            "CREATE TABLE benchmark_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
            "INSERT INTO benchmark_meta(key, value) VALUES "
            "('state', 'building'), ('generation', '1');"
        )
        file_ids = _topology(connection, workload)
        first_symbol_id = _insert_symbols(connection, actual_nodes, file_ids, kind_ids)
        actual_edges = _insert_edges(
            connection,
            actual_relations,
            actual_nodes,
            first_symbol_id,
            actual_distribution,
        )
        actual_sites = _insert_evidence(
            connection, actual_edges, file_ids, evidence_multiplier, evidence_max_rows
        ) if actual_edges else 0
        connection.execute(
            "INSERT INTO meta(key, value) VALUES('benchmark_manifest_sha256', ?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            (manifest_digest(manifest),),
        )
        connection.execute(
            "INSERT INTO meta(key, value) VALUES('benchmark_seed', ?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            (str(actual_seed),),
        )
        connection.execute(
            "INSERT INTO benchmark_meta(key, value) VALUES('distribution', ?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value", (actual_distribution,)
        )
        connection.execute(
            "INSERT INTO benchmark_meta(key, value) VALUES('generation_duration_ms', ?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            (str(round((time.perf_counter_ns() - started_ns) / 1_000_000, 6)),),
        )
        connection.commit()
        digest = semantic_digest(output)
        connection.execute(
            "INSERT INTO benchmark_meta(key, value) VALUES('semantic_digest', ?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value", (digest,)
        )
        connection.execute("UPDATE benchmark_meta SET value='current' WHERE key='state'")
        connection.commit()
        counts = {
            "nodes": connection.execute("SELECT COUNT(*) FROM symbol").fetchone()[0],
            "relations": connection.execute("SELECT COUNT(*) FROM edge").fetchone()[0],
            "evidence": actual_sites,
            "files": connection.execute("SELECT COUNT(*) FROM file").fetchone()[0],
            "repositories": connection.execute("SELECT COUNT(*) FROM repository").fetchone()[0],
        }
    finally:
        connection.close()
    return {
        "benchmark": "storage-m0/v1",
        "schema_version": SCHEMA_VERSION,
        "manifest_sha256": manifest_digest(manifest),
        "workload": workload_id,
        "scale": scale_id,
        "distribution": actual_distribution,
        "seed": actual_seed,
        "requested": {"nodes": requested_nodes, "relations": requested_relations},
        "materialization_cap": {"rows": max_rows, "evidence_rows": evidence_max_rows},
        "counts": counts,
        "semantic_digest": digest,
        "output": str(output),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--workload", default="synthetic")
    parser.add_argument("--scale", default="smoke")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--distribution")
    parser.add_argument("--max-rows", type=int, default=1_000_000)
    parser.add_argument("--evidence-max-rows", type=int, default=2_000_000)
    args = parser.parse_args(argv)
    summary = generate(
        args.manifest, args.workload, args.scale, args.output,
        seed=args.seed, distribution=args.distribution,
        max_rows=args.max_rows, evidence_max_rows=args.evidence_max_rows,
    )
    print(canonical_json(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
