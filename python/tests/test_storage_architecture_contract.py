import json
from pathlib import Path


ROOT = Path(__file__).parents[2]


def load_contract() -> dict:
    return json.loads((ROOT / "docs/storage/architecture-v1.json").read_text())


def load_version_contract() -> dict:
    return json.loads((ROOT / "spec/platform/version.json").read_text())


def test_storage_architecture_contract_is_versioned_and_authoritative():
    contract = load_contract()
    database = load_version_contract()["database"]

    assert contract["architecture_version"] == "storage-v1"
    assert contract["authority"] == "sqlite-core"
    assert contract["core"]["default_file"] == "index.db"
    assert contract["core"]["authoritative"] is True
    assert contract["core"]["single_database_default"] is True
    assert contract["schema"]["current"] == database["schema_version"]
    assert contract["schema"]["migration_floor"] == database["migration_floor"]
    assert contract["schema"]["reader_min"] == database["reader_min"]
    assert contract["schema"]["reader_max"] == database["reader_max"]
    assert contract["schema"]["qualification_baseline"] == 34
    assert contract["schema"]["source"] == "spec/platform/version.json#/database"
    assert contract["schema"]["migration"] == "deterministic"


def test_storage_physical_classes_preserve_typed_slots_and_evidence():
    classes = load_contract()["physical_classes"]

    assert set(classes["ordered_slot"]) == {
        "parameter",
        "template_param",
        "template_arg",
        "call_arg",
    }
    assert "edge_site" in classes["evidence"]
    assert "call_arg" not in classes["relation"]
    assert "template_arg" not in classes["relation"]


def test_storage_sidecars_are_manifest_governed_and_non_authoritative():
    sidecars = load_contract()["sidecars"]

    assert sidecars["allowed"] is True
    assert sidecars["attach_mode"] == "controlled_read_only"
    assert sidecars["unmanifested_attach"] is False
    assert sidecars["portable_integer_ids"] is False
    assert sidecars["core_fallback"] is True
    assert {
        "content_hash",
        "input_identity",
        "completeness",
    } <= set(sidecars["required_properties"])


def test_storage_qualification_does_not_overclaim_deferred_workloads():
    qualification = load_contract()["qualification"]

    assert "v34 synthetic smoke on local-macos-v1" in qualification["measured"]
    assert "cpp-indexer self-index" in qualification["deferred"]
    assert "banking corpus" in qualification["deferred"]
    assert "custom primary store" in qualification["deferred"]
    assert len(qualification["custom_store_gate"]) == 4


def test_storage_contract_references_exist():
    references = load_contract()["references"]

    for relative_path in references.values():
        assert (ROOT / relative_path).exists(), relative_path
