from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

from indexer import __version__
from indexer._version import CATALOG_HASH, DATABASE_SCHEMA_VERSION
from indexer.clang import LegacyPythonExtractionWarning
from indexer.clang import ast as legacy_ast

ROOT = Path(__file__).parents[2]


def test_generated_version_and_release_contract_are_current():
    source = json.loads((ROOT / "spec/platform/version.json").read_text())
    assert __version__ == source["product"]["version"]
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


def test_legacy_python_extraction_warns(monkeypatch):
    sentinel = object()
    monkeypatch.setattr(legacy_ast, "parse", lambda *args, **kwargs: sentinel)
    monkeypatch.setattr(legacy_ast, "collect_diagnostics", lambda *args: [])
    monkeypatch.setattr(legacy_ast, "index_symbols", lambda *args: (1, 2))
    monkeypatch.setattr(legacy_ast, "index_headers", lambda *args, **kwargs: 3)
    monkeypatch.setattr(legacy_ast, "index_edges", lambda *args, **kwargs: None)

    with pytest.warns(LegacyPythonExtractionWarning, match="C\\+\\+23 LibTooling"):
        result = legacy_ast.index_source(None, "fixture.cpp", [], 1)

    assert result == {
        "symbols": 1,
        "skipped": 2,
        "headers": 3,
        "diagnostics": [],
    }
