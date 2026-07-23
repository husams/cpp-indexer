"""Evaluate Storage M0 results against explicit SLO and decision gates."""

from __future__ import annotations

import argparse
import math
import sqlite3
from pathlib import Path
from typing import Any

from .common import canonical_json, latency_summary, load_json, manifest_digest, require_result_version, semantic_digest, sha256


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
    return {key: identity.get(key) for key in ("manifest_sha256", "workload", "scale", "seed", "distribution", "requested", "actual", "caps", "revision", "profile_id", "profile_sha256", "hardware_fingerprint")}


def _bound_profile(result: dict[str, Any]) -> dict[str, Any] | None:
    evidence = result.get("configuration_evidence")
    profile_artifact = evidence.get("profile_artifact") if isinstance(evidence, dict) else None
    if not profile_artifact or not Path(profile_artifact).is_file():
        return None
    try:
        profile = load_json(Path(profile_artifact))
    except (OSError, ValueError):
        return None
    return profile if isinstance(profile, dict) and profile.get("profile_id") == result.get("profile_id") and sha256(canonical_json(profile)) == result.get("profile_sha256") else None


def _failed_slo_ids(checks: list[dict[str, Any]]) -> set[str]:
    return {item["id"] for item in checks if item.get("status") == "fail" and item.get("id")}


def _slo_checks_sha256(result: dict[str, Any], checks: list[dict[str, Any]]) -> str:
    return sha256(canonical_json({"profile_sha256": result.get("profile_sha256"), "checks": checks}))


def _measurement_signature(result: dict[str, Any]) -> str:
    recovery = result.get("operations", {}).get("recovery", {})
    queries = [{key: query.get(key) for key in ("id", "category", "sql", "parameters", "plan", "status", "row_count", "execution_count", "latency_ms", "samples_ms")} for query in result.get("queries", [])]
    return sha256(canonical_json({
        "identity": _identity_dimensions(result),
        "semantic_digest": recovery.get("semantic_digest"),
        "queries": queries,
        "storage": result.get("storage"),
        "counters": result.get("counters"),
    }))


def _bound_result_errors(result: dict[str, Any], role: str) -> list[str]:
    errors: list[str] = []
    try:
        require_result_version(result)
    except ValueError as error:
        return [f"{role}: {error}"]
    evidence = result.get("configuration_evidence")
    if not isinstance(evidence, dict) or evidence.get("measured") is not True:
        errors.append(f"{role}: configuration evidence is not measured")
        return errors
    identity = result.get("identity")
    if not isinstance(identity, dict) or identity.get("configuration") != result.get("configuration"):
        errors.append(f"{role}: result identity configuration is not bound")
    elif sha256(canonical_json(identity))[:24] != result.get("run_id"):
        errors.append(f"{role}: run_id is not the canonical identity binding")
    for key in ("manifest_sha256", "workload", "scale", "seed", "distribution", "requested", "actual", "caps", "revision", "profile_id", "profile_sha256", "hardware_fingerprint", "configuration"):
        if isinstance(identity, dict) and result.get(key) != identity.get(key):
            errors.append(f"{role}: result field {key} is not bound to identity")
    for key in ("artifact", "result_artifact", "manifest_artifact", "profile_artifact"):
        value = evidence.get(key)
        if not value or not Path(value).is_file():
            errors.append(f"{role}: missing bound {key}")
    result_artifact = evidence.get("result_artifact")
    if result_artifact and Path(result_artifact).is_file():
        try:
            on_disk = load_json(Path(result_artifact))
            if canonical_json(on_disk) != canonical_json(result):
                errors.append(f"{role}: in-memory result differs from bound result artifact")
        except (OSError, ValueError) as error:
            errors.append(f"{role}: invalid result artifact ({error})")
    manifest_artifact = evidence.get("manifest_artifact")
    manifest: dict[str, Any] | None = None
    if manifest_artifact and Path(manifest_artifact).is_file():
        try:
            manifest = load_json(Path(manifest_artifact))
            if manifest_digest(manifest) != result.get("manifest_sha256"):
                errors.append(f"{role}: manifest digest does not match result identity")
        except (OSError, ValueError) as error:
            errors.append(f"{role}: invalid manifest artifact ({error})")
    db_artifact = evidence.get("artifact")
    if db_artifact and Path(db_artifact).is_file():
        try:
            connection = sqlite3.connect(db_artifact)
            try:
                configuration = connection.execute("SELECT value FROM benchmark_meta WHERE key='configuration'").fetchone()
                indexes = [row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type='index' ORDER BY name")]
                if manifest is not None:
                    workload = next((item for item in manifest.get("workloads", []) if item.get("id") == result.get("workload")), None)
                    if not isinstance(workload, dict):
                        errors.append(f"{role}: workload is not present in manifest")
                    else:
                        from .run import _parameter_value
                        expected = [{
                            "id": query.get("id"), "category": query.get("category", "other"),
                            "sql": query.get("sql"), "parameters": [_parameter_value(parameter, connection) for parameter in query.get("parameters", [])],
                        } for query in workload.get("queries", [])]
                        actual = [{key: item.get(key) for key in ("id", "category", "sql", "parameters")} for item in result.get("queries", [])]
                        if actual != expected:
                            errors.append(f"{role}: query SQL, parameters, categories, or ordering differ from manifest workload")
            finally:
                connection.close()
            if configuration is None or configuration[0] != result.get("configuration"):
                errors.append(f"{role}: database configuration is not bound to result")
            if indexes != evidence.get("indexes"):
                errors.append(f"{role}: index inventory is not measured from the bound database")
            if semantic_digest(Path(db_artifact)) != result.get("operations", {}).get("recovery", {}).get("semantic_digest"):
                errors.append(f"{role}: database semantic digest is not bound to result")
        except sqlite3.DatabaseError as error:
            errors.append(f"{role}: invalid database artifact ({error})")
    checks = evidence.get("checks")
    if not isinstance(checks, list) or not checks or any(
        not isinstance(item, dict) or item.get("status") != "pass" or not item.get("id") or "actual" not in item
        for item in checks
    ):
        errors.append(f"{role}: configuration checks must be structured measured pass records")
    if evidence.get("run_id") != result.get("run_id"):
        errors.append(f"{role}: configuration evidence run_id is not bound")
    if evidence.get("configuration") != result.get("configuration"):
        errors.append(f"{role}: configuration evidence configuration is not bound")
    if evidence.get("profile_sha256") != result.get("profile_sha256"):
        errors.append(f"{role}: configuration evidence profile digest is not bound")
    profile_artifact = evidence.get("profile_artifact")
    if profile_artifact and Path(profile_artifact).is_file():
        try:
            if _bound_profile(result) is None:
                errors.append(f"{role}: profile artifact is not bound to result")
        except (OSError, ValueError) as error:
            errors.append(f"{role}: invalid profile artifact ({error})")
    samples_artifact = result.get("raw_samples_artifact")
    if not samples_artifact or not Path(samples_artifact).is_file():
        errors.append(f"{role}: missing external raw-samples artifact")
    else:
        try:
            samples = load_json(Path(samples_artifact))
            if sha256(canonical_json(samples)) != result.get("raw_samples_sha256"):
                errors.append(f"{role}: raw-samples content digest mismatch")
            if samples.get("run_id") != result.get("run_id"):
                errors.append(f"{role}: raw-samples run identity mismatch")
            sample_rows = {item.get("id"): item for item in samples.get("queries", [])}
            result_rows = {item.get("id"): item for item in result.get("queries", [])}
            if set(sample_rows) != set(result_rows):
                errors.append(f"{role}: raw-samples query set mismatch")
            for query_id, query in result_rows.items():
                values = sample_rows.get(query_id, {}).get("samples_ms")
                if not isinstance(values, list) or latency_summary(values) != query.get("latency_ms") or len(values) != query.get("execution_count"):
                    errors.append(f"{role}: latency summary is not derived from external raw samples for {query_id}")
        except (OSError, ValueError, TypeError) as error:
            errors.append(f"{role}: invalid raw-samples artifact ({error})")
    return errors


def evaluate_regression(baseline: dict[str, Any], bad_config: dict[str, Any], profile: dict[str, Any]) -> dict[str, Any]:
    artifact_errors = _bound_result_errors(baseline, "baseline") + _bound_result_errors(bad_config, "bad-config")
    if artifact_errors:
        return {"id": "intentional_regression", "status": "fail", "reason": "; ".join(artifact_errors)}
    if _identity_dimensions(baseline) != _identity_dimensions(bad_config):
        return {"id": "intentional_regression", "status": "fail", "reason": "manifest/workload/scale/seed/distribution/count/cap/revision/profile/hardware mismatch"}
    if baseline.get("configuration") == bad_config.get("configuration"):
        return {"id": "intentional_regression", "status": "fail", "reason": "configuration was not changed"}
    evidence = bad_config.get("configuration_evidence", {})
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


def _result_content_sha256(result: dict[str, Any]) -> str:
    return sha256(canonical_json(result))


def _real_evidence(item: dict[str, Any], result: dict[str, Any] | None, failed_slos: set[str], seen: dict[str, set[str]]) -> bool:
    evidence = item.get("evidence")
    if result is None or item.get("measured") is not True or not isinstance(evidence, dict):
        return False
    artifact = evidence.get("artifact")
    if not artifact or not Path(artifact).is_file() or evidence.get("result_run_id") != result.get("run_id"):
        return False
    try:
        bound = load_json(Path(artifact)); require_result_version(bound)
    except (OSError, ValueError):
        return False
    if bound.get("run_id") == result.get("run_id") or _identity_dimensions(bound) != _identity_dimensions(result):
        return False
    content_sha256 = _result_content_sha256(bound)
    run_id = str(bound.get("run_id"))
    configuration = str(bound.get("configuration"))
    if (
        run_id in seen["run_id"]
        or content_sha256 in seen["content_sha256"]
        or configuration in seen["configuration"]
        or bound.get("run_id") != evidence.get("run_id")
        or evidence.get("content_sha256") != content_sha256
        or evidence.get("configuration") != bound.get("configuration")
        or _bound_result_errors(bound, "alternative")
    ):
        return False
    profile = _bound_profile(bound)
    if profile is None:
        return False
    slo_checks = evaluate_slos(bound, profile)
    bound_failed_slos = _failed_slo_ids(slo_checks)
    measurement_signature = _measurement_signature(bound)
    if bound_failed_slos != failed_slos or measurement_signature in seen["measurement_signature"]:
        return False
    checks = evidence.get("checks")
    outcome = evidence.get("outcome")
    valid = (
        isinstance(checks, list) and checks and all(isinstance(check, dict) and check.get("status") == "pass" and check.get("id") and "actual" in check and "placeholder" not in canonical_json(check).lower() for check in checks)
        and isinstance(outcome, dict) and outcome.get("measured") is True
        and outcome.get("measured_inability") is True and outcome.get("slo_status") == "fail"
        and outcome.get("alternative_class") == item.get("class")
        and outcome.get("run_id") == bound.get("run_id")
        and outcome.get("content_sha256") == content_sha256
        and outcome.get("configuration") == bound.get("configuration")
        and set(outcome.get("tested_failed_slos", [])) == bound_failed_slos
        and outcome.get("slo_checks_sha256") == _slo_checks_sha256(bound, slo_checks)
    )
    if valid:
        seen["run_id"].add(run_id)
        seen["content_sha256"].add(content_sha256)
        seen["configuration"].add(configuration)
        seen["measurement_signature"].add(measurement_signature)
    return valid


def _positive_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value)) and value > 0


def _meaningful_text(value: Any, minimum: int = 8) -> bool:
    return isinstance(value, str) and len(value.strip()) >= minimum and "placeholder" not in value.lower()


def _meaningful_value(value: Any) -> bool:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return math.isfinite(float(value))
    return _meaningful_text(value)


def _source_artifact_valid(value: Any) -> bool:
    if not value or not Path(str(value)).is_file():
        return False
    try:
        text = Path(str(value)).read_text(encoding="utf-8")
    except OSError:
        return False
    return bool(text.strip()) and "placeholder" not in text.lower()


def _cost_evidence_valid(costs: Any) -> bool:
    if not isinstance(costs, dict) or not isinstance(costs.get("engineering"), dict) or not isinstance(costs.get("compatibility"), dict):
        return False
    engineering = costs["engineering"]
    compatibility = costs["compatibility"]
    work_items = engineering.get("work_items")
    migration_plan = compatibility.get("migration_plan")
    compatibility_checks = compatibility.get("compatibility_checks")
    return (
        _positive_number(engineering.get("person_months"))
        and isinstance(work_items, list) and bool(work_items)
        and all(isinstance(item, dict) and _meaningful_text(item.get("id"), 3) and _meaningful_text(item.get("description"), 16) and _positive_number(item.get("person_months")) and isinstance(item.get("deliverables"), list) and bool(item["deliverables"]) and all(_meaningful_text(deliverable, 12) for deliverable in item["deliverables"]) for item in work_items)
        and _positive_number(compatibility.get("person_months"))
        and isinstance(migration_plan, list) and bool(migration_plan)
        and all(isinstance(item, dict) and _meaningful_text(item.get("id"), 3) and _meaningful_text(item.get("description"), 16) and _positive_number(item.get("person_months")) and isinstance(item.get("deliverables"), list) and bool(item["deliverables"]) and all(_meaningful_text(deliverable, 12) for deliverable in item["deliverables"]) for item in migration_plan)
        and isinstance(compatibility_checks, list) and bool(compatibility_checks)
        and all(isinstance(item, dict) and _meaningful_text(item.get("id"), 3) and item.get("status") in {"pass", "fail"} and _meaningful_value(item.get("actual")) and _meaningful_value(item.get("target")) and _meaningful_text(item.get("evidence"), 12) for item in compatibility_checks)
    )


def evaluate_custom_store(decision: dict[str, Any], *, require: bool, result: dict[str, Any] | None = None, slo_checks: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    requested = decision.get("decision", "hold")
    if requested not in {"hold", "reject", "propose"}:
        return {"id": "custom_store", "status": "fail", "reason": "invalid decision"}
    if requested != "propose":
        return {"id": "custom_store", "status": "pass", "decision": requested}
    declared = set(decision.get("failed_slos", [])); errors = []
    actual_failed: set[str] = set()
    primary_profile = _bound_profile(result) if result is not None else None
    if result is not None:
        errors.extend(_bound_result_errors(result, "primary"))
        if primary_profile is not None:
            actual_failed = _failed_slo_ids(evaluate_slos(result, primary_profile))
    if not actual_failed:
        errors.append("custom-store proposal has no actual failed SLO in the bound result")
    if declared != actual_failed:
        errors.append("failed_slos must exactly equal the failed checks from the bound result")
    if result is None or decision.get("result_run_id") != result.get("run_id"):
        errors.append("proposal must bind to the failed result run_id")
    alternatives = decision.get("alternatives", []); classes = {item.get("class") for item in alternatives}
    if not {"schema_tuning", "derived_accelerator"}.issubset(classes):
        errors.append("proposal needs separately measured schema/tuning and derived-accelerator alternatives")
    seen = {"run_id": set(), "content_sha256": set(), "configuration": set(), "measurement_signature": set()}
    if len(alternatives) != 2 or classes != {"schema_tuning", "derived_accelerator"} or any(not _real_evidence(item, result, actual_failed, seen) for item in alternatives):
        errors.append("every alternative needs measured evidence bound to an artifact and checks")
    costs = decision.get("costs", {})
    if (
        not _cost_evidence_valid(costs)
        or not costs["engineering"].get("source_artifact")
        or not costs["compatibility"].get("source_artifact")
        or not _source_artifact_valid(costs["engineering"].get("source_artifact"))
        or not _source_artifact_valid(costs["compatibility"].get("source_artifact"))
    ):
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
