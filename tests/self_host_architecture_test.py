#!/usr/bin/env python3
"""Positive and mutation tests for the HSE-71 self-host architecture report.

Mirrors the style of tests/architecture_test.py: small synthetic fixtures
(a tiny manifest/policy plus a hand-built SQLite index.db using only the
Layer-0 tables the report reads) rather than a full Clang-built self-index,
so the mutation matrix stays fast and hermetic. scripts/self_host_index.sh
exercises the same report generator against a real self-index separately.
"""

from __future__ import annotations

import json
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.self_host_architecture_report import generate_report  # noqa: E402

SCHEMA = """
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE component (id INTEGER PRIMARY KEY, name TEXT, path TEXT, version TEXT, repository_id INTEGER);
CREATE TABLE directory (id INTEGER PRIMARY KEY, component_id INTEGER, path TEXT);
CREATE TABLE file (id INTEGER PRIMARY KEY, directory_id INTEGER, name TEXT);
CREATE TABLE symbol (id INTEGER PRIMARY KEY, usr TEXT, spelling TEXT, qual_name TEXT, kind INTEGER, file_id INTEGER, line INTEGER);
CREATE TABLE edge_kind (id INTEGER PRIMARY KEY, name TEXT UNIQUE);
CREATE TABLE edge (id INTEGER PRIMARY KEY, src_id INTEGER, dst_id INTEGER, kind INTEGER, count INTEGER DEFAULT 1);
CREATE TABLE edge_site (edge_id INTEGER, file_id INTEGER, line INTEGER, col INTEGER);
"""


class _Fixture:
    """Builds a temp repo directory + manifest/policy + synthetic index.db."""

    def __init__(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="cidx-self-host-", dir="/tmp"))
        (self.root / "src/model").mkdir(parents=True)
        (self.root / "src/cli").mkdir(parents=True)
        (self.root / "src/extraction").mkdir(parents=True)
        (self.root / "src/persistence").mkdir(parents=True)
        (self.root / "src/query").mkdir(parents=True)
        (self.root / "catalogs").mkdir(parents=True)
        (self.root / "src/model/value.hpp").write_text("#pragma once\nstruct Value {};\n", encoding="utf-8")
        (self.root / "src/cli/format.hpp").write_text("#pragma once\nvoid render();\n", encoding="utf-8")
        (self.root / "src/extraction/pass.cpp").write_text(
            '#include "model/value.hpp"\nvoid run() {}\n', encoding="utf-8"
        )
        (self.root / "src/persistence/service.hpp").write_text(
            "#pragma once\nnamespace cidx { class SqliteStorageService { public: void write(); }; }\n",
            encoding="utf-8",
        )
        (self.root / "src/query/plan.cpp").write_text("void execute_plan() {}\n", encoding="utf-8")
        (self.root / "CMakeLists.txt").write_text(
            "add_library(core STATIC src/extraction/pass.cpp)\n"
            "target_link_libraries(core PRIVATE model_lib)\n",
            encoding="utf-8",
        )
        self.manifest = {
            "manifestVersion": 1,
            "repository": "test/fixture",
            "sourceRoots": ["src"],
            "cmakeFiles": ["CMakeLists.txt"],
            "modules": [
                {"id": "model", "paths": ["src/model/**"], "allowedDependencies": ["model"]},
                {"id": "cli", "paths": ["src/cli/**"], "allowedDependencies": ["cli"]},
                {
                    "id": "extraction",
                    "paths": ["src/extraction/**"],
                    "allowedDependencies": ["extraction", "model"],
                },
                {
                    "id": "persistence",
                    "paths": ["src/persistence/**"],
                    "allowedDependencies": ["persistence"],
                },
                {
                    "id": "query",
                    "paths": ["src/query/**"],
                    "allowedDependencies": ["query", "persistence"],
                },
            ],
            "targets": [{"name": "core", "module": "extraction", "links": ["model_lib"]}],
            "contracts": [],
            "exceptions": [],
        }
        self.policy = {
            "apiVersion": "cidx.self-host-architecture-policy/v1",
            "policyVersion": 1,
            "moduleCallGraphCheck": {"enabled": True},
            "legacyFacades": [
                {
                    "id": "storage-facade",
                    "calleeQualNamePrefixes": ["cidx::SqliteStorageService::"],
                    "exemptModules": ["persistence"],
                    "baseline": [],
                }
            ],
            "catalogGuard": {
                "guardedGroups": ["symbol_kinds"],
                "generatedOutputs": ["src/generated_catalog.hpp"],
            },
        }
        (self.root / "catalogs/core.json").write_text(
            json.dumps({"symbol_kinds": [{"id": 8, "name": "function"}], "relations": []}),
            encoding="utf-8",
        )
        self.manifest_path = self.root / "manifest.json"
        self.policy_path = self.root / "policy.json"
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")
        self.policy_path.write_text(json.dumps(self.policy), encoding="utf-8")
        self.index_path = self.root / "index.db"
        self._init_index()

    def _init_index(self) -> None:
        conn = sqlite3.connect(self.index_path)
        conn.executescript(SCHEMA)
        conn.execute("INSERT INTO meta VALUES ('schema_version', '42')")
        conn.execute("INSERT INTO meta VALUES ('catalog_version', '1')")
        conn.execute("INSERT INTO meta VALUES ('catalog_hash', 'deadbeef')")
        conn.execute("INSERT INTO meta VALUES ('graph_resolved_at', '2026-07-25T00:00:00Z')")
        conn.execute("INSERT INTO component VALUES (1, 'fixture', ?, '1', 1)", (str(self.root),))
        conn.execute("INSERT INTO directory VALUES (1, 1, 'src/extraction')")
        conn.execute("INSERT INTO directory VALUES (2, 1, 'src/model')")
        conn.execute("INSERT INTO directory VALUES (3, 1, 'src/persistence')")
        conn.execute("INSERT INTO directory VALUES (4, 1, 'src/cli')")
        conn.execute("INSERT INTO directory VALUES (5, 1, 'src/query')")
        conn.execute("INSERT INTO file VALUES (1, 1, 'pass.cpp')")
        conn.execute("INSERT INTO file VALUES (2, 2, 'value.hpp')")
        conn.execute("INSERT INTO file VALUES (3, 3, 'service.hpp')")
        conn.execute("INSERT INTO file VALUES (4, 4, 'format.hpp')")
        conn.execute("INSERT INTO file VALUES (5, 5, 'plan.cpp')")
        # symbol(id, usr, spelling, qual_name, kind, file_id, line)
        conn.execute("INSERT INTO symbol VALUES (1, 'u1', 'run', 'run', 8, 1, 3)")
        conn.execute("INSERT INTO symbol VALUES (2, 'u2', 'Value', 'Value', 4, 2, 2)")
        conn.execute("INSERT INTO symbol VALUES (3, 'u3', 'write', 'cidx::SqliteStorageService::write', 8, 3, 2)")
        conn.execute("INSERT INTO symbol VALUES (4, 'u4', 'render', 'render', 8, 4, 2)")
        conn.execute("INSERT INTO symbol VALUES (5, 'u5', 'execute_plan', 'execute_plan', 8, 5, 1)")
        conn.execute("INSERT INTO edge_kind VALUES (1, 'calls')")
        conn.execute("INSERT INTO edge_kind VALUES (2, 'dispatch_calls')")
        conn.commit()
        conn.close()

    def add_call(self, edge_id: int, src: int, dst: int, line: int = 1, col: int = 1) -> None:
        conn = sqlite3.connect(self.index_path)
        conn.execute("INSERT INTO edge VALUES (?, ?, ?, 1, 1)", (edge_id, src, dst))
        conn.execute("INSERT INTO edge_site VALUES (?, ?, ?, ?)", (edge_id, src, line, col))
        conn.commit()
        conn.close()

    def run(self, expected_repo_root: str | None = None) -> dict:
        return generate_report(
            self.root, self.manifest_path, self.policy_path, self.index_path, expected_repo_root
        )


class SelfHostArchitectureReportTests(unittest.TestCase):
    def test_clean_fixture_passes(self) -> None:
        fixture = _Fixture()
        fixture.add_call(100, 1, 2)  # extraction -> model: allowed
        report = fixture.run()
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["findings"]["moduleBoundaryViolations"], [])
        self.assertEqual(report["findings"]["legacyFacadeViolations"], [])
        self.assertEqual(report["findings"]["moduleCycles"], [])

    def test_module_boundary_violation_is_rejected_with_witness(self) -> None:
        fixture = _Fixture()
        fixture.add_call(200, 1, 4, line=7, col=3)  # extraction -> cli: not allowed
        report = fixture.run()
        self.assertEqual(report["status"], "fail")
        violations = report["findings"]["moduleBoundaryViolations"]
        self.assertEqual(len(violations), 1)
        self.assertEqual(violations[0]["fromModule"], "extraction")
        self.assertEqual(violations[0]["toModule"], "cli")
        self.assertEqual(violations[0]["witness"]["callee"]["file"], "src/cli/format.hpp")
        self.assertEqual(violations[0]["witness"]["call_site"]["line"], 7)

    def test_clang_style_leakage_out_of_the_model_layer_is_rejected(self) -> None:
        # model's allowedDependencies is ["model"] only -- exactly the ADR-011
        # "model has no Clang/LLVM/SQLite/CLI/process dependency" rule. Any
        # outbound call from a model symbol, to *any* other module, is a
        # violation regardless of what the callee actually is.
        fixture = _Fixture()
        fixture.add_call(500, 2, 1)  # model -> extraction
        report = fixture.run()
        violations = report["findings"]["moduleBoundaryViolations"]
        self.assertEqual(len(violations), 1)
        self.assertEqual(violations[0]["fromModule"], "model")
        self.assertEqual(violations[0]["toModule"], "extraction")

    def test_sqlite_style_leakage_into_extraction_is_rejected(self) -> None:
        # extraction's allowedDependencies do not include persistence; a call
        # straight into the SQLite-owning module without a manifest exception
        # is exactly the "SQLite leakage into extraction" scenario.
        fixture = _Fixture()
        fixture.add_call(501, 1, 3)  # extraction -> persistence
        report = fixture.run()
        violations = report["findings"]["moduleBoundaryViolations"]
        self.assertEqual(len(violations), 1)
        self.assertEqual(violations[0]["fromModule"], "extraction")
        self.assertEqual(violations[0]["toModule"], "persistence")

    def test_upward_dependency_from_query_into_cli_is_rejected(self) -> None:
        # ADR-011: "Query/analysis... Must not own: command parsing"; query's
        # allowedDependencies is ["query", "persistence"] and does not include
        # cli, so a call from query into the product surface is the "CLI
        # dependency from query" upward-dependency scenario.
        fixture = _Fixture()
        fixture.add_call(502, 5, 4)  # query -> cli
        report = fixture.run()
        violations = report["findings"]["moduleBoundaryViolations"]
        self.assertEqual(len(violations), 1)
        self.assertEqual(violations[0]["fromModule"], "query")
        self.assertEqual(violations[0]["toModule"], "cli")

    def test_module_boundary_violation_is_suppressed_by_matching_exception(self) -> None:
        fixture = _Fixture()
        fixture.manifest["exceptions"] = [
            {
                "source": "src/extraction/pass.cpp",
                "fromModule": "extraction",
                "toModule": "cli",
                "boundary": "test",
                "owner": "test",
                "rationale": "test",
                "expiresOn": "2099-01-01",
                "removalIssue": "HSE-58",
            }
        ]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.add_call(200, 1, 4)
        report = fixture.run()
        self.assertEqual(report["findings"]["moduleBoundaryViolations"], [])

    def test_module_cycle_over_resolved_calls_is_rejected(self) -> None:
        fixture = _Fixture()
        # extraction(1) -> cli(4) is disallowed on its own; add cli -> extraction
        # requires a cli-side caller. Reuse symbol 4 (render, in cli) calling
        # back into symbol 1 (run, in extraction) to complete a module cycle.
        fixture.manifest["modules"][1]["allowedDependencies"] = ["cli", "extraction"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.add_call(300, 1, 4)  # extraction -> cli (still undeclared -> violation + edge)
        fixture.add_call(301, 4, 1)  # cli -> extraction (declared allowed)
        report = fixture.run()
        cycles = report["findings"]["moduleCycles"]
        self.assertTrue(any(set(c["cycle"]) == {"extraction", "cli"} for c in cycles))

    def test_legacy_facade_violation_is_rejected(self) -> None:
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model", "persistence"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.add_call(400, 1, 3)  # extraction -> persistence, calling SqliteStorageService::write
        report = fixture.run()
        violations = report["findings"]["legacyFacadeViolations"]
        self.assertEqual(len(violations), 1)
        self.assertEqual(violations[0]["facade"], "storage-facade")
        self.assertEqual(violations[0]["witness"]["callee"]["symbol"], "cidx::SqliteStorageService::write")

    def test_legacy_facade_violation_is_suppressed_by_baseline(self) -> None:
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model", "persistence"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.policy["legacyFacades"][0]["baseline"] = [
            {
                "callerFile": "src/extraction/pass.cpp",
                "calleeQualName": "cidx::SqliteStorageService::write",
                "owner": "test",
                "rationale": "test",
                "expiresOn": "2099-01-01",
                "removalIssue": "HSE-62",
            }
        ]
        fixture.policy_path.write_text(json.dumps(fixture.policy), encoding="utf-8")
        fixture.add_call(400, 1, 3)
        report = fixture.run()
        self.assertEqual(report["findings"]["legacyFacadeViolations"], [])

    def test_baseline_entry_missing_exception_metadata_is_rejected(self) -> None:
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model", "persistence"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.policy["legacyFacades"][0]["baseline"] = [
            {"callerFile": "src/extraction/pass.cpp", "calleeQualName": "cidx::SqliteStorageService::write"}
        ]
        fixture.policy_path.write_text(json.dumps(fixture.policy), encoding="utf-8")
        fixture.add_call(400, 1, 3)
        report = fixture.run()
        # The baseline entry suppresses the semantic finding, but an
        # exception without owner/rationale/expiry/issue is not a valid
        # exception, so the gate still fails on the metadata check.
        self.assertEqual(report["findings"]["legacyFacadeViolations"], [])
        self.assertTrue(
            any("missing or empty" in error for error in report["findings"]["policyMetadata"])
        )
        self.assertEqual(report["status"], "fail")

    def test_expired_baseline_entry_is_rejected(self) -> None:
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model", "persistence"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.policy["legacyFacades"][0]["baseline"] = [
            {
                "callerFile": "src/extraction/pass.cpp",
                "calleeQualName": "cidx::SqliteStorageService::write",
                "owner": "test",
                "rationale": "test",
                "expiresOn": "2020-01-01",
                "removalIssue": "HSE-62",
            }
        ]
        fixture.policy_path.write_text(json.dumps(fixture.policy), encoding="utf-8")
        fixture.add_call(400, 1, 3)
        report = fixture.run()
        self.assertTrue(any("expired on" in error for error in report["findings"]["policyMetadata"]))
        self.assertEqual(report["status"], "fail")

    def test_catalog_duplicate_declaration_outside_generated_catalog_is_rejected(self) -> None:
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair(8, "function");\n', encoding="utf-8"
        )
        report = fixture.run()
        self.assertTrue(any("duplicates a generated catalog declaration" in error for error in report["findings"]["catalogGuard"]))

    def test_generated_marker_outside_declared_outputs_is_rejected(self) -> None:
        fixture = _Fixture()
        (fixture.root / "src/extraction/leak.hpp").write_text(
            "// Generated by scripts/generate_catalogs.py; DO NOT EDIT.\n", encoding="utf-8"
        )
        report = fixture.run()
        self.assertTrue(any("generated-catalog marker found outside generatedOutputs" in error for error in report["findings"]["catalogGuard"]))

    def test_unclassified_source_is_surfaced(self) -> None:
        fixture = _Fixture()
        (fixture.root / "src/unowned.hpp").write_text("#pragma once\n", encoding="utf-8")
        report = fixture.run()
        self.assertTrue(report["unclassifiedSources"])
        self.assertEqual(report["status"], "fail")

    def test_mismatched_repo_root_is_an_identity_issue_not_silence(self) -> None:
        fixture = _Fixture()
        fixture.add_call(100, 1, 2)
        report = fixture.run(expected_repo_root="/nonexistent/other/checkout")
        self.assertTrue(report["completeness"]["identityIssues"])
        self.assertEqual(report["completeness"]["semantic"], "partial")

    def test_empty_call_graph_is_recorded_as_a_limitation_not_a_clean_bill(self) -> None:
        fixture = _Fixture()
        report = fixture.run()
        self.assertTrue(
            any("zero 'calls'" in limitation for limitation in report["unresolvedLimitations"])
        )

    def test_bootstrap_and_semantic_layers_agree_when_include_and_call_both_cross(self) -> None:
        fixture = _Fixture()
        fixture.add_call(200, 1, 4)  # extraction -> cli via resolved call, no #include
        report = fixture.run()
        # No #include from pass.cpp into cli/format.hpp exists in this fixture, so
        # the bootstrap pass cannot see this coupling: that is a real, additional
        # semantic finding, not a disagreement to block on.
        self.assertEqual(report["crossCheck"]["status"], "ok")
        self.assertEqual(report["crossCheck"]["disagreements"], [])

    def test_bootstrap_and_semantic_layers_agree_when_both_observe_the_same_include(self) -> None:
        fixture = _Fixture()
        (fixture.root / "src/extraction/pass.cpp").write_text(
            '#include "model/value.hpp"\n#include "cli/format.hpp"\nvoid run() {}\n', encoding="utf-8"
        )
        fixture.add_call(200, 1, 4)
        report = fixture.run()
        self.assertEqual(report["crossCheck"]["status"], "ok")
        self.assertEqual(report["crossCheck"]["disagreements"], [])

    def test_disagreement_between_bootstrap_and_semantic_exception_matching_blocks_the_gate(self) -> None:
        # An exception with a specific `include` spelling only suppresses the
        # bootstrap include-scan finding when the spelling matches exactly
        # (scripts/check_architecture.py._exception_matches); the semantic
        # pass here only keys exceptions by (source, fromModule, toModule)
        # and ignores `include`. Giving the exception a spelling that will
        # never appear in the fixture file makes the two checkers disagree
        # on the exact same fact, which must block the gate rather than
        # being silently resolved by either side.
        fixture = _Fixture()
        (fixture.root / "src/extraction/pass.cpp").write_text(
            '#include "model/value.hpp"\n#include "cli/format.hpp"\nvoid run() {}\n', encoding="utf-8"
        )
        fixture.manifest["exceptions"] = [
            {
                "source": "src/extraction/pass.cpp",
                "fromModule": "extraction",
                "toModule": "cli",
                "include": "cli/never_spelled_this_way.hpp",
                "boundary": "test",
                "owner": "test",
                "rationale": "test",
                "expiresOn": "2099-01-01",
                "removalIssue": "HSE-58",
            }
        ]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.add_call(200, 1, 4)
        report = fixture.run()
        self.assertEqual(report["crossCheck"]["status"], "blocked")
        self.assertTrue(report["crossCheck"]["disagreements"])
        self.assertEqual(report["status"], "fail")


if __name__ == "__main__":
    unittest.main()
