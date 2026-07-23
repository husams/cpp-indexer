"""Evaluate Storage M0 results against explicit SLO and decision gates."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from .common import canonical_json, load_json, require_result_version


def _check(name: str, status: str, *, actual: Any = None, target: Any = None, reason: str | None = None) -> dict[str, Any]:
    item: dict[str, Any] = {"id": name, "status": status}
    if actual is not None: item["actual"] = actual
    if target is not None: item["target"] = target
    if reason: item["reason"] = reason
    return item


def evaluate_slos(result: dict[str, Any], profile: dict[str, Any]) -> list[dict[str, Any]]:
    checks: list[dict[str, Any]] = []
    slo = profile.get("slo", {})
    for check_id, actual, target in (
        ("disk.database_bytes", result.get("storage", {}).get("database_bytes"), slo.get("disk", {}).get("database_bytes_max")),
        ("memory.peak_rss_bytes", result.get("counters", {}).get("peak_rss_bytes"), slo.get("memory", {}).get("peak_rss_bytes_max")),
        ("throughput.cold_build_rows_per_s", result.get("operations", {}).get("cold_build", {}).get("rows_per_s"), slo.get("throughput", {}).get("cold_build_rows_per_s_min")),
    ):
        if target is not None:
            passed = actual is not None and ((actual <= target) if "bytes" in check_id else (actual >= target))
            checks.append(_check(check_id, "pass" if passed else "fail", actual=actual, target=target))
    storage_target = slo.get("storage", {}).get("database_bytes_max")
    if storage_target is not None:
        actual = result.get("storage", {}).get("database_bytes")
        checks.append(_check("storage.database_bytes", "pass" if actual is not None and actual <= storage_target else "fail", actual=actual, target=storage_target))
    for query_id, target in slo.get("queries", {}).items():
        query = next((item for item in result.get("queries", []) if item.get("id") == query_id), None)
        if query is None:
            checks.append(_check(f"query.{query_id}", "fail", reason="query not reported")); continue
        actual = query.get("latency_ms", {}).get("p95_ms")
        passed = query.get("status") == "ok" and actual is not None and query.get("row_count") is not None and actual <= target["p95_ms_max"]
        checks.append(_check(f"query.{query_id}.p95_ms", "pass" if passed else "fail", actual=actual, target=target["p95_ms_max"], reason=None if passed else query.get("error", "query failed, empty, or latency SLO exceeded")))
    recovery = result.get("operations", {}).get("recovery", {})
    checks.append(_check("recovery.integrity", "pass" if recovery.get("status") == "ok" and recovery.get("presented_as_current") else "fail", reason=None if recovery.get("status") == "ok" else "integrity, FK, generation, or semantic digest check failed"))
    required_ops = ("cold_build", "warm_noop", "changed_tu_update", "transform_rebuild", "migration", "backup")
    for operation in required_ops:
        item = result.get("operations", {}).get(operation, {})
        checks.append(_check(f"operation.{operation}", "pass" if item.get("status") == "ok" and item.get("semantic_equivalence", True) else "fail", reason=None if item.get("status") == "ok" else item.get("reason", "required measurement is missing or failed")))
    equivalence = result.get("gates", {}).get("semantic_equivalence")
    checks.append(_check("semantic_equivalence", "pass" if equivalence is True else "fail", reason=None if equivalence is True else "full/incremental semantic equivalence was not demonstrated"))
    return checks


def _identity_dimensions(result: dict[str, Any]) -> dict[str, Any]:
    identity = result.get("identity")
    if not isinstance(identity, dict):
        return {}
    return {key: identity.get(key) for key in ("manifest_sha256", "workload", "scale", "seed", "distribution", "requested", "actual", "caps", "revision", "profile_id", "hardware_fingerprint")}


def evaluate_regression(baseline: dict[str, Any], bad_config: dict[str, Any], profile: dict[str, Any]) -> dict[str, Any]:
    if _identity_dimensions(baseline) != _identity_dimensions(bad_config):
        return {"id": "intentional_regression", "status": "fail", "reason": "manifest/workload/scale/seed/distribution/count/cap/revision/profile/hardware mismatch"}
    if baseline.get("configuration") == bad_config.get("configuration"):
        return {"id": "intentional_regression", "status": "fail", "reason": "configuration was not changed"}
    evidence = bad_config.get("configuration_evidence", {})
    if evidence.get("measured") is not True or evidence.get("artifact") is None or not evidence.get("checks"):
        return {"id": "intentional_regression", "status": "fail", "reason": "bad configuration lacks measured artifact evidence"}
    if bad_config.get("configuration") == "drop_hot_indexes":
        hot = {"idx_symbol_qual", "idx_symbol_qual_nc", "idx_symbol_spelling", "idx_symbol_spelling_nc", "idx_edge_src", "idx_edge_dst"}
        if hot.intersection(evidence.get("indexes", [])):
            return {"id": "intentional_regression", "status": "fail", "reason": "drop_hot_indexes evidence still contains a hot index"}
    baseline_queries = {item.get("id"): item for item in baseline.get("queries", [])}
    candidate_queries = {item.get("id"): item for item in bad_config.get("queries", [])}
    if set(baseline_queries) != set(candidate_queries):
        return {"id": "intentional_regression", "status": "fail", "reason": "query set is not exact"}
    if baseline.get("counters", {}).get("query_set_sha256") != bad_config.get("counters", {}).get("query_set_sha256"):
        return {"id": "intentional_regression", "status": "fail", "reason": "query workload parameters differ"}
    comparisons = []
    factor = float(profile.get("gates", {}).get("minimum_regression_factor", 1.20))
    for query_id in sorted(baseline_queries):
        before, after = baseline_queries[query_id], candidate_queries[query_id]
        if before.get("status") != "ok" or after.get("status") != "ok" or not before.get("row_count") or not after.get("row_count"):
            return {"id": "intentional_regression", "status": "fail", "reason": f"query {query_id} is missing, errored, or empty"}
        base_p95, bad_p95 = before.get("latency_ms", {}).get("p95_ms"), after.get("latency_ms", {}).get("p95_ms")
        if base_p95 is None or bad_p95 is None or base_p95 <= 0:
            return {"id": "intentional_regression", "status": "fail", "reason": f"query {query_id} has no comparable latency"}
        comparisons.append({"query_id": query_id, "baseline_p95_ms": base_p95, "bad_config_p95_ms": bad_p95, "ratio": round(bad_p95 / base_p95, 6)})
    passed = any(item["ratio"] >= factor for item in comparisons)
    return {"id": "intentional_regression", "status": "pass" if passed else "fail", "minimum_factor": factor, "comparisons": comparisons, "reason": None if passed else "measured bad configuration did not exceed the regression factor"}


def _real_evidence(item: dict[str, Any]) -> bool:
    evidence = item.get("evidence")
    return bool(item.get("measured") is True and isinstance(evidence, dict) and evidence.get("run_id") and evidence.get("artifact") and evidence.get("checks"))


def evaluate_custom_store(decision: dict[str, Any], *, require: bool, result: dict[str, Any] | None = None, slo_checks: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    requested = decision.get("decision", "hold")
    if requested not in {"hold", "reject", "propose"}:
        return {"id": "custom_store", "status": "fail", "reason": "invalid decision"}
    if requested != "propose":
        return {"id": "custom_store", "status": "pass", "decision": requested}
    actual_failed = {item["id"] for item in (slo_checks or []) if item.get("status") == "fail"}
    declared = set(decision.get("failed_slos", [])); errors = []
    if not actual_failed:
        errors.append("custom-store proposal has no actual failed SLO in the bound result")
    if declared != actual_failed:
        errors.append("failed_slos must exactly equal the failed checks from the bound result")
    alternatives = decision.get("alternatives", []); classes = {item.get("class") for item in alternatives}
    if not {"schema_tuning", "derived_accelerator"}.issubset(classes):
        errors.append("proposal needs separately measured schema/tuning and derived-accelerator alternatives")
    if any(not _real_evidence(item) for item in alternatives):
        errors.append("every alternative needs measured evidence bound to an artifact and checks")
    costs = decision.get("costs", {})
    if not isinstance(costs.get("engineering"), dict) or not isinstance(costs.get("compatibility"), dict) or not costs["engineering"].get("person_months") or not costs["compatibility"].get("migration_plan"):
        errors.append("engineering and compatibility cost evidence are required")
    return {"id": "custom_store", "status": "pass" if not errors else ("fail" if require else "not_run"), "decision": requested, "failed_slos": sorted(declared), "alternatives": alternatives, "costs": costs, "errors": errors}


def evaluate(result: dict[str, Any], profile: dict[str, Any], *, baseline: dict[str, Any] | None = None, bad_config: dict[str, Any] | None = None, decision: dict[str, Any] | None = None, require_custom_store: bool = False) -> dict[str, Any]:
    require_result_version(result); checks = evaluate_slos(result, profile)
    if baseline is not None and bad_config is not None:
        checks.append(evaluate_regression(baseline, bad_config, profile))
    elif require_custom_store:
        checks.append(_check("intentional_regression", "fail", reason="baseline and measured bad-config results are required"))
    if decision is not None:
        checks.append(evaluate_custom_store(decision, require=require_custom_store, result=result, slo_checks=checks))
    elif require_custom_store:
        checks.append(_check("custom_store", "fail", reason="decision record is required"))
    statuses = [item["status"] for item in checks]
    return {"gate_version": "storage-m0/gate-v2", "result_run_id": result["run_id"], "checks": checks, "status": "pass" if statuses and all(status == "pass" for status in statuses) else "fail"}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("--result", type=Path, required=True); parser.add_argument("--profile", type=Path, required=True); parser.add_argument("--baseline", type=Path); parser.add_argument("--bad-config", type=Path); parser.add_argument("--decision", type=Path); parser.add_argument("--require-custom-store-decision", action="store_true"); parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv); result = load_json(args.result); profile = load_json(args.profile); baseline = load_json(args.baseline) if args.baseline else None; bad_config = load_json(args.bad_config) if args.bad_config else None; decision = load_json(args.decision) if args.decision else None
    gate = evaluate(result, profile, baseline=baseline, bad_config=bad_config, decision=decision, require_custom_store=args.require_custom_store_decision)
    if args.output: args.output.parent.mkdir(parents=True, exist_ok=True); args.output.write_text(canonical_json(gate) + "\n", encoding="utf-8")
    print(canonical_json(gate)); return 0 if gate["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
