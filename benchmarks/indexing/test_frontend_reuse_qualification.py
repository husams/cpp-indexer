from __future__ import annotations

import unittest

from benchmarks.indexing.frontend_reuse import (
    IDENTITY_VERSION,
    none_identity,
    qualification_contract,
    retain_trials,
)


class FrontEndReuseQualificationTest(unittest.TestCase):
    def test_identity_is_deterministic_and_versioned(self) -> None:
        configuration = {"driver": "clang++", "arguments": ["-std=c++23"]}
        first = none_identity(configuration)
        second = none_identity({"arguments": ["-std=c++23"], "driver": "clang++"})
        self.assertEqual(first, second)
        self.assertEqual(first["version"], IDENTITY_VERSION)
        self.assertNotEqual(first["sha256"], none_identity({"driver": "clang"})["sha256"])

    def test_trial_retention_keeps_trials_and_medians(self) -> None:
        fields = {
            "wall_seconds": 3.0,
            "cpu_seconds": 2.0,
            "peak_rss_bytes": 100,
            "clang_front_end_seconds": 1.5,
            "disk_bytes": 20,
        }
        report = retain_trials(
            [{**fields, "wall_seconds": value} for value in (3.0, 1.0, 2.0)]
        )
        self.assertEqual(report["trial_count"], 3)
        self.assertEqual(report["medians"]["wall_seconds"], 2.0)
        self.assertEqual(len(report["trials"]), 3)

    def test_contract_has_candidate_matrix_and_none_path(self) -> None:
        report = qualification_contract()
        self.assertEqual(report["mechanism"], "none")
        self.assertFalse(report["artifact_construction"])
        self.assertFalse(report["artifact_injection"])
        self.assertEqual(
            [candidate["name"] for candidate in report["candidates"]],
            ["no-reuse", "generated-umbrella-pch", "precompiled-preamble-astunit"],
        )
        self.assertTrue(
            all("requires_adr" in candidate for candidate in report["candidates"])
        )

    def test_explicit_disable_is_recorded_in_production_contract(self) -> None:
        report = qualification_contract(disabled=True)
        self.assertTrue(report["explicitly_disabled"])
        self.assertEqual(report["mechanism"], "none")
        self.assertFalse(report["artifact_construction"])
        self.assertFalse(report["artifact_injection"])


if __name__ == "__main__":
    unittest.main()
