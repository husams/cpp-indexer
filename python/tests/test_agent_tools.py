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


if __name__ == "__main__":
    unittest.main()
