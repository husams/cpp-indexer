"""Hermetic contract tests for the Storage M0 benchmark foundation."""

from __future__ import annotations

import copy
import json
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from benchmarks.storage_m0.common import load_json  # noqa: E402
from benchmarks.storage_m0.bad_config import drop_hot_indexes  # noqa: E402
from benchmarks.storage_m0.gate import evaluate, evaluate_custom_store, evaluate_regression  # noqa: E402
from benchmarks.storage_m0.generator import generate  # noqa: E402
from benchmarks.storage_m0.recovery import simulate  # noqa: E402
from benchmarks.storage_m0.run import run  # noqa: E402


MANIFEST = REPO_ROOT / "benchmarks/storage_m0/manifests/storage-m0-v1.json"
PROFILE = REPO_ROOT / "benchmarks/storage_m0/profiles/local-macos-v1.json"


def test_manifest_and_result_are_versioned():
    manifest = load_json(MANIFEST)
    schema = load_json(REPO_ROOT / "benchmarks/storage_m0/result.schema.json")
    assert manifest["manifest_version"] == "storage-m0/manifest-v1"
    assert manifest["schema_version"] == 34
    assert schema["properties"]["schema_version"]["const"] == 34
    assert {scale for scale in manifest["workloads"][0]["scales"]} >= {
        "smoke", "nodes-1m-relations-10m", "nodes-5m-relations-50m",
        "nodes-10m-relations-100m", "nodes-10m-relations-500m",
    }


def test_generator_is_deterministic_and_reports_caps(tmp_path):
    first = generate(MANIFEST, "synthetic", "smoke", tmp_path / "first.db")
    second = generate(MANIFEST, "synthetic", "smoke", tmp_path / "second.db")
    assert first["semantic_digest"] == second["semantic_digest"]
    assert first["counts"] == {
        "nodes": 128,
        "relations": 512,
        "evidence": 1536,
        "files": 4,
        "repositories": 1,
    }
    assert first["requested"] == {"nodes": 128, "relations": 512}
    assert first["distribution"] == "balanced"
    capped = generate(
        MANIFEST, "synthetic", "nodes-1m-relations-10m", tmp_path / "capped.db",
        max_rows=256, evidence_max_rows=512,
    )
    assert capped["counts"]["nodes"] == 256
    assert capped["counts"]["relations"] == 256
    assert capped["materialization_cap"]["rows"] == 256


def test_runner_captures_storage_query_and_integrity_evidence(tmp_path):
    db = tmp_path / "benchmark.db"
    generate(MANIFEST, "synthetic", "smoke", db)
    result = run(db, MANIFEST, "synthetic", PROFILE)
    assert result["schema_version"] == 34
    assert result["storage"]["inspection"] in {"dbstat", "page_count"}
    assert {item["id"] for item in result["queries"]} >= {
        "exact_identity", "name_prefix", "bounded_paths", "cxq_representative",
    }
    exact = next(item for item in result["queries"] if item["id"] == "exact_identity")
    assert exact["sql"]
    assert exact["parameters"] == [1]
    assert exact["plan"]
    assert exact["truncated"] is False
    assert result["operations"]["recovery"]["status"] == "ok"
    assert result["operations"]["cold_build"]["status"] == "ok"
    assert result["counters"]["prepare_count"] > 0
    assert result["operations"]["warm_noop"]["status"] == "not_run"


def test_bad_layout_preserves_semantics_and_is_identifiable(tmp_path):
    baseline_db = tmp_path / "baseline.db"
    bad_db = tmp_path / "bad.db"
    baseline = generate(MANIFEST, "synthetic", "smoke", baseline_db)
    bad = drop_hot_indexes(baseline_db, bad_db)
    assert bad["configuration"] == "drop_hot_indexes"
    assert bad["semantic_digest"] == baseline["semantic_digest"]
    assert "idx_symbol_qual_nc" not in bad["remaining_indexes"]


def test_recovery_never_presents_uncommitted_generation_as_current(tmp_path):
    db = tmp_path / "benchmark.db"
    generate(MANIFEST, "synthetic", "smoke", db)
    recovery = simulate(db)
    assert recovery["status"] == "pass"
    assert recovery["rollback_before_commit"]["presented_as_current"] is True
    assert recovery["committed_building_state"]["presented_as_current"] is False
    assert recovery["repaired"]["presented_as_current"] is True


def test_gate_requires_exact_failed_slo_before_custom_store_proposal(tmp_path):
    db = tmp_path / "benchmark.db"
    generate(MANIFEST, "synthetic", "smoke", db)
    result = run(db, MANIFEST, "synthetic", PROFILE)
    profile = load_json(PROFILE)
    baseline = copy.deepcopy(result)
    bad_config = copy.deepcopy(result)
    bad_config["run_id"] = "bad-config"
    bad_config["configuration"] = "drop_hot_indexes"
    for query in bad_config["queries"]:
        query["latency_ms"]["p95_ms"] = (query["latency_ms"]["p95_ms"] or 1) * 2
    regression = evaluate_regression(baseline, bad_config, profile)
    assert regression["status"] == "pass"
    hold = evaluate_custom_store({"decision": "hold"}, require=True)
    assert hold["status"] == "pass"
    proposal = evaluate_custom_store({"decision": "propose"}, require=True)
    assert proposal["status"] == "fail"
    gate = evaluate(
        result, profile, baseline=baseline, bad_config=bad_config,
        decision={"decision": "hold"}, require_custom_store=True,
    )
    assert gate["status"] == "pass"
