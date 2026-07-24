#!/usr/bin/env python3
"""Validate the machine-readable CIDX platform architecture contract."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "spec/platform/architecture.json"
REQUIRED_LAYERS = {"workspace", "frontend", "extraction", "model", "persistence", "derivation", "query", "proof", "product"}
REQUIRED_PORTS = {"WorkspaceSnapshot", "TranslationUnitDescriptor", "FrontendSessionFactory", "ExtractionPassRegistry", "FactEmitter", "EvidenceEmitter", "QueryPlan", "ApplicationContext"}
REQUIRED_ARTIFACT_FIELDS = {"identity", "source_revision", "configuration_identity", "producer", "producer_version", "schema_version", "catalog_version", "status", "completeness", "evidence", "freshness", "diagnostics", "budgets"}
REQUIRED_EXTENSIONS = {"cxq", "extraction-plan", "analysis-package"}
REQUIRED_TU_CONSUMERS = {"indexing", "astgraph", "diff", "include-validation", "proof-preparation"}


def _cycle(nodes: dict[str, list[str]]) -> list[str] | None:
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(node: str, path: list[str]) -> list[str] | None:
        if node in visiting:
            return path[path.index(node) :] + [node]
        if node in visited:
            return None
        visiting.add(node)
        for dependency in nodes[node]:
            found = visit(dependency, path + [dependency])
            if found:
                return found
        visiting.remove(node)
        visited.add(node)
        return None

    for node in nodes:
        found = visit(node, [node])
        if found:
            return found
    return None


def validate_contract(data: dict) -> list[str]:
    errors: list[str] = []
    if data.get("format_version") != 1:
        errors.append("format_version must be 1")
    if data.get("status") != "accepted":
        errors.append("status must be accepted")
    if not data.get("owner"):
        errors.append("owner must be non-empty")
    forbidden = data.get("forbidden_dependencies", {})
    for boundary in ("model", "public_artifacts", "query", "proof"):
        if not forbidden.get(boundary):
            errors.append(f"missing forbidden dependency boundary: {boundary}")
    missing_consumers = REQUIRED_TU_CONSUMERS - set(data.get("translation_unit_consumers", []))
    if missing_consumers:
        errors.append(f"missing translation-unit consumers: {sorted(missing_consumers)}")

    layers = data.get("layers", [])
    layer_ids = [layer.get("id") for layer in layers]
    if len(layer_ids) != len(set(layer_ids)):
        errors.append("layers must have unique ids")
    missing_layers = REQUIRED_LAYERS - set(layer_ids)
    if missing_layers:
        errors.append(f"missing layers: {sorted(missing_layers)}")
    layer_graph = {layer_id: [] for layer_id in layer_ids}
    for layer in layers:
        layer_id = layer.get("id")
        for dependency in layer.get("depends_on", []):
            if dependency not in layer_graph:
                errors.append(f"layer {layer_id} depends on unknown layer {dependency}")
            else:
                layer_graph[layer_id].append(dependency)
    cycle = _cycle(layer_graph) if not errors else None
    if cycle:
        errors.append(f"layer dependency cycle: {' -> '.join(cycle)}")

    ports = {port.get("id") for port in data.get("ports", [])}
    missing_ports = REQUIRED_PORTS - ports
    if missing_ports:
        errors.append(f"missing ports: {sorted(missing_ports)}")

    artifact = data.get("artifact_contract", {})
    missing_fields = REQUIRED_ARTIFACT_FIELDS - set(artifact.get("required_fields", []))
    if missing_fields:
        errors.append(f"missing artifact fields: {sorted(missing_fields)}")
    if artifact.get("default_storage") != "content-addressed-artifact":
        errors.append("custom artifacts must default to content-addressed-artifact")

    extensions = {extension.get("id") for extension in data.get("extension_surfaces", [])}
    missing_extensions = REQUIRED_EXTENSIONS - extensions
    if missing_extensions:
        errors.append(f"missing extension surfaces: {sorted(missing_extensions)}")
    if any(not extension.get("read_only") for extension in data.get("extension_surfaces", [])):
        errors.append("extension surfaces must be read-only")

    delivery_issues = {issue for milestone in data.get("delivery", []) for issue in milestone.get("issues", [])}
    expected_delivery = {f"HSE-{number}" for number in range(58, 74)}
    if delivery_issues != expected_delivery:
        errors.append("delivery must cover HSE-58 through HSE-73 exactly")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    args = parser.parse_args()
    try:
        data = json.loads(args.manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"platform contract check failed: {error}", file=sys.stderr)
        return 2
    errors = validate_contract(data)
    if errors:
        print("platform contract check failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"platform contract check passed: {args.manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
