#!/usr/bin/env python3
"""S-078 final indexing SLO contract and its decision arithmetic.

Everything here is pure: it reads measurement reports produced by
``benchmarks/indexing/production.py`` and ``benchmarks/indexing/integrated.py``
and returns verdicts. It never measures, indexes, builds a corpus, or opens a
database, so the whole contract is unit-testable offline with no binary and no
store.

The separation is deliberate. A performance contract that lives only in prose
gets weakened one sentence at a time; a contract that is executable code with
its own tests gets weakened only by editing a test that says so out loud.

Three rules the arithmetic enforces, because the acceptance criteria say they
must not be quietly softened:

* an absolute limit is a limit -- a measurement at or past it fails;
* a relative non-regression allowance is ``max(10 % of baseline, 50 ms)``, and
  anything larger needs an explicit recorded waiver naming benefit, cause and
  owner. A waiver is data in the report, not a code path that skips the check;
* the provisional ``>=4x`` cold goal may be *retained*, *superseded* or
  *rejected*, and each of those requires the quantitative attribution that
  justifies it. "Superseded" without a replacement contract, or with a
  replacement above the measured Amdahl ceiling, is rejected as malformed
  rather than accepted as a weaker target.
"""

from __future__ import annotations

import math
import statistics
from collections.abc import Iterable, Mapping, Sequence
from typing import Any

# --- the published contract -------------------------------------------------

#: Unchanged ("warm") indexing must complete below this many seconds.
WARM_ABSOLUTE_LIMIT_SECONDS = 5.0
#: A one-translation-unit incremental index must complete below this.
ONE_TU_ABSOLUTE_LIMIT_SECONDS = 2.0
#: The pre-existing synthetic 1,000-TU cold usability target (15 minutes).
SYNTHETIC_1000_COLD_LIMIT_SECONDS = 900.0
#: Relative part of the non-regression allowance.
REGRESSION_RELATIVE_TOLERANCE = 0.10
#: Absolute floor of the non-regression allowance, in seconds.
REGRESSION_ABSOLUTE_TOLERANCE_SECONDS = 0.050
#: The provisional cold self-indexing goal PERF-002 opened with.
PROVISIONAL_COLD_SPEEDUP_GOAL = 4.0
#: Fewest trials an authoritative aggregate may rest on.
MINIMUM_TRIALS = 3

#: Which workstream attacks the Clang front-end term, and how it was closed.
FRONT_END_WORKSTREAMS = (
    {
        "story": "S-074",
        "linear": "HSE-109",
        "mechanism": "bounded parallel translation-unit extraction",
        "attacks": "front-end wall time, by overlapping parses across workers",
    },
    {
        "story": "S-075",
        "linear": "HSE-110",
        "mechanism": "configuration-compatible PCH/preamble reuse",
        "attacks": "front-end CPU time, by reusing parsed preamble state",
    },
    {
        "story": "S-076",
        "linear": "HSE-111",
        "mechanism": "transitive-header invalidation and a content-addressed "
        "translation-unit fact cache",
        "attacks": "front-end calls, by avoiding the parse entirely on a hit",
    },
)


class SloContractError(ValueError):
    """A report does not carry the evidence a verdict needs."""


# --- small numeric helpers --------------------------------------------------


def _floats(values: Iterable[Any], where: str) -> list[float]:
    out: list[float] = []
    for index, value in enumerate(values):
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise SloContractError(f"{where}[{index}] is not numeric: {value!r}")
        if not math.isfinite(float(value)):
            raise SloContractError(f"{where}[{index}] is not finite: {value!r}")
        out.append(float(value))
    if not out:
        raise SloContractError(f"{where} is empty")
    return out


def _mapping(value: Any, where: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise SloContractError(f"{where} is not a mapping")
    return value


def _optional_number(value: Any) -> float | None:
    """A finite number, or None for anything that is not one."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    number = float(value)
    return number if math.isfinite(number) else None


def median(values: Iterable[Any], *, where: str = "series") -> float:
    """Median of a trial series, rejecting non-numeric or empty input."""
    return statistics.median(_floats(values, where))


def regression_allowance(baseline_seconds: float) -> float:
    """The greater of 10 % of the baseline and 50 ms, in seconds."""
    if baseline_seconds < 0:
        raise SloContractError("baseline seconds must not be negative")
    return max(
        REGRESSION_RELATIVE_TOLERANCE * float(baseline_seconds),
        REGRESSION_ABSOLUTE_TOLERANCE_SECONDS,
    )


# --- individual verdicts ----------------------------------------------------


def absolute_verdict(
    name: str, trials: Sequence[Any], limit_seconds: float
) -> dict[str, Any]:
    """Judge a trial series against an absolute ceiling.

    The limit is exclusive: a median exactly at the limit fails, so a
    borderline result cannot be reported as compliant.
    """
    series = _floats(trials, f"{name}.trials")
    value = statistics.median(series)
    return {
        "name": name,
        "kind": "absolute",
        "trials": series,
        "median_seconds": value,
        "limit_seconds": float(limit_seconds),
        "headroom_seconds": float(limit_seconds) - value,
        "trial_count": len(series),
        "ok": value < float(limit_seconds) and len(series) >= MINIMUM_TRIALS,
        "insufficient_trials": len(series) < MINIMUM_TRIALS,
    }


def regression_verdict(
    name: str,
    baseline_trials: Sequence[Any],
    candidate_trials: Sequence[Any],
    *,
    waiver: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Judge a candidate median against a recorded pre-feature baseline.

    Passing needs the three-trial median to stay within
    ``max(10 % of baseline, 50 ms)``. A larger regression passes only with a
    waiver that names a quantified benefit, a cause and an owner; a waiver
    missing any of those is reported as malformed and does not rescue the
    measurement.
    """
    baseline = _floats(baseline_trials, f"{name}.baseline_trials")
    candidate = _floats(candidate_trials, f"{name}.candidate_trials")
    baseline_median = statistics.median(baseline)
    candidate_median = statistics.median(candidate)
    delta = candidate_median - baseline_median
    allowance = regression_allowance(baseline_median)
    within = delta <= allowance
    enough_trials = (
        len(baseline) >= MINIMUM_TRIALS and len(candidate) >= MINIMUM_TRIALS
    )

    waiver_state = "absent"
    waiver_record: dict[str, Any] | None = None
    if waiver is not None:
        required = ("benefit", "cause", "owner")
        missing = [field for field in required if not str(waiver.get(field, "")).strip()]
        waiver_record = {field: waiver.get(field) for field in required}
        waiver_record["missing_fields"] = missing
        waiver_state = "malformed" if missing else "recorded"

    return {
        "name": name,
        "kind": "regression",
        "baseline_trials": baseline,
        "candidate_trials": candidate,
        "baseline_median_seconds": baseline_median,
        "candidate_median_seconds": candidate_median,
        "delta_seconds": delta,
        "relative_delta": (
            delta / baseline_median if baseline_median > 0 else math.inf
        ),
        "allowance_seconds": allowance,
        "within_allowance": within,
        "waiver_state": waiver_state,
        "waiver": waiver_record,
        "trial_count": {"baseline": len(baseline), "candidate": len(candidate)},
        "insufficient_trials": not enough_trials,
        "ok": enough_trials and (within or waiver_state == "recorded"),
    }


def speedup(baseline_trials: Sequence[Any], candidate_trials: Sequence[Any]) -> float:
    """Median-over-median speedup; infinite only if the candidate is zero."""
    baseline_median = statistics.median(_floats(baseline_trials, "baseline"))
    candidate_median = statistics.median(_floats(candidate_trials, "candidate"))
    if candidate_median <= 0:
        return math.inf
    return baseline_median / candidate_median


def amdahl_ceiling(front_end_share: float) -> float:
    """Best achievable speedup when the front-end fraction is irreducible.

    With a serial front-end fraction ``f`` that no amount of work on the rest
    of the pipeline can remove, total time cannot fall below ``f`` of the
    original, so the ceiling is ``1 / f``. A share of zero means the front end
    imposes no ceiling at all.
    """
    share = float(front_end_share)
    if not math.isfinite(share) or share < 0.0 or share > 1.0:
        raise SloContractError(f"front-end share must be in [0, 1]: {share!r}")
    if share == 0.0:
        return math.inf
    return 1.0 / share


def front_end_shares(
    stage: Mapping[str, Any], *, where: str = "stage"
) -> dict[str, Any]:
    """Measured Clang front-end share of one stage's wall and CPU time.

    ``clang_front_end`` excludes the registered visitor/persistence passes, so
    it is the parse-side term the Amdahl ceiling is defined against.
    ``clang_tool_inclusive`` retains the enclosing LibTooling wall time and is
    reported next to it as the wider bound.
    """
    stage = _mapping(stage, where)
    timings = _mapping(
        _mapping(stage.get("profile_summary"), f"{where}.profile_summary").get(
            "timings"
        ),
        f"{where}.profile_summary.timings",
    )

    def _timing(name: str) -> dict[str, Any]:
        entry = timings.get(name)
        if entry is None:
            raise SloContractError(f"{where} has no {name!r} timing")
        trials = _floats(
            _mapping(entry, f"{where}.timings.{name}").get("trials", []),
            f"{where}.timings.{name}.trials",
        )
        return {"trials": trials, "median": statistics.median(trials)}

    wall = _floats(stage.get("wall_seconds_trials", []), f"{where}.wall_seconds_trials")
    cpu = _floats(stage.get("cpu_seconds_trials", []), f"{where}.cpu_seconds_trials")
    exclusive = _timing("clang_front_end")
    inclusive = _timing("clang_tool_inclusive")

    wall_median = statistics.median(wall)
    cpu_median = statistics.median(cpu)
    if wall_median <= 0 or cpu_median <= 0:
        raise SloContractError(f"{where} has a non-positive wall or CPU median")

    return {
        "wall_seconds_median": wall_median,
        "cpu_seconds_median": cpu_median,
        "clang_front_end_seconds_median": exclusive["median"],
        "clang_tool_inclusive_seconds_median": inclusive["median"],
        "clang_front_end_trials": exclusive["trials"],
        "clang_tool_inclusive_trials": inclusive["trials"],
        "front_end_share_of_wall": exclusive["median"] / wall_median,
        "front_end_share_of_cpu": exclusive["median"] / cpu_median,
        "tool_inclusive_share_of_wall": inclusive["median"] / wall_median,
        "tool_inclusive_share_of_cpu": inclusive["median"] / cpu_median,
    }


def cold_goal_decision(
    *,
    measured_speedup: float,
    front_end_share: float,
    goal: float = PROVISIONAL_COLD_SPEEDUP_GOAL,
    disposition: str,
    replacement: Mapping[str, Any] | None = None,
    attribution: Sequence[str] = (),
) -> dict[str, Any]:
    """Decide what happens to the provisional ``>=4x`` cold goal.

    ``disposition`` is the recorded judgement -- ``retained``, ``superseded``
    or ``rejected`` -- and this function decides whether the evidence actually
    supports it:

    * ``retained`` is always structurally valid. It reports ``met`` from the
      measurement; a retained goal that was not met is a failure of the report,
      not a reason to move the goal;
    * ``superseded`` needs a replacement contract with a ``target`` speedup and
      a ``rationale``, and that target must be at or below the measured Amdahl
      ceiling and must itself be met. Superseding a goal with an unreachable or
      unmet target is malformed;
    * ``rejected`` needs the ceiling to be strictly below the goal -- that is,
      the arithmetic has to show the goal was never reachable. Rejecting a
      reachable goal is malformed.

    Every disposition needs at least one attribution string, because the
    acceptance criterion forbids changing this number without one.
    """
    if disposition not in {"retained", "superseded", "rejected"}:
        raise SloContractError(f"unknown cold-goal disposition: {disposition!r}")
    ceiling = amdahl_ceiling(front_end_share)
    measured = float(measured_speedup)
    goal_value = float(goal)
    met = measured >= goal_value
    reasons: list[str] = []
    if not list(attribution):
        reasons.append("no quantitative attribution recorded")

    replacement_record: dict[str, Any] | None = None
    replacement_met: bool | None = None
    if disposition == "superseded":
        if replacement is None:
            reasons.append("superseded without a replacement contract")
        else:
            target = _optional_number(replacement.get("target"))
            rationale = str(replacement.get("rationale", "")).strip()
            if target is None:
                reasons.append("replacement contract has no numeric target")
            if not rationale:
                reasons.append("replacement contract has no rationale")
            if target is not None:
                replacement_met = measured >= target
                if target > ceiling:
                    reasons.append(
                        "replacement target exceeds the measured Amdahl ceiling"
                    )
                if not replacement_met:
                    reasons.append("replacement target is not met by the measurement")
            replacement_record = {
                "target": target,
                "rationale": replacement.get("rationale"),
                "met": replacement_met,
            }
    elif replacement is not None:
        reasons.append(
            f"a replacement contract is meaningless for a {disposition} goal"
        )

    if disposition == "rejected" and ceiling >= goal_value:
        reasons.append(
            "goal rejected although the measured ceiling still admits it"
        )

    structurally_valid = not reasons
    if disposition == "retained":
        ok = structurally_valid and met
    elif disposition == "superseded":
        ok = structurally_valid and bool(replacement_met)
    else:
        ok = structurally_valid

    return {
        "goal": goal_value,
        "disposition": disposition,
        "measured_speedup": measured,
        "goal_met": met,
        "front_end_share": float(front_end_share),
        "amdahl_ceiling": ceiling,
        "goal_within_ceiling": ceiling >= goal_value,
        "replacement": replacement_record,
        "attribution": list(attribution),
        "front_end_workstreams": [dict(entry) for entry in FRONT_END_WORKSTREAMS],
        "malformed_reasons": reasons,
        "ok": ok,
    }


def equivalence_verdict(
    axis: str, arms: Sequence[Mapping[str, Any]], *, reference: str | None = None
) -> dict[str, Any]:
    """Judge one declared-equivalent axis of the integrated matrix.

    Two arms are canonically equivalent when the normalized semantic
    projection, the normalized Layer-0 dump and every non-volatile table row
    count agree, and both databases are sound. Timing is deliberately not part
    of it: the axes exist to prove that a faster mode publishes the same facts.
    """
    if len(arms) < 2:
        raise SloContractError(f"axis {axis!r} needs at least two arms")
    records = [_mapping(arm, f"{axis}.arm") for arm in arms]
    names = [str(arm.get("arm", index)) for index, arm in enumerate(records)]
    if len(set(names)) != len(names):
        raise SloContractError(f"axis {axis!r} has duplicate arm names")
    base_name = reference if reference is not None else names[0]
    if base_name not in names:
        raise SloContractError(f"axis {axis!r} has no {base_name!r} arm")
    base = records[names.index(base_name)]

    differences: list[str] = []
    for arm in records:
        name = str(arm.get("arm"))
        if name == base_name:
            continue
        for field in ("canonical_sha256", "normalized_layer0_sha256"):
            if arm.get(field) != base.get(field):
                differences.append(f"{name}: {field} differs from {base_name}")
        arm_counts = _mapping(arm.get("table_counts"), f"{axis}.{name}.table_counts")
        base_counts = _mapping(
            base.get("table_counts"), f"{axis}.{base_name}.table_counts"
        )
        for table in sorted(set(arm_counts) | set(base_counts)):
            if arm_counts.get(table) != base_counts.get(table):
                differences.append(
                    f"{name}: table {table} has {arm_counts.get(table)!r}, "
                    f"{base_name} has {base_counts.get(table)!r}"
                )
    unsound = [
        str(arm.get("arm"))
        for arm in records
        if arm.get("integrity_check") != "ok" or arm.get("foreign_key_check") != "ok"
    ]
    differences.extend(f"{name}: database is not sound" for name in unsound)

    return {
        "axis": axis,
        "reference_arm": base_name,
        "arms": names,
        "canonical_sha256": base.get("canonical_sha256"),
        "differences": differences,
        "ok": not differences,
    }


def residual_terms(
    *,
    root_traversals: Mapping[str, Any],
    scaling: Mapping[str, Any],
    publication: Mapping[str, Any],
) -> dict[str, Any]:
    """The named residual-cost list, each term with an owner and a threshold.

    A residual term is only closed when it has an owner, a numeric regression
    threshold and a measurement against it. A term with a measurement past its
    threshold stays open and fails the list; a term with no measurement at all
    is reported as unmeasured, never as passing.
    """
    declared = (
        {
            "term": "per-translation-unit rooted AST traversals",
            "shape": "fixed multiplier per TU",
            "owner": "T-139 (symbol root) / S-078 (aggregate)",
            "threshold_name": "fixed routed-root median seconds",
            "threshold": 0.025,
            "measured": root_traversals.get("fixed_root_median"),
            "context": {
                "registered_root_traversal_budget": root_traversals.get(
                    "registered_root_traversal_budget"
                ),
                "observed_root_traversals": root_traversals.get(
                    "observed_root_traversals"
                ),
                "share_of_cold_wall": root_traversals.get("share_of_cold_wall"),
                "pre_fusion_baseline_seconds": root_traversals.get(
                    "pre_fusion_baseline_seconds"
                ),
                "corpus": root_traversals.get("corpus"),
            },
            "note": (
                "S-098 shipped the two-root fusion (PR #86); two rooted "
                "whole-TU walks remain per translation unit and are budgeted, "
                "observed and published rather than left implicit."
            ),
        },
        {
            "term": "superlinear per-translation-unit cost against corpus size",
            "shape": "corpus-growth-sensitive",
            "owner": "S-068 (HSE-103) characterisation, S-078 publication",
            "threshold_name": "implied growth exponent",
            "threshold": scaling.get("threshold"),
            "measured": scaling.get("implied_exponent"),
            "context": {
                "small_corpus": scaling.get("small_corpus"),
                "large_corpus": scaling.get("large_corpus"),
                "per_tu_small_seconds": scaling.get("per_tu_small_seconds"),
                "per_tu_large_seconds": scaling.get("per_tu_large_seconds"),
                "model": scaling.get("model"),
            },
            "note": scaling.get("note"),
        },
        {
            "term": "serial controlled-writer publication",
            "shape": "fixed multiplier, not parallelised",
            "owner": "S-073 (writer) / S-074 (scheduler boundary)",
            "threshold_name": "publication share of cold wall time",
            "threshold": publication.get("threshold"),
            "measured": publication.get("share_of_cold_wall"),
            "context": {
                "writer_seconds_median": publication.get("writer_seconds_median"),
                "cold_wall_seconds_median": publication.get(
                    "cold_wall_seconds_median"
                ),
                "corpus": publication.get("corpus"),
            },
            "note": (
                "One controlled writer publishes every translation unit's "
                "facts; parallel extraction shrinks only the parse side, so "
                "this term bounds the achievable parallel speedup."
            ),
        },
    )

    terms: list[dict[str, Any]] = []
    failures: list[str] = []
    for entry in declared:
        record = dict(entry)
        threshold = _optional_number(record.get("threshold"))
        measured = _optional_number(record.get("measured"))
        if threshold is None or measured is None:
            record["status"] = "unmeasured"
            failures.append(f"{record['term']}: no measurement against its threshold")
        elif measured <= threshold:
            record["status"] = "within-threshold"
        else:
            record["status"] = "over-threshold"
            failures.append(
                f"{record['term']}: measured {measured} exceeds "
                f"{record['threshold_name']} {threshold}"
            )
        terms.append(record)

    return {"terms": terms, "failures": failures, "ok": not failures}


def slo_decision(
    *,
    identity: Mapping[str, Any],
    absolutes: Sequence[Mapping[str, Any]],
    regressions: Sequence[Mapping[str, Any]],
    cold_goal: Mapping[str, Any],
    equivalences: Sequence[Mapping[str, Any]],
    residuals: Mapping[str, Any],
    integrity: Mapping[str, Any],
    regression_guard: Mapping[str, Any],
) -> dict[str, Any]:
    """Assemble every verdict into the single publishable SLO decision.

    Nothing here is advisory. Any failing component fails the whole decision,
    and the failure list names which one, so a report cannot present a green
    headline over a red component.
    """
    failures: list[str] = []
    for verdict in absolutes:
        if not verdict.get("ok"):
            failures.append(f"absolute: {verdict.get('name')}")
    for verdict in regressions:
        if not verdict.get("ok"):
            failures.append(f"regression: {verdict.get('name')}")
    if not cold_goal.get("ok"):
        failures.append(f"cold-goal: {cold_goal.get('disposition')}")
    for verdict in equivalences:
        if not verdict.get("ok"):
            failures.append(f"equivalence: {verdict.get('axis')}")
    if not residuals.get("ok"):
        failures.extend(f"residual: {reason}" for reason in residuals.get("failures", []))
    if not integrity.get("ok"):
        failures.extend(
            f"integrity: {reason}" for reason in integrity.get("failures", [])
        )
    if not regression_guard.get("ok"):
        failures.extend(
            f"regression-guard: {reason}"
            for reason in regression_guard.get("failures", [])
        )

    return {
        "contract": {
            "warm_absolute_limit_seconds": WARM_ABSOLUTE_LIMIT_SECONDS,
            "one_tu_absolute_limit_seconds": ONE_TU_ABSOLUTE_LIMIT_SECONDS,
            "synthetic_1000_cold_limit_seconds": SYNTHETIC_1000_COLD_LIMIT_SECONDS,
            "regression_relative_tolerance": REGRESSION_RELATIVE_TOLERANCE,
            "regression_absolute_tolerance_seconds": (
                REGRESSION_ABSOLUTE_TOLERANCE_SECONDS
            ),
            "provisional_cold_speedup_goal": PROVISIONAL_COLD_SPEEDUP_GOAL,
            "minimum_trials": MINIMUM_TRIALS,
        },
        "identity": dict(identity),
        "absolutes": [dict(verdict) for verdict in absolutes],
        "regressions": [dict(verdict) for verdict in regressions],
        "cold_goal": dict(cold_goal),
        "equivalences": [dict(verdict) for verdict in equivalences],
        "residuals": dict(residuals),
        "integrity": dict(integrity),
        "regression_guard": dict(regression_guard),
        "failures": failures,
        "ok": not failures,
    }
