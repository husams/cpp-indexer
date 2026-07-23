"""Evaluate Storage M0 results against explicit SLO and decision gates."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from .common import canonical_json, load_json, require_result_version


def _check(name: str, status: str, *, actual: Any = None, target: Any = None, reason: str | None = None) -> dict[str, Any]:
    item: dict[str, Any] = {"id": name, "status": status}
    if actual is not None:
        item["actual"] = actual
    if target is not None:
        item["target"] = target
    if reason:
        item["reason"] = reason
    return item


def evaluate_slos(result: dict[str, Any], profile: dict[str, Any]) -> list[dict[str, Any]]:
    checks: list[dict[str, Any]] = []
    disk_limit = profile.get("slo", {}).get("disk", {}).get("database_bytes_max")
    if disk_limit is not None:
        actual = result["storage"].get("database_bytes")
        checks.append(_check(
            "disk.database_bytes", "pass" if actual is not None and actual <= disk_limit else "fail",
            actual=actual, target=disk_limit,
        ))
    memory_limit = profile.get("slo", {}).get("memory", {}).get("peak_rss_bytes_max")
    if memory_limit is not None:
        actual = result.get("counters", {}).get("peak_rss_bytes")
        checks.append(_check(
            "memory.peak_rss_bytes", "pass" if actual is not None and actual <= memory_limit else "fail",
            actual=actual, target=memory_limit,
        ))
    throughput_min = profile.get("slo", {}).get("throughput", {}).get("cold_build_rows_per_s_min")
    if throughput_min is not None:
        actual = result.get("operations", {}).get("cold_build", {}).get("rows_per_s")
        checks.append(_check(
            "throughput.cold_build_rows_per_s", "pass" if actual is not None and actual >= throughput_min else "fail",
            actual=actual, target=throughput_min,
        ))
    storage_limit = profile.get("slo", {}).get("storage", {}).get("database_bytes_max")
    if storage_limit is not None:
        actual = result["storage"].get("database_bytes")
        checks.append(
            _check(
                "storage.database_bytes",
                "pass" if actual is not None and actual <= storage_limit else "fail",
                actual=actual, target=storage_limit,
            )
        )
    for query_id, target in profile.get("slo", {}).get("queries", {}).items():
        matches = [query for query in result.get("queries", []) if query["id"] == query_id]
        if not matches:
            checks.append(_check(f"query.{query_id}", "fail", reason="query not reported"))
            continue
        query = matches[0]
        actual = query.get("latency_ms", {}).get("p95_ms")
        passed = query.get("status") == "ok" and actual is not None and actual <= target["p95_ms_max"]
        checks.append(_check(
            f"query.{query_id}.p95_ms", "pass" if passed else "fail",
            actual=actual, target=target["p95_ms_max"],
            reason=None if passed else query.get("error", "latency SLO exceeded or query failed"),
        ))
    recovery = result.get("operations", {}).get("recovery", {})
    checks.append(_check(
        "recovery.integrity", "pass" if recovery.get("status") == "ok" else "fail",
        reason=None if recovery.get("status") == "ok" else "integrity or generation check failed",
    ))
    equivalence = result.get("gates", {}).get("semantic_equivalence")
    if equivalence is not None:
        checks.append(_check("semantic_equivalence", "pass" if equivalence else "fail"))
    return checks


def evaluate_regression(
    baseline: dict[str, Any], bad_config: dict[str, Any], profile: dict[str, Any]
) -> dict[str, Any]:
    if baseline.get("manifest_sha256") != bad_config.get("manifest_sha256"):
        return {"id": "intentional_regression", "status": "fail", "reason": "manifest mismatch"}
    if baseline.get("profile_id") != bad_config.get("profile_id"):
        return {"id": "intentional_regression", "status": "fail", "reason": "hardware profile mismatch"}
    if baseline.get("configuration") == bad_config.get("configuration"):
        return {"id": "intentional_regression", "status": "fail", "reason": "configuration was not changed"}
    factor = float(profile.get("gates", {}).get("minimum_regression_factor", 1.20))
    comparisons = []
    for baseline_query in baseline.get("queries", []):
        candidate = next((q for q in bad_config.get("queries", []) if q["id"] == baseline_query["id"]), None)
        if candidate is None:
            continue
        base_p95 = baseline_query.get("latency_ms", {}).get("p95_ms")
        bad_p95 = candidate.get("latency_ms", {}).get("p95_ms")
        if base_p95 and bad_p95:
            comparisons.append({
                "query_id": baseline_query["id"],
                "baseline_p95_ms": base_p95,
                "bad_config_p95_ms": bad_p95,
                "ratio": round(bad_p95 / base_p95, 6),
            })
    passed = any(item["ratio"] >= factor for item in comparisons)
    return {
        "id": "intentional_regression",
        "status": "pass" if passed else "fail",
        "minimum_factor": factor,
        "comparisons": comparisons,
        "reason": None if passed else "bad configuration did not exceed the regression factor",
    }


def evaluate_custom_store(decision: dict[str, Any], *, require: bool) -> dict[str, Any]:
    requested = decision.get("decision", "hold")
    if requested not in {"hold", "reject", "propose"}:
        return {"id": "custom_store", "status": "fail", "reason": "invalid decision"}
    if requested != "propose":
        return {"id": "custom_store", "status": "pass", "decision": requested}
    failed_slos = decision.get("failed_slos", [])
    alternatives = decision.get("alternatives", [])
    costs = decision.get("costs", {})
    errors = []
    if not failed_slos:
        errors.append("a proposed custom store must identify an exact failed SLO")
    if not alternatives or any(not item.get("evidence") for item in alternatives):
        errors.append("schema/tuning/derived-accelerator alternatives need evidence")
    if not costs.get("engineering") or not costs.get("compatibility"):
        errors.append("engineering and compatibility costs are required")
    return {
        "id": "custom_store",
        "status": "pass" if not errors else ("fail" if require else "not_run"),
        "decision": requested,
        "failed_slos": failed_slos,
        "alternatives": alternatives,
        "errors": errors,
    }


def evaluate(
    result: dict[str, Any],
    profile: dict[str, Any],
    *,
    baseline: dict[str, Any] | None = None,
    bad_config: dict[str, Any] | None = None,
    decision: dict[str, Any] | None = None,
    require_custom_store: bool = False,
) -> dict[str, Any]:
    require_result_version(result)
    checks = evaluate_slos(result, profile)
    if baseline is not None and bad_config is not None:
        checks.append(evaluate_regression(baseline, bad_config, profile))
    elif require_custom_store:
        checks.append(_check("intentional_regression", "not_run", reason="baseline and bad-config results are required"))
    if decision is not None:
        checks.append(evaluate_custom_store(decision, require=require_custom_store))
    elif require_custom_store:
        checks.append(_check("custom_store", "fail", reason="decision record is required"))
    statuses = [item["status"] for item in checks]
    return {
        "gate_version": "storage-m0/gate-v1",
        "result_run_id": result["run_id"],
        "checks": checks,
        "status": "pass" if statuses and all(status == "pass" for status in statuses) else "fail",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--bad-config", type=Path)
    parser.add_argument("--decision", type=Path)
    parser.add_argument("--require-custom-store-decision", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    result = load_json(args.result)
    profile = load_json(args.profile)
    baseline = load_json(args.baseline) if args.baseline else None
    bad_config = load_json(args.bad_config) if args.bad_config else None
    decision = load_json(args.decision) if args.decision else None
    gate = evaluate(
        result, profile, baseline=baseline, bad_config=bad_config, decision=decision,
        require_custom_store=args.require_custom_store_decision,
    )
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(canonical_json(gate) + "\n", encoding="utf-8")
    print(canonical_json(gate))
    return 0 if gate["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
