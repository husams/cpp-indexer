#!/usr/bin/env python3
"""Validate the machine-readable CIDX platform architecture contract."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "spec/platform/architecture.json"
REQUIRED_LAYERS = {"foundation", "workspace", "frontend", "extraction", "model", "persistence", "derivation", "query", "proof", "product"}
PORT_OWNERS = {
    "WorkspaceSnapshot": "workspace",
    "TranslationUnitDescriptor": "workspace",
    "FrontendSessionFactory": "frontend",
    "ExtractionPassRegistry": "extraction",
    "FactEmitter": "model",
    "EvidenceEmitter": "model",
    "WorkspaceStore": "persistence",
    "SourceStore": "persistence",
    "SymbolStore": "persistence",
    "TypeStore": "persistence",
    "FactStore": "persistence",
    "EvidenceStore": "persistence",
    "AnalysisArtifactStore": "persistence",
    "ProofArtifactStore": "persistence",
    "TransformRegistry": "derivation",
    "AnalysisEngine": "derivation",
    "QueryPlan": "query",
    "ApplicationContext": "product",
}
REQUIRED_PORTS = set(PORT_OWNERS)
REQUIRED_ARTIFACT_FIELDS = {"identity", "source_revision", "configuration_identity", "producer", "producer_version", "schema_version", "catalog_version", "status", "completeness", "evidence", "freshness", "diagnostics", "budgets"}
REQUIRED_EXTENSIONS = {"cxq", "extraction-plan", "analysis-package"}
REQUIRED_TU_CONSUMERS = {"indexing", "astgraph", "diff", "include-validation", "proof-preparation"}
EXPECTED_EXTERNAL_OWNERSHIP = {
    "clang": {"frontend", "extraction"},
    "llvm": {"frontend", "extraction"},
    "sqlite": {"persistence"},
    "filesystem": {"workspace", "persistence"},
    "process": {"workspace", "derivation"},
    "cli": {"product"},
}
EXPECTED_FORBIDDEN_DEPENDENCIES = {
    "model": {"clang", "llvm", "sqlite", "cli", "filesystem", "process"},
    "public_artifacts": {"clang", "llvm", "sqlite", "cli", "filesystem", "process"},
    "query": {"clang", "llvm", "sqlite", "process"},
    "proof": {"clang", "llvm", "sqlite", "cli"},
}
REQUIRED_EXTENSION_BEHAVIORS = {
    "arbitrary native callbacks",
    "shared-library loading",
    "shell execution",
    "arbitrary SQL",
    "filesystem writes",
    "overwriting core facts",
}


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


def _validate_module_mapping(data: dict, module_manifest: dict, layer_graph: dict[str, list[str]]) -> list[str]:
    errors: list[str] = []
    module_ids = {module.get("id") for module in module_manifest.get("modules", [])}
    mapping = data.get("module_layer_mapping", {})
    if set(mapping) != module_ids:
        errors.append("module_layer_mapping must cover module manifest ids exactly")
    for module_id, layer in mapping.items():
        if layer not in layer_graph:
            errors.append(f"module {module_id} maps to unknown platform layer {layer}")
    for module in module_manifest.get("modules", []):
        module_id = module.get("id")
        source_layer = mapping.get(module_id)
        if source_layer not in layer_graph:
            continue
        for dependency in module.get("allowedDependencies", []):
            if dependency == module_id:
                continue
            destination_layer = mapping.get(dependency)
            if destination_layer is None:
                errors.append(f"module {module_id} depends on unmapped module {dependency}")
            elif destination_layer != source_layer and destination_layer not in layer_graph[source_layer]:
                errors.append(
                    f"platform graph rejects module dependency {module_id} -> {dependency} "
                    f"({source_layer} -> {destination_layer})"
                )
    return errors


def validate_contract(data: dict, module_manifest: dict | None = None) -> list[str]:
    errors: list[str] = []
    if data.get("format_version") != 1:
        errors.append("format_version must be 1")
    if data.get("status") != "accepted":
        errors.append("status must be accepted")
    if not data.get("owner"):
        errors.append("owner must be non-empty")
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
    cycle = _cycle(layer_graph) if layer_graph and not any("unknown layer" in error for error in errors) else None
    if cycle:
        errors.append(f"layer dependency cycle: {' -> '.join(cycle)}")

    port_entries = data.get("ports", [])
    port_ids = [port.get("id") for port in port_entries]
    if len(port_ids) != len(set(port_ids)):
        errors.append("ports must have unique ids")
    ports = set(port_ids)
    missing_ports = REQUIRED_PORTS - ports
    if missing_ports:
        errors.append(f"missing ports: {sorted(missing_ports)}")
    for port in port_entries:
        port_id = port.get("id")
        expected_owner = PORT_OWNERS.get(port_id)
        if expected_owner and port.get("owner") != expected_owner:
            errors.append(f"port {port_id} must be owned by {expected_owner}")
        elif port_id not in PORT_OWNERS:
            errors.append(f"port {port_id} has an unknown id")
        elif port.get("owner") not in layer_graph:
            errors.append(f"port {port_id} has an unknown owner layer {port.get('owner')}")

    artifact = data.get("artifact_contract", {})
    missing_fields = REQUIRED_ARTIFACT_FIELDS - set(artifact.get("required_fields", []))
    if missing_fields:
        errors.append(f"missing artifact fields: {sorted(missing_fields)}")
    if artifact.get("default_storage") != "content-addressed-artifact":
        errors.append("custom artifacts must default to content-addressed-artifact")

    forbidden = data.get("forbidden_dependencies", {})
    for boundary, expected in EXPECTED_FORBIDDEN_DEPENDENCIES.items():
        actual = set(forbidden.get(boundary, []))
        if actual != expected or len(forbidden.get(boundary, [])) != len(actual):
            errors.append(f"forbidden dependency boundary {boundary} does not match policy")

    external_ownership = data.get("external_ownership", {})
    if set(external_ownership) != set(EXPECTED_EXTERNAL_OWNERSHIP):
        errors.append("external_ownership must cover the declared external dependencies exactly")
    for dependency, expected in EXPECTED_EXTERNAL_OWNERSHIP.items():
        actual = set(external_ownership.get(dependency, []))
        if actual != expected or len(external_ownership.get(dependency, [])) != len(actual):
            errors.append(f"external ownership for {dependency} does not match policy")

    behaviors = data.get("forbidden_extension_behaviors", [])
    if set(behaviors) != REQUIRED_EXTENSION_BEHAVIORS or len(behaviors) != len(set(behaviors)):
        errors.append("forbidden_extension_behaviors does not match policy")

    extensions = {extension.get("id") for extension in data.get("extension_surfaces", [])}
    missing_extensions = REQUIRED_EXTENSIONS - extensions
    if missing_extensions:
        errors.append(f"missing extension surfaces: {sorted(missing_extensions)}")
    if len(extensions) != len(data.get("extension_surfaces", [])):
        errors.append("extension surfaces must have unique ids")
    if any(not extension.get("read_only") for extension in data.get("extension_surfaces", [])):
        errors.append("extension surfaces must be read-only")

    delivery_issues = {issue for milestone in data.get("delivery", []) for issue in milestone.get("issues", [])}
    expected_delivery = {f"HSE-{number}" for number in range(58, 74)}
    if delivery_issues != expected_delivery:
        errors.append("delivery must cover HSE-58 through HSE-73 exactly")
    ownership = data.get("ownership", {})
    for owner in ("cpp_core", "python_sdk", "generated_contracts", "parity_rule"):
        if not ownership.get(owner):
            errors.append(f"ownership.{owner} must be non-empty")
    if module_manifest is not None:
        errors.extend(_validate_module_mapping(data, module_manifest, layer_graph))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--module-manifest", type=Path, default=ROOT / "architecture/cidx-module-manifest.json")
    args = parser.parse_args()
    try:
        data = json.loads(args.manifest.read_text(encoding="utf-8"))
        module_manifest = json.loads(args.module_manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"platform contract check failed: {error}", file=sys.stderr)
        return 2
    errors = validate_contract(data, module_manifest)
    if errors:
        print("platform contract check failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"platform contract check passed: {args.manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
