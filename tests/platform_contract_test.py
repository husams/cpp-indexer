#!/usr/bin/env python3
"""Positive and mutation tests for the platform architecture contract."""

from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.check_platform_contract import validate_contract  # noqa: E402


class PlatformContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.contract = json.loads((ROOT / "spec/platform/architecture.json").read_text(encoding="utf-8"))

    def test_checked_in_contract_passes(self) -> None:
        self.assertEqual(validate_contract(self.contract), [])

    def test_missing_layer_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.contract)
        mutated["layers"] = [layer for layer in mutated["layers"] if layer["id"] != "model"]
        errors = validate_contract(mutated)
        self.assertTrue(any("missing layers" in error for error in errors))

    def test_layer_cycle_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.contract)
        mutated["layers"][0]["depends_on"] = ["product"]
        errors = validate_contract(mutated)
        self.assertTrue(any("dependency cycle" in error for error in errors))

    def test_artifact_provenance_is_required(self) -> None:
        mutated = copy.deepcopy(self.contract)
        mutated["artifact_contract"]["required_fields"].remove("source_revision")
        errors = validate_contract(mutated)
        self.assertTrue(any("missing artifact fields" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
