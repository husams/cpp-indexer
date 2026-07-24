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
    PackageManifest,
    PackageRegistry,
    PackagePolicy,
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
    entry = "extract.json" if kind == "cidx.extract" else "analysis.json"
    descriptor = ({"kind": kind, "name": name, "fact_source": "facts"}
                  if kind == "cidx.extract" else
                  {"kind": kind, "name": name, "result_source": "results"})
    (path / entry).write_text(json.dumps(descriptor, sort_keys=True) + "\n")
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


def _refresh_hash(path: Path) -> None:
    manifest_path = path / "package.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["content_hash"] = package_content_hash(path)
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


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
    query_manifest["entry_points"] = {"sql/main": "analysis.json"}
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
    (tmp_path / "fixture.json").write_text(json.dumps({"facts": [{"name": "valid"}]}))
    sdk = ConformanceSDK(PackageResolver(_registry(tmp_path, valid, too_large)))
    results = sdk.run([
        {"id": "positive", "package": str(valid), "expected": "valid",
         "fixture": str(tmp_path / "fixture.json"),
         "expected_facts": [{"name": "valid"}]},
        {"id": "budget", "package": str(too_large), "expected": "invalid", "checks": {"max_budget": 100}},
        {"id": "missing-dependency", "package": str(valid), "expected": "invalid",
         "checks": {"required_dependency": "not-materialized"}},
    ])
    assert [result["status"] for result in results] == ["passed", "passed", "passed"]


def test_unknown_fields_are_rejected_before_hashing_and_lock_verification(tmp_path: Path):
    package = _package(tmp_path, "strict", "1.0.0", dependencies=[{"name": "dep"}])
    manifest_path = package / "package.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["typo_field"] = "must not be ignored"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    with pytest.raises(PackageError, match="unknown manifest fields"):
        package_content_hash(package)
    with pytest.raises(PackageError, match="unknown manifest fields"):
        validate_package(package)

    package = _package(tmp_path, "dependency-fields", "1.0.0")
    manifest = json.loads((package / "package.json").read_text())
    manifest["dependencies"] = [{"name": "dep", "typo": True}]
    (package / "package.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    with pytest.raises(PackageError, match="unknown dependency fields"):
        PackageManifest.from_dict(manifest)

    lock_package = _package(tmp_path, "lockable", "1.0.0")
    lock = PackageResolver(_registry(tmp_path, lock_package)).resolve(["lockable"])
    lock_path = tmp_path / "strict.lock.json"
    lock.write(lock_path)
    raw_lock = json.loads(lock_path.read_text())
    raw_lock["unknown"] = 1
    lock_path.write_text(json.dumps(raw_lock))
    with pytest.raises(PackageError, match="unknown lockfile fields"):
        Lockfile.read(lock_path)
    raw_lock.pop("unknown")
    raw_lock["packages"][0]["unknown"] = 1
    lock_path.write_text(json.dumps(raw_lock))
    with pytest.raises(PackageError, match="unknown locked package fields"):
        Lockfile.read(lock_path)
    raw_lock["packages"][0].pop("unknown")
    raw_lock["packages"][0]["dependencies"] = [{"name": "dep", "version": "*", "unknown": 1}]
    lock_path.write_text(json.dumps(raw_lock))
    with pytest.raises(PackageError, match="unknown dependency fields"):
        Lockfile.read(lock_path)


def test_compatibility_policy_and_ambiguous_candidates_fail_closed(tmp_path: Path):
    missing_fact = _package(tmp_path, "missing-fact", "1.0.0", compatibility={"clang": ">=1"})
    with pytest.raises(PackageError, match="missing compatibility fact"):
        validate_package(missing_fact)

    denied = _package(tmp_path, "denied", "1.0.0", capabilities=["network"])
    with pytest.raises(PackageError, match="capabilities exceed policy"):
        PackageRegistry(tmp_path / "denied-registry").register(denied)
    with pytest.raises(PackageError, match="publisher is not allowed"):
        validate_package(denied, policy=PackagePolicy(allowed_publishers=frozenset({"trusted"})))

    first = _package(tmp_path / "first", "ambiguous", "1.0.0")
    second = _package(tmp_path / "second", "ambiguous", "1.0.0")
    (second / "extract.json").write_text(json.dumps({"kind": "cidx.extract", "name": "different"}) + "\n")
    _refresh_hash(second)
    registry = PackageRegistry(tmp_path / "ambiguous-registry")
    registry.register(first)
    registry.register(second)
    with pytest.raises(PackageResolutionError, match="ambiguous package candidates"):
        PackageResolver(CompositeRegistry([registry])).resolve([PackageDependency("ambiguous", "1.0.0")])
    explicit = PackageResolver(CompositeRegistry([registry])).resolve([
        PackageDependency("ambiguous", "1.0.0", content_hash=package_content_hash(first))
    ])
    assert explicit.packages[0].content_hash == package_content_hash(first)


def test_conformance_executes_fixtures_and_negative_matrix(tmp_path: Path):
    extract = _package(tmp_path, "matrix.extract", "1.0.0")
    analysis = _package(tmp_path, "matrix.analysis", "1.0.0", kind="cidx.analysis",
                        dependencies=[{"name": "matrix.extract"}])
    stale = _package(tmp_path, "matrix.stale", "1.0.0", compatibility={"catalog_version": 999})
    missing = _package(tmp_path, "matrix.missing", "1.0.0",
                       dependencies=[{"name": "matrix.not-materialized"}])
    malformed = tmp_path / "matrix-malformed"
    malformed.mkdir()
    (malformed / "package.json").write_text("{not-json\n")
    fixture = tmp_path / "matrix-fixture.json"
    fixture.write_text(json.dumps({"facts": [{"symbol": "Account"}], "results": [{"boundary": "Account"}]}))
    sdk = ConformanceSDK(PackageResolver(_registry(tmp_path, extract, analysis, stale, missing)))
    results = sdk.run([
        {"id": "extract", "package": str(extract), "fixture": str(fixture), "expected": "valid",
         "expected_facts": [{"symbol": "Account"}]},
        {"id": "analysis", "package": str(analysis), "fixture": str(fixture), "expected": "valid",
         "expected_results": [{"boundary": "Account"}]},
        {"id": "stale-schema", "package": str(stale), "expected": "invalid"},
        {"id": "budget", "package": str(extract), "expected": "invalid",
         "checks": {"max_budget": 1}},
        {"id": "malformed", "package": str(malformed), "expected": "invalid"},
        {"id": "missing-dependency", "package": str(missing), "expected": "invalid"},
    ])
    assert [result["status"] for result in results] == ["passed"] * 6
    assert results[0]["facts"] == [{"symbol": "Account"}]
    assert results[1]["results"] == [{"boundary": "Account"}]
