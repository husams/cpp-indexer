#!/usr/bin/env python3
"""Serial/parallel qualification matrix for bounded parallel extraction (S-074).

Runs the same corpus through `--jobs 1`, `--jobs 2`, `--jobs 4` and the
automatic policy, paired trial by trial, and reports:

  * canonical Layer-0 parity of every arm against `--jobs 1`;
  * aggregate owned-header amortisation counters per arm;
  * median wall time, speedup and efficiency against `--jobs 1`;
  * whether every paired automatic trial beat its `--jobs 1` partner.

Parity is the gate. A speedup number from a run whose facts differ from the
serial baseline is meaningless, so a parity failure is reported as a failure of
the whole matrix regardless of timing.

This extends the existing harness rather than competing with it: the corpus
generator is `run.generate_corpus` and the projection is
`scripts/dump_layer0.sh`, the same two the parity/repeatability gate uses.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run import generate_corpus  # noqa: E402

DUMP_LAYER0 = Path(__file__).resolve().parents[2] / "scripts/dump_layer0.sh"


def run_cidx(binary: Path, args: list[str], corpus: Path, cache: Path) -> tuple[str, float]:
    env = dict(os.environ)
    env["INDEXER_CACHE"] = str(cache)
    started = time.monotonic()
    done = subprocess.run(
        [str(binary), *args], cwd=str(corpus), env=env, capture_output=True, text=True
    )
    elapsed = time.monotonic() - started
    if done.returncode != 0:
        raise SystemExit(f"cidx {' '.join(args)} failed:\n{done.stdout}\n{done.stderr}")
    return done.stdout, elapsed


def jobs_args(jobs: int) -> list[str]:
    return [] if jobs == 0 else ["--jobs", str(jobs)]


def layer0(cache: Path, corpus: Path) -> str:
    text = subprocess.run(
        ["sh", str(DUMP_LAYER0), str(cache / "index.db")],
        capture_output=True, text=True, check=True,
    ).stdout
    return text.replace(str(corpus), "<CORPUS>").replace(str(cache), "<CACHE>")


# Tables whose contents legitimately differ between two runs of the same corpus
# in different directories, or between runs at different wall-clock times.
VOLATILE_TABLES = frozenset({"meta"})

# The optional translation-unit FactBatch cache. It is an accelerator, not a
# fact: a multi-worker run does not populate it, because the cache decision
# lives in the serial TuFactCacheIndexer wrapper that the scheduler bypasses.
# Reported separately so the difference is visible and never silently folded
# into a fact-parity claim.
CACHE_TABLES = frozenset({"artifact", "artifact_relation"})


def table_counts(cache: Path) -> dict[str, int]:
    """Row count for every user table -- including the ones dump_layer0.sh omits
    (definition, def_edge, type_node, type_edge, parameter, symbol_type,
    template_param, diagnostic, fact_applicability, file_config,
    translation_unit_config, semantic_universe, include_*)."""
    import sqlite3

    conn = sqlite3.connect(cache / "index.db")
    try:
        tables = [
            row[0]
            for row in conn.execute(
                "SELECT name FROM sqlite_schema WHERE type='table' "
                "AND name NOT LIKE 'sqlite_%' ORDER BY name"
            )
        ]
        return {
            table: conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
            for table in tables
            if table not in VOLATILE_TABLES and table not in CACHE_TABLES
        }
    finally:
        conn.close()


def cache_table_counts(cache: Path) -> dict[str, int]:
    import sqlite3

    conn = sqlite3.connect(cache / "index.db")
    try:
        return {
            table: conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
            for table in sorted(CACHE_TABLES)
        }
    finally:
        conn.close()


def integrity(cache: Path) -> dict[str, object]:
    import sqlite3

    conn = sqlite3.connect(cache / "index.db")
    try:
        return {
            "integrity_check": conn.execute("PRAGMA integrity_check").fetchone()[0],
            "foreign_key_violations": len(
                conn.execute("PRAGMA foreign_key_check").fetchall()
            ),
        }
    finally:
        conn.close()


def header_totals(stdout: str) -> dict[str, int]:
    payload = json.loads(stdout)
    result = payload.get("result", payload)
    totals = {"indexed": 0, "already": 0, "system": 0, "unowned": 0, "symbols": 0}
    for entry in result.get("files", []):
        if entry.get("status") != "indexed":
            continue
        totals["indexed"] += entry.get("headers_indexed", 0)
        totals["already"] += entry.get("headers_already", 0)
        totals["system"] += entry.get("headers_system", 0)
        totals["unowned"] += entry.get("headers_unowned", 0)
        totals["symbols"] += entry.get("headers_symbols", 0)
    return totals


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cidx", required=True)
    parser.add_argument("--count", type=int, default=200)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--jobs", nargs="+", type=int, default=[1, 2, 4, 0])
    parser.add_argument("--work", default="/tmp/s074-matrix")
    parser.add_argument("--output")
    args = parser.parse_args()

    binary = Path(args.cidx).resolve()
    work = Path(args.work)
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    arms: dict[str, dict] = {}
    for jobs in args.jobs:
        name = "auto" if jobs == 0 else f"jobs{jobs}"
        # One corpus per arm. The incremental probe below edits its sources, so
        # a shared corpus would leave later arms indexing different code and
        # turn a parity comparison into a comparison of two different programs.
        corpus = work / f"corpus-{name}"
        generate_corpus(corpus, args.count)
        cache = work / f"cache-{name}"
        cache.mkdir()
        run_cidx(binary, ["import", "--db", str(corpus / "compile_commands.json"),
                          "--name", "s074"], corpus, cache)
        durations = []
        totals = None
        for trial in range(args.trials):
            stdout, elapsed = run_cidx(
                binary, ["index", "rebuild", "--json", *jobs_args(jobs)], corpus, cache
            )
            durations.append(elapsed)
            trial_totals = header_totals(stdout)
            if totals is not None and trial_totals != totals:
                raise SystemExit(
                    f"{name}: header amortisation is not repeatable: "
                    f"{totals} then {trial_totals}"
                )
            totals = trial_totals
            print(f"  {name} trial {trial}: {elapsed:.2f}s headers={trial_totals}")
        # Parity is captured HERE, before the incremental probe below edits the
        # shared corpus: every arm must be compared against the same sources.
        run_cidx(binary, ["resolve"], corpus, cache)
        captured = {
            "layer0": layer0(cache, corpus),
            "table_counts": table_counts(cache),
            "integrity": integrity(cache),
        }
        # Incremental path: a no-op run over a current index, then a run after
        # one source changes. Both are the operator's warm SLO. The arms share
        # one corpus, so the edit has to be one each arm can make on its own.
        _, noop = run_cidx(binary, ["index", *jobs_args(jobs)], corpus, cache)
        changed = corpus / "unit_0007.cpp"
        changed.write_text(
            changed.read_text() + "int incremental_probe() { return 1; }\n",
            encoding="utf-8",
        )
        _, incremental = run_cidx(binary, ["index", *jobs_args(jobs)], corpus, cache)
        arms[name] = {
            "jobs": jobs,
            "durations": durations,
            "median": statistics.median(durations),
            "headers": totals,
            "tu_fact_cache_tables": cache_table_counts(cache),
            "noop_seconds": noop,
            "incremental_seconds": incremental,
            **captured,
        }

    baseline = arms["jobs1"]
    report: dict = {
        "corpus_translation_units": args.count,
        "trials": args.trials,
        "arms": {},
        "parity": {},
        "headers_match_serial": {},
    }
    ok = True
    report["table_counts_match_serial"] = {}
    report["integrity"] = {}
    for name, arm in arms.items():
        identical = arm["layer0"] == baseline["layer0"]
        headers_identical = arm["headers"] == baseline["headers"]
        counts_identical = arm["table_counts"] == baseline["table_counts"]
        sound = (
            arm["integrity"]["integrity_check"] == "ok"
            and arm["integrity"]["foreign_key_violations"] == 0
        )
        ok = ok and identical and headers_identical and counts_identical and sound
        report["parity"][name] = identical
        report["headers_match_serial"][name] = headers_identical
        report["table_counts_match_serial"][name] = counts_identical
        report["integrity"][name] = arm["integrity"]
        if not counts_identical:
            report.setdefault("table_count_deltas", {})[name] = {
                table: [baseline["table_counts"].get(table), value]
                for table, value in arm["table_counts"].items()
                if baseline["table_counts"].get(table) != value
            }
        paired_faster = all(
            arm["durations"][i] < baseline["durations"][i]
            for i in range(args.trials)
        )
        report["arms"][name] = {
            "jobs": arm["jobs"],
            "durations": arm["durations"],
            "median_seconds": arm["median"],
            "speedup_vs_serial": baseline["median"] / arm["median"],
            "all_paired_trials_faster": paired_faster,
            "headers": arm["headers"],
            "noop_seconds": arm["noop_seconds"],
            "incremental_one_file_seconds": arm["incremental_seconds"],
            "tu_fact_cache_tables": arm["tu_fact_cache_tables"],
        }
        if not identical:
            (work / f"layer0-diff-{name}.txt").write_text(
                "".join(
                    subprocess.run(
                        ["diff", "-u", "-", "-"], input="", capture_output=True, text=True
                    ).stdout
                ),
                encoding="utf-8",
            )
            (work / f"layer0-{name}.txt").write_text(arm["layer0"], encoding="utf-8")
            (work / "layer0-jobs1.txt").write_text(baseline["layer0"], encoding="utf-8")

    auto = report["arms"].get("auto")
    if auto is not None:
        report["material_speedup"] = {
            "bar": "median auto >= 1.20x median --jobs 1 and every paired trial faster",
            "speedup": auto["speedup_vs_serial"],
            "all_paired_trials_faster": auto["all_paired_trials_faster"],
            "met": auto["speedup_vs_serial"] >= 1.20 and auto["all_paired_trials_faster"],
        }
    report["parity_holds"] = ok
    text = json.dumps(report, indent=2, sort_keys=True)
    print(text)
    if args.output:
        Path(args.output).write_text(text + "\n", encoding="utf-8")
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
