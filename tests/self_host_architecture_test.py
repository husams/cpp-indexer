#!/usr/bin/env python3
"""Positive and mutation tests for the HSE-71 self-host architecture report.

Mirrors python/tests/test_queryplan.py's own convention
(`_seed_reverse_typed_graph`): the fixture is a REAL `Storage`, built
through its own mutation API (`add_component`/`add_symbol`/`add_edge`/...),
not a hand-rolled SQL schema. This keeps the fixture guaranteed
schema-compatible with whatever `Storage`/`Executor`/QueryPlan expect, and
lets the mutation tests exercise the exact same read path
(`scripts/self_host_architecture_report.py` routes symbol/edge/site facts
through `indexer.queryplan.Executor`) that a real self-index run does.
`scripts/self_host_index.sh` exercises the full pipeline against a real
self-index separately.
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
sys.path.insert(0, str(ROOT / "python"))

from scripts.self_host_architecture_report import generate_report  # noqa: E402
from indexer.storage import Storage, Symbol  # noqa: E402

CALLS_KIND = 1  # catalogs/core.json relation id for "calls"
DISPATCH_CALLS_KIND = 18  # ... for "dispatch_calls"


class _Fixture:
    """A tiny repo directory + manifest/policy + a REAL Storage-built index.db."""

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
        self._build_index()

    def _build_index(self) -> None:
        db = Storage(str(self.index_path))
        self._db = db  # kept open for add_call()/mutate(); auto-commits per call
        component = db.add_component("fixture", str(self.root))
        dir_extraction = db.add_directory(component, "src/extraction")
        dir_model = db.add_directory(component, "src/model")
        dir_persistence = db.add_directory(component, "src/persistence")
        dir_cli = db.add_directory(component, "src/cli")
        dir_query = db.add_directory(component, "src/query")
        # Only real translation units carry compile_options (a header is
        # never separately compiled); matches what a real `cidx import`
        # records from compile_commands.json.
        self.file_pass = db.add_file(dir_extraction, "pass.cpp", compile_options=["-std=c++23"])
        self.file_value = db.add_file(dir_model, "value.hpp")
        self.file_service = db.add_file(dir_persistence, "service.hpp")
        self.file_format = db.add_file(dir_cli, "format.hpp")
        self.file_plan = db.add_file(dir_query, "plan.cpp", compile_options=["-std=c++23"])

        def sym(usr, spelling, qual_name, kind, file_id, line):
            return db.add_symbol(
                Symbol(
                    usr=usr, spelling=spelling, qual_name=qual_name, kind=kind,
                    file_id=file_id, line=line, is_definition=True, resolved=True,
                )
            )

        self.sym_run = sym("u1", "run", "run", "function", self.file_pass, 3)
        self.sym_value = sym("u2", "Value", "Value", "class", self.file_value, 2)
        self.sym_write = sym(
            "u3", "write", "cidx::SqliteStorageService::write", "function",
            self.file_service, 2,
        )
        self.sym_render = sym("u4", "render", "render", "function", self.file_format, 2)
        self.sym_plan = sym("u5", "execute_plan", "execute_plan", "function", self.file_plan, 1)
        db._conn.execute(
            "INSERT OR REPLACE INTO meta(key, value) VALUES ('graph_resolved_at', ?)",
            ("2026-07-25T00:00:00Z",),
        )
        # A real `cidx import` registers a file (indexed=0) and a real `cidx
        # index` then parses it (indexed=1); this fixture skips straight to
        # inserting already-resolved symbols/edges, so mark every registered
        # file as actually indexed to match that end state.
        db._conn.execute("UPDATE file SET indexed = 1")
        db._conn.commit()
        # A real `cidx import`/`index`/`resolve` pipeline stamps the HSE-32
        # source/config identity (src/cli/commands_ingest.cpp,
        # src/application/services.cpp both call stamp_index_identity()).
        # Stamping it here makes this fixture's freshness computation
        # meaningful ("current") for the clean case, matching what a real
        # self-index actually leaves behind.
        db.stamp_index_identity()
        db._conn.commit()

    def add_call(self, src: int, dst: int, line: int = 1, col: int = 1, kind: int = CALLS_KIND) -> int:
        edge_id = self._db.add_edge(src, dst, kind)
        self._db.add_edge_site(edge_id, self._db._conn.execute(
            "SELECT file_id FROM symbol WHERE id = ?", (src,)
        ).fetchone()[0], line, col)
        # add_edge()/add_edge_site() are low-level primitives that do not
        # auto-commit (unlike add_component()/add_file()); commit explicitly
        # so a separately-opened read-only connection (the report generator)
        # can see the write.
        self._db._conn.commit()
        return edge_id

    def run(self, expected_repo_root: str | None = None) -> dict:
        return generate_report(
            self.root, self.manifest_path, self.policy_path, self.index_path, expected_repo_root
        )


class SelfHostArchitectureReportTests(unittest.TestCase):
    def test_clean_fixture_passes(self) -> None:
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_value)  # extraction -> model: allowed
        report = fixture.run()
        self.assertEqual(report["status"], "pass", report)
        self.assertEqual(report["findings"]["moduleBoundaryViolations"], [])
        self.assertEqual(report["findings"]["legacyFacadeViolations"], [])
        self.assertEqual(report["findings"]["moduleCycles"], [])

    def test_module_boundary_violation_is_rejected_with_witness(self) -> None:
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_render, line=7, col=3)  # extraction -> cli: not allowed
        report = fixture.run()
        self.assertEqual(report["status"], "fail")
        violations = report["findings"]["moduleBoundaryViolations"]
        self.assertEqual(len(violations), 1)
        self.assertEqual(violations[0]["fromModule"], "extraction")
        self.assertEqual(violations[0]["toModule"], "cli")
        self.assertEqual(violations[0]["witness"]["callee"]["file"], "src/cli/format.hpp")
        self.assertEqual(violations[0]["witness"]["call_site"]["line"], 7)

    def test_bulk_queries_are_not_truncated_by_the_default_1000_row_result_cap(self) -> None:
        # [Review round 2, blocker 1] QueryPlan applies a DEFAULT_RESULT_CAP
        # (1,000 rows) to any query with no explicit limit() stage. A
        # realistically sized self-index comfortably exceeds 1,000 symbols;
        # the reviewer's own 1,105-symbol fixture hit this and produced
        # status=fail/enumerationTruncated=true even though nothing was
        # actually wrong. Reproduce a >1,000-symbol index and assert the
        # bulk symbol/site reads request up to QueryPlan's real enumeration
        # budget (not the artificial default), so this never truncates
        # until a codebase is genuinely large enough to hit that budget.
        fixture = _Fixture()
        last_id = None
        for i in range(1100):
            last_id = fixture._db.add_symbol(
                Symbol(
                    usr=f"bulk::u{i}", spelling=f"bulk_{i}", qual_name=f"bulk_{i}",
                    kind="function", file_id=fixture.file_pass,
                    line=1000 + i, is_definition=True, resolved=True,
                )
            )
        fixture._db._conn.commit()
        # A call between two symbols near the END of that bulk range proves
        # the read isn't silently capped before reaching them.
        fixture.add_call(last_id, fixture.sym_render, line=42, col=1)
        report = fixture.run()
        self.assertFalse(report["index"]["coverage"]["enumerationTruncated"])
        self.assertEqual(len(report["findings"]["moduleBoundaryViolations"]), 1)
        self.assertEqual(report["index"]["symbolCount"], 1105)

    def test_clang_style_leakage_out_of_the_model_layer_is_rejected(self) -> None:
        # model's allowedDependencies is ["model"] only -- exactly the ADR-011
        # "model has no Clang/LLVM/SQLite/CLI/process dependency" rule. Any
        # outbound call from a model symbol, to *any* other module, is a
        # violation regardless of what the callee actually is.
        fixture = _Fixture()
        fixture.add_call(fixture.sym_value, fixture.sym_run)  # model -> extraction
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
        fixture.add_call(fixture.sym_run, fixture.sym_write)  # extraction -> persistence
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
        fixture.add_call(fixture.sym_plan, fixture.sym_render)  # query -> cli
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
        fixture.add_call(fixture.sym_run, fixture.sym_render)
        report = fixture.run()
        self.assertEqual(report["findings"]["moduleBoundaryViolations"], [])

    def test_module_cycle_over_resolved_calls_is_rejected(self) -> None:
        fixture = _Fixture()
        fixture.manifest["modules"][1]["allowedDependencies"] = ["cli", "extraction"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.add_call(fixture.sym_run, fixture.sym_render)  # extraction -> cli (still a violation)
        fixture.add_call(fixture.sym_render, fixture.sym_run)  # cli -> extraction (declared allowed)
        report = fixture.run()
        cycles = report["findings"]["moduleCycles"]
        self.assertTrue(any(set(c["cycle"]) == {"extraction", "cli"} for c in cycles))

    def test_legacy_facade_violation_is_rejected(self) -> None:
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model", "persistence"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.add_call(fixture.sym_run, fixture.sym_write)
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
                "line": 40,
                "col": 5,
                "owner": "test",
                "rationale": "test",
                "expiresOn": "2099-01-01",
                "removalIssue": "HSE-62",
            }
        ]
        fixture.policy_path.write_text(json.dumps(fixture.policy), encoding="utf-8")
        fixture.add_call(fixture.sym_run, fixture.sym_write, line=40, col=5)
        report = fixture.run()
        self.assertEqual(report["findings"]["legacyFacadeViolations"], [])

    def test_new_call_site_at_an_already_baselined_file_callee_pair_is_still_rejected(self) -> None:
        # [Review blocker 4] a baseline entry pins ONE call site; a second,
        # later call from the same file into the same facade callee, at a
        # DIFFERENT line/col, must not be silently grandfathered by the
        # first entry.
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model", "persistence"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.policy["legacyFacades"][0]["baseline"] = [
            {
                "callerFile": "src/extraction/pass.cpp",
                "calleeQualName": "cidx::SqliteStorageService::write",
                "line": 40,
                "col": 5,
                "owner": "test",
                "rationale": "test",
                "expiresOn": "2099-01-01",
                "removalIssue": "HSE-62",
            }
        ]
        fixture.policy_path.write_text(json.dumps(fixture.policy), encoding="utf-8")
        fixture.add_call(fixture.sym_run, fixture.sym_write, line=40, col=5)  # baselined site: OK
        fixture.add_call(fixture.sym_run, fixture.sym_write, line=99, col=9)  # NEW site: not OK
        report = fixture.run()
        violations = report["findings"]["legacyFacadeViolations"]
        self.assertEqual(len(violations), 1)
        self.assertEqual(violations[0]["witness"]["call_site"], {"line": 99, "col": 9})

    def test_every_call_site_of_an_aggregated_edge_is_preserved(self) -> None:
        # [Review blocker 4] `edge` dedups by (src,dst,kind) but `edge_site`
        # can carry many rows for that one edge; all of them must surface,
        # not just the first one encountered.
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model", "persistence"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        edge_id = fixture._db.add_edge(fixture.sym_run, fixture.sym_write, CALLS_KIND)
        fixture._db.add_edge_site(edge_id, fixture.file_pass, 10, 1)
        fixture._db.add_edge_site(edge_id, fixture.file_pass, 20, 2)
        fixture._db.add_edge_site(edge_id, fixture.file_pass, 30, 3)
        fixture._db._conn.commit()
        report = fixture.run()
        violations = report["findings"]["legacyFacadeViolations"]
        sites = {(v["witness"]["call_site"]["line"], v["witness"]["call_site"]["col"]) for v in violations}
        self.assertEqual(sites, {(10, 1), (20, 2), (30, 3)})

    def test_baseline_entry_missing_exception_metadata_is_rejected(self) -> None:
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model", "persistence"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.policy["legacyFacades"][0]["baseline"] = [
            {"callerFile": "src/extraction/pass.cpp", "calleeQualName": "cidx::SqliteStorageService::write",
             "line": 40, "col": 5}
        ]
        fixture.policy_path.write_text(json.dumps(fixture.policy), encoding="utf-8")
        fixture.add_call(fixture.sym_run, fixture.sym_write, line=40, col=5)
        report = fixture.run()
        self.assertEqual(report["findings"]["legacyFacadeViolations"], [])
        self.assertTrue(
            any("missing or empty" in error for error in report["findings"]["policyMetadata"])
        )
        self.assertEqual(report["status"], "fail")

    def test_baseline_entry_missing_call_site_is_rejected(self) -> None:
        # [Review blocker 4] a baseline entry without line/col would
        # grandfather every call from that file to that callee -- exactly
        # the bug the reviewer found. Metadata validation must refuse it.
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model", "persistence"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.policy["legacyFacades"][0]["baseline"] = [
            {
                "callerFile": "src/extraction/pass.cpp",
                "calleeQualName": "cidx::SqliteStorageService::write",
                "owner": "test", "rationale": "test",
                "expiresOn": "2099-01-01", "removalIssue": "HSE-62",
            }
        ]
        fixture.policy_path.write_text(json.dumps(fixture.policy), encoding="utf-8")
        report = fixture.run()
        self.assertTrue(
            any("must pin an exact call site" in error for error in report["findings"]["policyMetadata"])
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
                "line": 40, "col": 5,
                "owner": "test",
                "rationale": "test",
                "expiresOn": "2020-01-01",
                "removalIssue": "HSE-62",
            }
        ]
        fixture.policy_path.write_text(json.dumps(fixture.policy), encoding="utf-8")
        fixture.add_call(fixture.sym_run, fixture.sym_write, line=40, col=5)
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
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        report = fixture.run(expected_repo_root="/nonexistent/other/checkout")
        self.assertTrue(report["completeness"]["identityIssues"])
        self.assertEqual(report["completeness"]["semantic"], "partial")

    def test_empty_call_graph_is_recorded_as_a_limitation_not_a_clean_bill(self) -> None:
        fixture = _Fixture()
        report = fixture.run()
        self.assertTrue(
            any("zero 'calls'" in limitation for limitation in report["unresolvedLimitations"])
        )

    def test_deleted_file_row_with_a_dangling_call_edge_fails_closed(self) -> None:
        # [Review blocker 2] the reviewer's exact repro: delete an expected
        # indexed file row while leaving its call edge in place. The
        # violating caller/callee must not simply vanish from evidence --
        # the gap itself must be surfaced and the gate must not pass.
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_render, line=7, col=3)  # extraction -> cli
        fixture._db._conn.execute("DELETE FROM file WHERE id = ?", (fixture.file_format,))
        fixture._db._conn.commit()
        report = fixture.run()
        self.assertEqual(report["status"], "fail")
        self.assertEqual(report["findings"]["moduleBoundaryViolations"], [])  # the edge is now unwitnessable
        self.assertEqual(report["index"]["coverage"]["unwitnessedCallSites"], 1)
        self.assertTrue(
            any("cannot be witnessed" in issue for issue in report["completeness"]["identityIssues"])
        )
        self.assertEqual(report["completeness"]["semantic"], "partial")

    def test_missing_expected_translation_unit_fails_closed(self) -> None:
        # [Review blocker 2] a manifest-classified .cpp production source
        # that never made it into the index (e.g. skipped by the compile
        # database) must be surfaced, not silently treated as "nothing to
        # report".
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        fixture._db._conn.execute(
            "DELETE FROM file WHERE id = ?", (fixture.file_pass,)
        )
        # pass.cpp's own symbols/edges still reference file_pass (dangling),
        # so this also seeds the dangling-reference path; the missing-TU
        # message must additionally name pass.cpp specifically.
        fixture._db._conn.commit()
        report = fixture.run()
        self.assertTrue(
            any("translation unit" in issue and "pass.cpp" in issue
                for issue in report["completeness"]["identityIssues"])
        )
        self.assertEqual(report["status"], "fail")

    def test_fatal_diagnostic_fails_closed(self) -> None:
        # [Review blocker 2] a file with a recorded fatal parse diagnostic
        # has unreliable facts; that must be surfaced, not silently passed.
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        fixture._db._conn.execute(
            "INSERT INTO diagnostic (file_id, severity, spelling) VALUES (?, 4, 'fatal error: boom')",
            (fixture.file_pass,),
        )
        fixture._db._conn.commit()
        report = fixture.run()
        self.assertTrue(
            any("fatal/error diagnostic" in issue for issue in report["completeness"]["identityIssues"])
        )
        self.assertEqual(report["status"], "fail")

    def test_stale_source_fingerprint_fails_closed(self) -> None:
        # [Review round 2, blocker 2] verify real HSE-32 source/config
        # identity (Storage.index_identity()), not just schema/catalog
        # version. Modifying an indexed file's content on disk AFTER the
        # index was stamped must be caught: freshness recomputes the
        # fingerprint from the files on disk right now and compares it
        # against what was stamped at index time.
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        (fixture.root / "src/extraction/pass.cpp").write_text(
            '#include "model/value.hpp"\nvoid run() {}\n// modified after indexing\n',
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertTrue(
            any("source/config identity" in issue for issue in report["completeness"]["identityIssues"])
        )
        self.assertEqual(report["index"]["coverage"]["indexIdentity"]["freshness"], "stale")
        self.assertEqual(report["status"], "fail")

    def test_missing_index_identity_metadata_fails_closed(self) -> None:
        # An index that never stamped HSE-32 identity at all (e.g. an older
        # `cidx` build, or a hand-built test database) is "unverifiable",
        # not "current" -- it must not be treated as a clean bill of health
        # just because nothing was ever recorded to contradict.
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        for key in ("source_revision", "source_fingerprint", "index_config", "index_config_fingerprint", "index_identity_version"):
            fixture._db._conn.execute("DELETE FROM meta WHERE key = ?", (key,))
        fixture._db._conn.commit()
        report = fixture.run()
        self.assertEqual(report["index"]["coverage"]["indexIdentity"]["freshness"], "unverifiable")
        self.assertTrue(
            any("source/config identity" in issue for issue in report["completeness"]["identityIssues"])
        )
        self.assertEqual(report["status"], "fail")

    def test_missing_build_config_on_a_translation_unit_fails_closed(self) -> None:
        # [Review round 2, blocker 2] a translation unit indexed without a
        # recorded compile_options entry has unverifiable build identity.
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        fixture._db._conn.execute(
            "UPDATE file SET compile_options = NULL WHERE id = ?", (fixture.file_pass,)
        )
        fixture._db._conn.commit()
        report = fixture.run()
        self.assertEqual(report["index"]["coverage"]["missingBuildConfig"], 1)
        self.assertTrue(
            any("compile_options" in issue for issue in report["completeness"]["identityIssues"])
        )
        self.assertEqual(report["status"], "fail")

    def test_missing_build_config_on_a_header_is_not_flagged(self) -> None:
        # A header legitimately has no compile_options of its own (it is
        # never separately compiled); the build-identity check must be
        # scoped to actual translation units, not every indexed row.
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        report = fixture.run()
        self.assertEqual(report["index"]["coverage"]["missingBuildConfig"], 0)

    def test_stale_schema_version_fails_closed(self) -> None:
        # [Review blocker 2] validate schema identity against this
        # checkout's own current expectation, not just record it.
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        fixture._db._conn.execute(
            "UPDATE meta SET value = '1' WHERE key = 'schema_version'"
        )
        fixture._db._conn.commit()
        report = fixture.run()
        self.assertTrue(
            any("schema_version" in issue and "stale" in issue
                for issue in report["completeness"]["identityIssues"])
        )
        self.assertEqual(report["status"], "fail")

    def test_stale_catalog_hash_fails_closed(self) -> None:
        # Storage.from_connection validates the catalog hash itself and
        # raises before this script's own check_index_coverage() ever runs;
        # the report must still fail closed with a clear identity issue
        # rather than an unhandled crash.
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        fixture._db._conn.execute(
            "UPDATE meta SET value = 'not-the-real-hash' WHERE key = 'catalog_hash'"
        )
        fixture._db._conn.commit()
        report = fixture.run()
        self.assertTrue(
            any("could not open index" in issue and "catalog" in issue
                for issue in report["completeness"]["identityIssues"])
        )
        self.assertEqual(report["status"], "fail")

    def test_pending_unindexed_file_fails_closed(self) -> None:
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        fixture._db._conn.execute("UPDATE file SET indexed = 0 WHERE id = ?", (fixture.file_pass,))
        fixture._db._conn.commit()
        report = fixture.run()
        self.assertTrue(
            any("not yet indexed" in issue for issue in report["completeness"]["identityIssues"])
        )
        self.assertEqual(report["status"], "fail")

    def test_package_hash_is_independent_of_checkout_location(self) -> None:
        # [Review blocker 3] byte-identical manifest/policy/checker sources
        # in two different clean checkout directories must hash the same.
        first = _Fixture()
        first.add_call(first.sym_run, first.sym_value)
        first_report = first.run()

        # Copy the exact same manifest/policy/catalog bytes under a DIFFERENT root directory.
        second_root = Path(tempfile.mkdtemp(prefix="cidx-self-host-other-location-", dir="/tmp"))
        (second_root / "catalogs").mkdir(parents=True)
        second_manifest_path = second_root / "manifest.json"
        second_policy_path = second_root / "policy.json"
        second_manifest_path.write_text(first.manifest_path.read_text(encoding="utf-8"), encoding="utf-8")
        second_policy_path.write_text(first.policy_path.read_text(encoding="utf-8"), encoding="utf-8")
        (second_root / "catalogs/core.json").write_text(
            (first.root / "catalogs/core.json").read_text(encoding="utf-8"), encoding="utf-8"
        )
        second_report = generate_report(
            second_root, second_manifest_path, second_policy_path, first.index_path, None
        )
        self.assertEqual(
            first_report["config"]["packageHash"], second_report["config"]["packageHash"]
        )

    def test_bootstrap_and_semantic_layers_agree_when_include_and_call_both_cross(self) -> None:
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_render)  # extraction -> cli via resolved call, no #include
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
        fixture.add_call(fixture.sym_run, fixture.sym_render)
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
        fixture.add_call(fixture.sym_run, fixture.sym_render)
        report = fixture.run()
        self.assertEqual(report["crossCheck"]["status"], "blocked")
        self.assertTrue(report["crossCheck"]["disagreements"])
        self.assertEqual(report["status"], "fail")


if __name__ == "__main__":
    unittest.main()
