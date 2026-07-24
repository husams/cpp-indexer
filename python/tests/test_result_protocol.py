from __future__ import annotations

import json
import math
from pathlib import Path

import pytest

from indexer.result_protocol import (
    ArtifactRef,
    Completeness,
    Diagnostic,
    Evidence,
    Identity,
    Producer,
    ProgressEvent,
    ResultEnvelope,
    Status,
    ACCEPTANCE_VECTORS,
    from_query_result,
    redact_text,
)
from indexer.generated_result_protocol import (
    OVERSIZED_ASCII_BYTES,
    OVERSIZED_MULTIBYTE_CHARS,
    PLACEHOLDER_IDENTITIES,
)
from indexer.storage import Storage
from indexer.queryplan import Executor, codebase, nodes, start

ROOT = Path(__file__).resolve().parents[2]
GOLDEN = ROOT / "spec/contracts/golden/result-envelope.json"
EVENT_GOLDEN = ROOT / "spec/contracts/golden/event.json"
ERROR_GOLDEN = ROOT / "spec/contracts/golden/error-status.json"
SCHEMA = ROOT / "spec/contracts/result-envelope.schema.json"


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
    assert envelope.status is Status.UNKNOWN
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
    assert stale.exit_code() == 3
    assert stale.completeness.stale
    assert {item.code for item in stale.diagnostics} >= {"stale_input"}


def test_untrusted_text_is_redacted_and_bounded() -> None:
    assert "<redacted:secret>" in redact_text("TOKEN=hidden")
    assert "<redacted:size-limit>" in redact_text("x" * 5000)
    bounded = redact_text("😀" * 5000)
    assert len(bounded.encode("utf-8")) <= 4096
    envelope = _golden_envelope()
    envelope.status = Status.ERROR
    envelope.completeness = Completeness("unknown")
    envelope.diagnostics = [Diagnostic("backend_error", message="TOKEN=hidden", next_action="PASSWORD=next")]
    human = envelope.human_text()
    assert "TOKEN=hidden" not in human and "PASSWORD=next" not in human
    assert "<redacted:secret>" in human


def test_exit_reduction_does_not_upgrade_error_reasons() -> None:
    envelope = _golden_envelope()
    envelope.status = Status.ERROR
    envelope.diagnostics.append(Diagnostic("timeout"))
    assert envelope.exit_code() == 6
    envelope.diagnostics[:] = [Diagnostic("invalid_input")]
    assert envelope.exit_code() == 3
    envelope.diagnostics[:] = [Diagnostic("usage")]
    assert envelope.exit_code() == 2


def test_fail_closed_invariants_and_acceptance_vectors() -> None:
    envelope = _golden_envelope()
    envelope.completeness = Completeness("unknown")
    try:
        envelope.to_dict()
    except ValueError:
        pass
    else:
        raise AssertionError("contradictory completeness must be rejected")
    for vector in ACCEPTANCE_VECTORS:
        current = _golden_envelope()
        current.operation = vector["operation"]
        current.status = Status(vector["status"])
        current.identity = Identity("workspace://demo", "semantic-index://demo", ("symbols",), vector["freshness"], "git:abc123", "sha256:source")
        current.completeness = Completeness(vector["state"], vector["diagnostic"] == "truncated_budget", vector["freshness"] == "stale")
        current.diagnostics = [] if vector["diagnostic"] is None else [Diagnostic(vector["diagnostic"], "error", "vector diagnostic", "next")]
        current.to_dict()
        assert current.exit_class().value == vector["exit_class"]
        assert current.exit_code() == vector["exit_code"]


def test_fail_closed_cross_field_and_numeric_invariants() -> None:
    envelope = _golden_envelope()
    envelope.diagnostics = [Diagnostic("backend_error")]
    with pytest.raises(ValueError):
        envelope.to_dict()


@pytest.mark.parametrize("placeholder", PLACEHOLDER_IDENTITIES)
def test_generated_placeholder_identities_are_rejected(placeholder: str) -> None:
    envelope = _golden_envelope()
    envelope.identity = Identity(placeholder, "semantic-index://demo", ("symbols",), "current", "git:abc123", "sha256:source")
    with pytest.raises(ValueError):
        envelope.to_dict()
    envelope.identity = Identity("workspace://demo", placeholder, ("symbols",), "current", "git:abc123", "sha256:source")
    with pytest.raises(ValueError):
        envelope.to_dict()


def test_generated_reason_rules_and_utf8_identity_bounds_are_rejected() -> None:
    for code in ("unknown", "missing_evidence"):
        envelope = _golden_envelope()
        envelope.diagnostics = [Diagnostic(code, "warning", "weak")]
        with pytest.raises(ValueError):
            envelope.to_dict()

    envelope = _golden_envelope()
    envelope.status = Status.REFUTED
    envelope.completeness = Completeness("unknown")
    with pytest.raises(ValueError):
        envelope.to_dict()

    envelope = _golden_envelope()
    envelope.identity = Identity("x" * OVERSIZED_ASCII_BYTES, "semantic-index://demo", ("symbols",), "current", "git:abc123", "sha256:source")
    with pytest.raises(ValueError):
        envelope.to_dict()
    envelope.identity = Identity("😀" * OVERSIZED_MULTIBYTE_CHARS, "semantic-index://demo", ("symbols",), "current", "git:abc123", "sha256:source")
    with pytest.raises(ValueError):
        envelope.to_dict()

    envelope = _golden_envelope()
    envelope.identity = Identity("workspace://demo", "semantic-index://demo", ("symbols",), "current", "git:abc123", "sha256:source")
    envelope.status = Status.UNKNOWN
    envelope.completeness = Completeness("unknown")
    envelope.diagnostics = [Diagnostic("stale_input")]
    with pytest.raises(ValueError):
        envelope.to_dict()

    envelope = _golden_envelope()
    envelope.result["nan"] = math.nan
    with pytest.raises(ValueError):
        envelope.to_dict()
    envelope.result["nan"] = 2**63
    with pytest.raises(ValueError):
        envelope.to_dict()


def test_nested_evidence_text_is_validated_recursively() -> None:
    for child_text in (
        "x" * OVERSIZED_ASCII_BYTES,
        "😀" * OVERSIZED_MULTIBYTE_CHARS,
        "\ud800",
    ):
        envelope = _golden_envelope()
        envelope.evidence = [Evidence("root", children=(Evidence(child_text),))]
        with pytest.raises(ValueError):
            envelope.to_dict()


def test_event_and_error_goldens_are_executable() -> None:
    event = ProgressEvent(3, "index", "progress", "indexed 2 of 4 files", 2, 4)
    assert event.to_dict() == json.loads(EVENT_GOLDEN.read_text(encoding="utf-8"))
    envelope = _golden_envelope()
    envelope.status = Status.ERROR
    envelope.completeness = Completeness("unknown")
    envelope.diagnostics = [Diagnostic("backend_error", message="backend unavailable")]
    assert envelope.error_status_dict() == json.loads(ERROR_GOLDEN.read_text(encoding="utf-8"))


def test_generated_schema_carries_identity_and_utf8_contract_rules() -> None:
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    identity = schema["properties"]["identity"]["properties"]
    for field in ("workspace", "index"):
        assert identity[field]["x-maxUtf8Bytes"] == 4096
        assert set(PLACEHOLDER_IDENTITIES) == set(identity[field]["not"]["enum"])
    assert "maxLength" not in json.dumps(schema)
    assert any(
        rule.get("if", {}).get("properties", {}).get("status", {}).get("const") == "complete"
        and "allOf" in rule.get("then", {})
        for rule in schema["allOf"]
    )
