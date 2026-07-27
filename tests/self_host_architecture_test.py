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

import hashlib
import json
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "python"))

from scripts.self_host_architecture_report import (  # noqa: E402
    _edge_site_count,
    _eval_cpp_const_expr,
    ModuleGraph,
    SemanticFacts,
    check_policy_metadata,
    find_legacy_facade_violations,
    generate_report,
)
from indexer.storage import Storage, Symbol  # noqa: E402
from indexer import queryplan as qp  # noqa: E402

CALLS_KIND = 1  # catalogs/core.json relation id for "calls"
DISPATCH_CALLS_KIND = 18  # ... for "dispatch_calls"
CONSTRUCT_VALUE_KIND = 10  # ... for "construct-value"


def _is_construction_style_baseline(qual_name: str) -> bool:
    """True for a "Record::Record"-shaped `calleeQualName` (the identity
    `_constructor_style_name` synthesizes for a direct-construction witness
    -- see scripts/self_host_architecture_report.py), as opposed to a plain
    member-call baseline entry (e.g. "cidx::SqliteStorageService::
    index_identity", whose leaf differs from its owning class).
    """
    parts = qual_name.split("::")
    return len(parts) >= 2 and parts[-1] == parts[-2]


def _derived_construction_witness_column(entry: dict) -> int | None:
    """Re-derive, from the REAL committed source file named by a
    "Record::Record"-shaped baseline `entry`, the column a real
    `CXXConstructExpr` witness would report for it -- without needing a
    built self-index (see the hard constraint against running `cidx index`
    in review rounds).

    For a declared local `Type var(args);`, `emit_construction_form`
    (src/ast/statement_edge_visitor.cpp) anchors the site at the
    construct-expression's own begin loc, which for direct-init is Clang's
    declared-VARIABLE position, never the type spelling (the type is part
    of the VarDecl's declarator, not the CXXConstructExpr's own source
    range). This locates that identifier textually: it is the last
    whitespace-separated token between the baseline's recorded column and
    the FIRST '(' at/after it.

    Returns None for a `make_unique<Type>(...)`-shaped heap-construction
    entry (no declared variable at all -- the witness anchors on the
    template argument's own `Type` spelling there instead, which the
    baseline already records correctly): detected by a '<'/'>' between the
    recorded column and that same '('.
    """
    if not _is_construction_style_baseline(entry["calleeQualName"]):
        return None
    path = ROOT / entry["callerFile"]
    line_text = path.read_text(encoding="utf-8").splitlines()[entry["line"] - 1]
    col = entry["col"]
    paren = line_text.find("(", col - 1)
    if paren == -1:
        return None
    between = line_text[col - 1 : paren]
    if "<" in between or ">" in between:
        return None
    tokens = between.split()
    if not tokens:
        return None
    variable = tokens[-1]
    return (col - 1) + between.rindex(variable) + 1


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

        def sym(usr, spelling, qual_name, kind, file_id, line, decl_path=None):
            return db.add_symbol(
                Symbol(
                    usr=usr, spelling=spelling, qual_name=qual_name, kind=kind,
                    file_id=file_id, line=line, is_definition=True, resolved=True,
                    decl_path=decl_path,
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
        # [P1-1 fixture] a resolved callee the way a real self-index's
        # `ast::qualified_name` actually renders one -- signature-bearing
        # (`src/ast/names.hpp`'s "leaf function/method carries its full
        # signature"), unlike `sym_write` above, which -- like every other
        # synthetic symbol in this fixture -- is deliberately bare and would
        # never expose a baseline-key mismatch bug.
        self.sym_write_sig = sym(
            "u6", "write", "cidx::SqliteStorageService::write(const std::string &)",
            "function", self.file_service, 3,
        )
        # [P1-2 fixture] the RECORD `cidx::SqliteStorageService` itself, the
        # only identity a construction-kind edge's destination carries (see
        # `CONSTRUCTION_EDGE_KINDS`): `statement_edge_visitor.cpp`'s
        # `emit_construction_form` resolves a direct construction's `dst_id`
        # to the constructed TYPE, never a constructor symbol.
        self.sym_service_class = sym(
            "u7", "SqliteStorageService", "cidx::SqliteStorageService", "class",
            self.file_service, 1,
        )
        # [moduleCallGraphCheck false-positive fixture] an implicit
        # instantiation of an external-library template the way the real
        # extraction engine has been observed to misattribute one: `file_id`
        # points at a REGISTERED PROJECT file (here, the "extraction" module's
        # own `pass.cpp` -- standing in for the real bug's `src/ast/
        # indexing_plan.hpp`), but `decl_path` still names the symbol's true,
        # external declaration site (a path that can never resolve under
        # `self.root`). Any caller reaching this symbol must never be treated
        # as calling INTO a project module merely because of the
        # mis-attributed `file_id` -- see `_external_decl_path` in
        # scripts/self_host_architecture_report.py.
        self.sym_std_operator_plus = sym(
            "u8", "operator+", "std::operator+", "function",
            self.file_pass, 9, decl_path="/usr/include/c++/v1/string",
        )
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

    def test_module_boundary_violations_preserve_every_call_site_at_the_same_caller_callee_pair(
        self,
    ) -> None:
        # [Internal critic finding, PR #67 @ 40b1e93] find_module_boundary_
        # violations used to key a finding by (caller_file, from_module,
        # to_module, callee_symbol) alone -- one edge with three violating
        # call sites collapsed to a single finding, silently discarding the
        # other two. That is the same evidence-loss pattern already guarded
        # against for find_legacy_facade_violations (which keys per
        # call-site line/col); the two checkers must agree about what a
        # violation is. Seed the SAME edge (one add_edge call, idempotent)
        # with two distinct call sites and assert both are reported.
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_render, line=100, col=3)  # extraction -> cli
        fixture.add_call(fixture.sym_run, fixture.sym_render, line=200, col=5)  # same edge, 2nd site
        report = fixture.run()
        violations = report["findings"]["moduleBoundaryViolations"]
        self.assertEqual(len(violations), 2)
        reported_lines = sorted(v["witness"]["call_site"]["line"] for v in violations)
        self.assertEqual(reported_lines, [100, 200])

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
        self.assertEqual(report["index"]["symbolCount"], 1108)

    def test_violation_past_enumerate_budget_is_not_silently_dropped(self) -> None:
        # [Review round 3, blocker 1] a single Executor.run() call's own
        # symbol/edge enumeration is hard-capped at QueryPlan's real
        # ENUMERATE_BUDGET/TRAVERSE_NODE_BUDGET (10,000) -- a ceiling
        # `limit()` cannot lift, since it is enforced inside the
        # enumeration's own SQL before `limit()` ever runs. Before genuine
        # pagination, a violating caller/callee symbol whose id fell beyond
        # that first page was invisible to `_witness()` (its file was never
        # captured), so `find_module_boundary_violations` silently skipped
        # it -- exactly "absence of evidence of a violation" being produced
        # by an incomplete read, not evidence the code is clean. Seed
        # ENUMERATE_BUDGET filler symbols (pushing every later symbol's id
        # past the first page), then a real, late cross-module violation,
        # and prove it is still found -- with a genuine witness, not merely
        # a report that fails for the unrelated reason of an unresolvable
        # symbol reference.
        fixture = _Fixture()
        filler_count = qp.ENUMERATE_BUDGET
        fixture._db._conn.execute(
            "WITH RECURSIVE lines(n) AS (SELECT 0 UNION ALL SELECT n + 1 "
            f"FROM lines WHERE n < {filler_count - 1}) "
            "INSERT INTO symbol(usr, spelling, kind, file_id) "
            f"SELECT 'USR::pagination-filler-' || n, 'filler' || n, 8, {fixture.file_pass} FROM lines"
        )
        fixture._db._conn.commit()

        late_caller = fixture._db.add_symbol(
            Symbol(
                usr="USR::late-caller", spelling="late_run", qual_name="late_run",
                kind="function", file_id=fixture.file_pass, line=99,
                is_definition=True, resolved=True,
            )
        )
        late_callee = fixture._db.add_symbol(
            Symbol(
                usr="USR::late-callee", spelling="late_render", qual_name="late_render",
                kind="function", file_id=fixture.file_format, line=11,
                is_definition=True, resolved=True,
            )
        )
        fixture.add_call(late_caller, late_callee, line=99, col=5)  # extraction -> cli

        report = fixture.run()
        self.assertFalse(report["index"]["coverage"]["enumerationTruncated"])
        self.assertEqual(report["index"]["coverage"]["unwitnessedCallSites"], 0)
        violations = report["findings"]["moduleBoundaryViolations"]
        self.assertEqual(len(violations), 1)
        self.assertEqual(violations[0]["fromModule"], "extraction")
        self.assertEqual(violations[0]["toModule"], "cli")
        self.assertEqual(violations[0]["witness"]["caller"]["symbol"], "late_run")
        self.assertEqual(violations[0]["witness"]["callee"]["symbol"], "late_render")
        self.assertEqual(violations[0]["witness"]["call_site"]["line"], 99)
        self.assertEqual(report["status"], "fail")

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

    def test_callee_with_external_decl_path_is_never_a_module_boundary_violation(
        self,
    ) -> None:
        # [moduleCallGraphCheck false-positive fix] `sym_std_operator_plus`'s
        # `file_id` (mis-attributed, exactly like the real `std::operator+`
        # implicit-instantiation bug) resolves to `src/extraction/pass.cpp`
        # ("extraction" module), so a naive file-based classification would
        # report this `cli -> extraction` call as a boundary violation ("cli"
        # only allows "cli" per the fixture manifest). Its `decl_path`
        # resolves outside the fixture root, which must override `file_id`
        # and exclude the callee from module attribution entirely.
        fixture = _Fixture()
        fixture.add_call(fixture.sym_render, fixture.sym_std_operator_plus, line=42, col=7)
        report = fixture.run()
        self.assertEqual(report["findings"]["moduleBoundaryViolations"], [])

    def test_call_site_through_a_known_external_symbol_is_not_unwitnessed(self) -> None:
        # [unwitnessedCallSites false-positive fix, round 2] `sym_std_
        # operator_plus`'s `decl_path` positively confirms it lives outside
        # the fixture root (the same shape as a real `std::operator+`
        # implicit instantiation). Before this fix, `_read_semantic_facts`
        # excluded it from `symbol_file` for that reason and then counted
        # the resulting "dst_id not in symbol_file" as `unwitnessed_call_
        # sites` -- indistinguishable, at that point, from a symbol whose
        # file row was genuinely deleted out from under it
        # (`test_deleted_file_row_with_a_dangling_call_edge_fails_closed`
        # above). That conflation makes `unwitnessedCallSites` an
        # unconditional gate failure on any C++ project that calls the
        # standard library (33606/55920 on a real 212-TU self-index of this
        # repository). A call through a symbol AFFIRMATIVELY known to be
        # external must be counted separately (`externalCallSites`) and
        # must never force `completeness.semantic` to `"partial"` or
        # `status` to `"fail"`.
        fixture = _Fixture()
        fixture.add_call(fixture.sym_render, fixture.sym_std_operator_plus, line=42, col=7)
        report = fixture.run()
        self.assertEqual(report["index"]["coverage"]["unwitnessedCallSites"], 0)
        self.assertEqual(report["index"]["coverage"]["externalCallSites"], 1)
        self.assertEqual(report["completeness"]["semantic"], "complete")
        self.assertEqual(report["completeness"]["identityIssues"], [])
        self.assertEqual(report["status"], "pass")

        # Negative control: a call through a symbol with NO decl_path and NO
        # resolvable file (the real "file row deleted" / genuinely lost
        # identity case) must still fail closed exactly as before -- this
        # fix narrows the false-positive class, it does not weaken the
        # dangling-reference check itself.
        fixture2 = _Fixture()
        fixture2.add_call(fixture2.sym_run, fixture2.sym_render, line=7, col=3)
        fixture2._db._conn.execute("DELETE FROM file WHERE id = ?", (fixture2.file_format,))
        fixture2._db._conn.commit()
        report2 = fixture2.run()
        self.assertEqual(report2["index"]["coverage"]["unwitnessedCallSites"], 1)
        self.assertEqual(report2["index"]["coverage"]["externalCallSites"], 0)
        self.assertEqual(report2["status"], "fail")

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

    def test_baseline_suppresses_a_signature_bearing_resolved_callee(self) -> None:
        # [P1-1 regression] a real self-index's resolved callee name carries
        # the full C++ signature (`ast::qualified_name`, `src/ast/
        # names.hpp`), but architecture/cidx-self-host-policy.json's
        # baseline entries are authored bare (matching every one of the 38
        # `calleeQualName` values in the real policy file). Before the fix,
        # the baseline/witness comparison used the raw (signature-bearing)
        # name on one side and the bare name on the other, so this baseline
        # entry could never suppress its own exact, intentionally-baselined
        # call site.
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
        fixture.add_call(fixture.sym_run, fixture.sym_write_sig, line=40, col=5)
        report = fixture.run()
        self.assertEqual(report["findings"]["legacyFacadeViolations"], [])

    def test_signature_bearing_callee_at_a_new_site_is_still_rejected(self) -> None:
        # The companion case to the suppression above: a signature-bearing
        # callee at a DIFFERENT site than any baseline entry must still be
        # rejected (proving the bare-name normalization is used only for
        # comparison, not widened into "any call to this callee, anywhere").
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model", "persistence"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.add_call(fixture.sym_run, fixture.sym_write_sig, line=77, col=9)
        report = fixture.run()
        violations = report["findings"]["legacyFacadeViolations"]
        self.assertEqual(len(violations), 1)
        self.assertEqual(
            violations[0]["witness"]["callee"]["symbol"],
            "cidx::SqliteStorageService::write(const std::string &)",
        )

    def test_direct_construction_of_a_facade_record_is_a_module_boundary_violation(self) -> None:
        # [P1-2 regression] `Storage db(...)`-shaped direct constructions
        # resolve to a construct-value/-temp/-copy/-move/-heap edge whose
        # destination is the constructed RECORD (`cidx::SqliteStorageService`
        # here), never a constructor symbol (`statement_edge_visitor.cpp`'s
        # `emit_construction_form`). Before the fix, `CALL_EDGE_KINDS`
        # excluded these kinds entirely, so a direct construction across a
        # module boundary was invisible to BOTH the module-boundary and the
        # legacy-facade checks -- exactly the "manual audit only" gap
        # `docs/self-host-architecture.md` documents.
        fixture = _Fixture()
        fixture.add_call(
            fixture.sym_run, fixture.sym_service_class, line=12, col=3,
            kind=CONSTRUCT_VALUE_KIND,
        )
        report = fixture.run()
        violations = report["findings"]["moduleBoundaryViolations"]
        self.assertEqual(len(violations), 1)
        self.assertEqual(violations[0]["fromModule"], "extraction")
        self.assertEqual(violations[0]["toModule"], "persistence")
        self.assertEqual(
            violations[0]["witness"]["callee"]["symbol"],
            "cidx::SqliteStorageService::SqliteStorageService",
        )

    def test_direct_construction_of_a_facade_record_is_a_legacy_facade_violation(self) -> None:
        # Companion to the module-boundary case above: the SAME
        # construct-value edge, read against a facade policy whose
        # `calleeQualNamePrefixes` names the record
        # (`"cidx::SqliteStorageService::"`), must be visible to
        # `find_legacy_facade_violations` too -- presented the way a human
        # baseline entry already names a direct construction
        # ("cidx::SqliteStorageService::SqliteStorageService"), matching the
        # real policy file's own baseline convention.
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model", "persistence"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.add_call(
            fixture.sym_run, fixture.sym_service_class, line=12, col=3,
            kind=CONSTRUCT_VALUE_KIND,
        )
        report = fixture.run()
        violations = report["findings"]["legacyFacadeViolations"]
        self.assertEqual(len(violations), 1)
        self.assertEqual(
            violations[0]["witness"]["callee"]["symbol"],
            "cidx::SqliteStorageService::SqliteStorageService",
        )

    def test_direct_construction_is_suppressed_by_a_matching_construction_style_baseline(
        self,
    ) -> None:
        # The real policy file's baseline entries are all direct-
        # construction sites named exactly this way (e.g.
        # `"cidx::Storage::Storage"` for `Storage db(...)`); this proves that
        # authored shape actually suppresses a resolved construct-value
        # witness once P1-2's record->constructor-style-name synthesis is in
        # place.
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model", "persistence"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.policy["legacyFacades"][0]["baseline"] = [
            {
                "callerFile": "src/extraction/pass.cpp",
                "calleeQualName": "cidx::SqliteStorageService::SqliteStorageService",
                "line": 12,
                "col": 3,
                "owner": "test",
                "rationale": "test",
                "expiresOn": "2099-01-01",
                "removalIssue": "HSE-62",
            }
        ]
        fixture.policy_path.write_text(json.dumps(fixture.policy), encoding="utf-8")
        fixture.add_call(
            fixture.sym_run, fixture.sym_service_class, line=12, col=3,
            kind=CONSTRUCT_VALUE_KIND,
        )
        report = fixture.run()
        self.assertEqual(report["findings"]["legacyFacadeViolations"], [])

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

    def test_pagination_boundary_mid_edge_does_not_silently_lose_evidence(self) -> None:
        # [PR #67 round-4 review, AC7 blocker] the reviewer's exact repro:
        # seed ONE edge with ENUMERATE_BUDGET+1 edge_site rows. The site
        # enumeration's own real, working cursor
        # (queryplan.py's `_enumerate`, "edge" branch) only advances at EDGE
        # granularity -- it has no idea that this one edge's own sites just
        # got cut off mid-page by the pipeline's `limit(ENUMERATE_BUDGET)`.
        # A prior version of `_run_all_site_pages` fed the page's last SITE
        # row's edge_id back in as the pagination cursor, which resumed
        # enumeration from the NEXT edge -- silently discarding this edge's
        # own un-paged tail -- and then read the next (legitimately empty,
        # there being no higher edge id) page as "done, nothing truncated",
        # reporting `enumerationTruncated=false` over an incomplete read.
        # The fix must never make that claim: either every site is
        # recovered, or the gap is reported honestly as truncated.
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model", "persistence"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        edge_id = fixture._db.add_edge(fixture.sym_run, fixture.sym_write, CALLS_KIND)
        overflow = qp.ENUMERATE_BUDGET + 1
        fixture._db._conn.execute(
            "WITH RECURSIVE lines(n) AS (SELECT 1 UNION ALL SELECT n + 1 "
            f"FROM lines WHERE n < {overflow}) "
            "INSERT INTO edge_site(edge_id, file_id, line, col) "
            f"SELECT {edge_id}, {fixture.file_pass}, n, 1 FROM lines"
        )
        fixture._db._conn.commit()
        report = fixture.run()
        # The core AC7 assertion: never a false claim of completeness over a
        # read that actually dropped evidence.
        self.assertTrue(report["index"]["coverage"]["enumerationTruncated"])
        self.assertEqual(report["completeness"]["semantic"], "partial")
        self.assertEqual(report["status"], "fail")
        # Whatever subset of the overflowing edge's sites this pass DID
        # manage to read must still be real, genuine violations -- not
        # silently emptied out -- and must never exceed the real total
        # (no double-counting/fabricated witnesses either).
        violations = report["findings"]["legacyFacadeViolations"]
        self.assertGreater(len(violations), 0)
        self.assertLessEqual(len(violations), overflow)

    def test_edge_enumeration_truncation_upstream_of_sites_is_never_silently_dropped(self) -> None:
        # [PR #67 internal critic] a SECOND, independent truncation source
        # from the one above: the edge-NODE enumeration itself (`nodes()`,
        # before `sites()` ever runs) caps at ENUMERATE_BUDGET distinct edge
        # ids -- queryplan.py's `_enumerate` sets `st.truncated = True` on
        # ITS OWN result the moment that happens, even when the resulting
        # SITE page comes back far SHORTER than ENUMERATE_BUDGET (every
        # filler edge below has zero sites of its own). Before the round-2
        # P1-4 fix (`edge_id_plan` selecting the "edge" view's "id" field,
        # QueryPlan's PORTABLE/hashed logical identity, then feeding that hash
        # straight back in as `after_id` -- which the raw `WHERE id > ?`
        # cursor filter can never match) this pagination silently stopped
        # after the first ENUMERATE_BUDGET-sized window every time, no matter
        # how many real edges followed it: a violation whose edge sorts past
        # the cutoff was dropped and the run under-reported
        # (`enumerationTruncated` still correctly True from the raw `nodes()`
        # truncation, but `moduleBoundaryViolations` empty and
        # `semanticCompleteness: "partial"` masking the lost evidence rather
        # than surfacing it). [PR #67 QA round 3] a version of this test that
        # only asserts `enumerationTruncated`/`status`/`semantic` without
        # checking `moduleBoundaryViolations` content passes identically
        # whether or not the fix is present -- it never actually exercises
        # whether the real violation past the cutoff was found. This version
        # asserts the fixed, positive outcome (full enumeration, the real
        # violation present with its exact witness) and is proven below (see
        # this file's module docstring / implementation-notes.md) to go RED
        # under the pre-fix `edge_id_plan.select(["id"])` implementation.
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        # One allowed, witnessed call at the LOWEST edge id (captured in the
        # first page) so `facts.calls` is non-empty -- isolating whether a
        # dropped VIOLATION elsewhere is masked, rather than merely hitting
        # the separate "zero call edges" partial-completeness path.
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        # ENUMERATE_BUDGET filler edges with NO sites at all: each still
        # consumes one slot of the edge-id enumeration's own cursor cap,
        # but contributes zero rows to the site page.
        for i in range(qp.ENUMERATE_BUDGET):
            filler_dst = fixture._db.add_symbol(
                Symbol(
                    usr=f"filler::dst{i}", spelling=f"filler{i}", qual_name=f"filler{i}",
                    kind="function", file_id=fixture.file_value, line=100 + i,
                    is_definition=True, resolved=True,
                )
            )
            fixture._db.add_edge(fixture.sym_run, filler_dst, CALLS_KIND)
        # The (ENUMERATE_BUDGET+1)-th edge BY ID: a genuine cross-module
        # violation (extraction -> cli), with exactly one real call site.
        fixture.add_call(fixture.sym_run, fixture.sym_render, line=42, col=7)
        report = fixture.run()
        # Genuinely complete: both windows of the edge-id ground-truth cursor
        # were read (the second came back short of the budget), so nothing
        # was truncated and the pass is a real, positive proof rather than a
        # closed-fail placeholder.
        self.assertFalse(report["index"]["coverage"]["enumerationTruncated"])
        self.assertEqual(report["completeness"]["semantic"], "complete")
        self.assertEqual(report["status"], "fail")
        violations = report["findings"]["moduleBoundaryViolations"]
        self.assertEqual(len(violations), 1, violations)
        self.assertEqual(violations[0]["fromModule"], "extraction")
        self.assertEqual(violations[0]["toModule"], "cli")
        self.assertEqual(violations[0]["witness"]["call_site"]["line"], 42)
        self.assertEqual(violations[0]["witness"]["call_site"]["col"], 7)

    def test_same_endpoint_edges_of_different_relation_kinds_never_truncate_enumeration_early(
        self,
    ) -> None:
        # [AC7 blocker, PR #67 round-3 re-review, real reproduction at full
        # ENUMERATE_BUDGET scale] `_edge_site_count`'s completeness check
        # used to scope its verification query by (src_id, dst_id) alone,
        # not by the boundary edge's own id. When a SECOND edge connects
        # the exact same two symbols under a DIFFERENT relation kind -- an
        # ordinary shape in a real C++ codebase, e.g. a function that is
        # both `calls`- and `dispatch_calls`-related to the same callee --
        # that companion edge's (not-yet-visited) sites were silently
        # folded into the boundary edge's own completeness count. Whenever
        # the boundary edge's page was the FIRST page and consisted solely
        # of its own, fully-captured sites, the inflated count manufactured
        # a mismatch that was never really about the boundary edge, and
        # `_run_all_site_pages` treats a mismatch that fills the entire
        # page as unrecoverable: it gives up immediately. That still
        # reports `enumerationTruncated: true` (never a false completeness
        # claim), but it abandons the ENTIRE rest of the graph after just
        # that first page -- every subsequent edge and violation, however
        # large the remaining table, is silently never visited. This is
        # the general invariant AC7 requires: whether a violation is
        # reported must never depend on whether some OTHER, unrelated edge
        # happens to share the boundary edge's two endpoints.
        fixture = _Fixture()
        fixture.manifest["modules"][2]["allowedDependencies"] = ["extraction", "model"]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")

        def add_filler_edges(prefix: str, count: int) -> None:
            # Bulk SQL, not a `add_symbol`/`add_edge` Python call per row
            # (ENUMERATE_BUDGET of those takes minutes) -- mirrors
            # python/tests/test_queryplan.py's own bulk-fixture convention
            # for budget-scale tests. Distinct dangling symbols with no
            # file, edges from `sym_run` to each, zero sites: they consume
            # a raw edge-id slot but are invisible to the site pipeline and
            # to module classification (no `file_id`), exactly like the
            # filler edges in `test_edge_enumeration_truncation_upstream_
            # of_sites_is_never_silently_dropped` above.
            fixture._db._conn.execute(
                "WITH RECURSIVE lines(n) AS (SELECT 0 UNION ALL SELECT n + 1 "
                f"FROM lines WHERE n < {count - 1}) "
                "INSERT INTO symbol(usr, spelling, kind) "
                f"SELECT '{prefix}' || n, '{prefix}' || n, 8 FROM lines"
            )
            fixture._db._conn.execute(
                "INSERT INTO edge(src_id, dst_id, kind) "
                f"SELECT ?, id, ? FROM symbol WHERE usr LIKE '{prefix}%'",
                (fixture.sym_run, CALLS_KIND),
            )
            fixture._db._conn.commit()

        # ids 1..4000: filler edges (zero sites) -- positions the boundary
        # edge INSIDE the first ENUMERATE_BUDGET-sized raw window, not at
        # its very first slot, so this is not merely the id=1 special case.
        add_filler_edges("bulk1::", 4000)
        # id 4001: the boundary edge -- a genuine cross-module violation
        # (extraction -> cli), fully captured within this same first page.
        edge_a = fixture.add_call(fixture.sym_run, fixture.sym_render, line=11, col=1, kind=CALLS_KIND)
        self.assertEqual(edge_a, 4001)
        # ids 4002..10000: more filler edges, filling the raw window out to
        # exactly ENUMERATE_BUDGET (10000) total edges so far.
        add_filler_edges("bulk2::", qp.ENUMERATE_BUDGET - 4001)
        # id 10001: the companion -- SAME (src, dst) as the boundary edge,
        # DIFFERENT relation kind, positioned just past the first raw
        # window (invisible to it, unlike the existing `test_edge_site_
        # count_finds_the_real_edge_past_the_enumerate_budget` unit test,
        # which never puts two edges on the same endpoints).
        edge_b = fixture.add_call(fixture.sym_run, fixture.sym_render, line=12, col=2, kind=DISPATCH_CALLS_KIND)
        self.assertEqual(edge_b, qp.ENUMERATE_BUDGET + 1)
        # id 10002: a second, wholly independent violation further along --
        # the evidence the pre-fix give-up branch silently never reached.
        sym_render2 = fixture._db.add_symbol(
            Symbol(
                usr="u-render2", spelling="render2", qual_name="render2", kind="function",
                file_id=fixture.file_format, line=3, is_definition=True, resolved=True,
            )
        )
        edge_c = fixture.add_call(fixture.sym_run, sym_render2, line=42, col=7, kind=CALLS_KIND)
        self.assertEqual(edge_c, qp.ENUMERATE_BUDGET + 2)

        report = fixture.run()
        self.assertFalse(report["index"]["coverage"]["enumerationTruncated"])
        self.assertEqual(report["completeness"]["semantic"], "complete")
        self.assertEqual(report["status"], "fail")
        violations = report["findings"]["moduleBoundaryViolations"]
        sites = {(v["witness"]["call_site"]["line"], v["witness"]["call_site"]["col"]) for v in violations}
        # All three: the boundary edge's own site, its same-endpoint
        # companion's site, and the wholly independent later violation.
        self.assertEqual(sites, {(11, 1), (12, 2), (42, 7)})

    def test_edge_site_count_scopes_to_the_exact_edge_not_its_endpoints(self) -> None:
        # [AC7 fix, direct unit coverage] two edges sharing the exact same
        # (src, dst) under different relation kinds must never contaminate
        # each other's site count: `_edge_site_count` is asked about ONE of
        # them and must report only that edge's own true total, not the
        # sum. This is the exact defect `test_same_endpoint_edges_of_
        # different_relation_kinds_never_truncate_enumeration_early` proves
        # end-to-end; this unit test pins the specific function contract.
        fixture = _Fixture()
        edge_calls = fixture.add_call(fixture.sym_run, fixture.sym_render, line=1, col=1, kind=CALLS_KIND)
        fixture.add_call(fixture.sym_run, fixture.sym_render, line=2, col=2, kind=CALLS_KIND)
        edge_dispatch = fixture.add_call(
            fixture.sym_run, fixture.sym_render, line=3, col=3, kind=DISPATCH_CALLS_KIND
        )
        fixture.add_call(fixture.sym_run, fixture.sym_render, line=4, col=4, kind=DISPATCH_CALLS_KIND)
        fixture.add_call(fixture.sym_run, fixture.sym_render, line=5, col=5, kind=DISPATCH_CALLS_KIND)

        executor = qp.Executor(fixture._db)
        calls_count, calls_truncated = _edge_site_count(executor, edge_calls)
        dispatch_count, dispatch_truncated = _edge_site_count(executor, edge_dispatch)
        self.assertEqual(calls_count, 2)
        self.assertFalse(calls_truncated)
        self.assertEqual(dispatch_count, 3)
        self.assertFalse(dispatch_truncated)

    def test_edge_site_count_finds_the_real_edge_past_the_enumerate_budget(self) -> None:
        # [Round-6 internal critic P1] `_edge_site_count` used to call
        # `executor.run(plan)` with NO `after_id` on a `view("edge") |
        # nodes(eq("edge_id", ...))` plan. queryplan.py's `_enumerate` only
        # wires the pagination cursor into the "edge" branch when `after_id
        # is not None`; with none, it fetches the globally-FIRST
        # ENUMERATE_BUDGET edges by id and applies the predicate as a
        # post-fetch filter -- so any edge whose id sorts past that cutoff
        # was invisible to the filter no matter how many real sites it
        # owns. Reproduced directly here: >ENUMERATE_BUDGET filler edges
        # (each a distinct (src, dst) pair -- `add_edge` upserts on
        # (src_id, dst_id, kind), so repeating one pair would just bump its
        # `count` instead of consuming a fresh id) followed by ONE real
        # edge with 3 genuine call sites. Before the fix this returned
        # `(0, True)` -- wrong on both the count and the truncation flag --
        # for a real, populated edge.
        fixture = _Fixture()
        overflow = qp.ENUMERATE_BUDGET + 50
        for i in range(overflow):
            filler_dst = fixture._db.add_symbol(
                Symbol(
                    usr=f"filler::site_count_dst{i}", spelling=f"filler{i}",
                    qual_name=f"filler{i}", kind="function", file_id=fixture.file_value,
                    line=200 + i, is_definition=True, resolved=True,
                )
            )
            fixture._db.add_edge(fixture.sym_run, filler_dst, CALLS_KIND)
        real_edge_id = fixture._db.add_edge(fixture.sym_run, fixture.sym_write, CALLS_KIND)
        fixture._db.add_edge_site(real_edge_id, fixture.file_pass, 10, 1)
        fixture._db.add_edge_site(real_edge_id, fixture.file_pass, 20, 2)
        fixture._db.add_edge_site(real_edge_id, fixture.file_pass, 30, 3)
        fixture._db._conn.commit()

        executor = qp.Executor(fixture._db)
        real_count, truncated = _edge_site_count(executor, real_edge_id)
        self.assertEqual(real_count, 3)
        self.assertFalse(truncated)

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

    def test_catalog_duplicate_brace_init_declaration_is_rejected(self) -> None:
        # [Review round 3, blocker 5] the guard must catch ordinary
        # brace-init redeclarations (std::pair{8, "function"}), not only
        # the paren-call spelling.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_brace.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair{8, "function"};\n', encoding="utf-8"
        )
        report = fixture.run()
        self.assertTrue(any("duplicates a generated catalog declaration" in error for error in report["findings"]["catalogGuard"]))

    def test_catalog_duplicate_hex_spelled_declaration_is_rejected(self) -> None:
        # [PR #67 round-4 review, AC5 blocker] the reviewer's exact repro:
        # `0x8` is a different SPELLING of the exact same catalog id (8) as
        # the decimal-literal case already covered above -- a guard that
        # only recognizes one hard-coded spelling per id keeps losing to the
        # next equivalent spelling. The guard must compare the literal's
        # numeric VALUE, not its text.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_hex.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair{0x8, "function"};\n', encoding="utf-8"
        )
        report = fixture.run()
        self.assertTrue(any("duplicates a generated catalog declaration" in error for error in report["findings"]["catalogGuard"]))

    def test_catalog_duplicate_octal_and_binary_spelled_declarations_are_rejected(self) -> None:
        # Same value (8), two more equivalent C++ spellings: octal (010)
        # and binary (0b1000, C++14).
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_octal.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair{010, "function"};\n', encoding="utf-8"
        )
        (fixture.root / "src/extraction/dup_binary.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair{0b1000, "function"};\n', encoding="utf-8"
        )
        report = fixture.run()
        errors = report["findings"]["catalogGuard"]
        self.assertTrue(any("dup_octal.hpp" in error and "duplicates a generated catalog declaration" in error for error in errors))
        self.assertTrue(any("dup_binary.hpp" in error and "duplicates a generated catalog declaration" in error for error in errors))

    def test_catalog_duplicate_static_cast_of_literal_is_rejected(self) -> None:
        # [PR #67 internal critic, second finding] `static_cast<int>(8)` is
        # yet another spelling of the exact same catalog id (8): a guard
        # that only matched a BARE literal token immediately after `(`/`{`
        # let this straight through.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_static_cast.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair{static_cast<int>(8), "function"};\n',
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertTrue(any("duplicates a generated catalog declaration" in error for error in report["findings"]["catalogGuard"]))

    def test_catalog_duplicate_shift_expression_is_rejected(self) -> None:
        # `1 << 3` is a simple, foldable constant-arithmetic spelling of 8.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_shift.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair{1 << 3, "function"};\n',
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertTrue(any("duplicates a generated catalog declaration" in error for error in report["findings"]["catalogGuard"]))

    def test_catalog_duplicate_char_literal_is_rejected(self) -> None:
        # `'\b'` (backspace) is the character literal spelling of 8.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_char.hpp").write_text(
            "inline constexpr auto kSymbolKind = std::pair{'\\b', \"function\"};\n",
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertTrue(any("duplicates a generated catalog declaration" in error for error in report["findings"]["catalogGuard"]))

    def test_catalog_duplicate_unary_plus_literal_is_rejected(self) -> None:
        # [Round-6 internal critic P1] `+8` (a leading unary sign on a bare
        # literal) previously produced ZERO regex match at all -- not
        # matched-but-unsupported, genuinely invisible to the guard.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_unary_plus.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair{+8, "function"};\n',
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertTrue(any("duplicates a generated catalog declaration" in error for error in report["findings"]["catalogGuard"]))

    def test_catalog_duplicate_parenthesized_literal_is_rejected(self) -> None:
        # [Round-6 internal critic P1] `(8)` (a bare literal wrapped in one
        # layer of parentheses) previously produced zero regex match.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_paren_literal.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair{(8), "function"};\n',
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertTrue(any("duplicates a generated catalog declaration" in error for error in report["findings"]["catalogGuard"]))

    def test_catalog_duplicate_parenthesized_shift_expression_is_rejected(self) -> None:
        # [Round-6 internal critic P1] `(4 << 1)` (a shift expression wrapped
        # in one layer of parentheses) previously produced zero regex match.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_paren_shift.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair{(4 << 1), "function"};\n',
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertTrue(any("duplicates a generated catalog declaration" in error for error in report["findings"]["catalogGuard"]))

    def test_catalog_duplicate_nested_static_cast_is_rejected(self) -> None:
        # [Round-6 internal critic P1] `static_cast<unsigned>(static_cast<int>(8))`
        # (one cast wrapping another) previously produced zero regex match --
        # the original guard only ever accepted one cast around a bare atom.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_nested_cast.hpp").write_text(
            'inline constexpr auto kSymbolKind = '
            'std::pair{static_cast<unsigned>(static_cast<int>(8)), "function"};\n',
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertTrue(any("duplicates a generated catalog declaration" in error for error in report["findings"]["catalogGuard"]))

    def test_catalog_duplicate_signed_parenthesized_literal_is_rejected(self) -> None:
        # [Round-7 internal critic P1] `+(8)`/`-(8)` (a leading unary sign
        # directly on a parenthesized literal) previously produced ZERO
        # regex match at all: Round-6 closed a sign on a BARE literal and a
        # parenthesized literal as two separate shapes, but never their
        # composition.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_signed_paren_plus.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair{+(8), "function"};\n',
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertTrue(any("duplicates a generated catalog declaration" in error for error in report["findings"]["catalogGuard"]))

    def test_catalog_duplicate_signed_parenthesized_shift_expression_is_rejected(self) -> None:
        # [Round-7 internal critic P1] `-(-2 << 2)` must fold the shift
        # INSIDE the parentheses first (-2 << 2 == -8) and then negate the
        # WHOLE folded result (== 8, the guarded id) -- this is the discriminating
        # case, since applying the sign to only the first operand before
        # folding would instead compute `(- -2) << 2` == `2 << 2` == 4, a
        # DIFFERENT value that would NOT match the guarded id and so would
        # let this fixture through undetected. This is deliberately not the
        # simpler `-(4 << 1)`, whose two readings coincide at -8/8 for the
        # matching sign and so cannot tell precedence bugs apart.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_signed_paren_shift.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair{-(-2 << 2), "function"};\n',
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertTrue(any("duplicates a generated catalog declaration" in error for error in report["findings"]["catalogGuard"]))

    def test_catalog_duplicate_static_cast_of_signed_parenthesized_literal_is_rejected(self) -> None:
        # [Round-7 internal critic P1] the reviewer's exact repro:
        # `static_cast<int>(+(8))` combines a cast, a sign, and a paren in
        # the one shape Round-6's widening did not reach.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_cast_signed_paren.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair{static_cast<int>(+(8)), "function"};\n',
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertTrue(any("duplicates a generated catalog declaration" in error for error in report["findings"]["catalogGuard"]))

    def test_eval_cpp_const_expr_signed_paren_negates_the_folded_shift_not_the_first_operand(self) -> None:
        # [Round-7 internal critic P1] pins the precedence directly at the
        # eval level, since `-(4 << 1)` (-8) and `(-4) << 1` (also -8 in
        # Python/two's-complement arithmetic) coincide for this operand
        # pair and so cannot be told apart through the report-level fixture
        # above. `-(4 - 1)` is unambiguous: -(4 - 1) == -3, but
        # (-4) - 1 == -5.
        self.assertEqual(_eval_cpp_const_expr("-(4 - 1)"), -3)
        self.assertEqual(_eval_cpp_const_expr("+(4 - 1)"), 3)

    def test_catalog_duplicate_signed_cast_fails_closed(self) -> None:
        # The evaluator's closed grammar still cannot fold a sign directly
        # on a static_cast, but an explicit std::pair with a guarded name
        # must fail closed rather than receive a clean result.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_signed_cast.hpp").write_text(
            'inline constexpr auto kSymbolKind = std::pair{-static_cast<int>(-8), "function"};\n',
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertTrue(
            any("unverifiable hand-authored id expression" in error
                for error in report["findings"]["catalogGuard"])
        )
        self.assertEqual(report["status"], "fail")

    def test_catalog_duplicate_via_enum_constant_macro_or_named_variable_fails_closed(self) -> None:
        # A generated name paired with an identifier-based id must not receive
        # a clean result merely because this textual gate cannot evaluate the
        # identifier. Explicit std::pair declarations fail closed.
        fixture = _Fixture()
        (fixture.root / "src/extraction/dup_enum.hpp").write_text(
            "enum class Kind : int { Function = 8 };\n"
            'inline constexpr auto kA = std::pair{static_cast<int>(Kind::Function), "function"};\n',
            encoding="utf-8",
        )
        (fixture.root / "src/extraction/dup_macro.hpp").write_text(
            "#define K_FUNCTION 8\n"
            'inline constexpr auto kB = std::pair{K_FUNCTION, "function"};\n',
            encoding="utf-8",
        )
        (fixture.root / "src/extraction/dup_named_var.hpp").write_text(
            "inline constexpr int kEight = 8;\n"
            'inline constexpr auto kC = std::pair{kEight, "function"};\n',
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertEqual(
            {
                Path(error.split("catalog guard ", 1)[1].split(":", 1)[0]).name
                for error in report["findings"]["catalogGuard"]
            },
            {"dup_enum.hpp", "dup_macro.hpp", "dup_named_var.hpp"},
        )
        self.assertEqual(report["status"], "fail")

    def test_queryplan_unknown_evidence_fails_closed(self) -> None:
        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        fixture._db._conn.execute(
            "UPDATE symbol SET resolved = 0 WHERE id = ?", (fixture.sym_value,)
        )
        fixture._db._conn.commit()

        report = fixture.run()

        self.assertTrue(report["index"]["coverage"]["queryUnknown"])
        self.assertEqual(report["completeness"]["semantic"], "partial")
        self.assertEqual(report["status"], "fail")
        self.assertTrue(
            any(
                "QueryPlan semantic read reported unknown evidence" in issue
                for issue in report["completeness"]["identityIssues"]
            )
        )

    def test_catalog_unrelated_integer_literal_spelling_is_not_falsely_flagged(self) -> None:
        # A hex/octal/binary literal next to an UNRELATED string, or one
        # that evaluates to a DIFFERENT id than any guarded pair, must not
        # be flagged -- the fix must compare against the real guarded set,
        # not just "any integer literal near any string".
        fixture = _Fixture()
        (fixture.root / "src/extraction/not_a_dup.hpp").write_text(
            'inline constexpr auto kOther = std::pair{0x9, "not_a_kind"};\n'
            'inline constexpr auto kAlsoOther = std::pair{0x8, "not_function_either"};\n',
            encoding="utf-8",
        )
        report = fixture.run()
        self.assertEqual(report["findings"]["catalogGuard"], [])

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
        # [Review round 3, blocker 2] a partial semantic pass must never
        # report status="pass": the workflow gate (scripts/self_host_
        # index.sh's exit code) only checks status, so a "pass" here would
        # silently clear the gate despite having no positive evidence at all.
        self.assertEqual(report["completeness"]["semantic"], "partial")
        self.assertEqual(report["status"], "fail")

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

    def test_mutually_exclusive_source_group_satisfied_by_either_member(self) -> None:
        # [missingTranslationUnits false-positive fix] the real repository's
        # src/astgraph/souffle_stub.cpp and src/astgraph/souffle_runner.cpp
        # are both manifest-classified production sources, but
        # CMakeLists.txt only ever compiles ONE of them per build
        # (CIDX_ASTGRAPH_SOUFFLE_ENABLED); the other is never present in
        # compile_commands.json (and thus never indexed) in ANY
        # environment. Model that here with a second real .cpp under
        # src/query/ that the fixture never registers as an indexed
        # `file` row: with the group declared, a real self-index that only
        # ever indexed `plan.cpp` (never `plan_alt.cpp`) must not be
        # reported as missing evidence.
        fixture = _Fixture()
        (fixture.root / "src/query/plan_alt.cpp").write_text(
            "void execute_plan_alt() {}\n", encoding="utf-8"
        )
        fixture.manifest["mutuallyExclusiveSourceGroups"] = [
            {"sources": ["src/query/plan.cpp", "src/query/plan_alt.cpp"]}
        ]
        fixture.manifest_path.write_text(json.dumps(fixture.manifest), encoding="utf-8")
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        report = fixture.run()
        self.assertEqual(report["index"]["coverage"]["missingTranslationUnits"], [])
        self.assertEqual(report["status"], "pass")

        # Negative control: if NEITHER group member is indexed, that is a
        # genuine coverage gap (e.g. the whole astgraph module skipped) and
        # must still fail closed exactly as before this fix.
        fixture2 = _Fixture()
        (fixture2.root / "src/query/plan_alt.cpp").write_text(
            "void execute_plan_alt() {}\n", encoding="utf-8"
        )
        fixture2.manifest["mutuallyExclusiveSourceGroups"] = [
            {"sources": ["src/query/plan.cpp", "src/query/plan_alt.cpp"]}
        ]
        fixture2.manifest_path.write_text(json.dumps(fixture2.manifest), encoding="utf-8")
        fixture2.add_call(fixture2.sym_run, fixture2.sym_value)
        fixture2._db._conn.execute("DELETE FROM file WHERE id = ?", (fixture2.file_plan,))
        fixture2._db._conn.commit()
        report2 = fixture2.run()
        self.assertIn("src/query/plan.cpp", report2["index"]["coverage"]["missingTranslationUnits"])
        self.assertEqual(report2["status"], "fail")

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

    def test_package_hash_changes_when_the_queryplan_surface_it_executes_changes(self) -> None:
        # [Review round 3, blocker 6] this report's semantic reads execute
        # indexer.queryplan.Executor/Storage.index_identity() directly;
        # packageHash must be bound to that surface, not just the four
        # files under scripts/. Prove it's actually included by hashing a
        # MUTATED copy of queryplan.py under a fake project layout and
        # confirming the digest differs from the real one -- without
        # touching the real, shared queryplan.py on disk.
        import scripts.self_host_architecture_report as report_module

        fixture = _Fixture()
        fixture.add_call(fixture.sym_run, fixture.sym_value)
        real_hash = report_module._package_hash(
            fixture.root, fixture.manifest_path, fixture.policy_path
        )

        real_project_root = Path(report_module.__file__).resolve().parent.parent
        fake_project_root = Path(tempfile.mkdtemp(prefix="cidx-fake-project-", dir="/tmp"))
        (fake_project_root / "scripts").mkdir(parents=True)
        (fake_project_root / "python/indexer").mkdir(parents=True)
        for name in ("check_architecture.py", "check_platform_contract.py", "self_host_architecture_report.py"):
            (fake_project_root / "scripts" / name).write_text(
                (real_project_root / "scripts" / name).read_text(encoding="utf-8"), encoding="utf-8"
            )
        (fake_project_root / "python/indexer/storage.py").write_text(
            (real_project_root / "python/indexer/storage.py").read_text(encoding="utf-8"), encoding="utf-8"
        )
        # A single mutated byte in the QueryPlan surface, nothing else.
        (fake_project_root / "python/indexer/queryplan.py").write_text(
            (real_project_root / "python/indexer/queryplan.py").read_text(encoding="utf-8")
            + "\n# mutated for test_package_hash_changes_when_the_queryplan_surface_it_executes_changes\n",
            encoding="utf-8",
        )

        original_file = report_module.__file__
        try:
            report_module.__file__ = str(fake_project_root / "scripts/self_host_architecture_report.py")
            mutated_hash = report_module._package_hash(
                fixture.root, fixture.manifest_path, fixture.policy_path
            )
        finally:
            report_module.__file__ = original_file

        self.assertNotEqual(real_hash, mutated_hash)

    def test_report_is_reproducible_across_byte_identical_checkouts_at_different_paths(self) -> None:
        # [Review round 3, blocker 3] two clean checkouts of the exact same
        # commit, built and reported on independently, live at two different
        # absolute paths on disk. The report must not leak *where* either
        # checkout happens to sit -- only *what* it contains -- or a
        # byte-identical checkout would fail its own CI gate purely because
        # of directory placement. `_Fixture()` builds a deterministic,
        # byte-identical tree/index every time (no UUIDs, no timestamps tied
        # to wall clock other than the fixed literal stamped in
        # `_build_index`), so two independently-built fixtures with the same
        # `add_call` are exactly the "byte-identical checkout, different
        # path" case.
        first = _Fixture()
        first.add_call(first.sym_run, first.sym_render)
        first_report = first.run()

        second = _Fixture()
        second.add_call(second.sym_run, second.sym_render)
        second_report = second.run()

        self.assertNotEqual(first.root, second.root)

        def _normalized(report: dict) -> dict:
            report = json.loads(json.dumps(report))
            del report["generatedAt"]
            # `graphResolvedAt` is HSE-32's own wall-clock stamp of when
            # `cidx resolve` last ran on the self-index -- exactly as
            # ephemeral/run-time as `generatedAt` above, never derived from
            # what the index actually contains. See
            # `test_canonical_hash_is_stable_across_different_graph_resolved_at_stamps`
            # below for the case that specifically isolates this field.
            del report["index"]["graphResolvedAt"]
            return report

        self.assertEqual(_normalized(first_report), _normalized(second_report))
        self.assertEqual(first_report["canonicalHash"], second_report["canonicalHash"])

    def test_canonical_hash_is_stable_across_different_graph_resolved_at_stamps(self) -> None:
        # [PR #67 round-4 review, AC1 blocker] the reviewer's exact repro:
        # two otherwise byte-identical fixtures whose ONLY difference is
        # `meta.graph_resolved_at` (a real self-index run against the same
        # checkout at two different real times would legitimately stamp two
        # different values there). Before this fix, comparing two reports
        # with only `generatedAt` removed still differed at
        # `index.graphResolvedAt` -- the report was not actually
        # reproducible for byte-identical content. `canonicalHash` must
        # agree regardless, since it excludes that field by construction
        # (see `_canonical_report_hash`).
        first = _Fixture()
        first.add_call(first.sym_run, first.sym_render)
        first._db._conn.execute(
            "UPDATE meta SET value = ? WHERE key = 'graph_resolved_at'", ("2020-01-01T00:00:00Z",)
        )
        first._db._conn.commit()
        first_report = first.run()

        second = _Fixture()
        second.add_call(second.sym_run, second.sym_render)
        second._db._conn.execute(
            "UPDATE meta SET value = ? WHERE key = 'graph_resolved_at'", ("2030-06-15T12:34:56Z",)
        )
        second._db._conn.commit()
        second_report = second.run()

        self.assertNotEqual(
            first_report["index"]["graphResolvedAt"], second_report["index"]["graphResolvedAt"]
        )
        self.assertEqual(first_report["canonicalHash"], second_report["canonicalHash"])

    def test_canonical_hash_changes_when_a_real_finding_changes(self) -> None:
        # `canonicalHash` must not be so aggressively normalized that it
        # stops detecting real content changes -- prove it differs when an
        # actual violation is introduced.
        clean = _Fixture()
        clean.add_call(clean.sym_run, clean.sym_value)  # extraction -> model: allowed
        clean_report = clean.run()

        violating = _Fixture()
        violating.add_call(violating.sym_run, violating.sym_render, line=7, col=3)  # extraction -> cli
        violating_report = violating.run()

        self.assertNotEqual(clean_report["canonicalHash"], violating_report["canonicalHash"])

    def test_source_content_hash_changes_when_a_tus_own_build_flags_differ(self) -> None:
        # [PR #67 round-4 review, AC10 blocker] the reviewer's exact repro:
        # two self-index runs over BYTE-IDENTICAL source, one of them built
        # with an extra `-DADVERSARIAL_BUILD=1`, previously produced the
        # exact same `sourceContentHash` (and therefore a byte-identical
        # report) even though what was actually compiled genuinely
        # differed. Re-stamping identity after the flag change (rather than
        # mutating post-hoc) simulates a REAL index that was actually BUILT
        # with that flag from the start -- freshness stays "current" on
        # both sides, isolating this from the separate, already-tested
        # post-hoc-drift/staleness mechanism.
        first = _Fixture()
        first.add_call(first.sym_run, first.sym_value)
        first_report = first.run()

        second = _Fixture()
        second.add_call(second.sym_run, second.sym_value)
        second._db._conn.execute(
            "UPDATE file SET compile_options = ? WHERE id = ?",
            (json.dumps(["-std=c++23", "-DADVERSARIAL_BUILD=1"]), second.file_pass),
        )
        second._db._conn.commit()
        second._db.stamp_index_identity()
        second._db._conn.commit()
        second_report = second.run()

        self.assertEqual(second_report["index"]["coverage"]["indexIdentity"]["freshness"], "current")
        self.assertNotEqual(
            first_report["index"]["coverage"]["sourceContentHash"],
            second_report["index"]["coverage"]["sourceContentHash"],
        )
        self.assertNotEqual(first_report["canonicalHash"], second_report["canonicalHash"])

    def test_canonical_hash_binds_the_indexed_analysis_engine_source_package_hash_stays_scoped_to_python(
        self,
    ) -> None:
        # [PR #67 round-4 review, AC10 blocker] the reviewer's other repro:
        # mutating the C++ analysis/extraction engine source that produced
        # this self-index's OWN resolved facts (self-hosting indexes that
        # engine's own source as part of the SAME repository) must change
        # SOMETHING in the release identity, not leave it byte-identical.
        # `config.packageHash` is -- correctly, by design (see its own
        # docstring) -- scoped to the PYTHON checker/QueryPlan surface only,
        # so it does NOT change; `index.coverage.sourceContentHash` (which
        # covers every indexed file's content, C++ engine sources included)
        # and therefore the overall `canonicalHash` DO change, giving one
        # release identity that really does bind the executed analysis
        # implementation, not just the Python query layer.
        first = _Fixture()
        first.add_call(first.sym_run, first.sym_value)
        first_report = first.run()

        second = _Fixture()
        second.add_call(second.sym_run, second.sym_value)
        (second.root / "src/extraction/pass.cpp").write_text(
            '#include "model/value.hpp"\nvoid run() { /* mutated analysis engine behavior */ }\n',
            encoding="utf-8",
        )
        second._db.stamp_index_identity()
        second._db._conn.commit()
        second_report = second.run()

        self.assertEqual(second_report["index"]["coverage"]["indexIdentity"]["freshness"], "current")
        self.assertEqual(first_report["config"]["packageHash"], second_report["config"]["packageHash"])
        self.assertNotEqual(
            first_report["index"]["coverage"]["sourceContentHash"],
            second_report["index"]["coverage"]["sourceContentHash"],
        )
        self.assertNotEqual(first_report["canonicalHash"], second_report["canonicalHash"])

    def test_orphaned_file_directory_reference_fails_closed_and_is_not_silently_excluded(self) -> None:
        # [PR #67 internal critic, third finding] `_portable_source_hash`'s
        # (and `check_index_coverage`'s `indexed_paths`) file listing used
        # to be an INNER JOIN against `directory`. A plain directory delete
        # through the default `PRAGMA foreign_keys = ON` connection
        # correctly CASCADES (removing the file too) -- this fixture
        # reproduces the other, defense-in-depth case: no `Storage` method
        # today deletes/merges a directory with the pragma OFF (see
        # `_portable_source_hash`'s own comment for the exact code paths
        # checked), but SQLite never retroactively re-validates existing
        # rows once the pragma is switched back ON, so if a directory ever
        # WERE removed with it off -- exactly what this fixture does by
        # hand below -- its files' `directory_id` is left pointing at
        # nothing, permanently. The INNER JOIN
        # silently excluded such a file (and its content) from
        # `sourceContentHash` entirely -- collapsing its row to zero
        # contribution, as if it had never been indexed. This is not
        # fixable into "the hash reflects the orphaned file's real
        # content": `Storage.file_abs_path` itself needs that very same
        # directory row to reconstruct where the file lives on disk, so
        # the content is genuinely unreadable once orphaned -- but the row
        # must no longer be silently ABSENT, and the report must fail
        # closed on the coverage gap itself (matching the existing
        # dangling-symbol-file-reference pattern).
        fixture = _Fixture()
        dir_row = fixture._db._conn.execute(
            "SELECT directory_id FROM file WHERE id = ?", (fixture.file_pass,)
        ).fetchone()[0]
        fixture._db._conn.execute("PRAGMA foreign_keys = OFF")
        fixture._db._conn.execute("DELETE FROM directory WHERE id = ?", (dir_row,))
        fixture._db._conn.execute("PRAGMA foreign_keys = ON")
        fixture._db._conn.commit()
        report = fixture.run()
        self.assertEqual(report["index"]["coverage"]["danglingFileDirectoryReferences"], 1)
        self.assertTrue(
            any("no matching directory row" in issue for issue in report["completeness"]["identityIssues"])
        )
        self.assertEqual(report["status"], "fail")
        # The orphaned file's row must still be represented in the hash
        # computation -- no longer silently collapsed to the hash of empty
        # input, as if it had never been indexed at all.
        self.assertNotEqual(
            report["index"]["coverage"]["sourceContentHash"],
            hashlib.sha256(b"").hexdigest(),
        )

    def test_package_hash_changes_when_the_generated_catalog_source_changes(self) -> None:
        # [Internal critic finding, PR #67 @ 40b1e93] check_catalog_containment
        # reads catalogs/core.json directly to build the guarded (id, name)
        # pair list -- that file literally defines what the catalog guard
        # catches. It was missing from _package_hash's hashed file list, so
        # editing it changed the gate's behavior without changing the
        # recorded packageHash. Prove the fix: mutating catalogs/core.json
        # (without touching any of scripts/*.py or the QueryPlan/Storage
        # surface) must still change packageHash.
        import scripts.self_host_architecture_report as report_module

        fixture = _Fixture()
        before = report_module._package_hash(fixture.root, fixture.manifest_path, fixture.policy_path)

        catalog_path = fixture.root / "catalogs/core.json"
        catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
        catalog["symbol_kinds"].append({"id": 99, "name": "extra_kind"})
        catalog_path.write_text(json.dumps(catalog), encoding="utf-8")

        after = report_module._package_hash(fixture.root, fixture.manifest_path, fixture.policy_path)
        self.assertNotEqual(before, after)

    def test_package_hash_changes_when_the_platform_contract_changes(self) -> None:
        # [P2-1 regression] `generate_report` reads `spec/platform/
        # architecture.json` and feeds its errors straight into
        # `findings.platformContract` -- a real gate verdict input, exactly
        # like `catalogs/core.json` above -- but `_package_hash`'s hashed
        # file list omitted it, so editing it changed the gate's verdict
        # without changing the recorded packageHash.
        import scripts.self_host_architecture_report as report_module

        fixture = _Fixture()
        contract_path = fixture.root / "spec/platform/architecture.json"
        contract_path.parent.mkdir(parents=True, exist_ok=True)

        # Absent -> present is itself a real change (not merely absent's
        # sentinel happening to differ from a specific present-content byte
        # string, which this also proves).
        before_absent = report_module._package_hash(fixture.root, fixture.manifest_path, fixture.policy_path)
        contract_path.write_text(json.dumps({"contracts": []}), encoding="utf-8")
        after_created = report_module._package_hash(fixture.root, fixture.manifest_path, fixture.policy_path)
        self.assertNotEqual(before_absent, after_created)

        contract_path.write_text(json.dumps({"contracts": [{"id": "extra"}]}), encoding="utf-8")
        after_edited = report_module._package_hash(fixture.root, fixture.manifest_path, fixture.policy_path)
        self.assertNotEqual(after_created, after_edited)

    def test_real_policy_legacy_facade_baselines_are_populated_and_well_formed(self) -> None:
        # [Review round 3, blocker 4] the committed
        # architecture/cidx-self-host-policy.json shipped with BOTH legacy-
        # facade baselines empty, even though the real manifest classifies
        # src/cli/commands_graph.cpp and src/cli/commands_repo.cpp (and, per
        # a manual/textual audit -- see the facades' own `description`
        # fields and docs/self-host-architecture.md -- every other
        # non-exempt-module file that directly constructs cidx::Storage or
        # cidx::graph::GraphQuery) as real, pre-existing call sites. A
        # regression back to an empty baseline here would silently
        # re-introduce "claims a clean self-host gate with no real coverage"
        # without any fixture test catching it, since every other test in
        # this file uses a synthetic `_Fixture`, never the real committed
        # policy file.
        real_policy = json.loads((ROOT / "architecture/cidx-self-host-policy.json").read_text(encoding="utf-8"))
        facades = {facade["id"]: facade for facade in real_policy["legacyFacades"]}
        self.assertEqual(set(facades), {"storage-facade", "graph-query-bypass"})
        for facade_id, facade in facades.items():
            self.assertTrue(facade["baseline"], f"{facade_id} baseline must not be empty")
            for entry in facade["baseline"]:
                self.assertTrue(entry["callerFile"].startswith("src/"), entry)
                self.assertIn(entry["line"], range(1, 100_000))
                self.assertIn(entry["col"], range(1, 1000))
        # Every entry must satisfy the same owner/rationale/expiry/removal-
        # issue/exact-call-site metadata policy as a manifest exception --
        # `check_policy_metadata` is the same function `generate_report`
        # itself runs, so this is not a duplicate, weaker check.
        self.assertEqual(check_policy_metadata(real_policy), [])

    def test_real_policy_storage_facade_exempts_query_plans_own_read_adapter(self) -> None:
        # [P1-3] once the bare-name (P1-1) and construction-visibility (P1-2)
        # fixes make `find_legacy_facade_violations` actually able to match
        # real, resolved calls into cidx::SqliteStorageService, a completed
        # self-index surfaces src/query/exec.cpp's
        # cidx::query::SqliteQueryReadAdapter -- query.plan's own, singular,
        # sanctioned bridge into storage (it holds a SqliteStorageService&
        # and forwards every QueryReadPort/GraphReadPort method to it) --
        # as an unbaselined storage-facade violation, exactly the same
        # dependency-inversion shape persistence.sqlite/product.application
        # are already exempt for, and the one the sibling
        # graph-query-bypass facade already exempts query.plan for on
        # cidx::graph::GraphQuery. Without this exemption AC1 ("a real
        # clean self-index must complete") is unreachable for a reason
        # unrelated to any legacy call site pending migration.
        real_policy = json.loads((ROOT / "architecture/cidx-self-host-policy.json").read_text(encoding="utf-8"))
        real_manifest = json.loads((ROOT / "architecture/cidx-module-manifest.json").read_text(encoding="utf-8"))
        module_ids = {module["id"] for module in real_manifest["modules"]}
        self.assertIn("query.plan", module_ids)
        facades = {facade["id"]: facade for facade in real_policy["legacyFacades"]}
        self.assertIn("query.plan", facades["storage-facade"]["exemptModules"])

    def test_real_policy_storage_facade_exempts_graph_querys_own_read_port(self) -> None:
        # [P1-3, QA round-3 finding] a census against a real self-index of
        # this repository found ~26 unbaselined `storage-facade` violations
        # from src/graph/query.cpp (module analysis.graph) once P1-1/P1-2
        # made the checker able to match resolved calls at all: GraphQuery
        # (src/graph/query.hpp) holds a storage::GraphReadPort& (`db_`), a
        # pure-virtual read port; the only class implementing that port is
        # query.plan's own SqliteQueryReadAdapter (src/query/exec.hpp),
        # which forwards to cidx::SqliteStorageService -- the exact same
        # dependency-inversion shape query.plan's own adapter was already
        # exempted for (see the sibling test above), and the one
        # graph-query-bypass already exempts analysis.graph for on the
        # equivalent cidx::graph::GraphQuery facade. Without this exemption
        # AC1 ("a real clean self-index must complete") stays unreachable
        # for analysis.graph specifically, even after every other P1 fix.
        real_policy = json.loads((ROOT / "architecture/cidx-self-host-policy.json").read_text(encoding="utf-8"))
        real_manifest = json.loads((ROOT / "architecture/cidx-module-manifest.json").read_text(encoding="utf-8"))
        module_ids = {module["id"] for module in real_manifest["modules"]}
        self.assertIn("analysis.graph", module_ids)
        facades = {facade["id"]: facade for facade in real_policy["legacyFacades"]}
        self.assertIn("analysis.graph", facades["storage-facade"]["exemptModules"])

    def test_real_policy_baseline_suppresses_the_fact_providers_index_identity_reads(
        self,
    ) -> None:
        # [P1-3 round-2 fix] A census against a real self-index found 2 live,
        # unbaselined `storage-facade` violations that the previous
        # `exemptModules` widening (the two tests above) did NOT cover:
        # src/analysis/facts.cpp:541:36 and src/analysis/runner.cpp:1063:38,
        # both `storage.index_identity()` -- a member call through the
        # `cidx::Storage` reference the line directly above already
        # constructs (and already baselines as a direct construction). This
        # is exactly the member-call-through-an-already-held-reference case
        # docs/self-host-architecture.md's manual/textual-audit fallback
        # names as uncoverable by grep; a real resolved call edge is the
        # only way to enumerate it, which is why the fix is two new baseline
        # entries (not another exemptModules widening -- analysis.
        # fact-provider is NOT, and must not become, exempt: exemption would
        # also silence any real, NEW future coupling from that module into
        # the facade, whereas a baseline entry suppresses only the ONE
        # pinned call site).
        #
        # This runs `find_legacy_facade_violations` -- the exact function
        # `generate_report` calls -- against the REAL committed policy file
        # and a synthetic call graph shaped like the real one (construction
        # already covered elsewhere; this reproduces only the two
        # `index_identity()` reads), so it proves the two new baseline
        # entries actually suppress a matching witness end to end, not
        # merely that they are present in the JSON (the shape-only
        # criticism of `test_real_policy_legacy_facade_baselines_are_
        # populated_and_well_formed` above).
        real_policy = json.loads((ROOT / "architecture/cidx-self-host-policy.json").read_text(encoding="utf-8"))
        graph = ModuleGraph(
            module_of_path={
                "src/analysis/facts.cpp": "analysis.fact-provider",
                "src/analysis/runner.cpp": "analysis.fact-provider",
                "src/storage/storage.cpp": "persistence.sqlite",
            },
            allowed={"analysis.fact-provider": {"analysis.fact-provider"}},
            file_exceptions=set(),
            unclassified=[],
        )
        facts = SemanticFacts(
            symbol_name={
                1: "SqliteFactProvider::snapshot",
                2: "cidx::SqliteStorageService::index_identity()",
                3: "AnalysisRunner::publish",
                4: "cidx::SqliteStorageService::index_identity()",
            },
            symbol_file={
                1: "src/analysis/facts.cpp",
                2: "src/storage/storage.cpp",
                3: "src/analysis/runner.cpp",
                4: "src/storage/storage.cpp",
            },
            symbol_line={1: 534, 2: 849, 3: 1044, 4: 849},
            calls=[
                (900, 1, 2, 541, 36, "calls"),
                (901, 3, 4, 1063, 38, "calls"),
            ],
            truncated=False,
        )
        violations = find_legacy_facade_violations(facts, graph, real_policy)
        self.assertEqual(violations, [])

        # Negative control: a call at a DIFFERENT, unbaselined site through
        # the SAME facade is still rejected -- this is not "the checker
        # always passes analysis.fact-provider", it is specifically these
        # two pinned call sites.
        facts_new_site = SemanticFacts(
            symbol_name=facts.symbol_name,
            symbol_file=facts.symbol_file,
            symbol_line=facts.symbol_line,
            calls=[(902, 1, 2, 9999, 1, "calls")],
            truncated=False,
        )
        still_flagged = find_legacy_facade_violations(facts_new_site, graph, real_policy)
        self.assertEqual(len(still_flagged), 1)
        self.assertEqual(still_flagged[0]["witness"]["call_site"], {"line": 9999, "col": 1})

    def test_real_policy_construction_baselines_are_keyed_at_the_variable_not_the_type(
        self,
    ) -> None:
        # [P1-3 round-3 fix] A census against a REAL self-index (both QA's
        # and the senior reviewer's round-2 verdicts) found that EVERY
        # "Record::Record"-shaped (direct-construction) baseline entry in
        # this policy file was keyed at the wrong column: the manual/
        # textual-audit fallback (docs/self-host-architecture.md) grepped
        # for the TYPE name (`Storage db(...)` -> the "S" of "Storage"),
        # but `emit_construction_form`'s witness
        # (`src/ast/statement_edge_visitor.cpp`) anchors the site at the
        # CXXConstructExpr's own begin loc, which for a declared local
        # `Type var(args);` is Clang's declarator-name position (the "d" of
        # "db"), never the type spelling -- Clang's direct-init
        # CXXConstructExpr source range does not include the type. This was
        # never caught by
        # `test_real_policy_baseline_suppresses_the_fact_providers_index_
        # identity_reads` above because that test only ever exercised the
        # *member-call* baseline entries (`index_identity()`), never a
        # construction one.
        #
        # This test re-derives, straight from the REAL committed source
        # files (no self-index build required -- see the hard constraint
        # against running `cidx index` in this environment), the column a
        # real CXXConstructExpr witness would report for every
        # "Record::Record" baseline entry, and asserts the POLICY FILE'S
        # OWN recorded `col` already equals it. `make_unique<Type>(...)`
        # heap-construction entries have no declared variable at all (the
        # witness anchors on the template argument's own Type spelling
        # instead, which the manual audit already recorded correctly) and
        # are excluded from this check by the same "no bare identifier
        # between the recorded column and the constructor's own '('" rule
        # `_real_construction_baseline_entries` below uses.
        #
        # Reverting the round-2 fix (any of the 35 corrected `col` values)
        # makes this test fail with a message naming the exact
        # file:line/wrong-vs-right column, proving it is not shape-only.
        real_policy = json.loads((ROOT / "architecture/cidx-self-host-policy.json").read_text(encoding="utf-8"))
        checked = 0
        for facade in real_policy["legacyFacades"]:
            for entry in facade.get("baseline", []):
                derived = _derived_construction_witness_column(entry)
                if derived is None:
                    continue  # heap/make_unique form: no declared variable to check
                self.assertEqual(
                    entry["col"],
                    derived,
                    f"{entry['callerFile']}:{entry['line']}: baseline col "
                    f"{entry['col']} does not match the real CXXConstructExpr "
                    f"witness column {derived} for {entry['calleeQualName']!r} "
                    "-- a direct-construction baseline must be keyed at the "
                    "declared VARIABLE, not the type name",
                )
                checked += 1
        # Sanity: this must actually have exercised the bulk of the
        # direct-declaration baseline entries, not silently skipped them
        # all (e.g. because every entry got misclassified as heap-form).
        self.assertGreaterEqual(checked, 30)

    def test_storage_facade_exempts_a_configured_read_port_module_end_to_end(self) -> None:
        # Fixture-level companion to the two real-policy assertions above:
        # proves the MECHANISM works, not just that the real policy file
        # happens to list the right string. A call from a module that is
        # NOT in storage-facade's exemptModules is rejected; the identical
        # call from a module that IS listed is suppressed -- exercising
        # `find_legacy_facade_violations`'s `from_module in exempt` check
        # (scripts/self_host_architecture_report.py) on the signature-
        # bearing resolved callee a real self-index actually produces.
        fixture = _Fixture()
        fixture.add_call(fixture.sym_plan, fixture.sym_write_sig, line=9, col=5)
        report = fixture.run()
        violations = report["findings"]["legacyFacadeViolations"]
        self.assertEqual(len(violations), 1, violations)
        self.assertEqual(violations[0]["fromModule"], "query")

        fixture.policy["legacyFacades"][0]["exemptModules"].append("query")
        fixture.policy_path.write_text(json.dumps(fixture.policy), encoding="utf-8")
        report = fixture.run()
        self.assertEqual(report["findings"]["legacyFacadeViolations"], [])

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
