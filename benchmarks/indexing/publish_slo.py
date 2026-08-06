#!/usr/bin/env python3
"""Derive the published S-078 indexing SLO from recorded measurement reports.

This is the wiring, not the contract. Every threshold, allowance and verdict
rule lives in `slo.py` and is tested offline; this module only reads the JSON
that `production.py` and `integrated.py` wrote and hands the right series to
the right rule. Keeping it separate is what lets the contract be re-run against
a different set of measurements -- a later scheduled production-scale run, a
different reference host -- without touching a threshold.

    python3 benchmarks/indexing/publish_slo.py \\
      --shipped /tmp/s078-A.json \\
      --serial /tmp/s078-B.json \\
      --fusion /tmp/s078-C.json \\
      --integrated /tmp/s078-D.json \\
      --scale-files 1000 --representative-files 32 \\
      --output /tmp/s078-slo.json

The process exits non-zero when the assembled decision is not `ok`, so a
report that fails its own contract cannot be published by accident.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

try:  # imported as `benchmarks.indexing.publish_slo`
    from . import slo as SLO
except ImportError:  # run directly as a script
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import slo as SLO  # type: ignore[no-redef]

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
#: The workflow that carries the affordable subset and the scheduled
#: production-scale run. Its presence is part of the decision (AC14), so a
#: report cannot claim a guarded regression surface that does not exist.
REGRESSION_WORKFLOW = Path(".github/workflows/indexing-performance.yml")
REQUIRED_WORKFLOW_JOBS = ("contract:", "equivalence:", "production-scale:")


def _stage(report: Mapping[str, Any], case: str, stage: str) -> Mapping[str, Any]:
    aggregates = report.get("aggregates")
    if not isinstance(aggregates, Mapping) or case not in aggregates:
        raise SLO.SloContractError(f"report has no {case!r} aggregate")
    stages = aggregates[case].get("stages")
    if not isinstance(stages, Mapping) or stage not in stages:
        raise SLO.SloContractError(f"{case!r} has no {stage!r} stage")
    return stages[stage]


def _walls(report: Mapping[str, Any], case: str, stage: str) -> list[float]:
    return [float(value) for value in _stage(report, case, stage)["wall_seconds_trials"]]


def _timing_median(stage: Mapping[str, Any], name: str) -> float | None:
    timings = (stage.get("profile_summary") or {}).get("timings") or {}
    entry = timings.get(name)
    if not isinstance(entry, Mapping):
        return None
    value = entry.get("median")
    return float(value) if isinstance(value, (int, float)) else None


def _counter_median(stage: Mapping[str, Any], name: str) -> float | None:
    counters = (stage.get("profile_summary") or {}).get("counters") or {}
    entry = counters.get(name)
    if not isinstance(entry, Mapping):
        return None
    value = entry.get("median")
    return float(value) if isinstance(value, (int, float)) else None


def root_traversal_evidence(
    report: Mapping[str, Any], *, case: str
) -> dict[str, Any]:
    """The residual rooted-traversal term and its share of the cold wall."""
    stage = _stage(report, case, "cold")
    components = [
        _timing_median(stage, name) or 0.0 for name in SLO_FIXED_ROOT_TIMINGS
    ]
    fixed = sum(components)
    wall = float(stage["wall_seconds"])
    return {
        "corpus": case,
        "fixed_root_median": fixed,
        "components": dict(zip(SLO_FIXED_ROOT_TIMINGS, components)),
        "share_of_cold_wall": fixed / wall if wall > 0 else None,
        "cold_wall_seconds_median": wall,
        "registered_root_traversal_budget": _counter_median(
            stage, "registered_root_traversal_budget"
        ),
        "observed_root_traversals": _counter_median(stage, "observed_root_traversals"),
        "pre_fusion_baseline_seconds": 0.040611,
    }


SLO_FIXED_ROOT_TIMINGS = (
    "root_symbols",
    "root_declarations",
    "root_definitions",
    "root_namespaces",
)

#: Per-translation-unit cost may grow with corpus size, but the growth exponent
#: has to stay bounded. 1.4 is the threshold S-074's measured 1.25 (serial) and
#: 1.35 (parallel) sit under; anything past it reopens the term.
SCALING_EXPONENT_THRESHOLD = 1.4
#: The serial controlled writer publishes every unit's facts. Past this share
#: of cold wall time, parallel extraction cannot buy anything more and the term
#: has to be reopened rather than absorbed.
PUBLICATION_SHARE_THRESHOLD = 0.75


def scaling_evidence(
    report: Mapping[str, Any], *, small_case: str, large_case: str,
    small_files: int, large_files: int,
) -> dict[str, Any]:
    """Implied per-translation-unit growth exponent between two corpus sizes."""
    import math

    small = _walls(report, small_case, "cold")
    large = _walls(report, large_case, "cold")
    per_small = SLO.median(small) / small_files
    per_large = SLO.median(large) / large_files
    ratio = per_large / per_small if per_small > 0 else math.inf
    size_ratio = large_files / small_files
    exponent = 1.0 + (
        math.log(ratio) / math.log(size_ratio) if ratio > 0 and size_ratio > 1 else 0.0
    )
    analysis = _stage(report, large_case, "cold").get("per_tu_analysis") or {}
    return {
        "threshold": SCALING_EXPONENT_THRESHOLD,
        "implied_exponent": exponent,
        "small_corpus": small_files,
        "large_corpus": large_files,
        "per_tu_small_seconds": per_small,
        "per_tu_large_seconds": per_large,
        "per_tu_growth": ratio,
        "model": analysis.get("preferred_model"),
        "note": analysis.get("status"),
    }


def publication_evidence(
    report: Mapping[str, Any], *, case: str
) -> dict[str, Any]:
    """Share of cold wall time spent inside the serial controlled writer."""
    stage = _stage(report, case, "cold")
    writer = sum(
        _timing_median(stage, name) or 0.0
        for name in (
            "fact_batch_writer.prepare",
            "fact_batch_writer.virtual_machine",
            "fact_batch_writer.commit",
        )
    )
    wall = float(stage["wall_seconds"])
    return {
        "threshold": PUBLICATION_SHARE_THRESHOLD,
        "corpus": case,
        "writer_seconds_median": writer,
        "cold_wall_seconds_median": wall,
        "share_of_cold_wall": writer / wall if wall > 0 else None,
    }


def integrity_evidence(reports: Mapping[str, Mapping[str, Any]]) -> dict[str, Any]:
    """Every parity and soundness failure any recorded run reported."""
    failures: list[str] = []
    for name, report in reports.items():
        for failure in report.get("parity_failures") or []:
            failures.append(f"{name}: {failure}")
        for failure in report.get("failures") or []:
            failures.append(f"{name}: {failure}")
        if report.get("authoritative_timing") is False:
            failures.append(f"{name}: measured on a non-quiescent host")
    return {"failures": failures, "ok": not failures}


def regression_guard_evidence(root: Path = REPOSITORY_ROOT) -> dict[str, Any]:
    """The regression surface itself: a workflow, with the jobs it promises."""
    path = root / REGRESSION_WORKFLOW
    failures: list[str] = []
    jobs: list[str] = []
    if not path.is_file():
        failures.append(f"{REGRESSION_WORKFLOW} does not exist")
    else:
        text = path.read_text(encoding="utf-8")
        for job in REQUIRED_WORKFLOW_JOBS:
            if f"  {job}" in text:
                jobs.append(job.rstrip(":"))
            else:
                failures.append(f"{REGRESSION_WORKFLOW} has no {job.rstrip(':')} job")
        if "schedule:" not in text:
            failures.append(f"{REGRESSION_WORKFLOW} has no scheduled run")
    return {
        "workflow": str(REGRESSION_WORKFLOW),
        "jobs": jobs,
        "failures": failures,
        "ok": not failures,
    }


def assemble(
    *,
    shipped: Mapping[str, Any],
    serial: Mapping[str, Any],
    fusion: Mapping[str, Any],
    integrated: Mapping[str, Any],
    representative_files: int,
    scale_files: int,
    cold_goal_disposition: str,
    cold_goal_replacement: Mapping[str, Any] | None,
    waivers: Mapping[str, Mapping[str, Any]] | None = None,
) -> dict[str, Any]:
    """Judge one complete set of recorded measurements against the contract."""
    waivers = dict(waivers or {})
    scale_case = f"baseline:{scale_files}:forward"
    header_case = f"header-heavy:{representative_files}:forward"
    small_case = f"baseline:{representative_files}:forward"

    cold_stage = _stage(shipped, scale_case, "cold")
    shares = SLO.front_end_shares(cold_stage, where=f"{scale_case}.cold")

    absolutes = [
        SLO.absolute_verdict(
            "unchanged-warm", _walls(shipped, scale_case, "unchanged-warm"),
            SLO.WARM_ABSOLUTE_LIMIT_SECONDS,
        ),
        SLO.absolute_verdict(
            "one-source-incremental", _walls(shipped, scale_case, "one-source"),
            SLO.ONE_TU_ABSOLUTE_LIMIT_SECONDS,
        ),
        SLO.absolute_verdict(
            f"synthetic-{scale_files}-cold", _walls(shipped, scale_case, "cold"),
            SLO.SYNTHETIC_1000_COLD_LIMIT_SECONDS,
        ),
    ]

    pre_feature = integrated.get("pre_feature_ab") or {}
    timing = pre_feature.get("timing") or {}
    regressions: list[dict[str, Any]] = []
    for name, stage in (
        ("unchanged-warm", "unchanged-warm"),
        ("one-source-incremental", "one-source"),
        ("high-fan-in-header", "high-fan-in-header"),
    ):
        entry = timing.get(stage)
        if not isinstance(entry, Mapping):
            raise SLO.SloContractError(
                f"pre-feature A/B has no {stage!r} stage to compare"
            )
        regressions.append(
            SLO.regression_verdict(
                name,
                entry["baseline_wall_seconds_trials"],
                entry["candidate_wall_seconds_trials"],
                waiver=waivers.get(name),
            )
        )

    cold_entry = timing.get("cold")
    if not isinstance(cold_entry, Mapping):
        raise SLO.SloContractError("pre-feature A/B has no cold stage to compare")
    measured_speedup = SLO.speedup(
        cold_entry["baseline_wall_seconds_trials"],
        cold_entry["candidate_wall_seconds_trials"],
    )
    cold_goal = SLO.cold_goal_decision(
        measured_speedup=measured_speedup,
        front_end_share=shares["front_end_share_of_wall"],
        disposition=cold_goal_disposition,
        replacement=cold_goal_replacement,
        attribution=[
            f"cold speedup {measured_speedup:.3f}x against the pre-feature "
            f"executable on {cold_entry.get('baseline_wall_seconds_median')!r}s "
            f"baseline / {cold_entry.get('candidate_wall_seconds_median')!r}s "
            "candidate medians",
            f"measured Clang front-end share of cold wall time "
            f"{shares['front_end_share_of_wall']:.4f} "
            f"({shares['clang_front_end_seconds_median']:.4f}s of "
            f"{shares['wall_seconds_median']:.4f}s)",
            f"measured Clang front-end share of cold CPU time "
            f"{shares['front_end_share_of_cpu']:.4f}",
        ],
    )

    residuals = SLO.residual_terms(
        root_traversals=root_traversal_evidence(serial, case=header_case),
        scaling=scaling_evidence(
            shipped, small_case=small_case, large_case=scale_case,
            small_files=representative_files, large_files=scale_files,
        ),
        publication=publication_evidence(shipped, case=scale_case),
    )

    equivalences = list((integrated.get("equivalence") or {}).get("verdicts") or [])
    bounds = (integrated.get("bounds") or {}).get("verdict")
    if isinstance(bounds, Mapping):
        equivalences.append(dict(bounds))
    if not equivalences:
        raise SLO.SloContractError("the integrated report carries no equivalence axis")

    decision = SLO.slo_decision(
        identity={
            "shipped_report": shipped.get("identity"),
            "shipped_topology": shipped.get("index_topology"),
            "serial_topology": serial.get("index_topology"),
            "integrated_capabilities": integrated.get("capabilities"),
            "s098_root_fusion": fusion.get("s098_root_fusion"),
            "representative_files": representative_files,
            "scale_files": scale_files,
        },
        absolutes=absolutes,
        regressions=regressions,
        cold_goal=cold_goal,
        equivalences=equivalences,
        residuals=residuals,
        integrity=integrity_evidence(
            {"shipped": shipped, "serial": serial, "fusion": fusion,
             "integrated": integrated}
        ),
        regression_guard=regression_guard_evidence(),
    )
    decision["front_end"] = shares
    decision["pre_feature_semantic_delta"] = pre_feature.get("semantic_delta")
    decision["pre_feature_timing"] = timing
    return decision


def _load(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shipped", type=Path, required=True)
    parser.add_argument("--serial", type=Path, required=True)
    parser.add_argument("--fusion", type=Path, required=True)
    parser.add_argument("--integrated", type=Path, required=True)
    parser.add_argument("--representative-files", type=int, default=32)
    parser.add_argument("--scale-files", type=int, default=1000)
    parser.add_argument(
        "--cold-goal-disposition",
        choices=("retained", "superseded", "rejected"),
        required=True,
    )
    parser.add_argument("--cold-goal-replacement-target", type=float)
    parser.add_argument("--cold-goal-replacement-rationale")
    parser.add_argument(
        "--waivers", type=Path,
        help="JSON mapping a regression name to {benefit, cause, owner}",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)

    replacement = None
    if args.cold_goal_replacement_target is not None:
        replacement = {
            "target": args.cold_goal_replacement_target,
            "rationale": args.cold_goal_replacement_rationale or "",
        }

    decision = assemble(
        shipped=_load(args.shipped),
        serial=_load(args.serial),
        fusion=_load(args.fusion),
        integrated=_load(args.integrated),
        representative_files=args.representative_files,
        scale_files=args.scale_files,
        cold_goal_disposition=args.cold_goal_disposition,
        cold_goal_replacement=replacement,
        waivers=_load(args.waivers) if args.waivers else None,
    )
    args.output.write_text(json.dumps(decision, indent=2) + "\n", encoding="utf-8")
    print(args.output)
    return 0 if decision["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
