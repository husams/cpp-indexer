"""Hermetic contract tests for the Storage M0 benchmark foundation."""

from __future__ import annotations

import copy
import json
import shutil
import sqlite3
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from benchmarks.storage_m0.common import canonical_json, load_json, sha256  # noqa: E402
from benchmarks.storage_m0.bad_config import drop_hot_indexes  # noqa: E402
from benchmarks.storage_m0.gate import evaluate, evaluate_custom_store, evaluate_regression  # noqa: E402
from benchmarks.storage_m0.generator import _pair, generate  # noqa: E402
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
        "files": 6,
        "translation_units": 4,
        "shared_headers": 2,
        "repositories": 1,
        "types": 128,
        "entity_nodes": 128,
        "type_edges": 128,
        "entity_edges": 512,
        "include_edges": 6,
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
    result = run(db, MANIFEST, "synthetic", PROFILE, output=tmp_path / "result.json")
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
    assert result["operations"]["recovery"]["wal_boundaries"]["status"] == "pass"
    assert result["operations"]["cold_build"]["status"] == "ok"
    assert result["counters"]["prepare_count"] is None
    assert result["counters"]["prepare_step_counters"]["status"] == "unsupported"
    assert all(result["operations"][name]["status"] == "ok" for name in (
        "warm_noop", "changed_tu_update", "transform_rebuild", "migration", "backup",
    ))
    assert result["operations"]["migration"]["current_state"]["status"] == "ok"
    assert result["operations"]["migration"]["current_state"]["semantic_digest_matches"] is True
    assert result["gates"]["semantic_equivalence"] is True


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
    assert recovery["committed_building_state"]["wal_bytes"] > 0
    assert recovery["repaired"]["checkpointed"] is True
    assert recovery["repaired"]["presented_as_current"] is True
    assert recovery["digest_mismatch_not_current"]["presented_as_current"] is False
    assert recovery["digest_mismatch_not_current"]["status"] == "pass"


def test_all_declared_distributions_have_exact_distinct_cardinality(tmp_path):
    manifest = load_json(MANIFEST)
    digests = set()
    for item in manifest["distributions"]:
        result = generate(MANIFEST, "synthetic", "smoke", tmp_path / f"{item['id']}.db", distribution=item["id"])
        digests.add(result["semantic_digest"])
        assert result["distribution"] == item["id"]
        assert result["counts"]["nodes"] == result["requested"]["nodes"] == 128
        assert result["counts"]["relations"] == result["requested"]["relations"] == 512
        connection = sqlite3.connect(result["output"])
        try:
            assert connection.execute("SELECT COUNT(*) FROM (SELECT src_id,dst_id,kind FROM edge GROUP BY src_id,dst_id,kind)").fetchone()[0] == 512
            assert connection.execute("SELECT COUNT(*) FROM type_node").fetchone()[0] > 0
            assert connection.execute("SELECT COUNT(*) FROM entity_node").fetchone()[0] > 0
            assert connection.execute("SELECT COUNT(*) FROM include_edge").fetchone()[0] > 0
        finally:
            connection.close()
    assert len(digests) == len(manifest["distributions"])
    pairs = {item["id"]: [_pair(index, 128, item["id"], 20260723) for index in range(512)] for item in manifest["distributions"]}
    assert sum((dst - src) % 128 == 1 for src, dst in pairs["long-chain"]) == 128
    assert max(src for src, _ in pairs["skewed"]) > 0 and max(pairs["skewed"].count((src, dst)) for src, dst in pairs["skewed"]) == 1
    assert max(pairs["skewed"].count((source, target)) for source, target in pairs["skewed"]) == 1
    skewed_out = {source: sum(1 for left, _ in pairs["skewed"] if left == source) for source in range(128)}
    assert max(skewed_out.values()) >= 4 * min(skewed_out.values())
    diamond = set(pairs["diamond"])
    assert sum({(base, base + 1), (base, base + 2), (base + 1, base + 3), (base + 2, base + 3)} <= diamond for base in range(0, 128, 4)) == 32
    assert max(sum(1 for source, _ in pairs["high-fan-out"] if source == node) for node in range(128)) > 20
    assert max(sum(1 for _, target in pairs["high-fan-in"] if target == node) for node in range(128)) > 20


def test_composed_multi_repository_topology_is_materialized(tmp_path):
    result = generate(MANIFEST, "synthetic-multi-repo", "smoke", tmp_path / "multi.db")
    assert result["counts"]["repositories"] == 2
    assert result["counts"]["shared_headers"] == 8
    assert result["counts"]["translation_units"] == 8
    assert result["counts"]["include_edges"] > 0


def test_semantic_digest_covers_const_values_and_not_surrogate_ids(tmp_path):
    original = tmp_path / "original.db"
    generate(MANIFEST, "synthetic", "smoke", original)
    connection = sqlite3.connect(original)
    try:
        connection.execute("UPDATE symbol SET const_value='7' WHERE id=(SELECT MIN(id) FROM symbol)")
        connection.commit()
    finally:
        connection.close()
    from benchmarks.storage_m0.common import semantic_digest
    assert semantic_digest(original) != generate(MANIFEST, "synthetic", "smoke", tmp_path / "fresh.db")["semantic_digest"]


def test_regression_rejects_missing_query_and_identity_mismatch(tmp_path):
    db = tmp_path / "benchmark.db"
    generate(MANIFEST, "synthetic", "smoke", db)
    result = run(db, MANIFEST, "synthetic", PROFILE, output=tmp_path / "baseline.json")
    profile = load_json(PROFILE)
    candidate = copy.deepcopy(result)
    candidate["queries"] = candidate["queries"][:-1]
    assert evaluate_regression(result, candidate, profile)["status"] == "fail"
    candidate = copy.deepcopy(result)
    candidate["identity"]["hardware_fingerprint"] = "different"
    assert evaluate_regression(result, candidate, profile)["status"] == "fail"


def test_gate_requires_exact_failed_slo_before_custom_store_proposal(tmp_path):
    db = tmp_path / "benchmark.db"
    generate(MANIFEST, "synthetic", "smoke", db)
    result = run(db, MANIFEST, "synthetic", PROFILE, output=tmp_path / "baseline.json")
    profile = load_json(PROFILE)
    baseline = copy.deepcopy(result)
    bad_config = copy.deepcopy(result)
    bad_db = tmp_path / "bad.db"
    drop_hot_indexes(db, bad_db)
    bad_config = run(bad_db, MANIFEST, "synthetic", PROFILE, output=tmp_path / "bad.json", configuration="drop_hot_indexes")
    assert baseline["run_id"] != bad_config["run_id"]
    untrusted = copy.deepcopy(bad_config)
    for query in untrusted["queries"]:
        query["latency_ms"]["p95_ms"] = (query["latency_ms"]["p95_ms"] or 1) * 2
    assert evaluate_regression(baseline, untrusted, profile)["status"] == "fail"
    disk_tampered = load_json(tmp_path / "bad.json")
    disk_tampered["queries"][0]["latency_ms"]["p95_ms"] = (disk_tampered["queries"][0]["latency_ms"]["p95_ms"] or 1) * 2
    (tmp_path / "bad.json").write_text(json.dumps(disk_tampered, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
    assert evaluate_regression(baseline, load_json(tmp_path / "bad.json"), profile)["status"] == "fail"
    hold = evaluate_custom_store({"decision": "hold"}, require=True)
    assert hold["status"] == "pass"
    proposal = evaluate_custom_store({"decision": "propose"}, require=True)
    assert proposal["status"] == "fail"
    fabricated = {
        "decision": "propose", "result_run_id": result["run_id"],
        "failed_slos": ["query.fake"],
        "alternatives": [
            {"class": "schema_tuning", "measured": True, "evidence": {"run_id": "fake", "result_run_id": result["run_id"], "artifact": "/tmp/missing", "checks": [{"id": "fake", "status": "pass", "actual": 1}], "outcome": {"measured": True, "tested_failed_slos": ["query.fake"]}}},
            {"class": "derived_accelerator", "measured": True, "evidence": {"run_id": "fake", "result_run_id": result["run_id"], "artifact": "/tmp/missing", "checks": [{"id": "fake", "status": "pass", "actual": 1}], "outcome": {"measured": True, "tested_failed_slos": ["query.fake"]}}},
        ],
        "costs": {"engineering": {"person_months": 1, "source_artifact": "/tmp/missing"}, "compatibility": {"migration_plan": "fake", "source_artifact": "/tmp/missing"}},
    }
    assert evaluate_custom_store(fabricated, require=True, result=result, slo_checks=[{"id": "query.fake", "status": "fail"}])["status"] == "fail"
    same_valid_result = copy.deepcopy(fabricated)
    for alternative in same_valid_result["alternatives"]:
        alternative["evidence"] = {"run_id": result["run_id"], "result_run_id": result["run_id"], "artifact": str(tmp_path / "baseline.json"), "checks": [{"id": "alternative.slo", "status": "pass", "actual": 1}], "outcome": {"measured": True, "measured_inability": True, "slo_status": "fail", "configuration": result["configuration"], "tested_failed_slos": ["query.fake"]}}
    same_valid_result["costs"] = {"engineering": {"person_months": 1, "work_items": [{"id": "x"}], "source_artifact": str(MANIFEST)}, "compatibility": {"person_months": 1, "migration_plan": [{"id": "x"}], "compatibility_checks": [{"id": "x"}], "source_artifact": str(MANIFEST)}}
    assert evaluate_custom_store(same_valid_result, require=True, result=result, slo_checks=[{"id": "query.fake", "status": "fail"}])["status"] == "fail"
    gate = evaluate(
        result, profile, baseline=baseline, bad_config=bad_config,
        decision={"decision": "hold"}, require_custom_store=True,
    )
    assert gate["status"] == "fail"


def test_custom_store_rejects_duplicate_alternative_and_placeholder_costs(tmp_path):
    baseline_db = tmp_path / "baseline.db"
    generate(MANIFEST, "synthetic", "smoke", baseline_db)
    result = run(baseline_db, MANIFEST, "synthetic", PROFILE, output=tmp_path / "baseline.json")
    schema_db = tmp_path / "schema.db"
    drop_hot_indexes(baseline_db, schema_db)
    schema_result = run(schema_db, MANIFEST, "synthetic", PROFILE, output=tmp_path / "schema.json", configuration="drop_hot_indexes")
    accelerator_db = tmp_path / "accelerator.db"
    shutil.copy2(baseline_db, accelerator_db)
    with sqlite3.connect(accelerator_db) as connection:
        connection.execute("UPDATE benchmark_meta SET value='derived_accelerator' WHERE key='configuration'")
        connection.commit()
    accelerator_result = run(accelerator_db, MANIFEST, "synthetic", PROFILE, output=tmp_path / "accelerator.json", configuration="derived_accelerator")
    failed = ["query.fake"]

    def evidence(item_class, alternative, artifact):
        digest = sha256(canonical_json(alternative))
        return {
            "class": item_class,
            "measured": True,
            "evidence": {
                "run_id": alternative["run_id"], "result_run_id": result["run_id"], "artifact": str(artifact),
                "content_sha256": digest, "configuration": alternative["configuration"],
                "checks": [{"id": f"{item_class}.measured", "status": "pass", "actual": "measured", "target": "target"}],
                "outcome": {"measured": True, "measured_inability": True, "slo_status": "fail", "alternative_class": item_class, "run_id": alternative["run_id"], "content_sha256": digest, "configuration": alternative["configuration"], "tested_failed_slos": failed},
            },
        }

    costs = {
        "engineering": {"person_months": 1.5, "work_items": [{"id": "schema-work", "description": "Implement and benchmark the schema alternative", "person_months": 1.5, "deliverables": ["measured schema result"]}], "source_artifact": str(MANIFEST)},
        "compatibility": {"person_months": 0.5, "migration_plan": [{"id": "migration-work", "description": "Validate migration and compatibility paths", "person_months": 0.5, "deliverables": ["migration validation"]}], "compatibility_checks": [{"id": "api-compatibility", "status": "pass", "actual": "v34 query set", "target": "unchanged query set"}], "source_artifact": str(MANIFEST)},
    }
    valid = {"decision": "propose", "result_run_id": result["run_id"], "failed_slos": failed, "alternatives": [evidence("schema_tuning", schema_result, tmp_path / "schema.json"), evidence("derived_accelerator", accelerator_result, tmp_path / "accelerator.json")], "costs": costs}
    assert evaluate_custom_store(valid, require=True, result=result, slo_checks=[{"id": failed[0], "status": "fail"}])["status"] == "pass"

    duplicate_path = tmp_path / "duplicate.json"
    shutil.copy2(tmp_path / "schema.json", duplicate_path)
    duplicate = copy.deepcopy(valid)
    duplicate["alternatives"][1] = evidence("derived_accelerator", schema_result, duplicate_path)
    assert evaluate_custom_store(duplicate, require=True, result=result, slo_checks=[{"id": failed[0], "status": "fail"}])["status"] == "fail"

    placeholder = copy.deepcopy(valid)
    placeholder["costs"]["engineering"]["person_months"] = "placeholder"
    assert evaluate_custom_store(placeholder, require=True, result=result, slo_checks=[{"id": failed[0], "status": "fail"}])["status"] == "fail"
