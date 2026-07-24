from __future__ import annotations

import json
from pathlib import Path

from indexer.result_protocol import (
    ArtifactRef,
    Completeness,
    Diagnostic,
    Evidence,
    Identity,
    Producer,
    ResultEnvelope,
    Status,
    from_query_result,
    redact_text,
)
from indexer.storage import Storage
from indexer.queryplan import Executor, codebase, nodes, start

ROOT = Path(__file__).resolve().parents[2]
GOLDEN = ROOT / "spec/contracts/golden/result-envelope.json"


def _golden_envelope() -> ResultEnvelope:
    return ResultEnvelope(
        operation="query",
        identity=Identity(
            "workspace://demo",
            "semantic-index://demo",
            ("symbols", "files"),
            "current",
            "git:abc123",
            "sha256:source",
        ),
        producer=Producer("cidx", "0.53.0", "cpp", 1),
        result={
            "shape": "rows",
            "view": "symbol",
            "count": 1,
            "truncated": False,
            "rows": [{"id": 42, "name": "main", "kind": "function"}],
        },
        evidence=[Evidence("queryplan", "derived", "producer-verified", "bounded QueryPlan execution", "query://demo")],
        artifacts=[ArtifactRef("query-result", "query-result://demo", 1, 1, "sha256:catalog")],
    )


def test_shared_golden_serializes_deterministically() -> None:
    expected = json.loads(GOLDEN.read_text(encoding="utf-8"))
    envelope = _golden_envelope()
    assert envelope.to_dict() == expected
    assert json.loads(envelope.dumps()) == expected


def test_query_result_adapter_preserves_stale_and_truncated_semantics() -> None:
    db = Storage(":memory:")
    result = Executor(db).run((start(codebase()) | nodes()).plan)
    envelope = from_query_result(result, result.index)
    assert envelope.status is Status.COMPLETE
    assert envelope.identity.fact_sets == ("symbols",)

    result.truncated = True
    truncated = from_query_result(result, result.index)
    assert truncated.status is Status.PARTIAL
    assert truncated.completeness.truncated
    assert truncated.diagnostics[0].code == "truncated_budget"

    result.index = result.index.__class__(
        result.index.schema_version,
        result.index.source_revision,
        result.index.source_fingerprint,
        result.index.index_config,
        result.index.index_config_fingerprint,
        "stale",
    )
    stale = from_query_result(result, result.index)
    assert stale.status is Status.UNKNOWN
    assert stale.completeness.stale
    assert {item.code for item in stale.diagnostics} >= {"stale_input"}


def test_untrusted_text_is_redacted_and_bounded() -> None:
    assert "<redacted:secret>" in redact_text("TOKEN=hidden")
    assert "<redacted:size-limit>" in redact_text("x" * 5000)


def test_exit_reduction_does_not_upgrade_error_reasons() -> None:
    envelope = _golden_envelope()
    envelope.status = Status.ERROR
    envelope.diagnostics.append(Diagnostic("timeout"))
    assert envelope.exit_code() == 6
    envelope.diagnostics[:] = [Diagnostic("invalid_input")]
    assert envelope.exit_code() == 3
    envelope.diagnostics[:] = [Diagnostic("usage")]
    assert envelope.exit_code() == 2
