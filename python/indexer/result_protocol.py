"""Shared v1 result, evidence, diagnostic, and event protocol."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import StrEnum
import json
import re
from typing import Any, Iterable

from ._version import FULL_VERSION
from .generated_catalog import CATALOG_HASH, CATALOG_VERSION

PROTOCOL = "cidx.result/v1"
EVENT_PROTOCOL = "cidx.event/v1"
MAX_EVIDENCE_DEPTH = 4
MAX_EVIDENCE_NODES = 256
MAX_TEXT_BYTES = 4096


class Status(StrEnum):
    COMPLETE = "complete"
    PARTIAL = "partial"
    UNKNOWN = "unknown"
    REFUTED = "refuted"
    CONDITIONAL = "conditional"
    ERROR = "error"


class ExitClass(StrEnum):
    SUCCESS = "success"
    USAGE = "usage"
    INVALID_OR_STALE_INPUT = "invalid_or_stale_input"
    POLICY_FAILURE = "policy_failure"
    UNKNOWN = "unknown"
    INFRASTRUCTURE_FAILURE = "infrastructure_failure"


_EXIT_CODES = {
    ExitClass.SUCCESS: 0,
    ExitClass.USAGE: 2,
    ExitClass.INVALID_OR_STALE_INPUT: 3,
    ExitClass.POLICY_FAILURE: 4,
    ExitClass.UNKNOWN: 5,
    ExitClass.INFRASTRUCTURE_FAILURE: 6,
}
_SECRET = re.compile(r"(TOKEN|PASSWORD|SECRET)([=:])[^\s,;]+")


def redact_text(value: str, max_bytes: int = MAX_TEXT_BYTES) -> str:
    """Redact common secret assignments and bound untrusted text."""
    value = _SECRET.sub(lambda match: f"{match.group(1)}{match.group(2)}<redacted:secret>", value)
    if len(value) <= max_bytes:
        return value
    suffix = "...<redacted:size-limit>"
    if max_bytes <= len(suffix):
        return suffix[:max_bytes]
    return value[: max_bytes - len(suffix)] + suffix


def redact_arguments(arguments: Iterable[str], max_bytes: int = MAX_TEXT_BYTES) -> list[str]:
    return [redact_text(argument, max_bytes) for argument in arguments]


@dataclass(frozen=True)
class Identity:
    workspace: str = "unknown"
    index: str = "unknown"
    fact_sets: tuple[str, ...] = ()
    freshness: str = "unverifiable"
    source_revision: str | None = None
    source_fingerprint: str | None = None


@dataclass(frozen=True)
class Producer:
    package: str = "cidx"
    version: str = FULL_VERSION
    backend: str = "unknown"
    schema_version: int = 1


@dataclass(frozen=True)
class Completeness:
    state: str = "complete"
    truncated: bool = False
    stale: bool = False
    budget: int | None = None


@dataclass(frozen=True)
class Diagnostic:
    code: str
    severity: str = "error"
    message: str = ""
    next_action: str | None = None

    def to_dict(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "code": self.code,
            "severity": self.severity,
            "message": redact_text(self.message),
        }
        if self.next_action is not None:
            out["next_action"] = redact_text(self.next_action)
        return out


@dataclass(frozen=True)
class Evidence:
    id: str
    evidence_class: str = "derived"
    trust: str = "unverified"
    summary: str = ""
    source: str | None = None
    children: tuple["Evidence", ...] = ()

    def to_dict(self, depth: int = 1, count: list[int] | None = None) -> dict[str, Any]:
        if depth > MAX_EVIDENCE_DEPTH:
            raise ValueError("evidence tree exceeds protocol bounds")
        count = count if count is not None else [0]
        count[0] += 1
        if count[0] > MAX_EVIDENCE_NODES:
            raise ValueError("evidence tree exceeds protocol bounds")
        out: dict[str, Any] = {
            "id": self.id,
            "class": self.evidence_class,
            "trust": self.trust,
            "summary": redact_text(self.summary),
        }
        if self.source is not None:
            out["source"] = redact_text(self.source)
        if self.children:
            out["children"] = [child.to_dict(depth + 1, count) for child in self.children]
        return out


@dataclass(frozen=True)
class ArtifactRef:
    kind: str
    id: str
    schema_version: int = 1
    catalog_version: int = CATALOG_VERSION
    catalog_hash: str = CATALOG_HASH

    def to_dict(self) -> dict[str, Any]:
        return {
            "kind": self.kind,
            "id": redact_text(self.id),
            "schema_version": self.schema_version,
            "catalog_version": self.catalog_version,
            "catalog_hash": self.catalog_hash,
        }


@dataclass(frozen=True)
class ReplayInput:
    command: str
    arguments: tuple[str, ...] = ()

    def to_dict(self) -> dict[str, Any]:
        return {"command": redact_text(self.command), "arguments": redact_arguments(self.arguments)}


@dataclass(frozen=True)
class ResourceMetadata:
    elapsed_ms: int | None = None
    peak_bytes: int | None = None

    def to_dict(self) -> dict[str, int | None]:
        return {"elapsed_ms": self.elapsed_ms, "peak_bytes": self.peak_bytes}


@dataclass
class ResultEnvelope:
    operation: str
    status: Status = Status.COMPLETE
    identity: Identity = field(default_factory=Identity)
    producer: Producer = field(default_factory=Producer)
    completeness: Completeness = field(default_factory=Completeness)
    result: dict[str, Any] = field(default_factory=dict)
    diagnostics: list[Diagnostic] = field(default_factory=list)
    evidence: list[Evidence] = field(default_factory=list)
    artifacts: list[ArtifactRef] = field(default_factory=list)
    replay: ReplayInput | None = None
    resources: ResourceMetadata | None = None

    def exit_class(self) -> ExitClass:
        if self.status is Status.REFUTED:
            return ExitClass.POLICY_FAILURE
        if self.status is Status.ERROR:
            codes = {diagnostic.code for diagnostic in self.diagnostics}
            if "usage" in codes:
                return ExitClass.USAGE
            if codes & {"invalid_input", "stale_input"}:
                return ExitClass.INVALID_OR_STALE_INPUT
            if codes & {"backend_error", "timeout"}:
                return ExitClass.INFRASTRUCTURE_FAILURE
            return ExitClass.INFRASTRUCTURE_FAILURE
        if self.status is Status.UNKNOWN:
            return ExitClass.UNKNOWN
        return ExitClass.SUCCESS

    def exit_code(self) -> int:
        return _EXIT_CODES[self.exit_class()]

    def validate(self) -> None:
        if not self.operation or not self.identity.workspace or not self.identity.index:
            raise ValueError("result envelope identity is incomplete")
        if not self.identity.fact_sets:
            raise ValueError("result envelope must identify fact sets")
        if not self.producer.package or not self.producer.version or not self.producer.backend:
            raise ValueError("result envelope producer is incomplete")
        if self.completeness.truncated and self.completeness.state == "complete":
            raise ValueError("truncated results cannot be complete")
        if self.completeness.stale and self.identity.freshness != "stale":
            raise ValueError("stale completeness must identify a stale index")
        count = [0]
        for node in self.evidence:
            node.to_dict(count=count)

    def to_dict(self) -> dict[str, Any]:
        self.validate()
        out: dict[str, Any] = {
            "protocol": PROTOCOL,
            "operation": self.operation,
            "status": self.status.value,
            "exit_class": self.exit_class().value,
            "exit_code": self.exit_code(),
            "result": self.result,
            "identity": {
                "workspace": self.identity.workspace,
                "index": self.identity.index,
                "fact_sets": list(self.identity.fact_sets),
                "freshness": self.identity.freshness,
                "source_revision": self.identity.source_revision,
                "source_fingerprint": self.identity.source_fingerprint,
            },
            "producer": {
                "package": self.producer.package,
                "version": self.producer.version,
                "backend": self.producer.backend,
                "schema_version": self.producer.schema_version,
            },
            "completeness": {
                "state": self.completeness.state,
                "truncated": self.completeness.truncated,
                "stale": self.completeness.stale,
                "budget": self.completeness.budget,
            },
            "diagnostics": [diagnostic.to_dict() for diagnostic in self.diagnostics],
            "evidence": [node.to_dict() for node in self.evidence],
            "artifacts": [artifact.to_dict() for artifact in self.artifacts],
        }
        if self.replay is not None:
            out["replay"] = self.replay.to_dict()
        if self.resources is not None:
            out["resources"] = self.resources.to_dict()
        return out

    def dumps(self) -> str:
        return json.dumps(self.to_dict(), indent=2, ensure_ascii=True)

    def human_text(self) -> str:
        out = f"status: {self.status.value}"
        if self.completeness.truncated:
            out += " (truncated)"
        if self.completeness.stale:
            out += " (stale index)"
        if self.diagnostics:
            out += f"\nreason: {self.diagnostics[0].message}"
            if self.diagnostics[0].next_action:
                out += f"\nnext: {self.diagnostics[0].next_action}"
        return out


def from_query_result(result: Any, index: Any, *, operation: str = "query") -> ResultEnvelope:
    """Adapt a QueryPlan Result without allowing renderers to reclassify it."""
    stale = index.freshness == "stale"
    status = Status.UNKNOWN if stale else Status.PARTIAL if result.truncated else Status.COMPLETE
    state = "unknown" if stale else "partial" if result.truncated else "complete"
    payload: dict[str, Any] = {
        "shape": result.shape,
        "view": result.view,
        "count": result.scalar if result.shape == "scalar" else len(result.rows),
        "truncated": result.truncated,
    }
    if result.shape != "scalar":
        payload["rows"] = [dict(zip(result.fields, row)) for row in result.rows]
    envelope = ResultEnvelope(
        operation=operation,
        status=status,
        identity=Identity(
            index=f"semantic-index/schema/{index.schema_version}",
            fact_sets=("symbols" if result.view == "symbol" else "entities",),
            freshness=index.freshness,
            source_revision=index.source_revision,
            source_fingerprint=index.source_fingerprint,
        ),
        producer=Producer(backend="python"),
        completeness=Completeness(state=state, truncated=result.truncated, stale=stale),
        result=payload,
        evidence=[Evidence("queryplan", "derived", "producer-verified", "bounded QueryPlan execution")],
        artifacts=[ArtifactRef("semantic-index", f"semantic-index/schema/{index.schema_version}", index.schema_version)],
    )
    if result.truncated:
        envelope.diagnostics.append(Diagnostic(
            "truncated_budget", "warning",
            "result was bounded by the QueryPlan execution budget",
            "narrow the query or provide an explicit limit",
        ))
    if stale:
        envelope.diagnostics.append(Diagnostic(
            "stale_input", "error", "index contents are stale for the workspace",
            "re-index the affected sources before relying on this result",
        ))
    return envelope


@dataclass(frozen=True)
class ProgressEvent:
    sequence: int
    operation: str
    event: str = "progress"
    message: str = ""
    completed: int | None = None
    total: int | None = None

    def to_dict(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "protocol": EVENT_PROTOCOL,
            "sequence": self.sequence,
            "operation": self.operation,
            "event": self.event,
            "message": redact_text(self.message),
        }
        if self.completed is not None:
            out["completed"] = self.completed
        if self.total is not None:
            out["total"] = self.total
        return out
