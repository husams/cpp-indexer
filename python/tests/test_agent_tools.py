from __future__ import annotations

import json
import os
import sqlite3
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from indexer.agent_tools import AgentRequest, AgentTools, TOOLS
from indexer.storage import Storage, Symbol


class AgentToolsTests(unittest.TestCase):
    def _database(self) -> Path:
        handle = tempfile.NamedTemporaryFile(suffix=".db", delete=False)
        path = Path(handle.name)
        handle.close()
        db = Storage(str(path))
        db.add_symbol(Symbol(usr="USR::agent-first", spelling="first", kind="function"))
        db.add_symbol(Symbol(usr="USR::agent-second", spelling="second", kind="function"))
        db.close()
        return path

    def test_catalog_is_exactly_query_and_explain(self) -> None:
        self.assertEqual(TOOLS, ("query", "explain"))
        self.assertEqual(AgentTools.catalog(), TOOLS)

    def test_read_only_budget_and_provenance_contract(self) -> None:
        path = self._database()
        try:
            with AgentTools.from_path(path) as tools:
                result = tools.invoke(
                    AgentRequest(
                        1,
                        "query",
                        "codebase() | nodes() | select(name, file, line)",
                        1,
                    )
                )
                self.assertTrue(result["truncated"])
                self.assertEqual(result["budget"]["exhausted_at"], 1)
                self.assertEqual(
                    result["response"]["completeness"]["budget"], 1
                )
                self.assertEqual(result["response"]["result"]["rows"][0]["file"], None)
                self.assertEqual(result["response"]["status"], "partial")
                self.assertEqual(
                    result["response"]["diagnostics"][-1]["code"],
                    "missing_evidence",
                )

                complete = tools.invoke(
                    AgentRequest(
                        1,
                        "query",
                        "codebase() | nodes() | select(name, file, line)",
                        10,
                    )
                )
                self.assertEqual(complete["response"]["status"], "unknown")
                self.assertEqual(
                    complete["response"]["diagnostics"][-1]["code"],
                    "missing_evidence",
                )

                with self.assertRaises(sqlite3.OperationalError):
                    tools._connection.execute("CREATE TABLE forbidden(id INTEGER)")  # noqa: SLF001
        finally:
            path.unlink(missing_ok=True)

    def test_explain_uses_normalized_plan_and_version_errors_are_stable(self) -> None:
        path = self._database()
        try:
            with AgentTools.from_path(path) as tools:
                result = tools.invoke(
                    {
                        "version": 1,
                        "tool": "explain",
                        "cxq": "codebase() | nodes() | select(name, file, line)",
                        "budget": {"max_results": 2},
                    }
                )
                self.assertEqual(result["tool"], "explain")
                self.assertIn("plan", result["response"]["result"])
                self.assertIn("budgets", result["response"]["result"])
                self.assertFalse(result["truncated"])
                with self.assertRaisesRegex(
                    ValueError,
                    "E_PROTOCOL_VERSION: unsupported agent protocol version 99",
                ):
                    AgentRequest.from_dict(
                        {"version": 99, "tool": "query", "cxq": "codebase()"}
                    )
        finally:
            path.unlink(missing_ok=True)

    def test_typed_boundary_excludes_cli_state_and_catalog_is_closed(self) -> None:
        header = Path(__file__).parents[2] / "src" / "application" / "agent_tools.hpp"
        source = header.read_text(encoding="utf-8")
        self.assertNotIn("ParsedArgs", source)
        self.assertNotIn('#include "cli/', source)
        self.assertEqual(set(TOOLS), {"query", "explain"})

    def test_documented_process_adapter_is_built(self) -> None:
        executable = Path(__file__).parents[2] / "build" / "cidx-agent"
        self.assertTrue(executable.is_file())

    def test_cpp_and_python_process_adapters_emit_one_ndjson_frame(self) -> None:
        path = self._database()
        request = '{"version":1,"tool":"explain","cxq":"codebase() | nodes()"}\n'
        try:
            with AgentTools.from_path(path) as tools:
                expected = json.loads(tools.invoke_json(request))
            environment = {
                **os.environ,
                "PYTHONPATH": str(Path(__file__).parents[1]),
            }
            python_process = subprocess.run(
                [sys.executable, "-m", "indexer.agent_tools", "--index", str(path)],
                input=request,
                text=True,
                capture_output=True,
                check=True,
                env=environment,
            )
            cpp_process = subprocess.run(
                [str(Path(__file__).parents[2] / "build" / "cidx-agent"), "--index", str(path)],
                input=request,
                text=True,
                capture_output=True,
                check=True,
            )
            self.assertEqual(len(python_process.stdout.splitlines()), 1)
            self.assertEqual(len(cpp_process.stdout.splitlines()), 1)
            self.assertEqual(
                json.loads(python_process.stdout)["response"]["result"],
                expected["response"]["result"],
            )
            self.assertEqual(
                json.loads(cpp_process.stdout)["response"]["result"],
                expected["response"]["result"],
            )

            invalid_request = '{"version":99,"tool":"query","cxq":"codebase()"}\n'
            python_error = subprocess.run(
                [sys.executable, "-m", "indexer.agent_tools", "--index", str(path)],
                input=invalid_request,
                text=True,
                capture_output=True,
                check=True,
                env=environment,
            )
            cpp_error = subprocess.run(
                [str(Path(__file__).parents[2] / "build" / "cidx-agent"), "--index", str(path)],
                input=invalid_request,
                text=True,
                capture_output=True,
                check=True,
            )
            self.assertEqual(json.loads(cpp_error.stdout), json.loads(python_error.stdout))
        finally:
            path.unlink(missing_ok=True)

    def test_shared_golden_matrix_covers_agent_cpp_python_and_cli(self) -> None:
        matrix_path = Path(__file__).parents[2] / "tests" / "golden" / "agent_tools_matrix.json"
        matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
        path = self._database()
        try:
            environment = {
                **os.environ,
                "PYTHONPATH": str(Path(__file__).parents[1]),
            }
            with AgentTools.from_path(path) as tools:
                for case in matrix["cases"]:
                    case_id = case["id"]
                    surfaces = set(case["surfaces"])
                    request = {
                        "version": 1,
                        "tool": case["tool"],
                        "cxq": case["cxq"],
                        "budget": {"max_results": case["max_results"]},
                    }
                    request_line = json.dumps(request, separators=(",", ":")) + "\n"

                    if case["tool"] == "query" and case_id != "rejected_cxq":
                        library = tools.invoke(request)
                    else:
                        library = None

                    python_process = subprocess.run(
                        [sys.executable, "-m", "indexer.agent_tools", "--index", str(path)],
                        input=request_line,
                        text=True,
                        capture_output=True,
                        check=True,
                        env=environment,
                    )
                    cpp_process = subprocess.run(
                        [str(Path(__file__).parents[2] / "build" / "cidx-agent"), "--index", str(path)],
                        input=request_line,
                        text=True,
                        capture_output=True,
                        check=True,
                    )
                    python_frame = json.loads(python_process.stdout)
                    cpp_frame = json.loads(cpp_process.stdout)

                    self.assertIn("python", surfaces, case_id)
                    self._assert_matrix_frame(case, python_frame, case_id)
                    self.assertIn("cpp", surfaces, case_id)
                    self._assert_matrix_frame(case, cpp_frame, case_id)
                    self.assertEqual(
                        self._canonical_json(
                            self._without_producer_backend(python_frame)
                        ),
                        self._canonical_json(
                            self._without_producer_backend(cpp_frame)
                        ),
                        case_id,
                    )
                    if case_id == "rejected_cxq":
                        self.assertEqual(
                            self._canonical_json(python_frame),
                            self._canonical_json(cpp_frame),
                            case_id,
                        )
                    if library is not None:
                        self.assertEqual(library, tools.invoke(request), case_id)
                        self.assertEqual(
                            self._canonical_json(library["response"]["result"]),
                            self._canonical_json(python_frame["response"]["result"]),
                            case_id,
                        )

                    if "cli" in surfaces:
                        cli_args = [
                            str(Path(__file__).parents[2] / "build" / "cidx"),
                            "query",
                            case["cxq"],
                            "--db",
                            str(path),
                            "--json",
                        ]
                        if case["cli_mode"] == "explain":
                            cli_args.append("--explain")
                        cli_process = subprocess.run(
                            cli_args,
                            text=True,
                            capture_output=True,
                            check=False,
                        )
                        if "cli_error" in case:
                            self.assertNotEqual(cli_process.returncode, 0, case_id)
                            self.assertEqual(
                                cli_process.stderr,
                                f"error: {case['cli_error']}\n",
                                case_id,
                            )
                        else:
                            self.assertEqual(cli_process.returncode, 0, case_id)
                            cli_result = json.loads(cli_process.stdout)
                            self.assertEqual(
                                self._canonical_json(cli_result),
                                self._canonical_json(
                                    python_frame["response"]["result"]
                                ),
                                case_id,
                            )
        finally:
            path.unlink(missing_ok=True)

    @staticmethod
    def _canonical_json(value: object) -> str:
        return json.dumps(value, ensure_ascii=True, separators=(",", ":"))

    @staticmethod
    def _without_producer_backend(frame: dict) -> dict:
        normalized = json.loads(json.dumps(frame))
        normalized["response"]["producer"].pop("backend", None)
        return normalized

    @staticmethod
    def _assert_matrix_frame(case: dict, frame: dict, case_id: str) -> None:
        response = frame["response"]
        expected = case
        if expected["expected_status"] is not None:
            assert response["status"] == expected["expected_status"], case_id
        assert response["completeness"]["truncated"] == expected["expected_truncated"], case_id
        assert response["completeness"]["budget"] == expected["expected_budget"], case_id
        result = response["result"]
        if expected["expected_shape"] is not None:
            shape_key = "execution_shape" if case["tool"] == "explain" else "shape"
            assert result[shape_key] == expected["expected_shape"], case_id
        if expected["expected_count"] is not None:
            assert result["count"] == expected["expected_count"], case_id
        for key in expected["expected_result_keys"]:
            assert key in result, (case_id, key)
        diagnostic_codes = [item["code"] for item in response["diagnostics"]]
        non_freshness_codes = [code for code in diagnostic_codes if code != "unknown"]
        assert non_freshness_codes == expected["expected_diagnostics"], case_id
        assert not set(non_freshness_codes).intersection(
            expected.get("forbidden_diagnostics", [])
        ), case_id
        if "cli_error" in expected:
            invalid_diagnostics = [
                item
                for item in response["diagnostics"]
                if item["code"] == "invalid_input"
            ]
            assert len(invalid_diagnostics) == 1, case_id
            assert invalid_diagnostics[0]["message"] == expected["cli_error"], case_id
        if expected["expected_row_file_null"]:
            assert result["rows"][0]["file"] is None, case_id


if __name__ == "__main__":
    unittest.main()
