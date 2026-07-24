from __future__ import annotations

import json
from pathlib import Path

import pytest

from indexer.extensions import (
    CompositeRegistry,
    ConformanceSDK,
    Lockfile,
    PackageDependency,
    PackageError,
    PackageRegistry,
    PackageResolutionError,
    PackageResolver,
    ResultProvenance,
    artifact_identity,
    package_content_hash,
    validate_package,
)


def _package(root: Path, name: str, version: str, *, kind: str = "cidx.extract",
             dependencies: list[dict] | None = None, namespaces: list[str] | None = None,
             budgets: dict | None = None, capabilities: list[str] | None = None,
             compatibility: dict | None = None) -> Path:
    path = root / name.replace("/", "_") / version
    path.mkdir(parents=True)
    entry = "extract.json" if kind == "cidx.extract" else "analysis.dl"
    (path / entry).write_text(json.dumps({"kind": kind, "name": name}, sort_keys=True) + "\n")
    manifest = {
        "format": "cidx.package/v1",
        "name": name,
        "version": version,
        "kind": kind,
        "content_hash": "",
        "publisher": "test",
        "origin": "file:test",
        "license": "Apache-2.0",
        "entry_points": {"extract/main" if kind == "cidx.extract" else "analysis/main": entry},
        "namespaces": namespaces or [name],
        "compatibility": compatibility or {"catalog_version": 1},
        "dependencies": dependencies or [],
        "schemas": [],
        "budgets": budgets or {"steps": 100},
        "sandbox_profile": "declarative-default",
        "trust": "unverified",
        "evidence": "extension-analysis",
        "capabilities": capabilities or [],
        "metadata": {},
    }
    manifest_path = path / "package.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    manifest["content_hash"] = package_content_hash(path)
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return path


def _registry(tmp_path: Path, *packages: Path) -> CompositeRegistry:
    registry = PackageRegistry(tmp_path / "registry")
    for package in packages:
        registry.register(package)
    return CompositeRegistry([registry])


def test_lockfile_is_deterministic_and_verifies_offline(tmp_path: Path):
    dependency = _package(tmp_path, "banking.extract", "1.0.0")
    analysis = _package(
        tmp_path, "banking.analysis", "1.0.0", kind="cidx.analysis",
        dependencies=[{"name": "banking.extract", "version": "^1.0"}],
    )
    registry = _registry(tmp_path, dependency, analysis)
    resolver = PackageResolver(registry, environment={"catalog_version": 1})
    first = resolver.resolve([PackageDependency("banking.analysis", "1.x")])
    second = resolver.resolve([PackageDependency("banking.analysis", "1.x")])
    assert first.lock_hash == second.lock_hash
    assert [item.name for item in first.packages] == ["banking.analysis", "banking.extract"]
    lock_path = tmp_path / "cidx.lock.json"
    first.write(lock_path)
    loaded = Lockfile.read(lock_path)
    assert resolver.verify_lock(loaded, offline=True).keys() == {"banking.analysis", "banking.extract"}


def test_resolution_fails_closed_for_hash_namespace_cycle_and_compatibility(tmp_path: Path):
    one = _package(tmp_path, "one", "1.0.0", dependencies=[{"name": "two", "version": "*"}])
    two = _package(tmp_path, "two", "1.0.0", dependencies=[{"name": "one", "version": "*"}])
    registry = _registry(tmp_path, one, two)
    with pytest.raises(PackageResolutionError, match="cycle"):
        PackageResolver(registry).resolve(["one"])

    left = _package(tmp_path, "left", "1.0.0", namespaces=["shared"])
    right = _package(tmp_path, "right", "1.0.0", namespaces=["shared"])
    collision_registry = _registry(tmp_path, left, right)
    with pytest.raises(PackageResolutionError, match="collision"):
        PackageResolver(collision_registry).resolve(["left", "right"])

    incompatible = _package(tmp_path, "clang-only", "1.0.0", compatibility={"clang": ">=22"})
    with pytest.raises(PackageError, match="incompatible clang"):
        PackageResolver(_registry(tmp_path, incompatible), environment={"clang": "21.0.0"}).resolve(["clang-only"])

    drift = tmp_path / "drift" / "1.0.0"
    _package(tmp_path, "drift", "1.0.0")
    drift = tmp_path / "drift" / "1.0.0"
    (drift / "extract.json").write_text("changed\n")
    with pytest.raises(PackageError, match="hash drift"):
        validate_package(drift)


def test_execution_boundaries_and_invalidation_identity(tmp_path: Path):
    with pytest.raises(PackageError, match="forbidden executable"):
        _package(tmp_path, "unsafe", "1.0.0", capabilities=["sql"])

    query = _package(tmp_path, "query", "1.0.0", kind="cidx.query")
    query_manifest = json.loads((query / "package.json").read_text())
    query_manifest["entry_points"] = {"sql/main": "extract.json"}
    query_manifest["entry_points"] = {"sql/main": "analysis.dl"}
    (query / "package.json").write_text(json.dumps(query_manifest, indent=2, sort_keys=True) + "\n")
    query_manifest["content_hash"] = package_content_hash(query)
    (query / "package.json").write_text(json.dumps(query_manifest, indent=2, sort_keys=True) + "\n")
    with pytest.raises(PackageError, match="macro"):
        validate_package(query)

    package = _package(tmp_path, "identity", "1.0.0")
    lock = PackageResolver(_registry(tmp_path, package)).resolve(["identity"])
    first = artifact_identity(lock, artifact_kind="extension-facts", entry_point="extract/main", input_identity="tu:a")
    second = artifact_identity(lock, artifact_kind="extension-facts", entry_point="extract/main", input_identity="tu:b")
    assert first != second
    provenance = ResultProvenance("identity", "1.0.0", package_content_hash(package), "extract/main",
                                  ("facts:source",), "declarative-default", {"language": "c++"}, lock.lock_hash)
    assert provenance.as_dict()["package"]["hash"].startswith("sha256:")


def test_conformance_sdk_covers_positive_negative_and_budget_cases(tmp_path: Path):
    valid = _package(tmp_path, "valid", "1.0.0")
    too_large = _package(tmp_path, "too-large", "1.0.0", budgets={"steps": 1000})
    sdk = ConformanceSDK(PackageResolver(_registry(tmp_path, valid, too_large)))
    results = sdk.run([
        {"id": "positive", "package": str(valid), "expected": "valid"},
        {"id": "budget", "package": str(too_large), "expected": "invalid", "checks": {"max_budget": 100}},
        {"id": "missing-dependency", "package": str(valid), "expected": "invalid",
         "checks": {"required_dependency": "not-materialized"}},
    ])
    assert [result["status"] for result in results] == ["passed", "passed", "passed"]
