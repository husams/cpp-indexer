"""Shared v1 result, evidence, diagnostic, and event protocol."""

from __future__ import annotations

from dataclasses import dataclass, field
import json
import math
import re
from typing import Any, Iterable

from ._version import FULL_VERSION
from .generated_catalog import CATALOG_HASH, CATALOG_VERSION
from .generated_result_protocol import (
    ACCEPTANCE_VECTORS,
    ARTIFACT_KINDS,
    COMPLETENESS_STATES,
    DIAGNOSTIC_CODES,
    DIAGNOSTIC_SEVERITIES,
    EVIDENCE_CLASSES,
    EVENT_KINDS,
    EXIT_CODES,
    EXIT_REASON_PRECEDENCE,
    FRESHNESS,
    MAX_ARTIFACTS,
    MAX_DIAGNOSTICS,
    MAX_EVIDENCE,
    MAX_EVIDENCE_DEPTH,
    MAX_EVIDENCE_NODES,
    MAX_FACT_SETS,
    MAX_HUMAN_OUTPUT_BYTES,
    MAX_REPLAY_ARGUMENTS,
    MAX_RESULT_DEPTH,
    MAX_RESULT_ITEMS,
    MAX_RESULT_PROPERTIES,
    MAX_TEXT_BYTES,
    MAX_INTEGER,
    MIN_INTEGER,
    DIAGNOSTIC_STATUS_RULES,
    EVENT_PROTOCOL as GENERATED_EVENT_PROTOCOL,
    PROTOCOL as GENERATED_PROTOCOL,
    PROTOCOL_VERSION,
    PLACEHOLDER_IDENTITIES,
    Status,
    ExitClass,
    TRUST_LEVELS,
)

PROTOCOL = GENERATED_PROTOCOL
EVENT_PROTOCOL = GENERATED_EVENT_PROTOCOL
_SECRET = re.compile(r"(TOKEN|PASSWORD|SECRET)([=:])[^\s,;]+")
def _is_protocol_integer(value: Any) -> bool:
    return type(value) is int and MIN_INTEGER <= value <= MAX_INTEGER


def _valid_text(value: Any) -> bool:
    if not isinstance(value, str):
        return False
    try:
        return len(value.encode("utf-8")) <= MAX_TEXT_BYTES
    except UnicodeEncodeError:
        return False


def redact_text(value: str, max_bytes: int = MAX_TEXT_BYTES) -> str:
    """Redact common secret assignments and bound untrusted text."""
    value = _SECRET.sub(lambda match: f"{match.group(1)}{match.group(2)}<redacted:secret>", value)
    encoded = value.encode("utf-8")
    if len(encoded) <= max_bytes:
        return value
    suffix = "...<redacted:size-limit>"
    suffix_bytes = suffix.encode("utf-8")
    if max_bytes <= len(suffix_bytes):
        return suffix_bytes[:max_bytes].decode("utf-8", "ignore")
    prefix = encoded[: max_bytes - len(suffix_bytes)]
    return prefix.decode("utf-8", "ignore") + suffix


def redact_arguments(arguments: Iterable[str], max_bytes: int = MAX_TEXT_BYTES) -> list[str]:
    return [redact_text(argument, max_bytes) for argument in arguments]


def _sanitize_value(value: Any, depth: int = 0) -> Any:
    if depth > MAX_RESULT_DEPTH:
        raise ValueError("result payload exceeds protocol depth")
    if isinstance(value, str):
        return redact_text(value)
    if value is None or isinstance(value, bool):
        return value
    if isinstance(value, int):
        if value < MIN_INTEGER or value > MAX_INTEGER:
            raise ValueError("result integer exceeds the signed 64-bit protocol range")
        return value
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("non-finite result numbers are not valid JSON")
        raise ValueError("floating-point result numbers are not supported by the protocol")
    if isinstance(value, (list, tuple)):
        if len(value) > MAX_RESULT_ITEMS:
            raise ValueError("result payload exceeds protocol item bound")
        return [_sanitize_value(item, depth + 1) for item in value]
    if isinstance(value, dict):
        if len(value) > MAX_RESULT_PROPERTIES:
            raise ValueError("result payload exceeds protocol property bound")
        if any(not _valid_text(key) for key in value):
            raise ValueError("result property key exceeds protocol bounds")
        return {
            key: _sanitize_value(item, depth + 1)
            for key, item in value.items()
        }
    raise ValueError(f"unsupported result payload type: {type(value).__name__}")


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
        if self.evidence_class not in EVIDENCE_CLASSES or self.trust not in TRUST_LEVELS:
            raise ValueError("evidence domain is invalid")
        if any(not _valid_text(value) for value in (self.id, self.evidence_class, self.trust, self.summary, self.source) if value is not None):
            raise ValueError("evidence text exceeds protocol bounds")
        if depth > MAX_EVIDENCE_DEPTH:
            raise ValueError("evidence tree exceeds protocol bounds")
        count = count if count is not None else [0]
        count[0] += 1
        if count[0] > MAX_EVIDENCE_NODES:
            raise ValueError("evidence tree exceeds protocol bounds")
        out: dict[str, Any] = {
            "id": redact_text(self.id),
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
        if self.kind not in ARTIFACT_KINDS:
            raise ValueError("artifact kind is invalid")
        if not _is_protocol_integer(self.schema_version) or not _is_protocol_integer(self.catalog_version):
            raise ValueError("artifact versions must be signed 64-bit integers")
        if self.schema_version < 1 or self.catalog_version < 1:
            raise ValueError("artifact versions must be positive")
        return {
            "kind": self.kind,
            "id": redact_text(self.id),
            "schema_version": self.schema_version,
            "catalog_version": self.catalog_version,
            "catalog_hash": redact_text(self.catalog_hash),
        }


@dataclass(frozen=True)
class ReplayInput:
    command: str
    arguments: tuple[str, ...] = ()

    def to_dict(self) -> dict[str, Any]:
        if len(self.arguments) > MAX_REPLAY_ARGUMENTS:
            raise ValueError("replay arguments exceed protocol bounds")
        return {"command": redact_text(self.command), "arguments": redact_arguments(self.arguments)}


@dataclass(frozen=True)
class ResourceMetadata:
    elapsed_ms: int | None = None
    peak_bytes: int | None = None

    def to_dict(self) -> dict[str, int | None]:
        if any(value is not None and not _is_protocol_integer(value) for value in (self.elapsed_ms, self.peak_bytes)):
            raise ValueError("resource metadata must use signed 64-bit integers")
        if (self.elapsed_ms is not None and self.elapsed_ms < 0) or (self.peak_bytes is not None and self.peak_bytes < 0):
            raise ValueError("resource metadata cannot be negative")
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
        for code, exit_name, _ in EXIT_REASON_PRECEDENCE:
            if any(diagnostic.code == code for diagnostic in self.diagnostics):
                return ExitClass(exit_name)
        if self.status is Status.REFUTED:
            return ExitClass.POLICY_FAILURE
        if self.status is Status.ERROR:
            return ExitClass.INFRASTRUCTURE_FAILURE
        if self.status is Status.UNKNOWN or self.status is Status.CONDITIONAL:
            return ExitClass.UNKNOWN
        return ExitClass.SUCCESS

    def exit_code(self) -> int:
        return EXIT_CODES[self.exit_class().value]

    def validate(self) -> None:
        if not isinstance(self.status, Status):
            raise ValueError("result envelope status is invalid")
        if not _valid_text(self.operation) or not re.fullmatch(r"[a-z][a-z0-9._-]*", self.operation):
            raise ValueError("result envelope operation is invalid")
        if self.identity.workspace in PLACEHOLDER_IDENTITIES or self.identity.index in PLACEHOLDER_IDENTITIES:
            raise ValueError("result envelope identity is incomplete")
        if not _valid_text(self.identity.workspace) or not _valid_text(self.identity.index) or not self.identity.fact_sets or len(self.identity.fact_sets) > MAX_FACT_SETS or any(not _valid_text(item) for item in self.identity.fact_sets):
            raise ValueError("result envelope must identify fact sets")
        if any(value is not None and not _valid_text(value) for value in (self.identity.source_revision, self.identity.source_fingerprint)):
            raise ValueError("result envelope source identity is invalid")
        if self.identity.freshness not in FRESHNESS:
            raise ValueError("result envelope freshness is invalid")
        if not _valid_text(self.identity.freshness) or not _valid_text(self.producer.package) or not _valid_text(self.producer.version) or not _valid_text(self.producer.backend) or not self.producer.package or not self.producer.version or not self.producer.backend:
            raise ValueError("result envelope producer is incomplete")
        if not _is_protocol_integer(self.producer.schema_version) or self.producer.schema_version < 1:
            raise ValueError("producer schema version must be positive")
        if self.completeness.state not in COMPLETENESS_STATES or not _valid_text(self.completeness.state):
            raise ValueError("result envelope completeness state is invalid")
        if self.completeness.stale != (self.identity.freshness == "stale"):
            raise ValueError("stale completeness must match index freshness")
        if self.completeness.budget is not None and not _is_protocol_integer(self.completeness.budget):
            raise ValueError("completeness budget must be a signed 64-bit integer")
        if self.completeness.budget is not None and self.completeness.budget < 0:
            raise ValueError("completeness budget cannot be negative")
        expected_state = {Status.COMPLETE: "complete", Status.PARTIAL: "partial"}.get(self.status)
        if expected_state is not None and self.completeness.state != expected_state:
            raise ValueError("status and completeness state disagree")
        if self.completeness.truncated and self.status is not Status.PARTIAL:
            raise ValueError("truncated results must be partial")
        if self.status is Status.COMPLETE and (
            self.completeness.state != "complete"
            or self.completeness.truncated
            or self.completeness.stale
            or self.identity.freshness != "current"
        ):
            raise ValueError("complete results require current, complete freshness")
        if self.status in {Status.UNKNOWN, Status.CONDITIONAL, Status.REFUTED, Status.ERROR} and self.completeness.state != "unknown":
            raise ValueError("non-complete statuses require unknown completeness")
        if self.status is Status.PARTIAL and self.completeness.state != "partial":
            raise ValueError("partial results require partial completeness")
        if self.identity.freshness == "stale" and self.status is not Status.UNKNOWN:
            raise ValueError("stale indexes must be unknown")
        if self.status in {Status.UNKNOWN, Status.CONDITIONAL, Status.REFUTED, Status.ERROR} and not self.diagnostics:
            raise ValueError("non-complete outcomes require a stable diagnostic")
        codes = {diagnostic.code for diagnostic in self.diagnostics}
        if any(not _valid_text(value) for diagnostic in self.diagnostics for value in (diagnostic.code, diagnostic.severity, diagnostic.message, diagnostic.next_action) if value is not None):
            raise ValueError("diagnostic text exceeds protocol bounds")
        if "stale_input" in codes and (self.status is not Status.UNKNOWN or self.identity.freshness != "stale"):
            raise ValueError("stale_input requires an unknown result over a stale index")
        if codes.intersection({"backend_error", "timeout"}) and self.status is not Status.ERROR:
            raise ValueError("backend failures require an error result")
        if "policy_refuted" in codes and self.status is not Status.REFUTED:
            raise ValueError("policy_refuted requires a refuted result")
        if self.status is Status.UNKNOWN and not codes.intersection({"stale_input", "unknown", "missing_evidence"}):
            raise ValueError("unknown results require a stable unknown reason")
        if self.status is Status.CONDITIONAL and not codes.intersection({"unknown", "missing_evidence"}):
            raise ValueError("conditional results require an uncertainty reason")
        if self.status is Status.REFUTED and "policy_refuted" not in codes:
            raise ValueError("refuted outcomes require policy_refuted")
        if self.status is Status.ERROR and not codes.intersection({code for code, _, _ in EXIT_REASON_PRECEDENCE}):
            raise ValueError("error outcomes require a stable diagnostic")
        status_rule = DIAGNOSTIC_STATUS_RULES[self.status.value]
        if status_rule["required_any"] and not codes.intersection(status_rule["required_any"]):
            raise ValueError("status requires a matching stable diagnostic")
        if codes.intersection(status_rule["forbidden"]):
            raise ValueError("diagnostic reason is incompatible with result status")
        if any(d.code not in DIAGNOSTIC_CODES or d.severity not in DIAGNOSTIC_SEVERITIES for d in self.diagnostics):
            raise ValueError("diagnostic domain is invalid")
        if len(self.diagnostics) > MAX_DIAGNOSTICS or len(self.evidence) > MAX_EVIDENCE or len(self.artifacts) > MAX_ARTIFACTS:
            raise ValueError("result envelope collection exceeds protocol bounds")
        if len(self.identity.fact_sets) != len(set(self.identity.fact_sets)):
            raise ValueError("fact sets must be unique")
        if self.replay is not None and len(self.replay.arguments) > MAX_REPLAY_ARGUMENTS:
            raise ValueError("replay arguments exceed protocol bounds")
        _sanitize_value(self.result)
        count = [0]
        for node in self.evidence:
            if any(not _valid_text(value) for value in (node.id, node.evidence_class, node.trust, node.summary, node.source) if value is not None):
                raise ValueError("evidence text exceeds protocol bounds")
            node.to_dict(count=count)
        for artifact in self.artifacts:
            if any(not _valid_text(value) for value in (artifact.kind, artifact.id, artifact.catalog_hash)):
                raise ValueError("artifact text exceeds protocol bounds")
            artifact.to_dict()
        if self.replay is not None:
            if not _valid_text(self.replay.command) or any(not _valid_text(argument) for argument in self.replay.arguments):
                raise ValueError("replay text exceeds protocol bounds")
            self.replay.to_dict()
        if self.resources is not None:
            self.resources.to_dict()

    def to_dict(self) -> dict[str, Any]:
        self.validate()
        out: dict[str, Any] = {
            "protocol": PROTOCOL,
                "operation": redact_text(self.operation),
            "status": self.status.value,
            "exit_class": self.exit_class().value,
            "exit_code": self.exit_code(),
            "result": _sanitize_value(self.result),
            "identity": {
                "workspace": redact_text(self.identity.workspace),
                "index": redact_text(self.identity.index),
                "fact_sets": [redact_text(item) for item in self.identity.fact_sets],
                "freshness": redact_text(self.identity.freshness),
                "source_revision": redact_text(self.identity.source_revision) if self.identity.source_revision is not None else None,
                "source_fingerprint": redact_text(self.identity.source_fingerprint) if self.identity.source_fingerprint is not None else None,
            },
            "producer": {
                "package": redact_text(self.producer.package),
                "version": redact_text(self.producer.version),
                "backend": redact_text(self.producer.backend),
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

    def error_status_dict(self) -> dict[str, Any]:
        self.validate()
        if self.status is not Status.ERROR or not self.diagnostics:
            raise ValueError("error status requires an error envelope")
        diagnostic = self.diagnostics[0]
        return {
            "status": "error",
            "code": diagnostic.code,
            "message": redact_text(diagnostic.message),
            "exit_class": self.exit_class().value,
            "exit_code": self.exit_code(),
        }

    def human_text(self) -> str:
        out = f"status: {self.status.value}"
        if self.completeness.truncated:
            out += " (truncated)"
        if self.completeness.stale:
            out += " (stale index)"
        if self.diagnostics:
            out += f"\nreason: {redact_text(self.diagnostics[0].message)}"
            if self.diagnostics[0].next_action:
                out += f"\nnext: {redact_text(self.diagnostics[0].next_action)}"
        return redact_text(out, MAX_HUMAN_OUTPUT_BYTES)


def from_query_result(result: Any, index: Any, *, operation: str = "query") -> ResultEnvelope:
    """Adapt a QueryPlan Result without allowing renderers to reclassify it."""
    stale = index.freshness == "stale"
    status = Status.UNKNOWN if stale else Status.PARTIAL if result.truncated else Status.COMPLETE if index.freshness == "current" else Status.UNKNOWN
    state = "unknown" if status is Status.UNKNOWN else "partial" if result.truncated else "complete"
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
            workspace=index.workspace,
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
    elif index.freshness != "current" and not result.truncated:
        envelope.diagnostics.append(Diagnostic(
            "unknown", "warning", "index freshness could not be verified",
            "stamp or re-index the workspace before relying on this result",
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
        if not _is_protocol_integer(self.sequence) or self.sequence < 0 or self.event not in EVENT_KINDS or not re.fullmatch(r"[a-z][a-z0-9._-]*", self.operation) or len(self.operation.encode("utf-8")) > MAX_TEXT_BYTES:
            raise ValueError("event metadata is invalid")
        if any(value is not None and not _is_protocol_integer(value) for value in (self.completed, self.total)):
            raise ValueError("event progress must use signed 64-bit integers")
        if (self.completed is not None and self.completed < 0) or (self.total is not None and self.total < 0):
            raise ValueError("event progress cannot be negative")
        out: dict[str, Any] = {
            "protocol": EVENT_PROTOCOL,
            "sequence": self.sequence,
            "operation": redact_text(self.operation),
            "event": self.event,
            "message": redact_text(self.message),
        }
        if self.completed is not None:
            out["completed"] = self.completed
        if self.total is not None:
            out["total"] = self.total
        return out
