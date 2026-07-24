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
        self.module_manifest = json.loads(
            (ROOT / "architecture/cidx-module-manifest.json").read_text(encoding="utf-8")
        )

    def _errors(self, contract: dict | None = None) -> list[str]:
        return validate_contract(contract or self.contract, self.module_manifest)

    def test_checked_in_contract_passes(self) -> None:
        self.assertEqual(self._errors(), [])

    def test_missing_layer_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.contract)
        mutated["layers"] = [layer for layer in mutated["layers"] if layer["id"] != "model"]
        errors = self._errors(mutated)
        self.assertTrue(any("missing layers" in error for error in errors))

    def test_layer_cycle_is_rejected(self) -> None:
        mutated = copy.deepcopy(self.contract)
        mutated["layers"][0]["depends_on"] = ["product"]
        errors = self._errors(mutated)
        self.assertTrue(any("dependency cycle" in error for error in errors))

    def test_artifact_provenance_is_required(self) -> None:
        mutated = copy.deepcopy(self.contract)
        mutated["artifact_contract"]["required_fields"].remove("source_revision")
        errors = self._errors(mutated)
        self.assertTrue(any("missing artifact fields" in error for error in errors))

    def test_module_mapping_rejects_forbidden_manifest_edge(self) -> None:
        mutated = copy.deepcopy(self.contract)
        derivation = next(layer for layer in mutated["layers"] if layer["id"] == "derivation")
        derivation["depends_on"].remove("persistence")
        errors = self._errors(mutated)
        self.assertTrue(any("platform graph rejects module dependency" in error for error in errors))

    def test_external_ownership_is_fail_closed(self) -> None:
        mutated = copy.deepcopy(self.contract)
        mutated["external_ownership"]["sqlite"] = ["product"]
        errors = self._errors(mutated)
        self.assertTrue(any("external ownership for sqlite" in error for error in errors))

    def test_forbidden_dependency_policy_is_fail_closed(self) -> None:
        mutated = copy.deepcopy(self.contract)
        mutated["forbidden_dependencies"]["model"] = ["banana"]
        errors = self._errors(mutated)
        self.assertTrue(any("forbidden dependency boundary model" in error for error in errors))

    def test_extension_safety_policy_is_fail_closed(self) -> None:
        mutated = copy.deepcopy(self.contract)
        mutated["forbidden_extension_behaviors"] = []
        errors = self._errors(mutated)
        self.assertTrue(any("forbidden_extension_behaviors" in error for error in errors))

    def test_port_owner_and_uniqueness_are_enforced(self) -> None:
        bad_owner = copy.deepcopy(self.contract)
        bad_owner["ports"][0]["owner"] = "unknown"
        errors = self._errors(bad_owner)
        self.assertTrue(any("WorkspaceSnapshot" in error for error in errors))

        duplicate = copy.deepcopy(self.contract)
        duplicate["ports"].append(copy.deepcopy(duplicate["ports"][0]))
        errors = self._errors(duplicate)
        self.assertTrue(any("ports must have unique ids" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
