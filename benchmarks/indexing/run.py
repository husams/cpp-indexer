#!/usr/bin/env python3
"""Reproducible semantic-indexing scale benchmark for HSE-95."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import resource
import re
import subprocess
import statistics
import sys
import tempfile
import time
from typing import Any


EXPECTED_SCHEMA_VERSION = 39
EXPECTED_CATALOG_VERSION = 1
EXPECTED_CATALOG_HASH = "1adb5f6663a2e48dc3a624c79703ceaa5287f2784731a00bbc469dba8d5935d4"


def _measure_child() -> int:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--measure-child", action="store_true")
    parser.add_argument("--stdout", required=True)
    parser.add_argument("--stderr", required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command or args.command[0] != "--":
        raise SystemExit("measurement command must follow --")

    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.monotonic()
    with open(args.stdout, "w", encoding="utf-8") as stdout, open(
        args.stderr, "w", encoding="utf-8"
    ) as stderr:
        completed = subprocess.run(
            args.command[1:], stdout=stdout, stderr=stderr, check=False
        )
    elapsed = time.monotonic() - started
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    # macOS reports bytes; Linux reports KiB.
    rss = int(after.ru_maxrss)
    if sys.platform != "darwin":
        rss *= 1024
    print(
        json.dumps(
            {
                "returncode": completed.returncode,
                "wall_seconds": elapsed,
                "cpu_seconds": (after.ru_utime - before.ru_utime)
                + (after.ru_stime - before.ru_stime),
                "user_seconds": after.ru_utime - before.ru_utime,
                "system_seconds": after.ru_stime - before.ru_stime,
                "peak_rss_bytes": rss,
            }
        )
    )
    return completed.returncode


def run_timed(
    command: list[str], env: dict[str, str], run_root: Path, label: str
) -> dict[str, Any]:
    stdout_path = run_root / f"{label}.stdout"
    stderr_path = run_root / f"{label}.stderr"
    measurement = subprocess.run(
        [
            sys.executable,
            str(Path(__file__).resolve()),
            "--measure-child",
            "--stdout",
            str(stdout_path),
            "--stderr",
            str(stderr_path),
            "--",
            *command,
        ],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    metrics = json.loads(measurement.stdout)
    metrics["command"] = command
    metrics["stdout"] = stdout_path.read_text(encoding="utf-8")
    metrics["stderr"] = stderr_path.read_text(encoding="utf-8")
    if metrics["returncode"] != 0:
        raise RuntimeError(
            f"{label} failed ({metrics['returncode']}):\n"
            f"{metrics['stdout'][-2000:]}{metrics['stderr'][-2000:]}"
        )
    return metrics


def generate_corpus(root: Path, count: int) -> tuple[Path, list[Path], Path]:
    repeated_declarations = 16
    edge_targets = 8
    root.mkdir(parents=True)
    header = root / "shared.hpp"
    header.write_text(
        "#pragma once\nnamespace shared { inline int value() { return 7; } }\n",
        encoding="utf-8",
    )
    sources = []
    commands = []
    for index in range(count):
        source = root / f"unit_{index:04d}.cpp"
        source.write_text(
            (
                '#include "shared.hpp"\n'
                + "".join(
                f"int helper_{index}_{target}() {{ return {target}; }}\n"
                for target in range(edge_targets)
                )
                + f"int use_{index}() {{ return "
                + " + ".join(
                f"helper_{index}_{target}() + helper_{index}_{target}()"
                for target in range(edge_targets)
                )
                + "; }\n"
                + f"int unit_{index}();\n"
                + f"int unit_{index}() {{ return shared::value() + {index}; }}\n"
                + (f"int unit_{index}();\n" * repeated_declarations)
            ),
            encoding="utf-8",
        )
        sources.append(source)
        commands.append(
            {
                "directory": str(root),
                "file": str(source),
                "command": (
                    f"clang++ -std=c++23 -I{root} -c {source} "
                    f"-o {source.with_suffix('.o')}"
                ),
            }
        )
    compile_commands = root / "compile_commands.json"
    compile_commands.write_text(json.dumps(commands, indent=2), encoding="utf-8")
    return compile_commands, sources, header


def mutate_translation_unit(source: Path, old_value: int, new_value: int) -> None:
    text = source.read_text(encoding="utf-8")
    old = f"shared::value() + {old_value}"
    new = f"shared::value() + {new_value}"
    if text.count(old) != 1:
        raise RuntimeError(f"expected one mutation marker in {source}: {old!r}")
    source.write_text(text.replace(old, new), encoding="utf-8")


def _canonical_rows(
    connection: Any, corpus_root: Path
) -> dict[str, list[list[Any]]]:
    queries = {
        "file": """
            SELECT f.name, COALESCE(f.compile_options,''),
                   COALESCE(f.driver,''), f.indexed, COALESCE(f.md5,'')
            FROM file f ORDER BY f.name
        """,
        "symbol": """
            SELECT s.usr, s.spelling, COALESCE(s.qual_name,''),
                   COALESCE(s.display_name,''),
                   COALESCE(sk.name, CAST(s.kind AS TEXT)),
                   COALESCE(s.type_info,''), COALESCE(ff.name,''),
                   COALESCE(s.line,''), COALESCE(s.col,''),
                   COALESCE(df.name,''), COALESCE(s.decl_line,''),
                   COALESCE(s.decl_col,''), s.is_definition, s.is_pure,
                   s.is_static, s.is_instantiation, COALESCE(s.linkage,''),
                   COALESCE(s.access,''), COALESCE(s.parent_usr,''), s.resolved
            FROM symbol s
            LEFT JOIN symbol_kind sk ON sk.id = s.kind
            LEFT JOIN file ff ON ff.id = s.file_id
            LEFT JOIN file df ON df.id = s.decl_file_id
            ORDER BY s.usr
        """,
        "decl_site": """
            SELECT s.usr, COALESCE(f.name,''), COALESCE(d.line,''),
                   COALESCE(d.col,''), d.is_definition
            FROM decl_site d JOIN symbol s ON s.id = d.symbol_id
            LEFT JOIN file f ON f.id = d.file_id
            ORDER BY s.usr, f.name, d.line, d.col
        """,
        "edge": """
            SELECT ss.usr, ds.usr, COALESCE(ek.name, CAST(e.kind AS TEXT)),
                   e.count, COALESCE(e.base_access,''), COALESCE(e.is_virtual,'')
            FROM edge e JOIN symbol ss ON ss.id = e.src_id
            JOIN symbol ds ON ds.id = e.dst_id
            LEFT JOIN edge_kind ek ON ek.id = e.kind
            ORDER BY ss.usr, ds.usr, e.kind
        """,
        "edge_site": """
            SELECT ss.usr, ds.usr, COALESCE(ek.name, CAST(e.kind AS TEXT)),
                   COALESCE(f.name,''), COALESCE(es.line,''),
                   COALESCE(es.col,''), es.conditional, COALESCE(es.args_sig,''),
                   COALESCE(es.recv_src_kind,''), COALESCE(es.recv_type_usr,''),
                   COALESCE(es.recv_decl_usr,''), COALESCE(es.recv_param_pos,''),
                   COALESCE(es.recv_type_is_value,'')
            FROM edge_site es JOIN edge e ON e.id = es.edge_id
            JOIN symbol ss ON ss.id = e.src_id
            JOIN symbol ds ON ds.id = e.dst_id
            LEFT JOIN edge_kind ek ON ek.id = e.kind
            LEFT JOIN file f ON f.id = es.file_id
            ORDER BY ss.usr, ds.usr, e.kind, f.name, es.line, es.col
        """,
        "call_arg": """
            SELECT ss.usr, ds.usr, COALESCE(ek.name, CAST(e.kind AS TEXT)),
                   COALESCE(f.name,''), ca.line, ca.col, ca.position,
                   ca.src_kind, COALESCE(ca.type_usr,''),
                   COALESCE(ca.decl_usr,''), COALESCE(ca.callee_usr,''),
                   COALESCE(ca.type_is_value,'')
            FROM call_arg ca JOIN edge e ON e.id = ca.edge_id
            JOIN symbol ss ON ss.id = e.src_id
            JOIN symbol ds ON ds.id = e.dst_id
            LEFT JOIN edge_kind ek ON ek.id = e.kind
            LEFT JOIN file f ON f.id = ca.file_id
            ORDER BY ss.usr, ds.usr, e.kind, f.name, ca.line, ca.col,
                     ca.position
        """,
        "template_arg": """
            SELECT os.usr, ta.position, ta.arg_kind, COALESCE(rs.usr,''),
                   COALESCE(ta.literal,'')
            FROM template_arg ta JOIN symbol os ON os.id = ta.owner_id
            LEFT JOIN symbol rs ON rs.id = ta.ref_id
            ORDER BY os.usr, ta.position
        """,
    }

    root_text = str(corpus_root)

    def normalize(value: Any) -> Any:
        if isinstance(value, str):
            return value.replace(root_text, "<corpus>")
        return value

    return {
        name: [[normalize(value) for value in row]
               for row in connection.execute(query).fetchall()]
        for name, query in queries.items()
    }


def database_snapshot(database: Path, corpus_root: Path) -> dict[str, Any]:
    import sqlite3

    with sqlite3.connect(database) as connection:
        integrity = connection.execute("PRAGMA integrity_check").fetchone()[0]
        foreign_key_violations = connection.execute(
            "PRAGMA foreign_key_check"
        ).fetchall()
        metadata = dict(
            connection.execute(
                "SELECT key, value FROM meta WHERE key IN "
                "('schema_version', 'catalog_version', 'catalog_hash')"
            ).fetchall()
        )
        canonical = _canonical_rows(connection, corpus_root)
        canonical_json = json.dumps(
            canonical, sort_keys=True, separators=(",", ":")
        )
        page_count = connection.execute("PRAGMA page_count").fetchone()[0]
        page_size = connection.execute("PRAGMA page_size").fetchone()[0]
        freelist = connection.execute("PRAGMA freelist_count").fetchone()[0]
        tables = {}
        for table in ("file", "symbol", "edge", "edge_site", "call_arg"):
            try:
                tables[table] = connection.execute(
                    f"SELECT COUNT(*) FROM {table}"
                ).fetchone()[0]
            except sqlite3.OperationalError:
                tables[table] = None
    if integrity != "ok" or foreign_key_violations:
        raise RuntimeError(f"SQLite integrity check failed: {integrity}")
    if int(metadata.get("schema_version", -1)) != EXPECTED_SCHEMA_VERSION:
        raise RuntimeError(
            f"unexpected schema version: {metadata.get('schema_version')}"
        )
    if int(metadata.get("catalog_version", -1)) != EXPECTED_CATALOG_VERSION:
        raise RuntimeError(
            f"unexpected catalog version: {metadata.get('catalog_version')}"
        )
    if metadata.get("catalog_hash") != EXPECTED_CATALOG_HASH:
        raise RuntimeError(
            f"unexpected catalog hash: {metadata.get('catalog_hash')}"
        )
    return {
        "integrity_check": integrity,
        "foreign_key_check": "ok" if not foreign_key_violations else "failed",
        "schema_version": int(metadata["schema_version"]),
        "catalog_version": int(metadata["catalog_version"]),
        "catalog_hash": metadata["catalog_hash"],
        "canonical_sha256": hashlib.sha256(
            canonical_json.encode("utf-8")
        ).hexdigest(),
        "page_count": page_count,
        "page_size": page_size,
        "page_bytes": page_count * page_size,
        "freelist_pages": freelist,
        "rows": tables,
    }


def parse_header_counts(output: str) -> dict[str, int]:
    counts = {"indexed": 0, "already": 0, "system": 0, "unowned": 0}
    for line in output.splitlines():
        match = re.search(
            r"headers:\s+(\d+) indexed.*?,\s+(\d+) already,\s+"
            r"(\d+) system,\s+(\d+) unowned",
            line,
        )
        if match:
            counts["indexed"] += int(match.group(1))
            counts["already"] += int(match.group(2))
            counts["system"] += int(match.group(3))
            counts["unowned"] += int(match.group(4))
    return counts


def stage(
    cidx: Path,
    cache: Path,
    run_root: Path,
    corpus_root: Path,
    label: str,
    args: list[str],
    previous_db: dict[str, Any] | None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    env = dict(os.environ)
    env["INDEXER_CACHE"] = str(cache)
    metrics = run_timed([str(cidx), *args], env, run_root, label)
    metrics["label"] = label
    database = cache / "index.db"
    snapshot = database_snapshot(database, corpus_root)
    metrics["sqlite"] = {
        "snapshot": snapshot,
        "delta": snapshot if previous_db is None else {
            "page_bytes": snapshot["page_bytes"] - previous_db["page_bytes"],
            "rows": {
                key: snapshot["rows"][key] - previous_db["rows"][key]
                for key in snapshot["rows"]
                if snapshot["rows"][key] is not None
                and previous_db["rows"][key] is not None
            },
        },
    }
    metrics["header_counts"] = parse_header_counts(
        metrics["stdout"] + metrics["stderr"]
    )
    return metrics, snapshot


def run_case(cidx: Path, count: int, per_tu: int, case_root: Path) -> dict[str, Any]:
    source_root = case_root / "corpus"
    compile_commands, sources, header = generate_corpus(source_root, count)
    cache = case_root / "cache"
    cache.mkdir()
    stages = []
    previous_db = None
    for label, args in (
        ("import", ["import", "--db", str(compile_commands), "--name", "hse95"]),
        ("index-cold", ["index"]),
        ("resolve", ["resolve"]),
        ("index-warm", ["index"]),
    ):
        measured, previous_db = stage(
            cidx, cache, case_root, source_root, label, args, previous_db
        )
        stages.append(measured)

    changed = sources[0]
    mutate_translation_unit(changed, 0, 1)
    measured, previous_db = stage(
        cidx,
        cache,
        case_root,
        source_root,
        "index-incremental",
        ["index", str(changed)],
        previous_db,
    )
    stages.append(measured)

    per_tu_results = []
    for index in range(min(per_tu, count)):
        source = sources[index]
        mutate_translation_unit(source, index + 1 if index == 0 else index,
                                index + 2 if index == 0 else index + 1)
        measured, previous_db = stage(
            cidx,
            cache,
            case_root,
            source_root,
            f"tu-{index:04d}",
            ["index", str(source)],
            previous_db,
        )
        per_tu_results.append(
            {
                "file": source.name,
                "wall_seconds": measured["wall_seconds"],
                "cpu_seconds": measured["cpu_seconds"],
                "peak_rss_bytes": measured["peak_rss_bytes"],
            }
        )

    return {
        "files": count,
        "shared_header": header.name,
        "shared_header_fan_in": count,
        "stages": [
            {
                key: value
                for key, value in measured.items()
                if key not in ("stdout", "stderr", "command")
            }
            for measured in stages
        ],
        "per_tu": per_tu_results,
    }


def comparison(
    baseline: dict[str, Any], current: dict[str, Any]
) -> dict[str, Any]:
    result = {}
    for stage_name in ("index-cold", "index-warm", "index-incremental"):
        old = next(item for item in baseline["stages"] if item["label"] == stage_name)
        new = next(item for item in current["stages"] if item["label"] == stage_name)
        old_snapshot = old["sqlite"]["snapshot"]
        new_snapshot = new["sqlite"]["snapshot"]
        result[stage_name] = {
            "baseline_wall_seconds": old["wall_seconds"],
            "current_wall_seconds": new["wall_seconds"],
            "wall_delta_seconds": new["wall_seconds"] - old["wall_seconds"],
            "wall_improvement_percent": (
                (old["wall_seconds"] - new["wall_seconds"])
                / old["wall_seconds"]
                * 100
                if old["wall_seconds"]
                else None
            ),
            "cpu_utilization": {
                "baseline": old["cpu_seconds"] / old["wall_seconds"],
                "current": new["cpu_seconds"] / new["wall_seconds"],
            },
            "canonical_semantic_match": (
                old_snapshot["canonical_sha256"]
                == new_snapshot["canonical_sha256"]
            ),
            "database_integrity_match": (
                old_snapshot["integrity_check"] == "ok"
                and new_snapshot["integrity_check"] == "ok"
                and old_snapshot["foreign_key_check"] == "ok"
                and new_snapshot["foreign_key_check"] == "ok"
            ),
            "schema_catalog_match": (
                old_snapshot["schema_version"]
                == new_snapshot["schema_version"]
                == EXPECTED_SCHEMA_VERSION
                and old_snapshot["catalog_version"]
                == new_snapshot["catalog_version"]
                == EXPECTED_CATALOG_VERSION
                and old_snapshot["catalog_hash"]
                == new_snapshot["catalog_hash"]
                == EXPECTED_CATALOG_HASH
            ),
        }
    return result


def aggregate_cases(cases: list[dict[str, Any]]) -> dict[str, Any]:
    first = cases[0]

    def median(values: list[float | int]) -> float | int:
        return statistics.median(values)

    stages = []
    for stage_index, first_stage in enumerate(first["stages"]):
        stage_values = [case["stages"][stage_index] for case in cases]
        first_snapshot = first_stage["sqlite"]["snapshot"]
        stages.append(
            {
                "label": first_stage["label"],
                "returncode": 0,
                "wall_seconds": median(
                    [stage["wall_seconds"] for stage in stage_values]
                ),
                "wall_seconds_trials": [
                    stage["wall_seconds"] for stage in stage_values
                ],
                "cpu_seconds": median(
                    [stage["cpu_seconds"] for stage in stage_values]
                ),
                "cpu_seconds_trials": [
                    stage["cpu_seconds"] for stage in stage_values
                ],
                "user_seconds": median(
                    [stage["user_seconds"] for stage in stage_values]
                ),
                "system_seconds": median(
                    [stage["system_seconds"] for stage in stage_values]
                ),
                "peak_rss_bytes": median(
                    [stage["peak_rss_bytes"] for stage in stage_values]
                ),
                "cpu_utilization": median(
                    [stage["cpu_seconds"] / stage["wall_seconds"]
                     for stage in stage_values]
                ),
                "sqlite": {
                    "snapshot": first_snapshot,
                    "delta": {
                        "page_bytes": median(
                            [stage["sqlite"]["delta"]["page_bytes"]
                             for stage in stage_values]
                        ),
                        "rows": {
                            key: median(
                                [stage["sqlite"]["delta"]["rows"][key]
                                 for stage in stage_values]
                            )
                            for key in stage_values[0]["sqlite"]["delta"]["rows"]
                        },
                    },
                },
                "canonical_sha256_trials": [
                    stage["sqlite"]["snapshot"]["canonical_sha256"]
                    for stage in stage_values
                ],
                "header_counts": {
                    key: median(
                        [stage["header_counts"][key] for stage in stage_values]
                    )
                    for key in stage_values[0]["header_counts"]
                },
            }
        )

    return {
        "files": first["files"],
        "shared_header": first["shared_header"],
        "shared_header_fan_in": first["shared_header_fan_in"],
        "stages": stages,
        "per_tu": [
            {
                "file": first["per_tu"][tu_index]["file"],
                "wall_seconds": median(
                    [case["per_tu"][tu_index]["wall_seconds"]
                     for case in cases]
                ),
                "wall_seconds_trials": [
                    case["per_tu"][tu_index]["wall_seconds"]
                    for case in cases
                ],
                "cpu_seconds": median(
                    [case["per_tu"][tu_index]["cpu_seconds"]
                     for case in cases]
                ),
                "peak_rss_bytes": median(
                    [case["per_tu"][tu_index]["peak_rss_bytes"]
                     for case in cases]
                ),
            }
            for tu_index in range(len(first["per_tu"]))
        ],
    }


def main() -> int:
    if "--measure-child" in sys.argv[1:]:
        return _measure_child()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-cidx", type=Path)
    parser.add_argument("--current-cidx", type=Path, required=True)
    parser.add_argument("--representative-files", type=int, default=32)
    parser.add_argument("--scale-files", type=int, default=1000)
    parser.add_argument("--per-tu", type=int, default=5)
    parser.add_argument("--trials", type=int, default=1)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.trials < 1:
        raise SystemExit("--trials must be positive")

    executables = {"current": args.current_cidx}
    if args.baseline_cidx:
        executables["baseline"] = args.baseline_cidx
    report: dict[str, Any] = {
        "method": "HSE-95",
        "environment": {
            "platform": platform.platform(),
            "python": platform.python_version(),
        },
        "usability_target": {
            "scale_cold_wall_seconds_max": 900,
            "scale_warm_wall_seconds_max": 5,
            "incremental_wall_seconds_max": 2,
        },
        "cases": {},
        "aggregates": {},
    }
    with tempfile.TemporaryDirectory(prefix="hse95-indexing-") as temporary:
        root = Path(temporary)
        for trial in range(args.trials):
            for name, executable in executables.items():
                for count in dict.fromkeys(
                    (args.representative_files, args.scale_files)
                ):
                    case_root = root / f"trial-{trial + 1}" / name / str(count)
                    case_root.mkdir(parents=True)
                    report["cases"][f"trial-{trial + 1}:{name}:{count}"] = run_case(
                        executable, count, args.per_tu, case_root
                    )

    for name in executables:
        for count in dict.fromkeys(
            (args.representative_files, args.scale_files)
        ):
            report["aggregates"][f"{name}:{count}"] = aggregate_cases(
                [
                    report["cases"][f"trial-{trial + 1}:{name}:{count}"]
                    for trial in range(args.trials)
                ]
            )

    if args.baseline_cidx:
        report["comparison"] = {}
        for count in dict.fromkeys(
            (args.representative_files, args.scale_files)
        ):
            report["comparison"][str(count)] = comparison(
                report["aggregates"][f"baseline:{count}"],
                report["aggregates"][f"current:{count}"],
            )
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
