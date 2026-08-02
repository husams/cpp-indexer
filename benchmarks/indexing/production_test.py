"""Pure report/threshold tests for the S-098 routed root-fusion decision.

Everything here reads the checked-in compact S-071 fixture or an in-memory
mutation of it. No cidx binary, corpus, database, or backlog store is required,
so the suite is deterministic and offline.
"""

from __future__ import annotations

import ast
import copy
import hashlib
import inspect
import json
from pathlib import Path
import tempfile
from typing import Any
import unittest

from benchmarks.indexing import production


BASELINE_CASE = production.S098_BASELINE_CASE
BASELINE_STAGE = production.S098_BASELINE_STAGE
REPOSITORY_ROOT = Path(__file__).resolve().parents[2]


def _fixture() -> dict:
    return production.load_baseline_fixture()


def _stage(report: dict, case: str = BASELINE_CASE) -> dict:
    return report["aggregates"][case]["stages"][BASELINE_STAGE]


def _timing(report: dict, name: str) -> dict:
    return _stage(report)["profile_summary"]["timings"][name]


def _counter(report: dict, name: str) -> dict:
    return _stage(report)["profile_summary"]["counters"][name]


def _under_budget(report: dict) -> dict:
    """Scale the routed-root timings so the candidate clears 25 ms."""
    for name in production.S098_FIXED_ROOT_TIMINGS:
        entry = _timing(report, name)
        entry["trials"] = [value / 4.0 for value in entry["trials"]]
    return report


def _set_fixed_root(report: dict, seconds: float) -> dict:
    """Pin every trial's fixed routed-root total to exactly `seconds`."""
    for index, name in enumerate(production.S098_FIXED_ROOT_TIMINGS):
        entry = _timing(report, name)
        share = seconds if index == 0 else 0.0
        entry["trials"] = [share for _ in entry["trials"]]
    return report


class BaselineFixtureTest(unittest.TestCase):
    """AC #3180 - the frozen S-071 evidence the whole story is measured against."""

    def setUp(self) -> None:
        self.observation = production.root_observation(_fixture())

    def test_exactly_three_parity_clean_header_heavy_trials(self) -> None:
        self.assertEqual(self.observation["case"], "header-heavy:8:forward")
        self.assertEqual(self.observation["stage"], "cold")
        self.assertEqual(self.observation["trial_count"], 3)
        self.assertEqual(self.observation["parity_failures"], [])
        self.assertTrue(self.observation["semantic_parity"])
        self.assertTrue(self.observation["authoritative_timing"])
        self.assertTrue(self.observation["quiescent"])
        self.assertEqual(len(set(self.observation["canonical_sha256_trials"])), 1)

    def test_median_fixed_root_and_component_times(self) -> None:
        self.assertEqual(len(self.observation["fixed_root_trials"]), 3)
        self.assertAlmostEqual(
            self.observation["fixed_root_median"], 0.040611, places=9
        )
        self.assertAlmostEqual(
            self.observation["symbol_root_median"], 0.031091, places=9
        )
        self.assertAlmostEqual(
            self.observation["other_root_median"], 0.009520, places=9
        )
        self.assertAlmostEqual(
            self.observation["symbol_root_median"]
            + self.observation["other_root_median"],
            self.observation["fixed_root_median"],
            places=9,
        )

    def test_median_cold_wall_time(self) -> None:
        self.assertAlmostEqual(
            self.observation["wall_seconds_median"], 0.255732833, places=9
        )

    def test_registered_and_observed_visits(self) -> None:
        self.assertEqual(
            self.observation["registered_root_traversal_budget_trials"], [32, 32, 32]
        )
        self.assertEqual(
            self.observation["observed_root_traversal_trials"], [32, 32, 32]
        )
        self.assertEqual(self.observation["registered_root_traversal_budget"], 32)
        self.assertEqual(self.observation["observed_root_traversals"], 32)
        self.assertEqual(self.observation["traversal_budget_exceeded_trials"], [])

    def test_frozen_baseline_is_not_itself_ship_eligible(self) -> None:
        decision = production.fusion_decision(_fixture())
        self.assertFalse(decision["ship_eligible"])
        self.assertEqual(decision["reason"], "fixed-root-budget-not-met")
        self.assertAlmostEqual(
            decision["baseline_fixed_root_median_seconds"], 0.040611, places=9
        )


class ShipEligibilityTest(unittest.TestCase):
    """AC #3181 - ship-eligible only when every gate passes."""

    def test_all_gates_pass(self) -> None:
        decision = production.fusion_decision(_under_budget(_fixture()))
        self.assertTrue(decision["ship_eligible"])
        self.assertIsNone(decision["reason"])
        self.assertEqual(decision["reasons"], [])
        self.assertLess(
            decision["fixed_root_median_seconds"],
            production.S098_FIXED_ROOT_BUDGET_SECONDS,
        )
        self.assertGreater(decision["fixed_root_improvement_seconds"], 0.0)

    def test_more_than_three_trials_still_qualify(self) -> None:
        report = _under_budget(_fixture())
        stage = _stage(report)
        stage["wall_seconds_trials"].append(stage["wall_seconds_trials"][0])
        stage["canonical_sha256_trials"].append(stage["canonical_sha256_trials"][0])
        for name in production.S098_FIXED_ROOT_TIMINGS:
            entry = _timing(report, name)
            entry["trials"].append(entry["trials"][0])
        for name in (
            production.S098_REGISTERED_BUDGET_COUNTER,
            production.S098_OBSERVED_TRAVERSAL_COUNTER,
        ):
            entry = _counter(report, name)
            entry["trials"].append(entry["trials"][0])
        report["aggregates"][BASELINE_CASE]["trial_count"] = 4
        decision = production.fusion_decision(report)
        self.assertTrue(decision["ship_eligible"])
        self.assertEqual(decision["candidate"]["trial_count"], 4)

    def test_thresholds_are_recorded(self) -> None:
        decision = production.fusion_decision(_under_budget(_fixture()))
        self.assertEqual(decision["fixed_root_budget_seconds"], 0.025)
        self.assertTrue(decision["fixed_root_budget_is_strict"])
        self.assertEqual(decision["symbol_root_budget_seconds"], 0.015480)
        self.assertEqual(decision["symbol_root_budget_owner"], "T-139")
        self.assertIn("symbol_root_budget_met", decision)
        self.assertEqual(decision["minimum_trials"], 3)
        self.assertEqual(decision["contract"], "s098-root-fusion/v1")

    def test_symbol_budget_is_reported_but_does_not_gate(self) -> None:
        report = _fixture()
        # Total below 25 ms while root_symbols alone exceeds the T-139 budget.
        _set_fixed_root(report, 0.0)
        _timing(report, "root_symbols")["trials"] = [0.0200, 0.0200, 0.0200]
        decision = production.fusion_decision(report)
        self.assertTrue(decision["ship_eligible"])
        self.assertFalse(decision["symbol_root_budget_met"])
        self.assertAlmostEqual(decision["symbol_root_median_seconds"], 0.02, places=9)


class RejectionReasonTest(unittest.TestCase):
    """AC #3182 - every rejection is deterministic and named."""

    def assert_rejected(self, report: dict, reason: str) -> dict:
        decision = production.fusion_decision(report)
        self.assertFalse(decision["ship_eligible"], msg=f"expected {reason}")
        self.assertEqual(decision["reason"], reason)
        self.assertIn(reason, decision["reasons"])
        self.assertEqual(
            decision, production.fusion_decision(copy.deepcopy(report))
        )
        return decision

    def test_malformed_report(self) -> None:
        report = _under_budget(_fixture())
        del _stage(report)["profile_summary"]["timings"]["root_definitions"]
        self.assert_rejected(report, "malformed-report")

    def test_malformed_report_is_not_a_mapping(self) -> None:
        not_a_report: Any = ["not", "a", "report"]
        decision = production.fusion_decision(not_a_report)
        self.assertFalse(decision["ship_eligible"])
        self.assertEqual(decision["reason"], "malformed-report")

    def test_malformed_trial_series_lengths_disagree(self) -> None:
        report = _under_budget(_fixture())
        _timing(report, "root_namespaces")["trials"].pop()
        self.assert_rejected(report, "malformed-report")

    def test_malformed_declared_trial_count_disagrees(self) -> None:
        report = _under_budget(_fixture())
        report["aggregates"][BASELINE_CASE]["trial_count"] = 5
        self.assert_rejected(report, "malformed-report")

    def test_contended_host(self) -> None:
        report = _under_budget(_fixture())
        report["host_quiescence"]["quiescent"] = False
        report["host_quiescence"]["competing_cidx_indexers"] = ["4242 cidx index"]
        decision = self.assert_rejected(report, "host-not-quiescent")
        self.assertEqual(
            decision["candidate"]["competing_cidx_indexers"], ["4242 cidx index"]
        )

    def test_non_authoritative_timing(self) -> None:
        report = _under_budget(_fixture())
        report["authoritative_timing"] = False
        self.assert_rejected(report, "host-not-quiescent")

    def test_incomplete_trial_count(self) -> None:
        report = _under_budget(_fixture())
        stage = _stage(report)
        stage["wall_seconds_trials"].pop()
        stage["canonical_sha256_trials"].pop()
        for name in production.S098_FIXED_ROOT_TIMINGS:
            _timing(report, name)["trials"].pop()
        for name in (
            production.S098_REGISTERED_BUDGET_COUNTER,
            production.S098_OBSERVED_TRAVERSAL_COUNTER,
        ):
            _counter(report, name)["trials"].pop()
        report["aggregates"][BASELINE_CASE]["trial_count"] = 2
        decision = self.assert_rejected(report, "insufficient-trials")
        self.assertEqual(decision["candidate"]["trial_count"], 2)

    def test_parity_failure_recorded_on_the_report(self) -> None:
        report = _under_budget(_fixture())
        report["parity_failures"] = ["cold: canonical digests differ"]
        self.assert_rejected(report, "semantic-parity-failure")

    def test_parity_failure_recorded_on_the_aggregate(self) -> None:
        report = _under_budget(_fixture())
        report["aggregates"][BASELINE_CASE]["parity_failures"] = [
            "cold: integrity check failed"
        ]
        self.assert_rejected(report, "semantic-parity-failure")

    def test_parity_failure_from_semantic_parity_flag(self) -> None:
        report = _under_budget(_fixture())
        _stage(report)["semantic_parity"] = False
        self.assert_rejected(report, "semantic-parity-failure")

    def test_over_budget_traversals(self) -> None:
        report = _under_budget(_fixture())
        _counter(report, production.S098_OBSERVED_TRAVERSAL_COUNTER)["trials"] = [
            32,
            33,
            32,
        ]
        decision = self.assert_rejected(report, "traversal-budget-exceeded")
        self.assertEqual(decision["candidate"]["traversal_budget_exceeded_trials"], [2])

    def test_exactly_at_the_threshold_is_rejected(self) -> None:
        report = _set_fixed_root(_fixture(), production.S098_FIXED_ROOT_BUDGET_SECONDS)
        decision = self.assert_rejected(report, "fixed-root-budget-not-met")
        self.assertEqual(decision["fixed_root_median_seconds"], 0.025)

    def test_just_below_the_threshold_is_accepted(self) -> None:
        report = _set_fixed_root(_fixture(), 0.0249999)
        self.assertTrue(production.fusion_decision(report)["ship_eligible"])

    def test_identity_mismatch_on_schema_version(self) -> None:
        report = _under_budget(_fixture())
        report["identity"]["schema"]["version"] = 41
        self.assert_rejected(report, "identity-mismatch")

    def test_identity_mismatch_on_host(self) -> None:
        report = _under_budget(_fixture())
        report["identity"]["host"]["machine"] = "x86_64"
        self.assert_rejected(report, "identity-mismatch")

    def test_identity_mismatch_on_corpus_case(self) -> None:
        report = _under_budget(_fixture())
        report["aggregates"]["header-heavy:32:forward"] = report["aggregates"].pop(
            BASELINE_CASE
        )
        decision = production.fusion_decision(
            report, case="header-heavy:32:forward"
        )
        self.assertFalse(decision["ship_eligible"])
        self.assertEqual(decision["reason"], "identity-mismatch")

    def test_candidate_binary_may_differ_from_the_baseline_binary(self) -> None:
        report = _under_budget(_fixture())
        report["identity"]["executable"]["sha256"] = "0" * 64
        report["identity"]["executable"]["version"] = "cidx 0.54.0"
        report["identity"]["checkout"]["commit"] = "f" * 40
        self.assertTrue(production.fusion_decision(report)["ship_eligible"])

    def test_all_failures_are_reported_in_a_fixed_order(self) -> None:
        report = _fixture()
        report["authoritative_timing"] = False
        report["parity_failures"] = ["cold: canonical digests differ"]
        _counter(report, production.S098_OBSERVED_TRAVERSAL_COUNTER)["trials"] = [
            64,
            64,
            64,
        ]
        decision = production.fusion_decision(report)
        self.assertEqual(
            decision["reasons"],
            [
                "host-not-quiescent",
                "semantic-parity-failure",
                "traversal-budget-exceeded",
                "fixed-root-budget-not-met",
            ],
        )
        self.assertEqual(decision["reason"], "host-not-quiescent")


class ArtifactResolutionTest(unittest.TestCase):
    """AC #3183 - the full report comes from the backlog artifact registry."""

    def setUp(self) -> None:
        self.fixture = _fixture()
        self.expected = self.fixture["source_artifact"]
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.store = Path(self.temporary.name)
        self.artifacts_dir = self.store / "artifacts"
        self.target = self.artifacts_dir / "S-098" / self.expected["name"]
        self.target.parent.mkdir(parents=True)

    def write_artifact(self, payload: bytes) -> None:
        self.target.write_bytes(payload)

    def registry(self, records: list | None = None, root: str | None = None):
        listing = (
            [
                {
                    "rel_path": self.expected["rel_path"],
                    "title": "S-071 authoritative header-heavy baseline run",
                    "kind": "data",
                }
            ]
            if records is None
            else records
        )
        directory = str(self.artifacts_dir) if root is None else root

        def _registry(*, task: str, project: str):
            self.assertEqual(task, "S-098")
            self.assertEqual(project, "cpp-indexer")
            return listing, directory

        return _registry

    def authoritative_payload(self) -> bytes:
        """Rebuild a full-shaped report carrying the recorded digest and size."""
        report = copy.deepcopy(self.fixture)
        report.pop("fixture", None)
        report.pop("description", None)
        report.pop("source_artifact", None)
        report["padding"] = ""
        text = json.dumps(report, indent=2) + "\n"
        payload = text.encode("utf-8")
        report["padding"] = "x" * max(0, self.expected["bytes"] - len(payload))
        payload = (json.dumps(report, indent=2) + "\n").encode("utf-8")
        self.expected["bytes"] = len(payload)
        self.expected["sha256"] = hashlib.sha256(payload).hexdigest()
        return payload

    def test_resolves_through_the_registry_and_verifies_digest_and_shape(self) -> None:
        self.write_artifact(self.authoritative_payload())
        report = production.load_baseline_artifact(
            fixture=self.fixture, registry=self.registry()
        )
        self.assertEqual(
            production.root_observation(report),
            production.root_observation(self.fixture),
        )

    def test_resolved_path_joins_the_stores_artifact_root(self) -> None:
        self.write_artifact(b"{}")
        path = production.resolve_baseline_artifact(registry=self.registry())
        self.assertEqual(path, self.target)

    def test_missing_registration_is_named(self) -> None:
        self.write_artifact(b"{}")
        with self.assertRaises(production.BaselineError) as caught:
            production.resolve_baseline_artifact(registry=self.registry(records=[]))
        self.assertEqual(caught.exception.reason, "baseline-artifact-missing")

    def test_registered_but_absent_file_is_named(self) -> None:
        with self.assertRaises(production.BaselineError) as caught:
            production.resolve_baseline_artifact(registry=self.registry())
        self.assertEqual(caught.exception.reason, "baseline-artifact-missing")

    def test_store_without_an_artifact_root_is_named(self) -> None:
        with self.assertRaises(production.BaselineError) as caught:
            production.resolve_baseline_artifact(registry=self.registry(root=""))
        self.assertEqual(caught.exception.reason, "baseline-artifact-missing")

    def test_duplicate_registration_is_named(self) -> None:
        self.write_artifact(b"{}")
        record = {"rel_path": self.expected["rel_path"]}
        with self.assertRaises(production.BaselineError) as caught:
            production.resolve_baseline_artifact(
                registry=self.registry(records=[record, dict(record)])
            )
        self.assertEqual(caught.exception.reason, "baseline-artifact-mismatch")

    def test_escaping_registration_is_named(self) -> None:
        with self.assertRaises(production.BaselineError) as caught:
            production.resolve_baseline_artifact(
                registry=self.registry(
                    records=[{"rel_path": "../../etc/s071-authoritative-r3.json"}]
                )
            )
        self.assertEqual(caught.exception.reason, "baseline-artifact-mismatch")

    def test_size_mismatch_is_named(self) -> None:
        self.write_artifact(self.authoritative_payload() + b" ")
        with self.assertRaises(production.BaselineError) as caught:
            production.load_baseline_artifact(
                fixture=self.fixture, registry=self.registry()
            )
        self.assertEqual(caught.exception.reason, "baseline-artifact-mismatch")

    def test_digest_mismatch_is_named(self) -> None:
        payload = bytearray(self.authoritative_payload())
        payload[-2:-1] = b" "
        self.write_artifact(bytes(payload))
        with self.assertRaises(production.BaselineError) as caught:
            production.load_baseline_artifact(
                fixture=self.fixture, registry=self.registry()
            )
        self.assertEqual(caught.exception.reason, "baseline-artifact-mismatch")

    def test_shape_mismatch_is_named(self) -> None:
        payload = self.authoritative_payload()
        report = json.loads(payload)
        _timing(report, "root_symbols")["trials"] = [0.1, 0.1, 0.1]
        mutated = (json.dumps(report, indent=2) + "\n").encode("utf-8")
        self.expected["bytes"] = len(mutated)
        self.expected["sha256"] = hashlib.sha256(mutated).hexdigest()
        self.write_artifact(mutated)
        with self.assertRaises(production.BaselineError) as caught:
            production.load_baseline_artifact(
                fixture=self.fixture, registry=self.registry()
            )
        self.assertEqual(caught.exception.reason, "baseline-artifact-mismatch")

    def test_resolver_assumes_no_repository_or_temporary_path(self) -> None:
        """No resolver string literal may hardcode a store or scratch path.

        Prose in docstrings and comments is exempt; only values the code can
        actually turn into a path are checked.
        """
        for function in (
            production.resolve_baseline_artifact,
            production.backlog_artifact_registry,
            production.load_baseline_artifact,
        ):
            tree = ast.parse(inspect.getsource(function))
            docstrings = {
                id(node.value)
                for node in ast.walk(tree)
                if isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant)
            }
            for node in ast.walk(tree):
                if not isinstance(node, ast.Constant) or id(node) in docstrings:
                    continue
                if not isinstance(node.value, str):
                    continue
                for forbidden in (".backlog", "/tmp"):
                    self.assertNotIn(
                        forbidden,
                        node.value,
                        msg=f"{function.__name__} hardcodes {forbidden!r}",
                    )
                self.assertFalse(
                    node.value.startswith(("/", "~", "./", "../")),
                    msg=f"{function.__name__} hardcodes the path {node.value!r}",
                )

    def test_resolution_ignores_the_repository_backlog_directory(self) -> None:
        """The checkout has a `.backlog/artifacts` tree; it is not the store."""
        repository_copy = REPOSITORY_ROOT / ".backlog" / "artifacts" / "S-098"
        if not repository_copy.exists():
            repository_copy.mkdir(parents=True)
            self.addCleanup(repository_copy.rmdir)
        decoy = repository_copy / self.expected["name"]
        if not decoy.exists():
            decoy.write_bytes(b"{}")
            self.addCleanup(decoy.unlink)
        self.write_artifact(self.authoritative_payload())
        path = production.resolve_baseline_artifact(registry=self.registry())
        self.assertEqual(path, self.target)
        self.assertFalse(path.is_relative_to(REPOSITORY_ROOT))


class BoundaryTest(unittest.TestCase):
    """AC #3183 - measurement/report/fixture code only."""

    def test_fixture_records_its_authoritative_source(self) -> None:
        source = _fixture()["source_artifact"]
        self.assertEqual(source["backlog_task"], "S-098")
        self.assertEqual(source["name"], "s071-authoritative-r3.json")
        self.assertEqual(len(source["sha256"]), 64)
        self.assertGreater(source["bytes"], 0)

    def test_fixture_is_compact(self) -> None:
        self.assertLess(production.baseline_fixture_path().stat().st_size, 16 * 1024)

    def test_decision_helper_reaches_no_production_surface(self) -> None:
        for tree in ("src", "python", "schemas", "catalogs"):
            for path in (REPOSITORY_ROOT / tree).rglob("*"):
                if not path.is_file() or path.suffix not in {
                    ".cpp",
                    ".hpp",
                    ".h",
                    ".py",
                    ".sql",
                    ".json",
                }:
                    continue
                text = path.read_text(encoding="utf-8", errors="ignore")
                self.assertNotIn("fusion_decision", text, msg=str(path))
                self.assertNotIn("s098_root_fusion", text, msg=str(path))

    def test_decision_helper_needs_no_binary_corpus_or_database(self) -> None:
        decision = production.fusion_decision(_under_budget(_fixture()))
        self.assertTrue(decision["ship_eligible"])
        self.assertEqual(
            decision, production.fusion_decision(_under_budget(_fixture()))
        )


if __name__ == "__main__":
    unittest.main()
