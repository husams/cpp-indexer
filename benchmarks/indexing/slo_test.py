"""Offline tests for the S-078 indexing SLO contract and its arithmetic.

Nothing here runs a binary, generates a corpus, opens a database or reaches a
store: `slo.py` is pure, and that is the point. The contract these tests pin is
the one an acceptance criterion is read against, so a future change that
weakens a limit, widens an allowance, or lets an unattributed goal change
through has to edit a test that says so in words.
"""

from __future__ import annotations

import math
import unittest

from benchmarks.indexing import slo


def _stage(
    wall: list[float],
    cpu: list[float],
    front_end: list[float],
    inclusive: list[float],
) -> dict:
    return {
        "wall_seconds_trials": wall,
        "cpu_seconds_trials": cpu,
        "profile_summary": {
            "timings": {
                "clang_front_end": {"trials": front_end},
                "clang_tool_inclusive": {"trials": inclusive},
            }
        },
    }


def _arm(name: str, **overrides) -> dict:
    arm = {
        "arm": name,
        "canonical_sha256": "canonical",
        "normalized_layer0_sha256": "layer0",
        "table_counts": {"symbol": 10, "edge": 4},
        "integrity_check": "ok",
        "foreign_key_check": "ok",
    }
    arm.update(overrides)
    return arm


class AllowanceTest(unittest.TestCase):
    def test_allowance_is_the_greater_of_ten_percent_and_fifty_milliseconds(self):
        # Below 500 ms the absolute floor dominates; above it the relative one.
        self.assertAlmostEqual(slo.regression_allowance(0.100), 0.050)
        self.assertAlmostEqual(slo.regression_allowance(0.500), 0.050)
        self.assertAlmostEqual(slo.regression_allowance(2.000), 0.200)

    def test_a_negative_baseline_is_refused(self):
        with self.assertRaises(slo.SloContractError):
            slo.regression_allowance(-1.0)


class AbsoluteVerdictTest(unittest.TestCase):
    def test_a_median_below_the_limit_passes(self):
        verdict = slo.absolute_verdict("warm", [0.4, 0.5, 0.6], 5.0)
        self.assertTrue(verdict["ok"])
        self.assertAlmostEqual(verdict["median_seconds"], 0.5)
        self.assertAlmostEqual(verdict["headroom_seconds"], 4.5)

    def test_the_limit_is_exclusive(self):
        # A borderline measurement must not be reportable as compliant.
        self.assertFalse(slo.absolute_verdict("warm", [5.0, 5.0, 5.0], 5.0)["ok"])

    def test_fewer_than_three_trials_never_passes(self):
        verdict = slo.absolute_verdict("warm", [0.1, 0.1], 5.0)
        self.assertFalse(verdict["ok"])
        self.assertTrue(verdict["insufficient_trials"])

    def test_the_median_is_used_not_the_best_trial(self):
        verdict = slo.absolute_verdict("warm", [0.1, 6.0, 7.0], 5.0)
        self.assertFalse(verdict["ok"])

    def test_non_numeric_input_is_refused_rather_than_coerced(self):
        with self.assertRaises(slo.SloContractError):
            slo.absolute_verdict("warm", [0.1, "0.2", 0.3], 5.0)
        with self.assertRaises(slo.SloContractError):
            slo.absolute_verdict("warm", [0.1, math.nan, 0.3], 5.0)


class RegressionVerdictTest(unittest.TestCase):
    def test_a_small_regression_inside_the_floor_passes(self):
        verdict = slo.regression_verdict(
            "one-tu", [0.100, 0.100, 0.100], [0.140, 0.140, 0.140]
        )
        self.assertTrue(verdict["ok"])
        self.assertAlmostEqual(verdict["allowance_seconds"], 0.050)

    def test_a_regression_past_the_allowance_fails_without_a_waiver(self):
        verdict = slo.regression_verdict(
            "one-tu", [0.100, 0.100, 0.100], [0.200, 0.200, 0.200]
        )
        self.assertFalse(verdict["ok"])
        self.assertFalse(verdict["within_allowance"])
        self.assertEqual(verdict["waiver_state"], "absent")

    def test_a_complete_waiver_admits_a_larger_regression(self):
        verdict = slo.regression_verdict(
            "one-tu",
            [0.100, 0.100, 0.100],
            [0.400, 0.400, 0.400],
            waiver={"benefit": "3.1x cold", "cause": "publication fsync",
                    "owner": "S-078"},
        )
        self.assertTrue(verdict["ok"])
        self.assertEqual(verdict["waiver_state"], "recorded")

    def test_an_incomplete_waiver_does_not_rescue_the_measurement(self):
        for missing in ("benefit", "cause", "owner"):
            waiver = {"benefit": "b", "cause": "c", "owner": "o"}
            waiver[missing] = "   "
            with self.subTest(missing=missing):
                verdict = slo.regression_verdict(
                    "one-tu", [0.1, 0.1, 0.1], [0.4, 0.4, 0.4], waiver=waiver
                )
                self.assertEqual(verdict["waiver_state"], "malformed")
                self.assertFalse(verdict["ok"])
                self.assertIn(missing, verdict["waiver"]["missing_fields"])

    def test_an_improvement_passes_and_reports_a_negative_delta(self):
        verdict = slo.regression_verdict(
            "warm", [1.0, 1.0, 1.0], [0.5, 0.5, 0.5]
        )
        self.assertTrue(verdict["ok"])
        self.assertLess(verdict["delta_seconds"], 0)

    def test_too_few_trials_fails_even_when_the_delta_is_fine(self):
        verdict = slo.regression_verdict("warm", [1.0, 1.0], [1.0, 1.0])
        self.assertTrue(verdict["within_allowance"])
        self.assertTrue(verdict["insufficient_trials"])
        self.assertFalse(verdict["ok"])


class AmdahlTest(unittest.TestCase):
    def test_the_ceiling_is_the_reciprocal_of_the_irreducible_share(self):
        self.assertAlmostEqual(slo.amdahl_ceiling(0.25), 4.0)
        self.assertAlmostEqual(slo.amdahl_ceiling(0.10), 10.0)

    def test_a_zero_share_imposes_no_ceiling(self):
        self.assertEqual(slo.amdahl_ceiling(0.0), math.inf)

    def test_a_share_outside_zero_to_one_is_refused(self):
        for share in (-0.1, 1.5, math.nan):
            with self.subTest(share=share), self.assertRaises(slo.SloContractError):
                slo.amdahl_ceiling(share)

    def test_shares_come_from_the_measured_stage(self):
        stage = _stage([10.0, 10.0, 10.0], [12.0, 12.0, 12.0],
                       [2.0, 2.0, 2.0], [3.0, 3.0, 3.0])
        shares = slo.front_end_shares(stage)
        self.assertAlmostEqual(shares["front_end_share_of_wall"], 0.2)
        self.assertAlmostEqual(shares["front_end_share_of_cpu"], 2.0 / 12.0)
        self.assertAlmostEqual(shares["tool_inclusive_share_of_wall"], 0.3)

    def test_a_missing_front_end_timing_is_refused(self):
        stage = _stage([1.0], [1.0], [0.1], [0.2])
        del stage["profile_summary"]["timings"]["clang_front_end"]
        with self.assertRaises(slo.SloContractError):
            slo.front_end_shares(stage)


class ColdGoalTest(unittest.TestCase):
    def test_a_met_goal_is_retained_and_passes(self):
        decision = slo.cold_goal_decision(
            measured_speedup=4.5, front_end_share=0.1, disposition="retained",
            attribution=["measured on the reference host"],
        )
        self.assertTrue(decision["ok"])
        self.assertTrue(decision["goal_met"])

    def test_a_retained_but_unmet_goal_fails_rather_than_softening(self):
        decision = slo.cold_goal_decision(
            measured_speedup=2.0, front_end_share=0.1, disposition="retained",
            attribution=["front-end share 10%"],
        )
        self.assertFalse(decision["ok"])
        self.assertFalse(decision["goal_met"])
        self.assertEqual(decision["malformed_reasons"], [])

    def test_any_disposition_needs_attribution(self):
        decision = slo.cold_goal_decision(
            measured_speedup=4.5, front_end_share=0.1, disposition="retained",
        )
        self.assertFalse(decision["ok"])
        self.assertIn("no quantitative attribution recorded",
                      decision["malformed_reasons"])

    def test_superseding_needs_a_replacement_contract(self):
        decision = slo.cold_goal_decision(
            measured_speedup=2.0, front_end_share=0.4, disposition="superseded",
            attribution=["front-end share 40%"],
        )
        self.assertFalse(decision["ok"])
        self.assertIn("superseded without a replacement contract",
                      decision["malformed_reasons"])

    def test_a_replacement_above_the_ceiling_is_malformed(self):
        # Ceiling is 1/0.4 = 2.5x; a 3x replacement is arithmetically absurd.
        decision = slo.cold_goal_decision(
            measured_speedup=2.4, front_end_share=0.4, disposition="superseded",
            replacement={"target": 3.0, "rationale": "wishful"},
            attribution=["front-end share 40%"],
        )
        self.assertFalse(decision["ok"])
        self.assertIn("replacement target exceeds the measured Amdahl ceiling",
                      decision["malformed_reasons"])

    def test_an_unmet_replacement_is_malformed(self):
        decision = slo.cold_goal_decision(
            measured_speedup=1.5, front_end_share=0.4, disposition="superseded",
            replacement={"target": 2.0, "rationale": "front end dominates"},
            attribution=["front-end share 40%"],
        )
        self.assertFalse(decision["ok"])
        self.assertIn("replacement target is not met by the measurement",
                      decision["malformed_reasons"])

    def test_a_reachable_and_met_replacement_supersedes(self):
        decision = slo.cold_goal_decision(
            measured_speedup=2.2, front_end_share=0.4, disposition="superseded",
            replacement={"target": 2.0, "rationale": "front end dominates"},
            attribution=["front-end share 40%", "ceiling 2.5x"],
        )
        self.assertTrue(decision["ok"])
        self.assertAlmostEqual(decision["amdahl_ceiling"], 2.5)
        self.assertTrue(decision["replacement"]["met"])

    def test_rejecting_a_goal_the_ceiling_still_admits_is_malformed(self):
        decision = slo.cold_goal_decision(
            measured_speedup=1.0, front_end_share=0.1, disposition="rejected",
            attribution=["front-end share 10%"],
        )
        self.assertFalse(decision["ok"])
        self.assertIn("goal rejected although the measured ceiling still admits it",
                      decision["malformed_reasons"])

    def test_rejecting_an_unreachable_goal_is_valid(self):
        decision = slo.cold_goal_decision(
            measured_speedup=1.2, front_end_share=0.5, disposition="rejected",
            attribution=["front-end share 50%, ceiling 2x"],
        )
        self.assertTrue(decision["ok"])
        self.assertFalse(decision["goal_within_ceiling"])

    def test_a_replacement_on_a_retained_goal_is_malformed(self):
        decision = slo.cold_goal_decision(
            measured_speedup=4.5, front_end_share=0.1, disposition="retained",
            replacement={"target": 2.0, "rationale": "unused"},
            attribution=["measured"],
        )
        self.assertFalse(decision["ok"])

    def test_an_unknown_disposition_is_refused(self):
        with self.assertRaises(slo.SloContractError):
            slo.cold_goal_decision(
                measured_speedup=1.0, front_end_share=0.1,
                disposition="weakened", attribution=["x"],
            )

    def test_every_front_end_workstream_is_named(self):
        decision = slo.cold_goal_decision(
            measured_speedup=4.5, front_end_share=0.1, disposition="retained",
            attribution=["measured"],
        )
        stories = {entry["story"] for entry in decision["front_end_workstreams"]}
        self.assertEqual(stories, {"S-074", "S-075", "S-076"})


class EquivalenceTest(unittest.TestCase):
    def test_identical_arms_are_equivalent(self):
        verdict = slo.equivalence_verdict("axis", [_arm("a"), _arm("b")])
        self.assertTrue(verdict["ok"])
        self.assertEqual(verdict["reference_arm"], "a")

    def test_a_canonical_difference_fails(self):
        verdict = slo.equivalence_verdict(
            "axis", [_arm("a"), _arm("b", canonical_sha256="other")]
        )
        self.assertFalse(verdict["ok"])
        self.assertIn("b: canonical_sha256 differs from a", verdict["differences"])

    def test_a_layer0_difference_fails_even_when_canonical_agrees(self):
        verdict = slo.equivalence_verdict(
            "axis", [_arm("a"), _arm("b", normalized_layer0_sha256="other")]
        )
        self.assertFalse(verdict["ok"])

    def test_a_table_count_difference_fails_and_names_the_table(self):
        verdict = slo.equivalence_verdict(
            "axis", [_arm("a"), _arm("b", table_counts={"symbol": 10, "edge": 5})]
        )
        self.assertFalse(verdict["ok"])
        self.assertTrue(any("table edge" in text for text in verdict["differences"]))

    def test_a_table_present_in_only_one_arm_fails(self):
        verdict = slo.equivalence_verdict(
            "axis",
            [_arm("a"), _arm("b", table_counts={"symbol": 10, "edge": 4, "x": 1})],
        )
        self.assertFalse(verdict["ok"])

    def test_an_unsound_database_fails_even_with_matching_digests(self):
        for field, value in (("integrity_check", "malformed"),
                             ("foreign_key_check", "failed")):
            with self.subTest(field=field):
                verdict = slo.equivalence_verdict(
                    "axis", [_arm("a"), _arm("b", **{field: value})]
                )
                self.assertFalse(verdict["ok"])

    def test_an_unsound_reference_arm_also_fails(self):
        verdict = slo.equivalence_verdict(
            "axis", [_arm("a", integrity_check="malformed"), _arm("b")]
        )
        self.assertFalse(verdict["ok"])

    def test_a_single_arm_axis_is_refused(self):
        with self.assertRaises(slo.SloContractError):
            slo.equivalence_verdict("axis", [_arm("a")])

    def test_duplicate_arm_names_are_refused(self):
        with self.assertRaises(slo.SloContractError):
            slo.equivalence_verdict("axis", [_arm("a"), _arm("a")])

    def test_an_explicit_reference_must_exist(self):
        with self.assertRaises(slo.SloContractError):
            slo.equivalence_verdict("axis", [_arm("a"), _arm("b")], reference="c")


class ResidualTest(unittest.TestCase):
    def _inputs(self, **overrides):
        base = {
            "root_traversals": {"fixed_root_median": 0.0221,
                                "registered_root_traversal_budget": 16,
                                "observed_root_traversals": 16,
                                "share_of_cold_wall": 0.09,
                                "pre_fusion_baseline_seconds": 0.040611,
                                "corpus": "header-heavy:8:forward"},
            "scaling": {"threshold": 1.4, "implied_exponent": 1.25,
                        "small_corpus": 64, "large_corpus": 1000,
                        "per_tu_small_seconds": 0.0219,
                        "per_tu_large_seconds": 0.0437,
                        "model": "quadratic", "note": "measured"},
            "publication": {"threshold": 0.75, "share_of_cold_wall": 0.5,
                            "writer_seconds_median": 10.0,
                            "cold_wall_seconds_median": 20.0,
                            "corpus": "baseline:1000:forward"},
            "transform_evaluation": {"threshold": 1.0,
                                     "warm_seconds_median": 0.715,
                                     "transform_seconds_median": 0.376,
                                     "executed_transform_seconds": 0.0,
                                     "share_of_warm_wall": 0.525,
                                     "corpus": "baseline:1000:forward"},
        }
        base.update(overrides)
        return base

    def test_every_term_is_named_with_an_owner_and_a_threshold(self):
        result = slo.residual_terms(**self._inputs())
        self.assertTrue(result["ok"])
        self.assertEqual(len(result["terms"]), 4)
        for term in result["terms"]:
            self.assertTrue(term["owner"])
            self.assertTrue(term["threshold_name"])
            self.assertEqual(term["status"], "within-threshold")

    def test_the_root_traversal_term_carries_the_s098_evidence(self):
        result = slo.residual_terms(**self._inputs())
        root = result["terms"][0]
        self.assertIn("two-root fusion", root["note"])
        self.assertEqual(root["context"]["observed_root_traversals"], 16)
        self.assertEqual(root["context"]["share_of_cold_wall"], 0.09)

    def test_a_term_past_its_threshold_stays_open(self):
        inputs = self._inputs()
        inputs["root_traversals"]["fixed_root_median"] = 0.030
        result = slo.residual_terms(**inputs)
        self.assertFalse(result["ok"])
        self.assertEqual(result["terms"][0]["status"], "over-threshold")

    def test_an_unmeasured_term_is_not_reported_as_passing(self):
        inputs = self._inputs()
        inputs["scaling"]["implied_exponent"] = None
        result = slo.residual_terms(**inputs)
        self.assertFalse(result["ok"])
        self.assertEqual(result["terms"][1]["status"], "unmeasured")

    def test_a_boolean_is_not_accepted_as_a_measurement(self):
        inputs = self._inputs()
        inputs["publication"]["share_of_cold_wall"] = True
        result = slo.residual_terms(**inputs)
        self.assertEqual(result["terms"][2]["status"], "unmeasured")

    def test_the_transform_evaluation_term_is_named_with_its_owner(self):
        result = slo.residual_terms(**self._inputs())
        term = result["terms"][3]
        self.assertIn("derived-transform readiness", term["term"])
        self.assertIn("S-077", term["owner"])
        self.assertEqual(term["status"], "within-threshold")
        self.assertEqual(
            term["context"]["absolute_limit_seconds"],
            slo.WARM_ABSOLUTE_LIMIT_SECONDS,
        )

    def test_a_warm_median_past_its_threshold_reopens_the_transform_term(self):
        inputs = self._inputs()
        inputs["transform_evaluation"]["warm_seconds_median"] = 1.5
        result = slo.residual_terms(**inputs)
        self.assertFalse(result["ok"])
        self.assertEqual(result["terms"][3]["status"], "over-threshold")

    def test_the_transform_threshold_leaves_headroom_under_the_warm_limit(self):
        # A term bounded at the absolute limit itself would be no bound at all.
        inputs = self._inputs()
        self.assertLess(
            inputs["transform_evaluation"]["threshold"],
            slo.WARM_ABSOLUTE_LIMIT_SECONDS / 2,
        )


class SloDecisionTest(unittest.TestCase):
    def _green(self):
        return {
            "identity": {"commit": "abc"},
            "absolutes": [slo.absolute_verdict("warm", [0.1, 0.1, 0.1], 5.0)],
            "regressions": [
                slo.regression_verdict("warm", [0.1, 0.1, 0.1], [0.1, 0.1, 0.1])
            ],
            "cold_goal": slo.cold_goal_decision(
                measured_speedup=4.2, front_end_share=0.1,
                disposition="retained", attribution=["measured"],
            ),
            "equivalences": [slo.equivalence_verdict("axis", [_arm("a"), _arm("b")])],
            "residuals": {"ok": True, "failures": [], "terms": []},
            "integrity": {"ok": True, "failures": []},
            "regression_guard": {"ok": True, "failures": []},
            "dependency_invalidation": {"ok": True, "failures": []},
        }

    def test_a_fully_green_decision_publishes(self):
        decision = slo.slo_decision(**self._green())
        self.assertTrue(decision["ok"])
        self.assertEqual(decision["failures"], [])
        self.assertEqual(
            decision["contract"]["warm_absolute_limit_seconds"],
            slo.WARM_ABSOLUTE_LIMIT_SECONDS,
        )

    def test_one_failing_component_fails_the_whole_decision(self):
        for key, replacement, expected in (
            ("absolutes",
             [slo.absolute_verdict("warm", [9.0, 9.0, 9.0], 5.0)],
             "absolute: warm"),
            ("regressions",
             [slo.regression_verdict("warm", [0.1, 0.1, 0.1], [1.0, 1.0, 1.0])],
             "regression: warm"),
            ("equivalences",
             [slo.equivalence_verdict(
                 "axis", [_arm("a"), _arm("b", canonical_sha256="x")])],
             "equivalence: axis"),
            ("residuals",
             {"ok": False, "failures": ["term X is open"], "terms": []},
             "residual: term X is open"),
            ("integrity",
             {"ok": False, "failures": ["a database was not sound"]},
             "integrity: a database was not sound"),
            ("regression_guard",
             {"ok": False, "failures": ["no CI subset is wired"]},
             "regression-guard: no CI subset is wired"),
            ("dependency_invalidation",
             {"ok": False, "failures": ["a high-fan-in header rebuilt 0 units"]},
             "dependency-invalidation: a high-fan-in header rebuilt 0 units"),
        ):
            with self.subTest(component=key):
                arguments = self._green()
                arguments[key] = replacement
                decision = slo.slo_decision(**arguments)
                self.assertFalse(decision["ok"])
                self.assertIn(expected, decision["failures"])

    def test_an_unmet_cold_goal_fails_the_decision(self):
        arguments = self._green()
        arguments["cold_goal"] = slo.cold_goal_decision(
            measured_speedup=1.1, front_end_share=0.1,
            disposition="retained", attribution=["measured"],
        )
        decision = slo.slo_decision(**arguments)
        self.assertFalse(decision["ok"])
        self.assertIn("cold-goal: retained", decision["failures"])

    def test_the_published_contract_matches_the_module_constants(self):
        decision = slo.slo_decision(**self._green())
        contract = decision["contract"]
        self.assertEqual(contract["one_tu_absolute_limit_seconds"],
                         slo.ONE_TU_ABSOLUTE_LIMIT_SECONDS)
        self.assertEqual(contract["regression_relative_tolerance"],
                         slo.REGRESSION_RELATIVE_TOLERANCE)
        self.assertEqual(contract["regression_absolute_tolerance_seconds"],
                         slo.REGRESSION_ABSOLUTE_TOLERANCE_SECONDS)
        self.assertEqual(contract["provisional_cold_speedup_goal"],
                         slo.PROVISIONAL_COLD_SPEEDUP_GOAL)
        self.assertEqual(contract["minimum_trials"], slo.MINIMUM_TRIALS)


class ContractValuesTest(unittest.TestCase):
    """The published numbers themselves, pinned so a silent edit fails here."""

    def test_the_absolute_limits_are_five_and_two_seconds(self):
        self.assertEqual(slo.WARM_ABSOLUTE_LIMIT_SECONDS, 5.0)
        self.assertEqual(slo.ONE_TU_ABSOLUTE_LIMIT_SECONDS, 2.0)

    def test_the_synthetic_1000_tu_cold_target_is_fifteen_minutes(self):
        self.assertEqual(slo.SYNTHETIC_1000_COLD_LIMIT_SECONDS, 900.0)

    def test_the_regression_allowance_is_ten_percent_or_fifty_milliseconds(self):
        self.assertEqual(slo.REGRESSION_RELATIVE_TOLERANCE, 0.10)
        self.assertEqual(slo.REGRESSION_ABSOLUTE_TOLERANCE_SECONDS, 0.050)

    def test_the_provisional_cold_goal_is_four_times(self):
        self.assertEqual(slo.PROVISIONAL_COLD_SPEEDUP_GOAL, 4.0)

    def test_three_trials_is_the_minimum(self):
        self.assertEqual(slo.MINIMUM_TRIALS, 3)


class SpeedupTest(unittest.TestCase):
    def test_speedup_is_median_over_median(self):
        self.assertAlmostEqual(slo.speedup([4.0, 4.0, 4.0], [2.0, 2.0, 2.0]), 2.0)

    def test_a_zero_candidate_is_infinite_rather_than_a_division_error(self):
        self.assertEqual(slo.speedup([1.0], [0.0]), math.inf)


if __name__ == "__main__":
    unittest.main()
