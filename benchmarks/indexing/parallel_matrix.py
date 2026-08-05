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

    corpus = work / "corpus"
    generate_corpus(corpus, args.count)

    arms: dict[str, dict] = {}
    for jobs in args.jobs:
        name = "auto" if jobs == 0 else f"jobs{jobs}"
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
        run_cidx(binary, ["resolve"], corpus, cache)
        arms[name] = {
            "jobs": jobs,
            "durations": durations,
            "median": statistics.median(durations),
            "headers": totals,
            "layer0": layer0(cache, corpus),
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
    for name, arm in arms.items():
        identical = arm["layer0"] == baseline["layer0"]
        headers_identical = arm["headers"] == baseline["headers"]
        ok = ok and identical and headers_identical
        report["parity"][name] = identical
        report["headers_match_serial"][name] = headers_identical
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
