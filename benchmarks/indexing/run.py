#!/usr/bin/env python3
"""Reproducible semantic-indexing scale benchmark for HSE-95."""

from __future__ import annotations

import argparse
import json
import os
import platform
from pathlib import Path
import resource
import re
import subprocess
import sys
import tempfile
import time
from typing import Any


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


def database_snapshot(database: Path) -> dict[str, Any]:
    import sqlite3

    with sqlite3.connect(database) as connection:
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
    return {
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
    label: str,
    args: list[str],
    previous_db: dict[str, Any] | None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    env = dict(os.environ)
    env["INDEXER_CACHE"] = str(cache)
    metrics = run_timed([str(cidx), *args], env, run_root, label)
    metrics["label"] = label
    database = cache / "index.db"
    snapshot = database_snapshot(database)
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
            cidx, cache, case_root, label, args, previous_db
        )
        stages.append(measured)

    changed = sources[0]
    changed.write_text(
        changed.read_text(encoding="utf-8").replace("+ 0", "+ 1"),
        encoding="utf-8",
    )
    measured, previous_db = stage(
        cidx,
        cache,
        case_root,
        "index-incremental",
        ["index", str(changed)],
        previous_db,
    )
    stages.append(measured)

    per_tu_results = []
    for index in range(min(per_tu, count)):
        source = sources[index]
        text = source.read_text(encoding="utf-8")
        source.write_text(text.replace(f"+ {index}", f"+ {index + 1}"), encoding="utf-8")
        measured, previous_db = stage(
            cidx,
            cache,
            case_root,
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
        }
    return result


def main() -> int:
    if "--measure-child" in sys.argv[1:]:
        return _measure_child()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-cidx", type=Path)
    parser.add_argument("--current-cidx", type=Path, required=True)
    parser.add_argument("--representative-files", type=int, default=32)
    parser.add_argument("--scale-files", type=int, default=1000)
    parser.add_argument("--per-tu", type=int, default=5)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

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
    }
    with tempfile.TemporaryDirectory(prefix="hse95-indexing-") as temporary:
        root = Path(temporary)
        for name, executable in executables.items():
            for count in dict.fromkeys(
                (args.representative_files, args.scale_files)
            ):
                case_root = root / name / str(count)
                case_root.mkdir(parents=True)
                report["cases"][f"{name}:{count}"] = run_case(
                    executable, count, args.per_tu, case_root
                )

    if args.baseline_cidx:
        report["comparison"] = {}
        for count in dict.fromkeys(
            (args.representative_files, args.scale_files)
        ):
            report["comparison"][str(count)] = comparison(
                report["cases"][f"baseline:{count}"],
                report["cases"][f"current:{count}"],
            )
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
