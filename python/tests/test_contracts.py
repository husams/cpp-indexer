from __future__ import annotations

import copy
import importlib.util
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from indexer import __version__
from indexer._version import BASE_VERSION, CATALOG_HASH, DATABASE_SCHEMA_VERSION

ROOT = Path(__file__).parents[2]


def _generator_module():
    spec = importlib.util.spec_from_file_location(
        "cidx_generate_contracts", ROOT / "scripts/generate_contracts.py"
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_generated_version_and_release_contract_are_current():
    source = json.loads((ROOT / "spec/platform/version.json").read_text())
    assert __version__ == source["product"]["version"]
    assert BASE_VERSION == source["product"]["version"]
    assert DATABASE_SCHEMA_VERSION == source["database"]["schema_version"]
    assert CATALOG_HASH.startswith("sha256:")
    result = subprocess.run(
        [sys.executable, "scripts/check_release_contract.py"],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr


def test_prerelease_identity_reaches_package_and_binary_outputs():
    generator = _generator_module()
    source = json.loads((ROOT / "spec/platform/version.json").read_text())
    mutated = copy.deepcopy(source)
    mutated["product"]["prerelease"] = {"tag": "rc1"}
    generator.validate_contract(mutated)
    digest = generator.catalog_hash(mutated)

    python_output = generator.render_python(mutated, digest)
    cpp_output = generator.render_cpp(mutated, digest)
    assert '__version__ = "0.53.0-rc1"' in python_output
    assert 'FULL_VERSION = "0.53.0-rc1"' in python_output
    assert 'kProductVersion = "0.53.0-rc1"' in cpp_output
    assert 'kFullProductVersion = "0.53.0-rc1"' in cpp_output


def test_compatibility_windows_reject_impossible_ranges():
    generator = _generator_module()
    source = json.loads((ROOT / "spec/platform/version.json").read_text())

    impossible_database = copy.deepcopy(source)
    impossible_database["database"].update(reader_min=35, reader_max=34)
    with pytest.raises(ValueError, match="database reader_min"):
        generator.validate_contract(impossible_database)

    out_of_range_artifact = copy.deepcopy(source)
    out_of_range_artifact["artifact"].update(reader_min=2, reader_max=2)
    with pytest.raises(ValueError, match="artifact current version"):
        generator.validate_contract(out_of_range_artifact)

    impossible_api = copy.deepcopy(source)
    impossible_api["api"].update(reader_min=2, reader_max=1)
    with pytest.raises(ValueError, match="api reader_min"):
        generator.validate_contract(impossible_api)


def test_catalog_tamper_changes_release_digest(tmp_path):
    generator = _generator_module()
    source = json.loads((ROOT / "spec/platform/version.json").read_text())
    for relative in source["catalog"]["inputs"]:
        destination = tmp_path / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, destination)

    before = generator.catalog_hash(source, tmp_path)
    tampered = tmp_path / "spec/contracts/golden/catalog.json"
    catalog = json.loads(tampered.read_text())
    catalog["entries"][0]["id"] = 999
    tampered.write_text(json.dumps(catalog, indent=2) + "\n")

    assert generator.catalog_hash(source, tmp_path) != before
