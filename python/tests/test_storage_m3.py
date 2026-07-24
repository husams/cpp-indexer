"""Storage M3 optional-accelerator contract tests."""

from __future__ import annotations

import copy
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from benchmarks.storage_m3.gate import validate_report  # noqa: E402
from benchmarks.storage_m3.lifecycle import run_lifecycle_probe  # noqa: E402


REPORT = ROOT / "benchmarks/storage_m3/evaluation-v1.json"


def test_storage_m3_report_schema_and_gate_pass():
    report = json.loads(REPORT.read_text(encoding="utf-8"))
    schema = json.loads(
        (REPORT.parent / "report.schema.json").read_text(encoding="utf-8")
    )
    assert schema["properties"]["report_version"]["const"] == report["report_version"]
    assert (
        schema["properties"]["authoritative_store"]["properties"]["engine"]["const"]
        == "SQLite"
    )
    assert (
        "required_failed_slo"
        in schema["properties"]["comparison"]["properties"]["custom_store_gate"][
            "required"
        ]
    )
    result = validate_report(report, ROOT)
    assert result["status"] == "pass", result
    assert result["failed_slos"] == []


def test_accelerator_lifecycle_is_disposable(tmp_path):
    evidence = run_lifecycle_probe(tmp_path)
    assert evidence["status"] == "pass"
    assert evidence["authoritative_unchanged_after_delete"] is True
    assert evidence["rebuild_content_identity_matches"] is True


def test_gate_requires_projection_metadata_and_no_mandatory_service():
    report = json.loads(REPORT.read_text(encoding="utf-8"))
    broken = copy.deepcopy(report)
    broken["candidates"][1]["identity_mapping"].pop("generation_dictionary")
    assert validate_report(broken, ROOT)["status"] == "fail"

    broken = copy.deepcopy(report)
    broken["candidates"][1]["execution"]["mandatory_service"] = True
    assert validate_report(broken, ROOT)["status"] == "fail"


@pytest.mark.parametrize(
    ("field", "value", "check_id"),
    [
        ("engine", "Neo4j", "authority.engine"),
        ("role", "secondary graph store", "authority.role"),
        ("schema_version", 35, "authority.schema"),
    ],
)
def test_gate_rejects_non_sqlite_authority_contract(field, value, check_id):
    report = json.loads(REPORT.read_text(encoding="utf-8"))
    broken = copy.deepcopy(report)
    broken["authoritative_store"][field] = value
    result = validate_report(broken, ROOT)
    assert result["status"] == "fail"
    assert (
        next(check for check in result["checks"] if check["id"] == check_id)["status"]
        == "fail"
    )


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("required_failed_slo", False),
        ("required_alternatives", []),
        ("required_costs", []),
    ],
)
def test_gate_rejects_missing_custom_store_prerequisite(field, value):
    report = json.loads(REPORT.read_text(encoding="utf-8"))
    broken = copy.deepcopy(report)
    broken["comparison"]["custom_store_gate"][field] = value
    result = validate_report(broken, ROOT)
    assert result["status"] == "fail"
    assert (
        next(
            check
            for check in result["checks"]
            if check["id"] == f"custom_store_gate.{field.removeprefix('required_')}"
        )["status"]
        == "fail"
    )


@pytest.mark.parametrize(
    "field", ["end_to_end_latency", "crash_recovery", "portability_packaging"]
)
def test_gate_requires_total_cost_dimensions(field):
    report = json.loads(REPORT.read_text(encoding="utf-8"))
    broken = copy.deepcopy(report)
    broken["comparison"]["end_to_end_cost_dimensions"].remove(field)
    assert validate_report(broken, ROOT)["status"] == "fail"
