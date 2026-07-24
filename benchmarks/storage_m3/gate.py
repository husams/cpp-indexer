"""Validate the Storage M3 accelerator evaluation and custom-store gate."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from benchmarks.storage_m0.common import canonical_json, sha256
from benchmarks.storage_m0.gate import evaluate_slos


REQUIRED_DESCRIPTOR_FIELDS = {
    "source_fact_sets",
    "version",
    "build_options",
    "identity_mapping",
    "invalidation_dependencies",
    "publication",
    "execution",
    "query_contract",
    "cost_model",
}
REQUIRED_COST_DIMENSIONS = {
    "build_update",
    "hot_query_latency",
    "end_to_end_latency",
    "duplicate_disk",
    "memory_mapping",
    "open_attach",
    "cleanup_retention",
    "portability_packaging",
    "crash_recovery",
    "maintenance",
}
REQUIRED_PROJECTION_CONTRACT = {
    "query_semantics",
    "completeness",
    "evidence",
    "ordering",
    "fallback",
}


def _load(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected object: {path}")
    return value


def _check(
    checks: list[dict[str, Any]], check_id: str, passed: bool, reason: str = ""
) -> None:
    checks.append(
        {
            "id": check_id,
            "status": "pass" if passed else "fail",
            **({"reason": reason} if reason else {}),
        }
    )


def _path(root: Path, relative: str) -> Path:
    candidate = Path(relative)
    return candidate if candidate.is_absolute() else root / candidate


def _meaningful(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value.strip()) >= 3
        and "placeholder" not in value.lower()
    )


def _validate_descriptor(
    checks: list[dict[str, Any]], candidate: dict[str, Any]
) -> None:
    candidate_id = candidate.get("id", "unknown")
    missing = sorted(REQUIRED_DESCRIPTOR_FIELDS - candidate.keys())
    _check(
        checks,
        f"candidate.{candidate_id}.descriptor",
        not missing,
        f"missing: {', '.join(missing)}",
    )
    _check(
        checks,
        f"candidate.{candidate_id}.source_fact_sets",
        isinstance(candidate.get("source_fact_sets"), list)
        and bool(candidate["source_fact_sets"]),
    )
    _check(
        checks,
        f"candidate.{candidate_id}.version",
        _meaningful(candidate.get("version")),
    )
    _check(
        checks,
        f"candidate.{candidate_id}.build_options",
        isinstance(candidate.get("build_options"), dict)
        and bool(candidate["build_options"]),
    )
    identity = candidate.get("identity_mapping", {})
    _check(
        checks,
        f"candidate.{candidate_id}.identity_mapping",
        isinstance(identity, dict)
        and _meaningful(identity.get("exported_identity"))
        and _meaningful(identity.get("local_id_scope"))
        and _meaningful(identity.get("generation_dictionary")),
    )
    _check(
        checks,
        f"candidate.{candidate_id}.invalidation",
        isinstance(candidate.get("invalidation_dependencies"), list)
        and bool(candidate["invalidation_dependencies"]),
    )
    publication = candidate.get("publication", {})
    role = candidate.get("role")
    publication_valid = (
        isinstance(publication, dict)
        and all(
            publication.get(key) is True
            for key in (
                "immutable_or_atomic_replace",
                "content_addressed",
                "disposable",
                "rebuildable",
            )
        )
        if role == "derived_accelerator"
        else (
            isinstance(publication, dict)
            and (
                (
                    role == "unsuitable_projection"
                    and all(
                        publication.get(key) is False
                        for key in (
                            "immutable_or_atomic_replace",
                            "content_addressed",
                            "disposable",
                            "rebuildable",
                        )
                    )
                    and _meaningful(candidate.get("rejection_reason"))
                )
                or (
                    role != "unsuitable_projection"
                    and publication.get("rebuildable") is True
                )
            )
        )
    )
    _check(
        checks,
        f"candidate.{candidate_id}.publication",
        publication_valid,
    )
    execution = candidate.get("execution", {})
    _check(
        checks,
        f"candidate.{candidate_id}.execution",
        isinstance(execution, dict)
        and execution.get("local_embedded") is True
        and execution.get("mandatory_service") is False,
    )
    contract = candidate.get("query_contract", {})
    _check(
        checks,
        f"candidate.{candidate_id}.query_contract",
        isinstance(contract, dict)
        and REQUIRED_PROJECTION_CONTRACT <= contract.keys()
        and all(_meaningful(contract.get(key)) for key in REQUIRED_PROJECTION_CONTRACT),
    )
    cost_model = candidate.get("cost_model", {})
    dimensions = cost_model.get("dimensions") if isinstance(cost_model, dict) else None
    _check(
        checks,
        f"candidate.{candidate_id}.end_to_end_cost",
        isinstance(dimensions, list) and REQUIRED_COST_DIMENSIONS <= set(dimensions),
        f"missing: {', '.join(sorted(REQUIRED_COST_DIMENSIONS - set(dimensions or [])))}"
        if isinstance(dimensions, list)
        else "dimensions are required",
    )


def validate_report(report: dict[str, Any], root: Path) -> dict[str, Any]:
    checks: list[dict[str, Any]] = []
    _check(
        checks,
        "report.version",
        report.get("report_version") == "storage-m3/evaluation-v1",
    )
    _check(checks, "report.issue", report.get("source_issue") == "HSE-81")

    authority = report.get("authoritative_store", {})
    baseline = report.get("baseline", {})
    result_path = _path(root, authority.get("result_artifact", ""))
    profile_path = _path(root, authority.get("profile_artifact", ""))
    summary_path = _path(root, authority.get("summary_artifact", ""))
    source_paths_ok = all(
        path.is_file() for path in (result_path, profile_path, summary_path)
    )
    _check(checks, "baseline.source_artifacts", source_paths_ok)

    if source_paths_ok:
        result = _load(result_path)
        profile = _load(profile_path)
        summary = _load(summary_path)
        actual_checks = evaluate_slos(result, profile)
        failed = sorted(
            item["id"] for item in actual_checks if item["status"] == "fail"
        )
        _check(checks, "baseline.schema", result.get("schema_version") == 34)
        _check(
            checks,
            "baseline.run_binding",
            result.get("run_id") == baseline.get("result_run_id"),
        )
        _check(
            checks,
            "baseline.profile_binding",
            result.get("profile_id") == profile.get("profile_id")
            and result.get("profile_sha256") == sha256(canonical_json(profile)),
        )
        _check(
            checks,
            "baseline.semantic_identity",
            result.get("operations", {}).get("recovery", {}).get("semantic_digest")
            == baseline.get("semantic_digest"),
        )
        _check(
            checks,
            "baseline.summary_binding",
            summary.get("semantic_digest") == baseline.get("semantic_digest"),
        )
        _check(
            checks,
            "baseline.slos",
            failed == baseline.get("failed_slos", []),
            f"actual failed SLOs: {failed}",
        )
    else:
        result = {}
        failed = ["baseline.source_artifacts"]

    candidates = report.get("candidates", [])
    ids = [
        candidate.get("id") for candidate in candidates if isinstance(candidate, dict)
    ]
    _check(checks, "candidates.unique_ids", len(ids) == len(set(ids)) and bool(ids))
    _check(
        checks,
        "candidates.sqlite_baseline",
        sum(candidate.get("role") == "sqlite_baseline" for candidate in candidates)
        == 1,
    )
    _check(
        checks,
        "candidates.unsuitable_control",
        sum(
            candidate.get("role") == "unsuitable_projection" for candidate in candidates
        )
        == 1,
    )
    for candidate in candidates:
        if isinstance(candidate, dict):
            _validate_descriptor(checks, candidate)

    comparison = report.get("comparison", {})
    dimensions = comparison.get("end_to_end_cost_dimensions", [])
    _check(
        checks,
        "comparison.end_to_end_cost",
        REQUIRED_COST_DIMENSIONS <= set(dimensions),
    )
    contract = comparison.get("projection_contract", {})
    _check(
        checks,
        "comparison.projection_contract",
        REQUIRED_PROJECTION_CONTRACT <= contract.keys(),
    )

    lifecycle = report.get("lifecycle_evidence", {})
    _check(
        checks,
        "lifecycle.disposable_rebuildable",
        lifecycle.get("status") == "pass"
        and lifecycle.get("authoritative_unchanged_after_delete") is True
        and lifecycle.get("rebuild_content_identity_matches") is True,
    )
    decision = report.get("decision", {})
    recommendation = decision.get("recommendation")
    if not failed:
        _check(checks, "decision.do_nothing_is_valid", recommendation == "do_nothing")
        _check(
            checks,
            "decision.custom_store_gate_closed",
            decision.get("custom_store_gate") == "closed",
        )
    else:
        _check(
            checks,
            "decision.failed_slo_is_acknowledged",
            recommendation != "do_nothing",
        )
        _check(
            checks,
            "decision.custom_store_gate_requires_evidence",
            decision.get("custom_store_gate") == "evidence_required",
        )
    _check(
        checks,
        "decision.no_mandatory_infrastructure",
        all(
            candidate.get("execution", {}).get("mandatory_service") is False
            for candidate in candidates
        ),
    )
    status = (
        "pass"
        if checks and all(item["status"] == "pass" for item in checks)
        else "fail"
    )
    return {
        "gate_version": "storage-m3/gate-v1",
        "status": status,
        "checks": checks,
        "failed_slos": failed,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path("."))
    args = parser.parse_args(argv)
    result = validate_report(_load(args.report), args.root)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
