"""Offline tests for the pure parts of the S-078 integrated qualification.

The measuring parts of `integrated.py` need a binary and a corpus and are
exercised by the authoritative run recorded in the SLO report. What is tested
here is everything that decides whether a measurement *counts*: the
pre-feature semantic-delta rule, which tables are excluded from a parity
claim, and the cited-qualification table that the matrix runs. Those are the
places where a report could quietly start claiming more than it measured.
"""

from __future__ import annotations

import unittest

from benchmarks.indexing import integrated


def _snapshot(**counts) -> dict:
    return {"canonical_row_counts": dict(counts)}


class SemanticDeltaTest(unittest.TestCase):
    def test_identical_fact_sets_report_no_delta(self):
        delta = integrated.semantic_delta(
            _snapshot(symbol=10, edge=5), _snapshot(symbol=10, edge=5)
        )
        self.assertTrue(delta["ok"])
        self.assertTrue(delta["identical"])
        self.assertTrue(delta["superset"])

    def test_a_candidate_superset_passes_and_reports_the_growth(self):
        # PERF-002 recovers `uses` edges into header-owned namespaces that the
        # pre-feature binary dropped, so growth is the expected shape.
        delta = integrated.semantic_delta(
            _snapshot(symbol=10, def_edge=39), _snapshot(symbol=10, def_edge=43)
        )
        self.assertTrue(delta["ok"])
        self.assertFalse(delta["identical"])
        self.assertTrue(delta["superset"])
        self.assertEqual(delta["grew"]["def_edge"]["delta"], 4)

    def test_any_shrinking_section_is_a_failure(self):
        delta = integrated.semantic_delta(
            _snapshot(symbol=10, edge=5), _snapshot(symbol=9, edge=5)
        )
        self.assertFalse(delta["ok"])
        self.assertFalse(delta["superset"])
        self.assertIn("symbol", delta["shrank"])
        self.assertTrue(any("shrank" in text for text in delta["failures"]))

    def test_growth_does_not_excuse_a_shrink_elsewhere(self):
        delta = integrated.semantic_delta(
            _snapshot(symbol=10, edge=5), _snapshot(symbol=9, edge=50)
        )
        self.assertFalse(delta["ok"])
        self.assertIn("edge", delta["grew"])
        self.assertIn("symbol", delta["shrank"])

    def test_a_section_only_the_baseline_had_counts_as_a_shrink(self):
        delta = integrated.semantic_delta(
            _snapshot(symbol=10, diagnostic=1), _snapshot(symbol=10)
        )
        self.assertFalse(delta["ok"])
        self.assertIn("diagnostic", delta["shrank"])

    def test_a_section_only_the_candidate_has_counts_as_growth(self):
        delta = integrated.semantic_delta(
            _snapshot(symbol=10), _snapshot(symbol=10, diagnostic=1)
        )
        self.assertTrue(delta["ok"])
        self.assertEqual(delta["grew"]["diagnostic"]["delta"], 1)


class ExcludedTableTest(unittest.TestCase):
    def test_meta_is_volatile_and_never_compared(self):
        # `meta` carries run timestamps and identities; comparing it would fail
        # every honest re-run.
        self.assertIn("meta", integrated.VOLATILE_TABLES)

    def test_the_accelerator_tables_are_reported_separately_not_compared(self):
        # The translation-unit cache is an accelerator: a parallel run does not
        # populate it at all, so its rows must never enter a fact-parity claim.
        self.assertIn("artifact", integrated.ACCELERATOR_TABLES)
        self.assertIn("artifact_relation", integrated.ACCELERATOR_TABLES)
        self.assertFalse(integrated.ACCELERATOR_TABLES & integrated.VOLATILE_TABLES)

    def test_no_core_fact_table_is_excluded(self):
        excluded = integrated.VOLATILE_TABLES | integrated.ACCELERATOR_TABLES
        for table in ("symbol", "edge", "definition", "def_edge", "diagnostic",
                      "fact_applicability", "file", "file_config", "type_node"):
            self.assertNotIn(table, excluded)


class CitedQualificationTest(unittest.TestCase):
    def test_every_entry_names_a_test_a_label_and_what_it_covers(self):
        for entry in integrated.CITED_QUALIFICATION:
            with self.subTest(test=entry.get("test")):
                self.assertTrue(entry["test"])
                self.assertIn(entry["label"], {"default", "clang"})
                self.assertTrue(entry["covers"])
                self.assertTrue(entry["criteria"])

    def test_test_names_are_unique(self):
        names = [entry["test"] for entry in integrated.CITED_QUALIFICATION]
        self.assertEqual(len(names), len(set(names)))

    def test_the_criteria_that_are_cited_rather_than_measured_are_covered(self):
        # These four are the ones the matrix does not re-measure end to end:
        # worker reordering, artifact states, failure atomicity and bounds.
        covered = {
            criterion
            for entry in integrated.CITED_QUALIFICATION
            for criterion in entry["criteria"]
        }
        for criterion in ("AC8", "AC10", "AC11", "AC12", "AC13"):
            self.assertIn(criterion, covered)

    def test_failure_atomicity_cites_the_process_level_injection_test(self):
        entry = next(
            item for item in integrated.CITED_QUALIFICATION
            if item["test"] == "clean_rebuild_process_test"
        )
        self.assertIn("AC11", entry["criteria"])
        self.assertIn("SIGINT", entry["covers"])

    def test_worker_reordering_cites_the_row_by_row_database_test(self):
        entry = next(
            item for item in integrated.CITED_QUALIFICATION
            if item["test"] == "parallel_index_database_test"
        )
        self.assertIn("AC8", entry["criteria"])
        self.assertIn("reverse of dispatch order", entry["covers"])


class ReplayContractTest(unittest.TestCase):
    def test_a_replay_arm_runs_more_than_one_round(self):
        # One round cannot hit: the cold run's entries are keyed under a
        # workspace identity that changes when the cold index completes.
        self.assertGreaterEqual(integrated.REPLAY_ROUNDS, 2)


if __name__ == "__main__":
    unittest.main()
