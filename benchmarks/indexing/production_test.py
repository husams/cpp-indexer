"""Pure report/threshold tests for the S-098 routed root-fusion decision.

Everything here reads the checked-in compact S-071 fixture or an in-memory
mutation of it. No cidx binary, corpus, database, or backlog store is required,
so the suite is deterministic and offline. Nothing in this suite writes into
the repository working tree; `setUpModule`/`tearDownModule` enforce that.
"""

from __future__ import annotations

import ast
import copy
import hashlib
import inspect
import json
import os
from pathlib import Path
import tempfile
from typing import Any
import unittest

from benchmarks.indexing import production


BASELINE_CASE = production.S098_BASELINE_CASE
BASELINE_STAGE = production.S098_BASELINE_STAGE
REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
REPOSITORY_BACKLOG = REPOSITORY_ROOT / ".backlog"

# The authoritative artifact's recorded identity, pinned here so a silent edit
# to the fixture fails a check instead of redefining what "authoritative" means.
RECORDED_ARTIFACT_SHA256 = (
    "014795c0eaf813e75ab81adf7b925e6df2a8bc616319ad2c2214b9c015d8fede"
)
RECORDED_ARTIFACT_BYTES = 3196668
RECORDED_ARTIFACT_NAME = "s071-authoritative-r3.json"
RECORDED_ARTIFACT_TASK = "S-098"

_REPOSITORY_SNAPSHOT: dict[str, str] = {}


def _snapshot_repository_backlog() -> dict[str, str]:
    """Content digest per path under the repository `.backlog` tree.

    Directory mtimes are deliberately not part of this: creating and then
    removing a file changes the parent's mtime while leaving the tree
    unmodified. What matters is that no path and no file content differs.
    """
    if not REPOSITORY_BACKLOG.exists():
        return {}
    snapshot: dict[str, str] = {}
    for path in sorted(REPOSITORY_BACKLOG.rglob("*")):
        name = str(path.relative_to(REPOSITORY_ROOT))
        if path.is_dir():
            snapshot[name] = "<dir>"
        else:
            snapshot[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return snapshot


def _repository_backlog_differences() -> list[str]:
    after = _snapshot_repository_backlog()
    return sorted(
        f"{'added' if name not in _REPOSITORY_SNAPSHOT else 'removed' if name not in after else 'changed'}: {name}"
        for name in set(after) | set(_REPOSITORY_SNAPSHOT)
        if after.get(name) != _REPOSITORY_SNAPSHOT.get(name)
    )


def setUpModule() -> None:
    _REPOSITORY_SNAPSHOT.clear()
    _REPOSITORY_SNAPSHOT.update(_snapshot_repository_backlog())


def tearDownModule() -> None:
    differences = _repository_backlog_differences()
    if differences:
        raise AssertionError(
            "the test suite modified the repository .backlog tree: "
            + "; ".join(differences)
        )


def _fixture() -> dict:
    return production.load_baseline_fixture()


def _stage(report: dict, case: str = BASELINE_CASE) -> dict:
    return report["aggregates"][case]["stages"][BASELINE_STAGE]


def _timing(report: dict, name: str) -> dict:
    return _stage(report)["profile_summary"]["timings"][name]


def _counter(report: dict, name: str) -> dict:
    return _stage(report)["profile_summary"]["counters"][name]


def _corpus_trials(report: dict, case: str = BASELINE_CASE) -> list[dict]:
    return [
        value
        for key, value in report["individual_trials"].items()
        if key.startswith(f"{case}:trial-")
    ]


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


class IdentityBindingTest(unittest.TestCase):
    """C-1219 - the fingerprint binds the corpus, method, and reuse mechanism.

    The case key only carries shape, file count, and order. Before this
    binding, a candidate measured on a lighter header-heavy corpus - fewer
    distinct owned headers, so less routed-root work - compared as
    identity-matched and could clear 25 ms on easier evidence.
    """

    def test_fixture_records_the_corpus_it_was_measured_on(self) -> None:
        fixture = _fixture()
        trials = _corpus_trials(fixture)
        self.assertEqual(len(trials), 3)
        for trial in trials:
            self.assertEqual(trial["shape"], "header-heavy")
            self.assertEqual(trial["order"], "forward")
            self.assertEqual(trial["files"], 8)
            self.assertEqual(trial["baseline_distinct_owned_headers"], 2)
            self.assertEqual(trial["target_distinct_owned_headers"], 18)
        self.assertEqual(fixture["corpus_contract"]["many_header_target"], 16)
        self.assertIsNone(fixture["front_end_reuse"])

    def test_fingerprint_exposes_corpus_and_reuse_identity(self) -> None:
        identity = production.root_observation(_fixture())["identity"]
        self.assertEqual(identity["corpus"]["target_distinct_owned_headers"], 18)
        self.assertEqual(identity["corpus"]["files"], 8)
        self.assertEqual(identity["corpus_contract"]["many_header_target"], 16)
        self.assertIsNone(identity["front_end_reuse"])
        self.assertEqual(identity["method"], "HSE-103")

    def test_lighter_corpus_is_not_identity_matched(self) -> None:
        """The finding, reproduced: same case key, materially lighter corpus."""
        report = _under_budget(_fixture())
        for trial in _corpus_trials(report):
            trial["target_distinct_owned_headers"] = 6
        decision = production.fusion_decision(report)
        self.assertFalse(decision["ship_eligible"])
        self.assertEqual(decision["reason"], "identity-mismatch")
        self.assertEqual(
            decision["candidate"]["identity"]["corpus"][
                "target_distinct_owned_headers"
            ],
            6,
        )

    def test_different_baseline_owned_header_count_is_not_identity_matched(self) -> None:
        report = _under_budget(_fixture())
        for trial in _corpus_trials(report):
            trial["baseline_distinct_owned_headers"] = 4
        self.assertEqual(
            production.fusion_decision(report)["reason"], "identity-mismatch"
        )

    def test_different_corpus_contract_is_not_identity_matched(self) -> None:
        report = _under_budget(_fixture())
        report["corpus_contract"]["many_header_target"] = 128
        self.assertEqual(
            production.fusion_decision(report)["reason"], "identity-mismatch"
        )

    def test_enabling_front_end_reuse_is_not_identity_matched(self) -> None:
        report = _under_budget(_fixture())
        report["front_end_reuse"] = {
            "identity_version": "front-end-reuse/v1",
            "mechanism": "generated-umbrella-pch",
            "explicitly_disabled": False,
            "artifact_construction": True,
            "artifact_injection": True,
        }
        self.assertEqual(
            production.fusion_decision(report)["reason"], "identity-mismatch"
        )

    def test_declared_no_reuse_still_differs_from_a_report_without_the_section(
        self,
    ) -> None:
        report = _under_budget(_fixture())
        report["front_end_reuse"] = production.qualification_contract()
        decision = production.fusion_decision(report)
        self.assertEqual(decision["reason"], "identity-mismatch")
        self.assertEqual(
            decision["candidate"]["identity"]["front_end_reuse"]["mechanism"], "none"
        )

    def test_missing_individual_trials_is_malformed(self) -> None:
        report = _under_budget(_fixture())
        del report["individual_trials"]
        self.assertEqual(
            production.fusion_decision(report)["reason"], "malformed-report"
        )

    def test_missing_corpus_for_the_case_is_malformed(self) -> None:
        report = _under_budget(_fixture())
        report["individual_trials"] = {"baseline:8:forward:trial-1": {}}
        self.assertEqual(
            production.fusion_decision(report)["reason"], "malformed-report"
        )

    def test_trial_without_corpus_fields_is_malformed(self) -> None:
        report = _under_budget(_fixture())
        for trial in _corpus_trials(report):
            del trial["target_distinct_owned_headers"]
        self.assertEqual(
            production.fusion_decision(report)["reason"], "malformed-report"
        )

    def test_trials_measured_on_different_corpora_are_malformed(self) -> None:
        report = _under_budget(_fixture())
        _corpus_trials(report)[1]["target_distinct_owned_headers"] = 6
        self.assertEqual(
            production.fusion_decision(report)["reason"], "malformed-report"
        )

    def test_missing_corpus_contract_is_malformed(self) -> None:
        report = _under_budget(_fixture())
        del report["corpus_contract"]
        self.assertEqual(
            production.fusion_decision(report)["reason"], "malformed-report"
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
        report["individual_trials"][f"{BASELINE_CASE}:trial-4"] = copy.deepcopy(
            _corpus_trials(report)[0]
        )
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

    def test_fixture_only_decisions_declare_the_artifact_unverified(self) -> None:
        decision = production.fusion_decision(_under_budget(_fixture()))
        self.assertFalse(decision["baseline_artifact_verified"])

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
        del report["individual_trials"][f"{BASELINE_CASE}:trial-3"]
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
        for index in (1, 2, 3):
            report["individual_trials"][f"header-heavy:32:forward:trial-{index}"] = (
                report["individual_trials"].pop(f"{BASELINE_CASE}:trial-{index}")
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
        report["identity"]["schema"]["version"] = 41
        _counter(report, production.S098_OBSERVED_TRAVERSAL_COUNTER)["trials"] = [
            64,
            64,
            64,
        ]
        decision = production.fusion_decision(report)
        self.assertEqual(
            decision["reasons"],
            [
                "identity-mismatch",
                "host-not-quiescent",
                "semantic-parity-failure",
                "traversal-budget-exceeded",
                "fixed-root-budget-not-met",
            ],
        )
        self.assertEqual(decision["reason"], "identity-mismatch")


class ArtifactVerificationTest(unittest.TestCase):
    """AC #3183 and C-1220 - the authoritative report is resolved and verified.

    The recorded size and digest are treated as fixed inputs here: a payload is
    built once, its true digest measured once, and every later assertion
    mutates the *bytes* while the recorded values stay put. No assertion
    recomputes the value it is checking against.
    """

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.store = Path(self.temporary.name) / "store"
        self.artifacts_dir = self.store / "artifacts"
        self.target = self.artifacts_dir / RECORDED_ARTIFACT_TASK / RECORDED_ARTIFACT_NAME
        self.target.parent.mkdir(parents=True)
        self.rel_path = (
            f"artifacts/{RECORDED_ARTIFACT_TASK}/{RECORDED_ARTIFACT_NAME}"
        )
        # One authoritative payload; its identity is measured once and then
        # treated as a recorded constant for the rest of the test.
        report = _fixture()
        for key in ("fixture", "description", "source_artifact"):
            report.pop(key, None)
        self.payload = (json.dumps(report, indent=2) + "\n").encode("utf-8")
        self.fixture = _fixture()
        self.fixture["source_artifact"] = {
            "backlog_task": RECORDED_ARTIFACT_TASK,
            "name": RECORDED_ARTIFACT_NAME,
            "rel_path": self.rel_path,
            "sha256": hashlib.sha256(self.payload).hexdigest(),
            "bytes": len(self.payload),
        }
        self.recorded = dict(self.fixture["source_artifact"])

    def write_artifact(self, payload: bytes) -> None:
        self.target.write_bytes(payload)

    def registry(self, records: list | None = None, root: str | None = None):
        listing = (
            [{"rel_path": self.rel_path, "kind": "data"}]
            if records is None
            else records
        )
        directory = str(self.artifacts_dir) if root is None else root

        def _registry(*, task: str, project: str):
            self.assertEqual(task, RECORDED_ARTIFACT_TASK)
            self.assertEqual(project, "cpp-indexer")
            return listing, directory

        return _registry

    def assert_recorded_values_untouched(self) -> None:
        """No test may quietly re-record what it is supposed to be checking."""
        self.assertEqual(self.fixture["source_artifact"], self.recorded)

    # -- the fixture's recorded identity -----------------------------------

    def test_recorded_digest_and_size_are_pinned(self) -> None:
        source = _fixture()["source_artifact"]
        self.assertEqual(source["sha256"], RECORDED_ARTIFACT_SHA256)
        self.assertEqual(source["bytes"], RECORDED_ARTIFACT_BYTES)
        self.assertEqual(source["name"], RECORDED_ARTIFACT_NAME)
        self.assertEqual(source["backlog_task"], RECORDED_ARTIFACT_TASK)

    def test_recorded_digest_matches_the_registered_artifact(self) -> None:
        """Verify the recorded identity against the real store when reachable."""
        try:
            path = production.resolve_baseline_artifact()
        except production.BaselineError as error:
            self.skipTest(f"backlog store unreachable: {error.reason}")
        self.assertEqual(path.stat().st_size, RECORDED_ARTIFACT_BYTES)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        self.assertEqual(digest, RECORDED_ARTIFACT_SHA256)

    # -- verification -------------------------------------------------------

    def test_verification_reports_what_it_verified(self) -> None:
        self.write_artifact(self.payload)
        evidence = production.verify_baseline_artifact(
            fixture=self.fixture, registry=self.registry()
        )
        self.assertTrue(evidence["verified"])
        self.assertEqual(evidence["resolved_path"], str(self.target))
        self.assertEqual(evidence["sha256"], self.recorded["sha256"])
        self.assertEqual(evidence["bytes"], self.recorded["bytes"])
        self.assertEqual(
            production.root_observation(evidence["report"]),
            production.root_observation(self.fixture),
        )
        self.assert_recorded_values_untouched()

    def test_resolved_path_joins_the_stores_artifact_root(self) -> None:
        self.write_artifact(b"{}")
        path = production.resolve_baseline_artifact(registry=self.registry())
        self.assertEqual(path, self.target)

    def test_one_byte_change_fails_the_digest(self) -> None:
        mutated = bytearray(self.payload)
        mutated[-2] = mutated[-2] ^ 0x20
        self.assertNotEqual(bytes(mutated), self.payload)
        self.assertEqual(len(mutated), self.recorded["bytes"])
        self.write_artifact(bytes(mutated))
        with self.assertRaises(production.BaselineError) as caught:
            production.verify_baseline_artifact(
                fixture=self.fixture, registry=self.registry()
            )
        self.assertEqual(caught.exception.reason, "baseline-artifact-mismatch")
        self.assertIn("digest", caught.exception.detail)
        self.assert_recorded_values_untouched()

    def test_truncation_fails_the_size(self) -> None:
        self.write_artifact(self.payload[:-1])
        with self.assertRaises(production.BaselineError) as caught:
            production.verify_baseline_artifact(
                fixture=self.fixture, registry=self.registry()
            )
        self.assertEqual(caught.exception.reason, "baseline-artifact-mismatch")
        self.assertIn("bytes", caught.exception.detail)
        self.assert_recorded_values_untouched()

    def test_right_digest_but_wrong_measurements_fails_the_shape(self) -> None:
        report = json.loads(self.payload)
        _timing(report, "root_symbols")["trials"] = [0.1, 0.1, 0.1]
        mutated = (json.dumps(report, indent=2) + "\n").encode("utf-8")
        self.write_artifact(mutated)
        self.fixture["source_artifact"]["sha256"] = hashlib.sha256(mutated).hexdigest()
        self.fixture["source_artifact"]["bytes"] = len(mutated)
        with self.assertRaises(production.BaselineError) as caught:
            production.verify_baseline_artifact(
                fixture=self.fixture, registry=self.registry()
            )
        self.assertEqual(caught.exception.reason, "baseline-artifact-mismatch")
        self.assertIn("fixture", caught.exception.detail)

    def test_missing_registration_is_named(self) -> None:
        self.write_artifact(self.payload)
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
        self.write_artifact(self.payload)
        record = {"rel_path": self.rel_path}
        with self.assertRaises(production.BaselineError) as caught:
            production.resolve_baseline_artifact(
                registry=self.registry(records=[record, dict(record)])
            )
        self.assertEqual(caught.exception.reason, "baseline-artifact-mismatch")

    def test_escaping_registration_is_named(self) -> None:
        with self.assertRaises(production.BaselineError) as caught:
            production.resolve_baseline_artifact(
                registry=self.registry(
                    records=[{"rel_path": f"../../etc/{RECORDED_ARTIFACT_NAME}"}]
                )
            )
        self.assertEqual(caught.exception.reason, "baseline-artifact-mismatch")

    # -- verification is on the qualification path --------------------------

    def test_qualification_verifies_the_artifact_before_deciding(self) -> None:
        self.write_artifact(self.payload)
        candidate = _under_budget(_fixture())
        decision = production.qualify(
            candidate, fixture=self.fixture, registry=self.registry()
        )
        self.assertTrue(decision["ship_eligible"])
        self.assertTrue(decision["baseline_artifact_verified"])
        self.assertEqual(
            decision["baseline_artifact"]["resolved_path"], str(self.target)
        )
        self.assertEqual(
            decision["baseline_artifact"]["sha256"], self.recorded["sha256"]
        )
        self.assert_recorded_values_untouched()

    def test_qualification_compares_against_the_artifact_not_the_fixture(self) -> None:
        """The verified artifact supplies the baseline the candidate is judged on."""
        self.write_artifact(self.payload)
        decision = production.qualify(
            _under_budget(_fixture()), fixture=self.fixture, registry=self.registry()
        )
        self.assertEqual(
            decision["baseline"], production.root_observation(json.loads(self.payload))
        )

    def test_qualification_refuses_a_missing_artifact(self) -> None:
        candidate = _under_budget(_fixture())
        decision = production.qualify(
            candidate, fixture=self.fixture, registry=self.registry(records=[])
        )
        self.assertFalse(decision["ship_eligible"])
        self.assertEqual(decision["reason"], "baseline-artifact-missing")
        self.assertFalse(decision["baseline_artifact_verified"])
        self.assertIsNone(decision["candidate"])

    def test_qualification_refuses_a_mismatched_artifact_despite_a_good_candidate(
        self,
    ) -> None:
        """A passing candidate must not ship on unverified baseline evidence."""
        mutated = bytearray(self.payload)
        mutated[-2] = mutated[-2] ^ 0x20
        self.write_artifact(bytes(mutated))
        decision = production.qualify(
            _under_budget(_fixture()), fixture=self.fixture, registry=self.registry()
        )
        self.assertFalse(decision["ship_eligible"])
        self.assertEqual(decision["reason"], "baseline-artifact-mismatch")
        self.assertFalse(decision["baseline_artifact_verified"])

    def test_production_run_uses_the_verified_qualification_path(self) -> None:
        """A full run must qualify against the verified artifact, not the fixture."""
        module = ast.parse(Path(production.__file__).read_text(encoding="utf-8"))
        called: list[str] = []
        for node in ast.walk(module):
            if not isinstance(node, ast.Assign) or not isinstance(
                node.value, ast.Call
            ):
                continue
            for target in node.targets:
                if (
                    isinstance(target, ast.Subscript)
                    and isinstance(target.slice, ast.Constant)
                    and target.slice.value == "s098_root_fusion"
                ):
                    function = node.value.func
                    called.append(
                        function.id
                        if isinstance(function, ast.Name)
                        else ast.dump(function)
                    )
        self.assertEqual(called, ["qualify"])

    # -- no assumed repository or scratch path ------------------------------

    def test_resolver_assumes_no_repository_or_temporary_path(self) -> None:
        """No resolver string literal may hardcode a store or scratch path.

        Prose in docstrings and comments is exempt; only values the code can
        actually turn into a path are checked.
        """
        for function in (
            production.resolve_baseline_artifact,
            production.backlog_artifact_registry,
            production.verify_baseline_artifact,
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

    def test_a_repository_relative_decoy_is_ignored(self) -> None:
        """C-1221 - the decoy lives in a throwaway checkout, never in this one.

        A resolver that fell back to a checkout-relative `.backlog/artifacts`
        path would find this decoy, because the process runs with that fake
        checkout as its working directory.
        """
        fake_checkout = Path(self.temporary.name) / "checkout"
        decoy = fake_checkout / ".backlog" / "artifacts" / RECORDED_ARTIFACT_TASK
        decoy.mkdir(parents=True)
        (decoy / RECORDED_ARTIFACT_NAME).write_bytes(b"{}")
        self.write_artifact(self.payload)
        previous = Path.cwd()
        os.chdir(fake_checkout)
        self.addCleanup(os.chdir, previous)
        path = production.resolve_baseline_artifact(registry=self.registry())
        self.assertEqual(path, self.target)
        self.assertFalse(path.is_relative_to(fake_checkout))
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

    def test_suite_leaves_the_repository_backlog_tree_untouched(self) -> None:
        """C-1221 - checked here and again unconditionally in tearDownModule."""
        self.assertEqual(_snapshot_repository_backlog(), _REPOSITORY_SNAPSHOT)


if __name__ == "__main__":
    unittest.main()
