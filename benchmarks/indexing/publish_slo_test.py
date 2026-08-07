"""Offline tests for the S-078 SLO assembler.

`publish_slo.py` is the wiring between recorded measurement JSON and the pure
contract in `slo.py`. What is tested here is that the wiring reads the series
it claims to read, that the residual evidence is derived from measurements
rather than asserted, and that the regression-guard check actually inspects the
workflow it names. Everything runs from in-memory reports: no binary, no
corpus, no store.
"""

from __future__ import annotations

import math
import tempfile
import unittest
from pathlib import Path

from benchmarks.indexing import publish_slo, slo


def _profile_stage(
    *,
    wall: list[float],
    cpu: list[float] | None = None,
    timings: dict | None = None,
    counters: dict | None = None,
) -> dict:
    cpu = cpu or [value * 1.2 for value in wall]
    return {
        "wall_seconds": slo.median(wall),
        "wall_seconds_trials": wall,
        "cpu_seconds": slo.median(cpu),
        "cpu_seconds_trials": cpu,
        "per_tu_analysis": {"preferred_model": "linear", "status": "measured"},
        "profile_summary": {
            "timings": {
                name: {"median": value, "trials": [value] * len(wall)}
                for name, value in (timings or {}).items()
            },
            "counters": {
                name: {"median": value, "trials": [value] * len(wall)}
                for name, value in (counters or {}).items()
            },
        },
    }


def _report(cases: dict) -> dict:
    return {
        "aggregates": {
            case: {"trial_count": 3, "stages": stages, "parity_failures": []}
            for case, stages in cases.items()
        },
        "parity_failures": [],
        "authoritative_timing": True,
    }


class StageReadingTest(unittest.TestCase):
    def test_a_missing_case_is_refused_rather_than_defaulted(self):
        report = _report({"baseline:32:forward": {"cold": _profile_stage(wall=[1.0])}})
        with self.assertRaises(slo.SloContractError):
            publish_slo._walls(report, "baseline:1000:forward", "cold")

    def test_a_missing_stage_is_refused(self):
        report = _report({"baseline:32:forward": {"cold": _profile_stage(wall=[1.0])}})
        with self.assertRaises(slo.SloContractError):
            publish_slo._walls(report, "baseline:32:forward", "unchanged-warm")

    def test_trial_series_are_read_not_recomputed_from_the_median(self):
        report = _report(
            {"baseline:32:forward": {"cold": _profile_stage(wall=[1.0, 3.0, 9.0])}}
        )
        self.assertEqual(
            publish_slo._walls(report, "baseline:32:forward", "cold"),
            [1.0, 3.0, 9.0],
        )


class RootTraversalEvidenceTest(unittest.TestCase):
    def _report(self) -> dict:
        return _report(
            {
                "header-heavy:32:forward": {
                    "cold": _profile_stage(
                        wall=[0.25, 0.26, 0.27],
                        timings={
                            "root_symbols": 0.0149,
                            "root_declarations": 0.0050,
                            "root_definitions": 0.0012,
                            "root_namespaces": 0.0009,
                        },
                        counters={
                            "registered_root_traversal_budget": 64,
                            "observed_root_traversals": 64,
                        },
                    )
                }
            }
        )

    def test_the_fixed_root_term_sums_the_four_rooted_timings(self):
        evidence = publish_slo.root_traversal_evidence(
            self._report(), case="header-heavy:32:forward"
        )
        self.assertAlmostEqual(evidence["fixed_root_median"], 0.0220)
        self.assertEqual(set(evidence["components"]),
                         set(publish_slo.SLO_FIXED_ROOT_TIMINGS))

    def test_the_share_of_cold_wall_is_published(self):
        evidence = publish_slo.root_traversal_evidence(
            self._report(), case="header-heavy:32:forward"
        )
        self.assertAlmostEqual(evidence["share_of_cold_wall"], 0.0220 / 0.26, places=6)

    def test_the_budget_and_observed_traversals_are_carried_through(self):
        evidence = publish_slo.root_traversal_evidence(
            self._report(), case="header-heavy:32:forward"
        )
        self.assertEqual(evidence["registered_root_traversal_budget"], 64)
        self.assertEqual(evidence["observed_root_traversals"], 64)

    def test_a_missing_timing_contributes_zero_rather_than_crashing(self):
        report = self._report()
        del report["aggregates"]["header-heavy:32:forward"]["stages"]["cold"][
            "profile_summary"
        ]["timings"]["root_namespaces"]
        evidence = publish_slo.root_traversal_evidence(
            report, case="header-heavy:32:forward"
        )
        self.assertAlmostEqual(evidence["fixed_root_median"], 0.0211)


class ScalingEvidenceTest(unittest.TestCase):
    def _report(self, *, small: list[float], large: list[float]) -> dict:
        return _report(
            {
                "baseline:32:forward": {"cold": _profile_stage(wall=small)},
                "baseline:1000:forward": {"cold": _profile_stage(wall=large)},
            }
        )

    def test_flat_per_unit_cost_is_exponent_one(self):
        evidence = publish_slo.scaling_evidence(
            self._report(small=[3.2, 3.2, 3.2], large=[100.0, 100.0, 100.0]),
            small_case="baseline:32:forward", large_case="baseline:1000:forward",
            small_files=32, large_files=1000,
        )
        self.assertAlmostEqual(evidence["implied_exponent"], 1.0, places=6)
        self.assertAlmostEqual(evidence["per_tu_growth"], 1.0, places=6)

    def test_a_doubling_of_per_unit_cost_lands_above_one(self):
        evidence = publish_slo.scaling_evidence(
            self._report(small=[3.2, 3.2, 3.2], large=[200.0, 200.0, 200.0]),
            small_case="baseline:32:forward", large_case="baseline:1000:forward",
            small_files=32, large_files=1000,
        )
        self.assertGreater(evidence["implied_exponent"], 1.0)
        self.assertLess(evidence["implied_exponent"], 1.4)

    def test_the_threshold_is_carried_into_the_residual_list(self):
        evidence = publish_slo.scaling_evidence(
            self._report(small=[1.0], large=[1.0]),
            small_case="baseline:32:forward", large_case="baseline:1000:forward",
            small_files=32, large_files=1000,
        )
        self.assertEqual(evidence["threshold"],
                         publish_slo.SCALING_EXPONENT_THRESHOLD)


class PublicationEvidenceTest(unittest.TestCase):
    def test_the_writer_share_sums_its_three_timings(self):
        report = _report(
            {
                "baseline:1000:forward": {
                    "cold": _profile_stage(
                        wall=[100.0, 100.0, 100.0],
                        timings={
                            "fact_batch_writer.prepare": 5.0,
                            "fact_batch_writer.virtual_machine": 20.0,
                            "fact_batch_writer.commit": 5.0,
                        },
                    )
                }
            }
        )
        evidence = publish_slo.publication_evidence(
            report, case="baseline:1000:forward"
        )
        self.assertAlmostEqual(evidence["writer_seconds_median"], 30.0)
        self.assertAlmostEqual(evidence["share_of_cold_wall"], 0.30)
        self.assertEqual(evidence["threshold"],
                         publish_slo.PUBLICATION_SHARE_THRESHOLD)

    def test_the_publication_threshold_leaves_headroom_over_the_measurement(self):
        # 0.755 is what the shipped configuration measures at 1,000 units; a
        # threshold at or below it would be a line already crossed on the day
        # it was published.
        self.assertGreater(publish_slo.PUBLICATION_SHARE_THRESHOLD, 0.755)
        self.assertLess(publish_slo.PUBLICATION_SHARE_THRESHOLD, 1.0)

    def test_a_report_without_writer_timings_reports_a_zero_share(self):
        # The parallel topology used to publish none of these; a zero share is
        # visibly wrong rather than silently absent.
        report = _report(
            {"baseline:1000:forward": {"cold": _profile_stage(wall=[100.0])}}
        )
        evidence = publish_slo.publication_evidence(
            report, case="baseline:1000:forward"
        )
        self.assertEqual(evidence["writer_seconds_median"], 0.0)
        self.assertEqual(evidence["share_of_cold_wall"], 0.0)


class TransformEvaluationEvidenceTest(unittest.TestCase):
    def _report(self, *, executed: dict | None = None) -> dict:
        timings = {"transforms": 0.376}
        timings.update(executed or {})
        return _report(
            {
                "baseline:1000:forward": {
                    "unchanged-warm": _profile_stage(
                        wall=[0.70, 0.715, 0.73], timings=timings
                    )
                }
            }
        )

    def test_a_warm_run_that_executed_no_transform_is_all_evaluation(self):
        evidence = publish_slo.transform_evaluation_evidence(
            self._report(), case="baseline:1000:forward"
        )
        self.assertAlmostEqual(evidence["warm_seconds_median"], 0.715)
        self.assertAlmostEqual(evidence["transform_seconds_median"], 0.376)
        self.assertEqual(evidence["executed_transform_seconds"], 0.0)
        self.assertAlmostEqual(evidence["share_of_warm_wall"], 0.376 / 0.715,
                               places=6)

    def test_individually_timed_transforms_are_separated_from_the_pipeline(self):
        evidence = publish_slo.transform_evaluation_evidence(
            self._report(executed={"transform.entity-graph-rollup": 0.039,
                                   "transform.edge-site-count-rollup": 0.017}),
            case="baseline:1000:forward",
        )
        self.assertAlmostEqual(evidence["executed_transform_seconds"], 0.056)
        self.assertAlmostEqual(evidence["transform_seconds_median"], 0.376)

    def test_the_threshold_is_a_fraction_of_the_absolute_warm_limit(self):
        evidence = publish_slo.transform_evaluation_evidence(
            self._report(), case="baseline:1000:forward"
        )
        self.assertEqual(evidence["threshold"],
                         publish_slo.TRANSFORM_EVALUATION_WARM_THRESHOLD_SECONDS)
        self.assertLess(evidence["threshold"], slo.WARM_ABSOLUTE_LIMIT_SECONDS)


class IntegrityEvidenceTest(unittest.TestCase):
    def test_a_clean_set_of_reports_passes(self):
        self.assertTrue(
            publish_slo.integrity_evidence({"a": _report({}), "b": _report({})})["ok"]
        )

    def test_a_parity_failure_anywhere_fails_and_names_its_report(self):
        broken = _report({})
        broken["parity_failures"] = ["cold: canonical digests differ"]
        evidence = publish_slo.integrity_evidence({"shipped": broken})
        self.assertFalse(evidence["ok"])
        self.assertIn("shipped: cold: canonical digests differ", evidence["failures"])

    def test_a_matrix_failure_list_is_folded_in(self):
        broken = _report({})
        broken["failures"] = ["equivalence: cache-replay differs"]
        evidence = publish_slo.integrity_evidence({"integrated": broken})
        self.assertFalse(evidence["ok"])

    def test_a_non_quiescent_run_cannot_be_published(self):
        contended = _report({})
        contended["authoritative_timing"] = False
        evidence = publish_slo.integrity_evidence({"shipped": contended})
        self.assertFalse(evidence["ok"])
        self.assertIn("shipped: measured on a non-quiescent host",
                      evidence["failures"])


class DependencyInvalidationTest(unittest.TestCase):
    def _report(self, *, low=2, high=32, generated=32, warm=0) -> dict:
        def stage(units: int) -> dict:
            entry = _profile_stage(wall=[0.1, 0.1, 0.1])
            entry["per_tu_analysis"] = {"sample_count": units}
            return entry

        return _report(
            {
                "fan-in:32:forward": {
                    "low-fan-in-header": stage(low),
                    "high-fan-in-header": stage(high),
                },
                "baseline:32:forward": {
                    "generated-input": stage(generated),
                    "unchanged-warm": stage(warm),
                },
            }
        )

    def test_the_correct_affected_set_passes(self):
        evidence = publish_slo.dependency_invalidation_evidence(
            self._report(), representative_files=32
        )
        self.assertTrue(evidence["ok"], msg=evidence["failures"])
        self.assertEqual(evidence["low_fan_in_header_units"], 2)
        self.assertEqual(evidence["high_fan_in_header_units"], 32)

    def test_rebuilding_nothing_fails(self):
        # The behaviour that shipped before S-078: a stale header is deferred
        # and its dependents report unchanged.
        evidence = publish_slo.dependency_invalidation_evidence(
            self._report(low=0, high=0, generated=0), representative_files=32
        )
        self.assertFalse(evidence["ok"])
        self.assertEqual(len(evidence["failures"]), 3)

    def test_rebuilding_everything_also_fails(self):
        # The lazy fix. A low-fan-in header is included by two units, so
        # rebuilding all of them is the wrong answer too.
        evidence = publish_slo.dependency_invalidation_evidence(
            self._report(low=32), representative_files=32
        )
        self.assertFalse(evidence["ok"])
        self.assertTrue(
            any("low-fan-in" in text for text in evidence["failures"])
        )

    def test_inventing_work_on_an_unchanged_run_fails(self):
        evidence = publish_slo.dependency_invalidation_evidence(
            self._report(warm=3), representative_files=32
        )
        self.assertFalse(evidence["ok"])
        self.assertTrue(
            any("invents work" in text for text in evidence["failures"])
        )

    def test_a_missing_sample_count_is_refused_rather_than_assumed(self):
        report = self._report()
        del report["aggregates"]["fan-in:32:forward"]["stages"][
            "high-fan-in-header"
        ]["per_tu_analysis"]["sample_count"]
        with self.assertRaises(slo.SloContractError):
            publish_slo.dependency_invalidation_evidence(
                report, representative_files=32
            )


class RegressionGuardTest(unittest.TestCase):
    def test_the_real_workflow_satisfies_the_guard(self):
        evidence = publish_slo.regression_guard_evidence()
        self.assertTrue(evidence["ok"], msg=evidence["failures"])
        self.assertEqual(
            evidence["jobs"],
            [job.rstrip(":") for job in publish_slo.REQUIRED_WORKFLOW_JOBS],
        )

    def test_a_missing_workflow_fails_the_guard(self):
        with tempfile.TemporaryDirectory() as root:
            evidence = publish_slo.regression_guard_evidence(Path(root))
            self.assertFalse(evidence["ok"])
            self.assertTrue(
                any("does not exist" in text for text in evidence["failures"])
            )

    def test_a_workflow_without_a_scheduled_run_fails_the_guard(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / publish_slo.REGRESSION_WORKFLOW
            path.parent.mkdir(parents=True)
            path.write_text(
                "jobs:\n  contract:\n  equivalence:\n  production-scale:\n",
                encoding="utf-8",
            )
            evidence = publish_slo.regression_guard_evidence(Path(root))
            self.assertFalse(evidence["ok"])
            self.assertTrue(
                any("scheduled run" in text for text in evidence["failures"])
            )

    def test_a_workflow_missing_a_promised_job_fails_the_guard(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / publish_slo.REGRESSION_WORKFLOW
            path.parent.mkdir(parents=True)
            path.write_text(
                "on:\n  schedule:\n    - cron: '0 0 * * 1'\n"
                "jobs:\n  contract:\n  equivalence:\n",
                encoding="utf-8",
            )
            evidence = publish_slo.regression_guard_evidence(Path(root))
            self.assertFalse(evidence["ok"])
            self.assertTrue(
                any("production-scale" in text for text in evidence["failures"])
            )


class InfinityTest(unittest.TestCase):
    def test_a_zero_baseline_per_unit_cost_does_not_divide_by_zero(self):
        evidence = publish_slo.scaling_evidence(
            _report(
                {
                    "baseline:32:forward": {"cold": _profile_stage(wall=[0.0])},
                    "baseline:1000:forward": {"cold": _profile_stage(wall=[1.0])},
                }
            ),
            small_case="baseline:32:forward", large_case="baseline:1000:forward",
            small_files=32, large_files=1000,
        )
        self.assertEqual(evidence["per_tu_growth"], math.inf)


if __name__ == "__main__":
    unittest.main()
