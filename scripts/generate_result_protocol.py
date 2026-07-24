#!/usr/bin/env python3
"""Generate the C++/Python result protocol and its schemas/goldens."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "spec/contracts/result-protocol.json"


def enum_name(value: str) -> str:
    return "".join(part.capitalize() for part in value.replace("-", "_").split("_"))


def load() -> dict:
    data = json.loads(SOURCE.read_text(encoding="utf-8"))
    if data.get("format") != "cidx.result-protocol/v1":
        raise ValueError("unsupported result protocol source")
    for key in ("statuses", "exit_classes", "freshness", "completeness_states", "diagnostic_severities", "evidence_classes", "trust_levels", "event_kinds", "diagnostic_codes", "artifact_kinds", "placeholder_identities", "diagnostic_invariants", "diagnostic_status_invariants", "acceptance_vectors", "adversarial_vectors"):
        if not data.get(key):
            raise ValueError(f"protocol source requires {key}")
    status_set = set(data["statuses"])
    freshness_set = set(data["freshness"])
    state_set = set(data["completeness_states"])
    diagnostic_set = set(data["diagnostic_codes"])
    if len(data["placeholder_identities"]) != len(set(data["placeholder_identities"])):
        raise ValueError("placeholder identities must be unique")
    if data["numeric"]["min_integer"] >= data["numeric"]["max_integer"]:
        raise ValueError("numeric integer bounds are invalid")
    for invariant in data["diagnostic_invariants"]:
        if not set(invariant["codes"]).issubset(diagnostic_set) or not set(invariant["status"]).issubset(status_set):
            raise ValueError("diagnostic invariant contains an unknown domain value")
        if not set(invariant.get("freshness", freshness_set)).issubset(freshness_set):
            raise ValueError("diagnostic invariant contains an unknown freshness")
    seen_statuses = set()
    for invariant in data["diagnostic_status_invariants"]:
        status = invariant["status"]
        if status not in status_set or status in seen_statuses:
            raise ValueError("diagnostic status invariants must cover each status once")
        seen_statuses.add(status)
        for key in ("required_any", "forbidden"):
            if not set(invariant.get(key, [])).issubset(diagnostic_set):
                raise ValueError("diagnostic status invariant contains an unknown code")
        if set(invariant.get("required_any", [])) & set(invariant.get("forbidden", [])):
            raise ValueError("diagnostic status invariant requires and forbids the same code")
    if seen_statuses != status_set:
        raise ValueError("diagnostic status invariants must cover every status")
    exit_set = {item["name"] for item in data["exit_classes"]}
    exit_codes = {item["name"]: item["code"] for item in data["exit_classes"]}
    for vector in data["acceptance_vectors"]:
        if vector["status"] not in status_set or vector["freshness"] not in freshness_set or vector["state"] not in state_set:
            raise ValueError(f"invalid acceptance vector {vector['name']}")
        if vector["diagnostic"] is not None and vector["diagnostic"] not in diagnostic_set:
            raise ValueError(f"invalid diagnostic in {vector['name']}")
        if vector["exit_class"] not in exit_set or not isinstance(vector["exit_code"], int):
            raise ValueError(f"invalid exit expectation in {vector['name']}")
        if vector["exit_code"] != exit_codes[vector["exit_class"]]:
            raise ValueError(f"exit expectation does not match its class in {vector['name']}")
    return data


def render_cpp(data: dict) -> str:
    limits = data["limits"]
    statuses = data["statuses"]
    exits = data["exit_classes"]
    vectors = data["acceptance_vectors"]
    rules = data["exit_reason_precedence"]
    status_enum = ",\n  ".join(enum_name(x) for x in statuses)
    exit_enum = ",\n  ".join(enum_name(x["name"]) for x in exits)
    status_names = ", ".join(json.dumps(x) for x in statuses)
    exit_names = ", ".join(json.dumps(x["name"]) for x in exits)
    exit_codes = ", ".join(str(x["code"]) for x in exits)
    domain_arrays = "\n".join(
        f'inline constexpr std::array<std::string_view, {len(data[key])}> k{enum_name(key)} = '
        + "{{" + ", ".join(json.dumps(x) for x in data[key]) + "}};"
        for key in ("freshness", "completeness_states", "diagnostic_severities", "evidence_classes", "trust_levels", "event_kinds", "diagnostic_codes", "artifact_kinds")
    )
    rule_rows = "\n".join(
        f'  {{.code = "{r["code"]}", .exit_class = ExitClass::{enum_name(r["class"])}, .exit_code = {r["exit_code"]}}},'
        for r in rules
    )
    vector_rows = "\n".join(
        f'  {{.name = "{v["name"]}", .operation = "{v["operation"]}", .status = Status::{enum_name(v["status"])}, .completeness_state = "{v["state"]}", .freshness = "{v["freshness"]}", .diagnostic = {json.dumps(v["diagnostic"] or "")}, .exit_class = ExitClass::{enum_name(v["exit_class"])}, .exit_code = {v["exit_code"]}}},'
        for v in vectors
    )
    status_rule_rows = "\n".join(
        f'  {{.status = Status::{enum_name(rule["status"])}, .required_any = {json.dumps("|".join(rule.get("required_any", [])))}, .forbidden = {json.dumps("|".join(rule.get("forbidden", [])))}}},'
        for rule in data["diagnostic_status_invariants"]
    )
    status_rule_type = "struct DiagnosticStatusRule { Status status; std::string_view required_any; std::string_view forbidden; };"
    limit_lines = "\n".join(f"inline constexpr std::size_t k{enum_name(k)} = {v};" for k, v in limits.items())
    numeric_lines = "\n".join(
        f"inline constexpr long long k{enum_name(key)} = "
        f"{('-9223372036854775807LL - 1' if key == 'min_integer' else '9223372036854775807LL')};"
        for key in ("min_integer", "max_integer")
    )
    return f'''// Generated by scripts/generate_result_protocol.py; DO NOT EDIT.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cidx::protocol::generated {{
inline constexpr int kProtocolVersion = {data["protocol_version"]};
inline constexpr std::string_view kProtocol = "cidx.result/v1";
inline constexpr std::string_view kEventProtocol = "cidx.event/v1";
{limit_lines}
{numeric_lines}
enum class Status : std::uint8_t {{
  {status_enum}
}};
enum class ExitClass : std::uint8_t {{
  {exit_enum}
}};
struct ExitRule {{ std::string_view code; ExitClass exit_class; int exit_code; }};
{status_rule_type}
struct AcceptanceVector {{ std::string_view name; std::string_view operation; Status status; std::string_view completeness_state; std::string_view freshness; std::string_view diagnostic; ExitClass exit_class; int exit_code; }};
inline constexpr std::array<std::string_view, {len(statuses)}> kStatusNames = {{{{{status_names}}}}};
inline constexpr std::array<std::string_view, {len(exits)}> kExitClassNames = {{{{{exit_names}}}}};
inline constexpr std::array<int, {len(exits)}> kExitCodes = {{{{{exit_codes}}}}};
{domain_arrays}
inline constexpr std::array<std::string_view, {len(data["placeholder_identities"])}> kPlaceholderIdentities = {{{{{", ".join(json.dumps(x) for x in data["placeholder_identities"])}}}}};
inline constexpr std::size_t kOversizedAsciiBytes = {data["adversarial_vectors"]["oversized_ascii_bytes"]};
inline constexpr std::size_t kOversizedMultibyteChars = {data["adversarial_vectors"]["oversized_multibyte_chars"]};
inline constexpr std::array<DiagnosticStatusRule, {len(data["diagnostic_status_invariants"])}> kDiagnosticStatusRules = {{{{
{status_rule_rows}
}}}};
inline constexpr std::array<ExitRule, {len(rules)}> kExitReasonPrecedence = {{{{
{rule_rows}
}}}};
inline constexpr std::array<AcceptanceVector, {len(vectors)}> kAcceptanceVectors = {{{{
{vector_rows}
}}}};
}} // namespace cidx::protocol::generated
'''


def render_python(data: dict) -> str:
    limits = data["limits"]
    statuses = data["statuses"]
    exits = data["exit_classes"]
    vector_repr = repr(tuple({"name": v["name"], "operation": v["operation"], "status": v["status"], "state": v["state"], "freshness": v["freshness"], "diagnostic": v["diagnostic"], "exit_class": v["exit_class"], "exit_code": v["exit_code"]} for v in data["acceptance_vectors"]))
    exit_repr = repr({e["name"]: e["code"] for e in exits})
    rules_repr = repr(tuple((r["code"], r["class"], r["exit_code"]) for r in data["exit_reason_precedence"]))
    status_rules_repr = repr({rule["status"]: {"required_any": tuple(rule.get("required_any", [])), "forbidden": tuple(rule.get("forbidden", []))} for rule in data["diagnostic_status_invariants"]})
    enums = "\n".join(f"    {enum_name(v).upper()} = {v!r}" for v in statuses)
    exit_enums = "\n".join(f"    {v['name'].upper()} = {v['name']!r}" for v in exits)
    limit_lines = "\n".join(f"{k.upper()} = {v}" for k, v in limits.items())
    numeric_lines = "\n".join(f"{key.upper()} = {value}" for key, value in (("min_integer", data["numeric"]["min_integer"]), ("max_integer", data["numeric"]["max_integer"])))
    domain_lines = "\n".join(f"{key.upper()} = {tuple(data[key])!r}" for key in ("freshness", "completeness_states", "diagnostic_severities", "evidence_classes", "trust_levels", "event_kinds", "diagnostic_codes", "artifact_kinds"))
    return f'''# Generated by scripts/generate_result_protocol.py; DO NOT EDIT.
from enum import StrEnum

PROTOCOL_VERSION = {data["protocol_version"]}
PROTOCOL = 'cidx.result/v1'
EVENT_PROTOCOL = 'cidx.event/v1'
{limit_lines}
{numeric_lines}
{domain_lines}

class Status(StrEnum):
{enums}

class ExitClass(StrEnum):
{exit_enums}

EXIT_CODES = {exit_repr}
EXIT_REASON_PRECEDENCE = {rules_repr}
PLACEHOLDER_IDENTITIES = {tuple(data["placeholder_identities"])!r}
OVERSIZED_ASCII_BYTES = {data["adversarial_vectors"]["oversized_ascii_bytes"]}
OVERSIZED_MULTIBYTE_CHARS = {data["adversarial_vectors"]["oversized_multibyte_chars"]}
DIAGNOSTIC_STATUS_RULES = {status_rules_repr}
ACCEPTANCE_VECTORS = {vector_repr}
'''


def bounded_schema(data: dict) -> dict:
    limits = data["limits"]
    numeric = data["numeric"]
    text = {"type": "string", "x-maxUtf8Bytes": limits["max_text_bytes"]}
    defs: dict[str, dict] = {}
    primitive = [text, {"type": "integer", "minimum": numeric["min_integer"], "maximum": numeric["max_integer"]}, {"type": "boolean"}, {"type": "null"}]
    for depth in range(limits["max_result_depth"] + 1):
        choices = list(primitive)
        if depth < limits["max_result_depth"]:
            choices.extend([
                {"type": "array", "maxItems": limits["max_result_items"], "items": {"$ref": f"#/$defs/value{depth + 1}"}},
                {"type": "object", "maxProperties": limits["max_result_properties"], "additionalProperties": {"$ref": f"#/$defs/value{depth + 1}"}},
            ])
        defs[f"value{depth}"] = {"anyOf": choices}
    return defs


def render_schema(data: dict) -> dict:
    limits = data["limits"]
    numeric = data["numeric"]
    text = {"type": "string", "x-maxUtf8Bytes": limits["max_text_bytes"]}
    identity_text = {
        **text,
        "minLength": 1,
        "not": {"enum": data["placeholder_identities"]},
    }
    schema = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$defs": bounded_schema(data),
        "title": "CIDX result envelope v1",
        "type": "object",
        "required": ["protocol", "operation", "status", "exit_class", "exit_code", "result", "identity", "producer", "completeness", "diagnostics", "evidence", "artifacts"],
        "additionalProperties": False,
        "properties": {
            "protocol": {"const": "cidx.result/v1"}, "operation": {**text, "pattern": "^[a-z][a-z0-9._-]*$"},
            "status": {"enum": data["statuses"]}, "exit_class": {"enum": [x["name"] for x in data["exit_classes"]]}, "exit_code": {"type": "integer", "minimum": 0, "maximum": 6},
            "result": {"type": "object", "maxProperties": limits["max_result_properties"], "additionalProperties": {"$ref": "#/$defs/value0"}},
            "identity": {"type": "object", "required": ["workspace", "index", "fact_sets", "freshness", "source_revision", "source_fingerprint"], "additionalProperties": False, "properties": {"workspace": identity_text, "index": identity_text, "fact_sets": {"type": "array", "maxItems": limits["max_fact_sets"], "items": text}, "freshness": {"enum": data["freshness"]}, "source_revision": {"anyOf": [text, {"type": "null"}]}, "source_fingerprint": {"anyOf": [text, {"type": "null"}]}}},
            "producer": {"type": "object", "required": ["package", "version", "backend", "schema_version"], "additionalProperties": False, "properties": {"package": text, "version": text, "backend": text, "schema_version": {"type": "integer", "minimum": 1, "maximum": numeric["max_integer"]}}},
            "completeness": {"type": "object", "required": ["state", "truncated", "stale", "budget"], "additionalProperties": False, "properties": {"state": {"enum": data["completeness_states"]}, "truncated": {"type": "boolean"}, "stale": {"type": "boolean"}, "budget": {"anyOf": [{"type": "integer", "minimum": 0, "maximum": numeric["max_integer"]}, {"type": "null"}]}}},
            "diagnostics": {"type": "array", "maxItems": limits["max_diagnostics"], "items": {"type": "object", "required": ["code", "severity", "message"], "additionalProperties": False, "properties": {"code": {"enum": data["diagnostic_codes"]}, "severity": {"enum": data["diagnostic_severities"]}, "message": text, "next_action": {"anyOf": [text, {"type": "null"}]}}}},
            "evidence": {"type": "array", "maxItems": limits["max_evidence"], "items": {"$ref": "#/$defs/evidence0"}},
            "artifacts": {"type": "array", "maxItems": limits["max_artifacts"], "items": {"type": "object", "required": ["kind", "id", "schema_version", "catalog_version", "catalog_hash"], "additionalProperties": False, "properties": {"kind": {"enum": data["artifact_kinds"]}, "id": text, "schema_version": {"type": "integer", "minimum": 1, "maximum": numeric["max_integer"]}, "catalog_version": {"type": "integer", "minimum": 1, "maximum": numeric["max_integer"]}, "catalog_hash": text}}},
            "replay": {"type": "object", "required": ["command", "arguments"], "additionalProperties": False, "properties": {"command": text, "arguments": {"type": "array", "maxItems": limits["max_replay_arguments"], "items": text}}},
            "resources": {"type": "object", "required": ["elapsed_ms", "peak_bytes"], "additionalProperties": False, "properties": {"elapsed_ms": {"anyOf": [{"type": "integer", "minimum": 0, "maximum": numeric["max_integer"]}, {"type": "null"}]}, "peak_bytes": {"anyOf": [{"type": "integer", "minimum": 0, "maximum": numeric["max_integer"]}, {"type": "null"}]}}},
        },
    }
    for invariant in data["diagnostic_invariants"]:
        code_condition = {"anyOf": [{"contains": {"type": "object", "required": ["code"], "properties": {"code": {"const": code}}}} for code in invariant["codes"]]}
        then_properties = {"status": {"enum": invariant["status"]}}
        if "freshness" in invariant:
            then_properties["identity"] = {"properties": {"freshness": {"enum": invariant["freshness"]}}}
        schema.setdefault("allOf", []).append({"if": {"properties": {"diagnostics": code_condition}}, "then": {"properties": then_properties}})
    status_conditions = {
        "complete": {"completeness": {"properties": {"state": {"const": "complete"}, "truncated": {"const": False}, "stale": {"const": False}}}, "identity": {"properties": {"freshness": {"const": "current"}}}},
        "partial": {"completeness": {"properties": {"state": {"const": "partial"}}}},
        "unknown": {"completeness": {"properties": {"state": {"const": "unknown"}}}},
        "conditional": {"completeness": {"properties": {"state": {"const": "unknown"}}}},
        "refuted": {"completeness": {"properties": {"state": {"const": "unknown"}}}},
        "error": {"completeness": {"properties": {"state": {"const": "unknown"}}}},
    }
    for status, properties in status_conditions.items():
        schema.setdefault("allOf", []).append({"if": {"properties": {"status": {"const": status}}}, "then": {"properties": properties}})
    for rule in data["diagnostic_status_invariants"]:
        then_all_of = []
        if rule.get("required_any"):
            then_all_of.append({"properties": {"diagnostics": {"contains": {"type": "object", "required": ["code"], "properties": {"code": {"enum": rule["required_any"]}}}}}})
        if rule.get("forbidden"):
            then_all_of.append({"properties": {"diagnostics": {"not": {"contains": {"type": "object", "required": ["code"], "properties": {"code": {"enum": rule["forbidden"]}}}}}}})
        schema.setdefault("allOf", []).append({"if": {"properties": {"status": {"const": rule["status"]}}}, "then": {"allOf": then_all_of}})
    schema.setdefault("allOf", []).append({"if": {"properties": {"completeness": {"properties": {"truncated": {"const": True}}}}}, "then": {"properties": {"status": {"const": "partial"}}}})
    schema.setdefault("allOf", []).append({"if": {"properties": {"identity": {"properties": {"freshness": {"const": "stale"}}}}}, "then": {"properties": {"status": {"const": "unknown"}}}})
    for depth in range(limits["max_evidence_depth"] + 1):
        properties = {"id": text, "class": {"enum": data["evidence_classes"]}, "trust": {"enum": data["trust_levels"]}, "summary": text, "source": {"anyOf": [text, {"type": "null"}]}}
        if depth < limits["max_evidence_depth"]:
            properties["children"] = {"type": "array", "maxItems": limits["max_evidence"], "items": {"$ref": f"#/$defs/evidence{depth + 1}"}}
        schema["$defs"][f"evidence{depth}"] = {"type": "object", "required": ["id", "class", "trust", "summary"], "additionalProperties": False, "properties": properties}
    return schema


def render_event_schema(data: dict) -> dict:
    limits = data["limits"]
    maximum = data["numeric"]["max_integer"]
    text = {"type": "string", "x-maxUtf8Bytes": limits["max_text_bytes"]}
    return {"$schema": "https://json-schema.org/draft/2020-12/schema", "title": "CIDX JSONL event v1", "type": "object", "required": ["protocol", "sequence", "operation", "event", "message"], "additionalProperties": False, "properties": {"protocol": {"const": "cidx.event/v1"}, "sequence": {"type": "integer", "minimum": 0, "maximum": maximum}, "operation": text, "event": {"enum": data["event_kinds"]}, "message": text, "completed": {"type": "integer", "minimum": 0, "maximum": maximum}, "total": {"type": "integer", "minimum": 0, "maximum": maximum}}}


def render_error_schema(data: dict) -> dict:
    return {"$schema": "https://json-schema.org/draft/2020-12/schema", "title": "CIDX stable error reduction", "type": "object", "required": ["status", "code", "message", "exit_class", "exit_code"], "additionalProperties": False, "properties": {"status": {"const": "error"}, "code": {"enum": data["diagnostic_codes"]}, "message": {"type": "string", "x-maxUtf8Bytes": data["limits"]["max_text_bytes"]}, "exit_class": {"enum": [x["name"] for x in data["exit_classes"] if x["name"] != "success"]}, "exit_code": {"type": "integer", "minimum": 1, "maximum": data["numeric"]["max_integer"]}}}


def write_or_check(outputs: dict[Path, str], check: bool) -> int:
    stale = [path for path, content in outputs.items() if not path.exists() or path.read_text(encoding="utf-8") != content]
    if check:
        for path in stale:
            print(f"stale generated protocol: {path.relative_to(ROOT)}", file=sys.stderr)
        return int(bool(stale))
    for path, content in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        data = load()
        goldens = data["goldens"]
        outputs = {
            ROOT / "src/query/generated_result_protocol.hpp": render_cpp(data),
            ROOT / "python/indexer/generated_result_protocol.py": render_python(data),
            ROOT / "spec/contracts/result-envelope.schema.json": json.dumps(render_schema(data), indent=2) + "\n",
            ROOT / "spec/contracts/event.schema.json": json.dumps(render_event_schema(data), indent=2) + "\n",
            ROOT / "spec/contracts/error-status.schema.json": json.dumps(render_error_schema(data), indent=2) + "\n",
            ROOT / "spec/contracts/golden/result-envelope.json": json.dumps(goldens["result-envelope"], indent=2) + "\n",
            ROOT / "spec/contracts/golden/event.json": json.dumps(goldens["event"], indent=2) + "\n",
            ROOT / "spec/contracts/golden/error-status.json": json.dumps(goldens["error-status"], indent=2) + "\n",
            ROOT / "spec/contracts/golden/protocol-vectors.json": json.dumps({"vectors": data["acceptance_vectors"]}, indent=2) + "\n",
        }
        return write_or_check(outputs, args.check)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"result protocol generation failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
