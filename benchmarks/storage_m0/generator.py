"""Generate deterministic SQLite v34 benchmark databases."""

from __future__ import annotations

import argparse
import json
import sqlite3
import time
from pathlib import Path
from typing import Any

from .common import SCHEMA_VERSION, canonical_json, load_cidx_schema, load_json, manifest_digest, semantic_digest


def _workload(manifest: dict[str, Any], workload_id: str) -> dict[str, Any]:
    for workload in manifest.get("workloads", []):
        if workload.get("id") == workload_id:
            return workload
    raise ValueError(f"unknown workload {workload_id!r}")


def _scale(workload: dict[str, Any], scale_id: str) -> dict[str, int]:
    value = workload.get("scales", {}).get(scale_id)
    if not isinstance(value, dict):
        raise ValueError(f"unknown scale {scale_id!r}")
    return {key: int(number) for key, number in value.items()}


def _topology(connection: sqlite3.Connection, workload: dict[str, Any]) -> dict[str, list[int]]:
    topology = workload.get("topology", {})
    repositories = int(topology.get("repositories", 1))
    components = int(topology.get("components_per_repository", 1))
    translation_units = int(topology.get("translation_units_per_component", 1))
    shared_headers = int(topology.get("shared_headers", 0))
    tus_per_header = int(topology.get("translation_units_per_shared_header", 1))
    files: list[int] = []
    translation_unit_ids: list[int] = []
    header_ids: list[int] = []
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
        connection.execute("UPDATE repository SET active_clone_id=? WHERE id=?", (clone_id, repository_id))
        for component in range(components):
            connection.execute(
                "INSERT INTO component(name, path, kind, repository_id) VALUES (?, ?, 'repo', ?)",
                (f"component-{repository:04d}-{component:04d}", f"component-{component:04d}", repository_id),
            )
            component_id = connection.execute("SELECT last_insert_rowid()").fetchone()[0]
            connection.execute("INSERT INTO directory(component_id, path) VALUES (?, '')", (component_id,))
            directory_id = connection.execute("SELECT last_insert_rowid()").fetchone()[0]
            for tu in range(translation_units):
                connection.execute(
                    "INSERT INTO file(directory_id, name, md5, compile_options, driver, indexed) VALUES (?, ?, ?, ?, ?, 1)",
                    (directory_id, f"tu-{tu:04d}.cpp", f"benchmark-tu-{repository}-{component}-{tu:04d}",
                     json.dumps(["-std=c++23"], separators=(",", ":")), "c++"),
                )
                file_id = connection.execute("SELECT last_insert_rowid()").fetchone()[0]
                files.append(file_id)
                translation_unit_ids.append(file_id)
            for header in range(shared_headers):
                connection.execute(
                    "INSERT INTO file(directory_id, name, md5, compile_options, driver, indexed) VALUES (?, ?, ?, ?, ?, 1)",
                    (directory_id, f"shared-{header:04d}.hpp", f"benchmark-header-{repository}-{component}-{header:04d}",
                     json.dumps(["-std=c++23"], separators=(",", ":")), "c++"),
                )
                header_ids.append(connection.execute("SELECT last_insert_rowid()").fetchone()[0])
                files.append(header_ids[-1])
    if not files:
        raise ValueError("topology must create at least one file")
    config_ids: list[int] = []
    for index, tu_id in enumerate(translation_unit_ids):
        connection.execute(
            "INSERT INTO include_config(tu_file_id, digest, driver, working_dir, arguments, lang_mode, resource_dir) "
            "VALUES (?, ?, 'c++', '/storage-m0', ?, 'c++', '/clang/resource')",
            (tu_id, f"config-{index:08d}", json.dumps(["-std=c++23"], separators=(",", ":"))),
        )
        config_ids.append(connection.execute("SELECT last_insert_rowid()").fetchone()[0])
    if header_ids:
        for index, (tu_id, config_id) in enumerate(zip(translation_unit_ids, config_ids)):
            headers_for_tu = header_ids[index % len(header_ids):] or header_ids
            for header_id in headers_for_tu[:max(1, min(tus_per_header, len(header_ids)))]:
                header_name = connection.execute("SELECT name FROM file WHERE id=?", (header_id,)).fetchone()[0]
                connection.execute(
                    "INSERT INTO include_edge(src_file_id, dst_file_id, dst_path, config_id, count) VALUES (?, ?, ?, ?, 1)",
                    (tu_id, header_id, header_name, config_id),
                )
                edge_id = connection.execute("SELECT last_insert_rowid()").fetchone()[0]
                connection.execute(
                    "INSERT INTO include_site(edge_id, line, col, begin_offset, end_offset, spelling, directive) "
                    "VALUES (?, ?, 1, ?, ?, ?, 1)",
                    (edge_id, index + 1, index * 10, index * 10 + len(header_name), header_name),
                )
    return {"files": files, "translation_units": translation_unit_ids, "headers": header_ids}


def _pair(index: int, nodes: int, distribution: str, seed: int) -> tuple[int, int]:
    """Return an injective ordered pair for every index in the valid domain."""
    if nodes < 2:
        raise ValueError("at least two nodes are required for a relation")
    if distribution in {"high-fan-in", "high-fan-out"}:
        fan = max(2, min(nodes - 1, (nodes // 32 or 2) + 1))
        ordinal, bucket = divmod(index, fan)
        if distribution == "high-fan-in":
            target = bucket
            source = ordinal % (nodes - 1)
            if source >= target:
                source += 1
            return source, target
        source = bucket
        target = ordinal % (nodes - 1)
        if target >= source:
            target += 1
        return source, target
    offset = (index // nodes) % (nodes - 1) + 1
    source = (index + seed) % nodes
    target = (source + offset) % nodes
    return source, target


def _insert_symbols(connection: sqlite3.Connection, nodes: int, file_ids: list[int], kind_ids: dict[str, int], seed: int, chunk_size: int = 10_000) -> int:
    kinds = ["class", "struct", "function", "variable", "typedef"]
    first_id = int(connection.execute("SELECT COALESCE(MAX(id), 0) + 1 FROM symbol").fetchone()[0])
    for start in range(0, nodes, chunk_size):
        rows = []
        for index in range(start, min(nodes, start + chunk_size)):
            kind = kinds[index % len(kinds)]
            rows.append((
                f"benchmark:seed:{seed}:node:{index:012d}", f"Node{index:012d}",
                f"benchmark::Node{index:012d}", kind_ids[kind], file_ids[index % len(file_ids)],
                (index % 100_000) + 1, 1, 1, 1, 1, 1,
            ))
        connection.executemany(
            "INSERT INTO symbol(usr, spelling, qual_name, kind, file_id, line, col, is_definition, resolved, is_static, is_instantiation) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", rows,
        )
    return first_id


def _insert_types_and_entities(connection: sqlite3.Connection, nodes: int, first_symbol_id: int, seed: int, chunk_size: int = 10_000) -> int:
    first_type_id = int(connection.execute("SELECT COALESCE(MAX(id), 0) + 1 FROM type_node").fetchone()[0])
    for start in range(0, nodes, chunk_size):
        rows = [
            (f"benchmark:seed:{seed}:type:{index:012d}", f"NodeType{index:012d}", 2, 0, 0, 0,
             f"benchmark:seed:{seed}:node:{index:012d}")
            for index in range(start, min(nodes, start + chunk_size))
        ]
        connection.executemany(
            "INSERT INTO type_node(type_key, spelling, kind, is_const, is_volatile, is_restrict, decl_usr) VALUES (?, ?, ?, ?, ?, ?, ?)", rows
        )
        connection.executemany(
            "INSERT INTO entity_node(id, kind) VALUES (?, 1)",
            [(first_symbol_id + index,) for index in range(start, min(nodes, start + chunk_size))],
        )
        connection.executemany(
            "INSERT INTO symbol_type(symbol_id, kind, type_id) VALUES (?, 2, ?)",
            [(first_symbol_id + index, first_type_id + index) for index in range(start, min(nodes, start + chunk_size))],
        )
    connection.execute("UPDATE type_node SET canonical_id=id")
    return first_type_id


def _insert_edges(connection: sqlite3.Connection, relations: int, nodes: int, first_symbol_id: int, distribution: str, seed: int, chunk_size: int = 10_000) -> int:
    first_edge_id = int(connection.execute("SELECT COALESCE(MAX(id), 0) + 1 FROM edge").fetchone()[0])
    for start in range(0, relations, chunk_size):
        rows = []
        for index in range(start, min(relations, start + chunk_size)):
            source, target = _pair(index, nodes, distribution, seed)
            rows.append((first_symbol_id + source, first_symbol_id + target, 1, 1, None, None))
        connection.executemany("INSERT INTO edge(src_id, dst_id, kind, count, base_access, is_virtual) VALUES (?, ?, ?, ?, ?, ?)", rows)
    return first_edge_id


def _insert_derived_facts(connection: sqlite3.Connection, nodes: int, relations: int, first_symbol_id: int, first_type_id: int, first_edge_id: int, file_ids: list[int], chunk_size: int = 10_000) -> None:
    first_definition_id = int(connection.execute("SELECT COALESCE(MAX(id), 0) + 1 FROM definition").fetchone()[0])
    for start in range(0, nodes, chunk_size):
        end = min(nodes, start + chunk_size)
        connection.executemany(
            "INSERT INTO definition(symbol_id, component_id, file_id, line, col, end_line, end_col, init_text) "
            "SELECT ?, c.id, ?, ?, 1, ?, 2, NULL FROM component c WHERE c.id=(SELECT component_id FROM directory WHERE id=(SELECT directory_id FROM file WHERE id=?))",
            [(first_symbol_id + index, file_ids[index % len(file_ids)], index + 1, index + 1, file_ids[index % len(file_ids)]) for index in range(start, end)],
        )
        connection.executemany(
            "INSERT INTO parameter(owner_id, position, name, type_id, declared_type_id, adjusted_type_id, file_id, line, col) VALUES (?, 0, ?, ?, ?, ?, ?, ?, 1)",
            [(first_symbol_id + index, f"arg{index}", first_type_id + index, first_type_id + index, first_type_id + index, file_ids[index % len(file_ids)], index + 1) for index in range(start, end) if index % 5 == 0],
        )
        connection.executemany(
            "INSERT INTO template_param(owner_id, position, param_kind, name, type_id) VALUES (?, 0, 1, ?, ?)",
            [(first_symbol_id + index, f"T{index}", first_type_id + index) for index in range(start, end) if index % 7 == 0],
        )
        connection.executemany(
            "INSERT INTO template_arg(owner_id, position, arg_kind, ref_id, type_id) VALUES (?, 0, 1, ?, ?)",
            [(first_symbol_id + index, first_symbol_id + index, first_type_id + index) for index in range(start, end) if index % 11 == 0],
        )
    for start in range(0, relations, chunk_size):
        end = min(relations, start + chunk_size)
        connection.executemany(
            "INSERT INTO entity_edge(src_id, dst_id, kind, count, via_member_id, multiplicity, access, is_virtual, partial) "
            "SELECT src_id, dst_id, 8, count, src_id, 1, 0, 0, 0 FROM edge WHERE id BETWEEN ? AND ?",
            [(first_edge_id + start, first_edge_id + end - 1)],
        )
        connection.executemany(
            "INSERT INTO type_edge(src_id, kind, position, dst_id) VALUES (?, 1, 0, ?)",
            [(first_type_id + index, first_type_id + ((index + 1) % nodes))
             for index in range(start, min(end, nodes))],
        )
        connection.execute(
            "INSERT INTO def_edge(src_def_id, dst_id, kind, count) "
            "SELECT ? + e.src_id - ?, e.dst_id, 1, e.count FROM edge e WHERE e.id BETWEEN ? AND ?",
            (first_definition_id, first_symbol_id, first_edge_id + start, first_edge_id + end - 1),
        )
        connection.execute(
            "INSERT INTO possible_call(src_def_id, dst_def_id, count) "
            "SELECT ? + e.src_id - ?, ? + e.dst_id - ?, e.count FROM edge e WHERE e.id BETWEEN ? AND ?",
            (first_definition_id, first_symbol_id, first_definition_id, first_symbol_id,
             first_edge_id + start, first_edge_id + end - 1),
        )
        connection.executemany(
            "INSERT INTO call_arg(edge_id, file_id, line, col, position, src_kind, type_usr, decl_usr, callee_usr, type_is_value) "
            "SELECT id, ?, ?, 1, 0, 'value', NULL, NULL, NULL, 1 FROM edge WHERE id BETWEEN ? AND ?",
            [(file_ids[start % len(file_ids)], start + 1, first_edge_id + start, first_edge_id + end - 1)],
        )


def _insert_evidence(connection: sqlite3.Connection, edges: int, file_ids: list[int], multiplier: int, max_rows: int, chunk_size: int = 10_000) -> int:
    target = min(edges * multiplier, max_rows)
    inserted = 0
    rows: list[tuple[Any, ...]] = []
    for edge_row in connection.execute("SELECT id FROM edge ORDER BY id"):
        for site in range(multiplier):
            if inserted >= target:
                break
            rows.append((edge_row[0], file_ids[inserted % len(file_ids)], site + 1, (inserted % 80) + 1, 0, None))
            inserted += 1
            if len(rows) >= chunk_size:
                connection.executemany("INSERT INTO edge_site(edge_id, file_id, line, col, conditional, args_sig) VALUES (?, ?, ?, ?, ?, ?)", rows)
                rows.clear()
        if inserted >= target:
            break
    if rows:
        connection.executemany("INSERT INTO edge_site(edge_id, file_id, line, col, conditional, args_sig) VALUES (?, ?, ?, ?, ?, ?)", rows)
    return inserted


def generate(manifest_path: Path, workload_id: str, scale_id: str, output: Path, *, seed: int | None = None, distribution: str | None = None, max_rows: int = 1_000_000, evidence_max_rows: int = 2_000_000) -> dict[str, Any]:
    manifest = load_json(manifest_path)
    if manifest.get("manifest_version") != "storage-m0/manifest-v1" or manifest.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("unsupported Storage M0 v34 manifest")
    workload = _workload(manifest, workload_id)
    scale = _scale(workload, scale_id)
    requested_nodes, requested_relations = int(scale["nodes"]), int(scale["relations"])
    actual_nodes, actual_relations = min(requested_nodes, max_rows), min(requested_relations, max_rows)
    if actual_nodes < 2 or actual_relations < 1 or actual_relations > actual_nodes * (actual_nodes - 1):
        raise ValueError("requested relation plan cannot be represented by distinct ordered pairs")
    actual_distribution = distribution or str(workload.get("distribution", "balanced"))
    distributions = {item["id"] for item in manifest.get("distributions", [])}
    if actual_distribution not in distributions:
        raise ValueError(f"unknown distribution {actual_distribution!r}")
    actual_seed = int(manifest.get("seed", 0) if seed is None else seed)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()
    schema, kind_ids = load_cidx_schema()
    started_ns = time.perf_counter_ns()
    connection = sqlite3.connect(output)
    try:
        connection.executescript(schema)
        connection.executescript("CREATE TABLE benchmark_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL); INSERT INTO benchmark_meta(key,value) VALUES ('state','building'),('generation','1');")
        topology = _topology(connection, workload)
        first_symbol_id = _insert_symbols(connection, actual_nodes, topology["files"], kind_ids, actual_seed)
        first_type_id = _insert_types_and_entities(connection, actual_nodes, first_symbol_id, actual_seed)
        first_edge_id = _insert_edges(connection, actual_relations, actual_nodes, first_symbol_id, actual_distribution, actual_seed)
        _insert_derived_facts(connection, actual_nodes, actual_relations, first_symbol_id, first_type_id, first_edge_id, topology["files"])
        actual_sites = _insert_evidence(connection, actual_relations, topology["files"], int(scale.get("evidence_multiplier", 3)), evidence_max_rows)
        manifest_hash = manifest_digest(manifest)
        for key, value in {
            "benchmark_manifest_sha256": manifest_hash, "benchmark_seed": str(actual_seed),
            "scale": scale_id, "distribution": actual_distribution,
            "requested_nodes": str(requested_nodes), "requested_relations": str(requested_relations),
            "actual_nodes": str(actual_nodes), "actual_relations": str(actual_relations),
            "row_cap": str(max_rows), "evidence_row_cap": str(evidence_max_rows),
            "configuration": "v34-default",
        }.items():
            connection.execute("INSERT INTO benchmark_meta(key,value) VALUES (?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key, value))
        connection.execute("INSERT INTO meta(key,value) VALUES('benchmark_manifest_sha256',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (manifest_hash,))
        connection.execute("INSERT INTO meta(key,value) VALUES('benchmark_seed',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (str(actual_seed),))
        connection.execute("INSERT INTO benchmark_meta(key,value) VALUES('generation_duration_ms',?)", (str(round((time.perf_counter_ns() - started_ns) / 1_000_000, 6)),))
        connection.commit()
        digest = semantic_digest(output)
        connection.execute("INSERT INTO benchmark_meta(key,value) VALUES('semantic_digest',?)", (digest,))
        connection.execute("UPDATE benchmark_meta SET value='current' WHERE key='state'")
        connection.commit()
        counts = {
            "nodes": connection.execute("SELECT COUNT(*) FROM symbol").fetchone()[0],
            "relations": connection.execute("SELECT COUNT(*) FROM edge").fetchone()[0],
            "evidence": actual_sites, "files": connection.execute("SELECT COUNT(*) FROM file").fetchone()[0],
            "translation_units": len(topology["translation_units"]), "shared_headers": len(topology["headers"]),
            "repositories": connection.execute("SELECT COUNT(*) FROM repository").fetchone()[0],
            "types": connection.execute("SELECT COUNT(*) FROM type_node").fetchone()[0],
            "entity_nodes": connection.execute("SELECT COUNT(*) FROM entity_node").fetchone()[0],
            "type_edges": connection.execute("SELECT COUNT(*) FROM type_edge").fetchone()[0],
            "entity_edges": connection.execute("SELECT COUNT(*) FROM entity_edge").fetchone()[0],
            "include_edges": connection.execute("SELECT COUNT(*) FROM include_edge").fetchone()[0],
        }
    finally:
        connection.close()
    return {
        "benchmark": "storage-m0/v1", "schema_version": SCHEMA_VERSION, "manifest_sha256": manifest_digest(manifest),
        "workload": workload_id, "scale": scale_id, "distribution": actual_distribution, "seed": actual_seed,
        "requested": {"nodes": requested_nodes, "relations": requested_relations},
        "materialization_cap": {"rows": max_rows, "evidence_rows": evidence_max_rows},
        "counts": counts, "semantic_digest": digest, "output": str(output),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True); parser.add_argument("--workload", default="synthetic")
    parser.add_argument("--scale", default="smoke"); parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int); parser.add_argument("--distribution")
    parser.add_argument("--max-rows", type=int, default=1_000_000); parser.add_argument("--evidence-max-rows", type=int, default=2_000_000)
    args = parser.parse_args(argv)
    print(canonical_json(generate(args.manifest, args.workload, args.scale, args.output, seed=args.seed, distribution=args.distribution, max_rows=args.max_rows, evidence_max_rows=args.evidence_max_rows)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
