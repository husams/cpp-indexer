"""Versioned read-only ``query`` and ``explain`` agent tools.

The adapter is deliberately limited to the maintained storage/query surface:
it opens SQLite with ``mode=ro`` and ``PRAGMA query_only`` and delegates CXQ
parsing, normalization, execution, and result-envelope construction to the
existing QueryPlan implementation.
"""

from __future__ import annotations

from dataclasses import dataclass
import argparse
import json
from pathlib import Path
import sqlite3
import os
import sys
from typing import Any
from urllib.parse import quote

from .generated_catalog import CATALOG_HASH, CATALOG_VERSION
from .queryplan import Executor, PlanError, parse_cxq
from .result_protocol import (
    ArtifactRef,
    Completeness,
    Diagnostic,
    Evidence,
    Identity,
    Producer,
    ResultEnvelope,
    Status,
)
from .storage import Storage

PROTOCOL = "cidx.agent/v1"
PROTOCOL_VERSION = 1
MAX_RESULTS = 10_000
TOOLS = ("query", "explain")


@dataclass(frozen=True)
class AgentRequest:
    version: int
    tool: str
    cxq: str
    max_results: int = 1000

    def __post_init__(self) -> None:
        if type(self.version) is not int:
            raise ValueError("E_PROTOCOL_SCHEMA: 'version' must be an integer")
        if self.version != PROTOCOL_VERSION:
            raise ValueError(
                f"E_PROTOCOL_VERSION: unsupported agent protocol version {self.version}"
            )
        if self.tool not in TOOLS:
            raise ValueError(f"E_TOOL: unsupported agent tool {self.tool!r}")
        if not isinstance(self.cxq, str) or not self.cxq:
            raise ValueError("E_PROTOCOL_SCHEMA: 'cxq' must be a non-empty string")
        if type(self.max_results) is not int or not 1 <= self.max_results <= MAX_RESULTS:
            raise ValueError(
                f"E_BUDGET: max_results must be between 1 and {MAX_RESULTS}"
            )

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> "AgentRequest":
        version = value.get("version")
        if type(version) is not int:
            raise ValueError("E_PROTOCOL_SCHEMA: 'version' must be an integer")
        if version != PROTOCOL_VERSION:
            raise ValueError(
                f"E_PROTOCOL_VERSION: unsupported agent protocol version {version}"
            )
        tool = value.get("tool")
        if tool not in TOOLS:
            raise ValueError(f"E_TOOL: unsupported agent tool {tool!r}")
        cxq = value.get("cxq")
        if not isinstance(cxq, str) or not cxq:
            raise ValueError("E_PROTOCOL_SCHEMA: 'cxq' must be a non-empty string")
        budget = value.get("budget", {})
        if not isinstance(budget, dict):
            raise ValueError("E_PROTOCOL_SCHEMA: 'budget' must be an object")
        max_results = budget.get("max_results", 1000)
        if type(max_results) is not int or not 1 <= max_results <= MAX_RESULTS:
            raise ValueError(
                f"E_BUDGET: max_results must be between 1 and {MAX_RESULTS}"
            )
        return cls(version, tool, cxq, max_results)


def _envelope_identity(index: Any) -> Identity:
    return Identity(
        workspace=index.workspace,
        index=f"semantic-index/schema/{index.schema_version}",
        fact_sets=("symbols",),
        freshness=index.freshness,
        source_revision=index.source_revision,
        source_fingerprint=index.source_fingerprint,
    )


def _explain_envelope(explanation: dict[str, Any], index: Any) -> ResultEnvelope:
    freshness = index.freshness
    if freshness == "stale":
        status = Status.UNKNOWN
        state = "unknown"
        diagnostics = [
            Diagnostic(
                "stale_input",
                "error",
                "index contents are stale for the workspace",
                "re-index the affected sources before relying on this result",
            )
        ]
    elif freshness != "current":
        status = Status.UNKNOWN
        state = "unknown"
        diagnostics = [
            Diagnostic(
                "unknown",
                "warning",
                "index freshness could not be verified",
                "stamp or re-index the workspace before relying on this result",
            )
        ]
    else:
        status = Status.COMPLETE
        state = "complete"
        diagnostics = []
    return ResultEnvelope(
        operation="explain",
        status=status,
        identity=_envelope_identity(index),
        producer=Producer(backend="python"),
        completeness=Completeness(state=state, stale=freshness == "stale"),
        result=explanation,
        diagnostics=diagnostics,
        evidence=[Evidence("queryplan", "derived", "producer-verified", "bounded QueryPlan execution")],
        artifacts=[
            ArtifactRef(
                "semantic-index",
                f"semantic-index/schema/{index.schema_version}",
                index.schema_version,
                CATALOG_VERSION,
                CATALOG_HASH,
            )
        ],
    )


class AgentTools:
    """The complete agent catalog and its read-only implementation."""

    def __init__(self, db: Storage, connection: sqlite3.Connection | None = None):
        self._db = db
        self._connection = connection

    @classmethod
    def from_path(cls, path: str | Path) -> "AgentTools":
        absolute = str(Path(path).expanduser().resolve())
        uri = f"file:{quote(absolute, safe='/')}?mode=ro"
        connection = sqlite3.connect(uri, uri=True)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA query_only = ON")
        return cls(Storage.from_connection(connection, absolute), connection)

    @classmethod
    def catalog(cls) -> tuple[str, ...]:
        return TOOLS

    def close(self) -> None:
        if self._connection is not None:
            self._connection.close()
            self._connection = None

    def __enter__(self) -> "AgentTools":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def invoke(self, request: AgentRequest | dict[str, Any]) -> dict[str, Any]:
        if isinstance(request, dict):
            request = AgentRequest.from_dict(request)
        if request.version != PROTOCOL_VERSION:
            raise ValueError(
                f"E_PROTOCOL_VERSION: unsupported agent protocol version {request.version}"
            )
        plan = parse_cxq(request.cxq)
        executor = Executor(self._db)
        if request.tool == "query":
            envelope = executor.run(plan, result_cap=request.max_results).to_envelope_dict()
            rows = envelope["result"].get("rows", [])
            if any(
                row.get("file") is None or row.get("line") is None for row in rows
            ):
                if not envelope["completeness"]["truncated"]:
                    envelope["status"] = "unknown"
                    envelope["completeness"]["state"] = "unknown"
                envelope["diagnostics"].append(
                    {
                        "code": "missing_evidence",
                        "severity": "warning",
                        "message": "result row has no resolvable file/line provenance",
                        "next_action": "select file and line evidence or inspect the index",
                    }
                )
        else:
            envelope = _explain_envelope(
                executor.explain(plan), self._db.index_identity()
            ).to_dict()
        exhausted = envelope["completeness"]["budget"]
        return {
            "protocol": PROTOCOL,
            "version": PROTOCOL_VERSION,
            "tool": request.tool,
            "response": envelope,
            "truncated": envelope["completeness"]["truncated"],
            "budget": {
                "max_results": request.max_results,
                "exhausted": exhausted is not None,
                "exhausted_at": exhausted,
            },
        }

    def invoke_json(self, line: str) -> str:
        try:
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError("E_PROTOCOL_SCHEMA: request must be an object")
            return json.dumps(
                self.invoke(value), ensure_ascii=True, separators=(",", ":")
            )
        except (PlanError, ValueError, json.JSONDecodeError) as error:
            index = self._db.index_identity()
            envelope = ResultEnvelope(
                operation="query",
                status=Status.ERROR,
                identity=_envelope_identity(index),
                producer=Producer(backend="cpp"),
                completeness=Completeness(
                    state="unknown", stale=index.freshness == "stale"
                ),
                result={},
                diagnostics=[Diagnostic("invalid_input", "error", str(error))],
                evidence=[Evidence("queryplan", "derived", "producer-verified", "bounded QueryPlan execution")],
                artifacts=[
                    ArtifactRef(
                        "semantic-index",
                        f"semantic-index/schema/{index.schema_version}",
                        index.schema_version,
                        CATALOG_VERSION,
                        CATALOG_HASH,
                    )
                ],
            ).to_dict()
            return json.dumps(
                {
                    "protocol": PROTOCOL,
                    "version": PROTOCOL_VERSION,
                    "tool": "query",
                    "response": envelope,
                    "truncated": False,
                    "budget": {
                        "max_results": 1000,
                        "exhausted": False,
                        "exhausted_at": None,
                    },
                },
                ensure_ascii=True,
                separators=(",", ":"),
            )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="cidx agent NDJSON adapter")
    parser.add_argument(
        "--index",
        default=str(Path(os.environ.get("INDEXER_CACHE", "~/.cache/cidx")) / "index.db"),
    )
    args = parser.parse_args(argv)
    with AgentTools.from_path(args.index) as tools:
        for line in sys.stdin:
            if line.strip():
                print(tools.invoke_json(line), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


__all__ = [
    "AgentRequest",
    "AgentTools",
    "MAX_RESULTS",
    "PROTOCOL",
    "PROTOCOL_VERSION",
    "TOOLS",
    "main",
]
