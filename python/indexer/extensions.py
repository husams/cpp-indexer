"""Versioned declarative extension packages for the CIDX Python SDK.

The package format is deliberately data-only.  Resolution produces an immutable
lock description; execution engines consume the lock and the provenance helper
but are not part of this module.  Keeping those boundaries explicit prevents a
package install from becoming an implicit trust decision.
"""

from __future__ import annotations

import hashlib
import io
import json
import gzip
import os
import re
import shutil
import tarfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

from ._version import ARTIFACT_VERSION, __version__
from .generated_catalog import CATALOG_VERSION


PACKAGE_FORMAT = "cidx.package/v1"
LOCK_FORMAT = "cidx.lock/v1"
PACKAGE_MANIFEST = "package.json"
FORBIDDEN_CAPABILITIES = frozenset(
    {"sql", "executor", "shell", "python", "shared_library", "native_plugin"}
)
DECLARATIVE_SUFFIXES = frozenset(
    {".json", ".yaml", ".yml", ".toml", ".cxq", ".dl", ".md", ".txt"}
)
MANIFEST_KEYS = frozenset({
    "format", "name", "version", "kind", "content_hash", "publisher", "origin", "license",
    "entry_points", "namespaces", "compatibility", "dependencies", "schemas",
    "budgets", "sandbox_profile", "trust", "evidence", "capabilities", "metadata", "signature",
})
DEPENDENCY_KEYS = frozenset({"name", "version", "kind", "content_hash"})
LOCK_KEYS = frozenset({"format", "roots", "packages", "lock_hash"})
LOCK_PACKAGE_KEYS = frozenset({"name", "version", "kind", "content_hash", "registry", "dependencies", "namespaces"})
COMPATIBILITY_KEYS = frozenset({"cidx", "cidx_version", "clang", "catalog_version", "artifact_version"})
SAFE_SANDBOX_PROFILES = frozenset({"declarative-default", "declarative-analysis", "declarative-query", "model-review"})


class PackageError(ValueError):
    """A package, registry, lockfile, or conformance contract is invalid."""


class PackageResolutionError(PackageError):
    """Resolution cannot produce one deterministic, compatible graph."""


def runtime_environment() -> dict[str, str | int]:
    """Return compatibility facts owned by the installed CIDX runtime."""

    return {
        "cidx": __version__,
        "cidx_version": __version__,
        "catalog_version": CATALOG_VERSION,
        "artifact_version": ARTIFACT_VERSION,
    }


@dataclass(frozen=True)
class PackagePolicy:
    """Trust policy supplied by the host, never by a package manifest."""

    allowed_registries: frozenset[str] = frozenset()
    allowed_publishers: frozenset[str] = frozenset()
    allowed_hashes: frozenset[str] = frozenset()
    allowed_kinds: frozenset[str] = frozenset({"cidx.extract", "cidx.analysis", "cidx.query", "cidx.model"})
    allowed_capabilities: frozenset[str] = frozenset()
    allowed_sandbox_profiles: frozenset[str] = SAFE_SANDBOX_PROFILES
    verified_publishers: frozenset[str] = frozenset()
    verified_hashes: frozenset[str] = frozenset()
    trusted_publishers: frozenset[str] = frozenset()
    trusted_signatures: frozenset[str] = frozenset()
    reviewed_model_hashes: frozenset[str] = frozenset()
    require_signatures: bool = True

    @classmethod
    def local_development(cls) -> "PackagePolicy":
        """Safe local default: unverified declarative packages only."""

        return cls()

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "PackagePolicy":
        allowed = {
            "allowed_registries", "allowed_publishers", "allowed_hashes", "allowed_kinds",
            "allowed_capabilities", "allowed_sandbox_profiles", "trusted_publishers",
            "verified_publishers", "verified_hashes", "trusted_signatures", "reviewed_model_hashes",
            "require_signatures",
        }
        unknown = set(value) - allowed
        if unknown:
            raise PackageError(f"unknown policy fields: {', '.join(sorted(unknown))}")

        source = value

        def strings(key: str, default: Iterable[str]) -> frozenset[str]:
            selected = source.get(key, list(default))
            if not isinstance(selected, list) or any(not isinstance(item, str) or not item for item in selected):
                raise PackageError(f"policy {key} must be a list of strings")
            return frozenset(selected)

        require_signatures = value.get("require_signatures", True)
        if not isinstance(require_signatures, bool):
            raise PackageError("policy require_signatures must be boolean")
        return cls(
            allowed_registries=strings("allowed_registries", ()),
            allowed_publishers=strings("allowed_publishers", ()),
            allowed_hashes=strings("allowed_hashes", ()),
            allowed_kinds=strings("allowed_kinds", cls().allowed_kinds),
            allowed_capabilities=strings("allowed_capabilities", ()),
            allowed_sandbox_profiles=strings("allowed_sandbox_profiles", SAFE_SANDBOX_PROFILES),
            verified_publishers=strings("verified_publishers", ()),
            verified_hashes=strings("verified_hashes", ()),
            trusted_publishers=strings("trusted_publishers", ()),
            trusted_signatures=strings("trusted_signatures", ()),
            reviewed_model_hashes=strings("reviewed_model_hashes", ()),
            require_signatures=require_signatures,
        )

    @classmethod
    def read(cls, path: str | os.PathLike[str]) -> "PackagePolicy":
        try:
            value = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise PackageError(f"cannot read package policy: {path}") from exc
        if not isinstance(value, dict):
            raise PackageError("package policy must be an object")
        return cls.from_dict(value)

    def check(self, manifest: "PackageManifest", registry_id: str | None = None) -> None:
        if self.allowed_registries and registry_id not in self.allowed_registries:
            raise PackageError(f"registry is not allowed by policy: {registry_id}")
        if self.allowed_publishers and manifest.publisher not in self.allowed_publishers:
            raise PackageError(f"publisher is not allowed by policy: {manifest.publisher}")
        if self.allowed_hashes and manifest.content_hash not in self.allowed_hashes:
            raise PackageError(f"package hash is not allowed by policy: {manifest.content_hash}")
        if manifest.kind not in self.allowed_kinds:
            raise PackageError(f"package kind is not allowed by policy: {manifest.kind}")
        if not set(manifest.capabilities).issubset(self.allowed_capabilities):
            raise PackageError(f"package capabilities exceed policy: {manifest.capabilities}")
        if manifest.sandbox_profile not in self.allowed_sandbox_profiles:
            raise PackageError(f"sandbox profile is not allowed by policy: {manifest.sandbox_profile}")
        if manifest.trust == "publisher-verified":
            if not self.verified_publishers or manifest.publisher not in self.verified_publishers:
                raise PackageError("publisher-verified package lacks external publisher evidence")
            if not self.verified_hashes or manifest.content_hash not in self.verified_hashes:
                raise PackageError("publisher-verified package hash lacks external evidence")
        if manifest.kind == "cidx.model" and manifest.content_hash not in self.reviewed_model_hashes:
            raise PackageError("model package hash lacks external review evidence")
        if manifest.trust == "trusted":
            if manifest.publisher not in self.trusted_publishers:
                raise PackageError("trusted package publisher is not allowlisted")
            if (not self.trusted_signatures or not manifest.signature or
                    manifest.signature not in self.trusted_signatures):
                raise PackageError("trusted package signature is not externally allowlisted")
            if not self.allowed_hashes or manifest.content_hash not in self.allowed_hashes:
                raise PackageError("trusted package hash is not externally allowlisted")


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_value(value: Any) -> str:
    return _sha256_bytes(canonical_json(value).encode("utf-8"))


@dataclass(frozen=True, order=True)
class Version:
    """Small semver subset used by the package contract."""

    major: int
    minor: int = 0
    patch: int = 0
    prerelease: str = field(default="", compare=True)

    _pattern = re.compile(r"^(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:-([0-9A-Za-z.-]+))?$")

    @classmethod
    def parse(cls, value: str | int | float) -> "Version":
        match = cls._pattern.fullmatch(str(value).strip())
        if not match:
            raise PackageError(f"invalid version: {value!r}")
        return cls(
            int(match.group(1)),
            int(match.group(2) or 0),
            int(match.group(3) or 0),
            match.group(4) or "",
        )

    def __str__(self) -> str:
        suffix = f"-{self.prerelease}" if self.prerelease else ""
        return f"{self.major}.{self.minor}.{self.patch}{suffix}"


def version_satisfies(version: str | Version, requirement: str | None) -> bool:
    value = version if isinstance(version, Version) else Version.parse(version)
    text = (requirement or "*").strip()
    if text in {"", "*", "latest"}:
        return True
    for part in (p.strip() for p in text.split(",")):
        if not part or part == "*":
            continue
        if "x" in part.lower() or "*" in part:
            wildcard = part.lower().replace("*", "x").split(".")
            if wildcard[0] != "x" and value.major != int(wildcard[0]):
                return False
            if len(wildcard) > 1 and wildcard[1] != "x" and value.minor != int(wildcard[1]):
                return False
            if len(wildcard) > 2 and wildcard[2] != "x" and value.patch != int(wildcard[2]):
                return False
            continue
        if part.startswith("^"):
            lower = Version.parse(part[1:])
            upper = Version(lower.major + 1, 0, 0)
            if not (value >= lower and value < upper):
                return False
            continue
        if part.startswith("~"):
            lower = Version.parse(part[1:])
            upper = Version(lower.major, lower.minor + 1, 0)
            if not (value >= lower and value < upper):
                return False
            continue
        match = re.match(r"^(>=|<=|>|<|=)?(.+)$", part)
        if not match:
            return False
        operator, target_text = match.groups()
        target = Version.parse(target_text)
        if operator == ">=" and not value >= target:
            return False
        if operator == "<=" and not value <= target:
            return False
        if operator == ">" and not value > target:
            return False
        if operator == "<" and not value < target:
            return False
        if operator in {None, "="} and value != target:
            return False
    return True


def _string_list(value: Any, field_name: str) -> tuple[str, ...]:
    if not isinstance(value, list) or any(not isinstance(item, str) or not item for item in value):
        raise PackageError(f"{field_name} must be a list of non-empty strings")
    return tuple(sorted(set(value)))


@dataclass(frozen=True)
class PackageDependency:
    name: str
    version: str = "*"
    kind: str | None = None
    content_hash: str | None = None

    @classmethod
    def from_value(cls, value: Any) -> "PackageDependency":
        if isinstance(value, cls):
            return value
        if isinstance(value, str):
            return cls(value)
        if not isinstance(value, dict) or not isinstance(value.get("name"), str):
            raise PackageError("dependencies must contain names or {name, version} objects")
        unknown = set(value) - DEPENDENCY_KEYS
        if unknown:
            raise PackageError(f"unknown dependency fields: {', '.join(sorted(unknown))}")
        name = value["name"]
        if not name:
            raise PackageError("dependency name must not be empty")
        kind = value.get("kind")
        if kind is not None and (not isinstance(kind, str) or not kind):
            raise PackageError("dependency kind must be a non-empty string")
        content_hash = value.get("content_hash")
        if content_hash is not None and not re.fullmatch(r"sha256:[0-9a-f]{64}", content_hash):
            raise PackageError("dependency content_hash must be sha256:<64 lowercase hex digits>")
        return cls(name, str(value.get("version", "*")), kind, content_hash)

    def as_dict(self) -> dict[str, str]:
        result = {"name": self.name, "version": self.version}
        if self.kind is not None:
            result["kind"] = self.kind
        if self.content_hash is not None:
            result["content_hash"] = self.content_hash
        return result


@dataclass(frozen=True)
class PackageManifest:
    name: str
    version: str
    kind: str
    content_hash: str
    publisher: str
    origin: str
    license: str
    entry_points: Mapping[str, str]
    namespaces: tuple[str, ...]
    compatibility: Mapping[str, Any]
    dependencies: tuple[PackageDependency, ...]
    schemas: tuple[str, ...]
    budgets: Mapping[str, int]
    sandbox_profile: str
    trust: str
    evidence: str
    capabilities: tuple[str, ...] = ()
    metadata: Mapping[str, Any] = field(default_factory=dict)
    signature: str | None = None

    @classmethod
    def from_dict(cls, value: Mapping[str, Any], *, require_hash: bool = True) -> "PackageManifest":
        if not isinstance(value, dict):
            raise PackageError("package manifest must be an object")
        unknown = set(value) - MANIFEST_KEYS
        if unknown:
            raise PackageError(f"unknown manifest fields: {', '.join(sorted(unknown))}")
        if value.get("format") != PACKAGE_FORMAT:
            raise PackageError(f"manifest format must be {PACKAGE_FORMAT}")
        required = ("name", "version", "kind", "publisher", "origin", "license")
        missing = [key for key in required if not isinstance(value.get(key), str) or not value[key]]
        if missing:
            raise PackageError(f"manifest missing required fields: {', '.join(missing)}")
        name = value["name"]
        if not re.fullmatch(r"[a-z][a-z0-9_.-]*(?:/[a-z][a-z0-9_.-]*)?", name):
            raise PackageError(f"invalid package name: {name!r}")
        kind = value["kind"]
        if kind not in {"cidx.extract", "cidx.analysis", "cidx.query", "cidx.model"}:
            raise PackageError(f"unsupported package kind: {kind!r}")
        content_hash = value.get("content_hash", "")
        if require_hash and not re.fullmatch(r"sha256:[0-9a-f]{64}", content_hash):
            raise PackageError("content_hash must be sha256:<64 lowercase hex digits>")
        entry_points = value.get("entry_points")
        if not isinstance(entry_points, dict) or not entry_points:
            raise PackageError("entry_points must be a non-empty object")
        if any(not isinstance(k, str) or not isinstance(v, str) or not v for k, v in entry_points.items()):
            raise PackageError("entry_points must map names to declarative artifact paths")
        namespaces_value = value.get("namespaces", [name])
        namespaces = _string_list(namespaces_value, "namespaces")
        compatibility = value.get("compatibility", {})
        if not isinstance(compatibility, dict):
            raise PackageError("compatibility must be an object")
        unknown_compatibility = set(compatibility) - COMPATIBILITY_KEYS
        if unknown_compatibility:
            raise PackageError(
                f"unknown compatibility fields: {', '.join(sorted(unknown_compatibility))}"
            )
        for key, requirement in compatibility.items():
            if key in {"catalog_version", "artifact_version"}:
                if isinstance(requirement, bool) or not isinstance(requirement, int):
                    raise PackageError(f"compatibility {key} must be an integer")
            elif not isinstance(requirement, (str, int, float)):
                raise PackageError(f"compatibility {key} must be a version value")
        dependencies = tuple(PackageDependency.from_value(item) for item in value.get("dependencies", []))
        if len({item.name for item in dependencies}) != len(dependencies):
            raise PackageError("a package may declare each dependency only once")
        schemas = _string_list(value.get("schemas", []), "schemas")
        budgets = value.get("budgets", {})
        if not isinstance(budgets, dict) or any(
            not isinstance(k, str) or not isinstance(v, int) or v < 0 for k, v in budgets.items()
        ):
            raise PackageError("budgets must map names to non-negative integers")
        sandbox = value.get("sandbox_profile")
        if not isinstance(sandbox, str) or not sandbox:
            raise PackageError("sandbox_profile is required")
        trust = value.get("trust", "unverified")
        evidence = value.get("evidence", "extension-analysis")
        if trust not in {"unverified", "publisher-verified", "trusted"}:
            raise PackageError(f"invalid trust class: {trust!r}")
        if evidence not in {"extension-analysis", "source", "derived", "inferred", "proof"}:
            raise PackageError(f"invalid evidence class: {evidence!r}")
        capabilities = _string_list(value.get("capabilities", []), "capabilities")
        forbidden = sorted(set(capabilities) & FORBIDDEN_CAPABILITIES)
        if forbidden:
            raise PackageError(f"forbidden executable capabilities: {', '.join(forbidden)}")
        metadata = value.get("metadata", {})
        if not isinstance(metadata, dict):
            raise PackageError("metadata must be an object")
        signature = value.get("signature")
        if signature is not None and (not isinstance(signature, str) or not signature):
            raise PackageError("signature must be a non-empty string when present")
        return cls(
            name=name,
            version=str(Version.parse(value["version"])),
            kind=kind,
            content_hash=content_hash,
            publisher=value["publisher"],
            origin=value["origin"],
            license=value["license"],
            entry_points={key: entry_points[key] for key in sorted(entry_points)},
            namespaces=namespaces,
            compatibility=dict(compatibility),
            dependencies=tuple(sorted(dependencies, key=lambda item: item.name)),
            schemas=schemas,
            budgets={key: budgets[key] for key in sorted(budgets)},
            sandbox_profile=sandbox,
            trust=trust,
            evidence=evidence,
            capabilities=capabilities,
            metadata=dict(metadata),
            signature=signature,
        )

    def as_dict(self, *, include_hash: bool = True) -> dict[str, Any]:
        result: dict[str, Any] = {
            "format": PACKAGE_FORMAT,
            "name": self.name,
            "version": self.version,
            "kind": self.kind,
            "publisher": self.publisher,
            "origin": self.origin,
            "license": self.license,
            "entry_points": dict(self.entry_points),
            "namespaces": list(self.namespaces),
            "compatibility": dict(self.compatibility),
            "dependencies": [item.as_dict() for item in self.dependencies],
            "schemas": list(self.schemas),
            "budgets": dict(self.budgets),
            "sandbox_profile": self.sandbox_profile,
            "trust": self.trust,
            "evidence": self.evidence,
            "capabilities": list(self.capabilities),
            "metadata": dict(self.metadata),
        }
        if self.signature is not None:
            result["signature"] = self.signature
        if include_hash:
            result["content_hash"] = self.content_hash
        return result

    def canonical(self, *, include_hash: bool = True) -> str:
        return canonical_json(self.as_dict(include_hash=include_hash))


def _manifest_path(root: str | os.PathLike[str]) -> Path:
    path = Path(root).resolve()
    if not path.is_dir():
        raise PackageError(f"package directory not found: {path}")
    return path / PACKAGE_MANIFEST


def package_content_hash(root: str | os.PathLike[str]) -> str:
    """Hash the normalized manifest and every declarative package file."""

    manifest_path = _manifest_path(root)
    package_root = manifest_path.parent
    try:
        raw = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest = PackageManifest.from_dict(raw, require_hash=False)
    except (OSError, json.JSONDecodeError) as exc:
        raise PackageError(f"cannot read package manifest: {manifest_path}") from exc
    digest = hashlib.sha256()
    entries: list[tuple[str, bytes]] = [(PACKAGE_MANIFEST, manifest.canonical(include_hash=False).encode())]
    for item in sorted(package_root.rglob("*")):
        if item.is_symlink():
            relative = item.relative_to(package_root).as_posix()
            raise PackageError(f"package contains an unsafe file: {relative}")
        if item.is_dir():
            continue
        relative = item.relative_to(package_root).as_posix()
        if relative == PACKAGE_MANIFEST:
            continue
        if item.suffix.lower() not in DECLARATIVE_SUFFIXES:
            raise PackageError(f"package file is not declarative: {relative}")
        entries.append((relative, item.read_bytes()))
    for relative, data in entries:
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(_sha256_bytes(data).encode("ascii"))
        digest.update(b"\n")
    return "sha256:" + digest.hexdigest()


def validate_package(
    root: str | os.PathLike[str],
    *,
    environment: Mapping[str, Any] | None = None,
    expected_hash: str | None = None,
    policy: PackagePolicy | None = None,
    registry_identity: str | None = None,
    check_compatibility: bool = True,
) -> PackageManifest:
    path = _manifest_path(root)
    try:
        manifest = PackageManifest.from_dict(json.loads(path.read_text(encoding="utf-8")))
    except json.JSONDecodeError as exc:
        raise PackageError(f"malformed package manifest: {path}") from exc
    actual_hash = package_content_hash(path.parent)
    if actual_hash != manifest.content_hash:
        raise PackageError(
            f"package hash drift for {manifest.name}@{manifest.version}: "
            f"declared {manifest.content_hash}, computed {actual_hash}"
        )
    if expected_hash is not None and actual_hash != expected_hash:
        raise PackageError(f"package hash does not match lockfile: {actual_hash}")
    for entry in manifest.entry_points.values():
        entry_path = (path.parent / entry).resolve()
        if not entry_path.is_relative_to(path.parent) or not entry_path.is_file():
            raise PackageError(f"entry point is missing or outside the package: {entry}")
    for schema in manifest.schemas:
        schema_path = (path.parent / schema).resolve()
        if not schema_path.is_relative_to(path.parent) or not schema_path.is_file():
            raise PackageError(f"schema is missing or outside the package: {schema}")
    if check_compatibility:
        actual_environment = runtime_environment()
        if environment is not None:
            actual_environment.update(environment)
        for key, requirement in manifest.compatibility.items():
            actual = actual_environment.get(key)
            if actual is None:
                raise PackageError(f"missing compatibility fact: {key}")
            if key in {"catalog_version", "artifact_version"}:
                if int(actual) != int(requirement):
                    raise PackageError(f"incompatible {key}: requires {requirement}, have {actual}")
            elif not version_satisfies(str(actual), str(requirement)):
                raise PackageError(f"incompatible {key}: requires {requirement}, have {actual}")
    (policy or PackagePolicy.local_development()).check(manifest, registry_identity)
    if manifest.kind == "cidx.query":
        for name in manifest.entry_points:
            if not name.startswith("macro/"):
                raise PackageError("query packages may expose only macro entry points")
        for entry in manifest.entry_points.values():
            entry_text = (path.parent / entry).read_text(encoding="utf-8").lower()
            if re.search(r"\b(select|pragma|create\s+table|executor|shared[_ -]?library)\b", entry_text):
                raise PackageError("query macros may not contain SQL or executor code")
    if manifest.kind == "cidx.model" and manifest.trust == "trusted":
        if manifest.metadata.get("review_status") != "approved":
            raise PackageError("trusted model packages require approved review evidence")
        if not manifest.metadata.get("applicability_rules"):
            raise PackageError("trusted model packages require explicit applicability rules")
    return manifest


class PackageRegistry:
    """Content-addressed package store rooted at one deterministic location."""

    def __init__(self, root: str | os.PathLike[str], *, identity: str | None = None,
                 policy: PackagePolicy | None = None):
        self.root = Path(root).expanduser().resolve()
        self.identity = identity or str(self.root)
        self.policy = policy or PackagePolicy.local_development()

    def _destination(self, manifest: PackageManifest) -> Path:
        return self.root / manifest.name / manifest.version / manifest.content_hash.removeprefix("sha256:")

    def register(self, package: str | os.PathLike[str]) -> PackageManifest:
        manifest = validate_package(
            package, policy=self.policy, registry_identity=self.identity,
            check_compatibility=False,
        )
        destination = self._destination(manifest)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            existing = validate_package(
                destination, policy=self.policy, registry_identity=self.identity,
                check_compatibility=False,
            )
            if existing.content_hash != manifest.content_hash:
                raise PackageError("registry path already contains different content")
            return existing
        shutil.copytree(Path(package).resolve(), destination, symlinks=False)
        return validate_package(
            destination, policy=self.policy, registry_identity=self.identity,
            check_compatibility=False,
        )

    def candidates(self, name: str, policy: PackagePolicy | None = None,
                   environment: Mapping[str, Any] | None = None) -> list[PackageManifest]:
        base = self.root / name
        if not base.is_dir():
            return []
        result: list[PackageManifest] = []
        for manifest_path in sorted(base.glob("*/*/package.json")):
            try:
                result.append(validate_package(
                    manifest_path.parent, environment=environment,
                    policy=policy or self.policy, registry_identity=self.identity,
                ))
            except PackageError as exc:
                raise PackageResolutionError(
                    f"invalid materialized package under registry: {manifest_path.parent}: {exc}"
                ) from exc
        return sorted(result, key=lambda item: Version.parse(item.version), reverse=True)

    def exact(self, name: str, version: str, content_hash: str | None = None,
              policy: PackagePolicy | None = None,
              environment: Mapping[str, Any] | None = None) -> PackageManifest:
        candidates = [item for item in self.candidates(name, policy, environment)
                      if item.version == str(Version.parse(version))]
        if content_hash is not None:
            candidates = [item for item in candidates if item.content_hash == content_hash]
        if len(candidates) != 1:
            raise PackageResolutionError(f"materialized package not found: {name}@{version}")
        return candidates[0]

    def path_for(self, manifest: PackageManifest) -> Path:
        path = self._destination(manifest)
        if not path.is_dir():
            raise PackageResolutionError(f"package is not materialized: {manifest.name}@{manifest.version}")
        return path


class CompositeRegistry:
    """Repository-local, user-local, then explicitly configured registries."""

    def __init__(self, registries: Sequence[PackageRegistry]):
        self.registries = tuple(registries)

    def candidates(self, name: str, policy: PackagePolicy | None = None,
                   environment: Mapping[str, Any] | None = None) -> list[tuple[int, PackageRegistry, PackageManifest]]:
        rows = []
        for priority, registry in enumerate(self.registries):
            rows.extend((priority, registry, item)
                        for item in registry.candidates(name, policy, environment))
        return sorted(rows, key=lambda row: (Version.parse(row[2].version), -row[0], row[2].content_hash), reverse=True)

    def exact(self, name: str, version: str, content_hash: str,
              policy: PackagePolicy | None = None,
              environment: Mapping[str, Any] | None = None) -> tuple[PackageRegistry, PackageManifest]:
        for registry in self.registries:
            try:
                return registry, registry.exact(name, version, content_hash, policy, environment)
            except PackageResolutionError:
                continue
        raise PackageResolutionError(f"locked package is not materialized: {name}@{version}")


@dataclass(frozen=True)
class LockedPackage:
    name: str
    version: str
    kind: str
    content_hash: str
    registry: str
    dependencies: tuple[PackageDependency, ...]
    namespaces: tuple[str, ...]

    @classmethod
    def from_manifest(cls, manifest: PackageManifest, registry: str) -> "LockedPackage":
        return cls(manifest.name, manifest.version, manifest.kind, manifest.content_hash,
                   registry, manifest.dependencies, manifest.namespaces)

    def as_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "version": self.version,
            "kind": self.kind,
            "content_hash": self.content_hash,
            "registry": self.registry,
            "dependencies": [item.as_dict() for item in self.dependencies],
            "namespaces": list(self.namespaces),
        }


@dataclass(frozen=True)
class Lockfile:
    roots: tuple[PackageDependency, ...]
    packages: tuple[LockedPackage, ...]
    lock_hash: str

    @classmethod
    def create(cls, roots: Iterable[PackageDependency], packages: Iterable[LockedPackage]) -> "Lockfile":
        root_rows = tuple(sorted(roots, key=lambda item: item.name))
        package_rows = tuple(sorted(packages, key=lambda item: (item.name, Version.parse(item.version))))
        if len({item.name for item in root_rows}) != len(root_rows):
            raise PackageError("lockfile contains duplicate roots")
        if len({item.name for item in package_rows}) != len(package_rows):
            raise PackageError("lockfile contains duplicate package names")
        payload = {"format": LOCK_FORMAT, "roots": [item.as_dict() for item in root_rows],
                   "packages": [item.as_dict() for item in package_rows]}
        return cls(root_rows, package_rows, "sha256:" + _sha256_value(payload))

    def as_dict(self) -> dict[str, Any]:
        return {"format": LOCK_FORMAT, "roots": [item.as_dict() for item in self.roots],
                "packages": [item.as_dict() for item in self.packages], "lock_hash": self.lock_hash}

    def write(self, path: str | os.PathLike[str]) -> None:
        destination = Path(path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(self.as_dict(), indent=2, sort_keys=True) + "\n", encoding="utf-8")

    @classmethod
    def read(cls, path: str | os.PathLike[str]) -> "Lockfile":
        try:
            value = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise PackageError(f"cannot read lockfile: {path}") from exc
        if not isinstance(value, dict):
            raise PackageError("lockfile must be an object")
        unknown = set(value) - LOCK_KEYS
        if unknown:
            raise PackageError(f"unknown lockfile fields: {', '.join(sorted(unknown))}")
        if value.get("format") != LOCK_FORMAT:
            raise PackageError(f"lockfile format must be {LOCK_FORMAT}")
        if not isinstance(value.get("roots"), list) or not isinstance(value.get("packages"), list):
            raise PackageError("lockfile roots and packages must be arrays")
        if not isinstance(value.get("lock_hash"), str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", value["lock_hash"]):
            raise PackageError("lock_hash must be sha256:<64 lowercase hex digits>")
        roots = tuple(PackageDependency.from_value(item) for item in value["roots"])
        packages = []
        for item in value["packages"]:
            if not isinstance(item, dict):
                raise PackageError("lockfile packages must be objects")
            unknown_package = set(item) - LOCK_PACKAGE_KEYS
            if unknown_package:
                raise PackageError(f"unknown locked package fields: {', '.join(sorted(unknown_package))}")
            required = ("name", "version", "kind", "content_hash", "registry")
            if any(key not in item for key in required):
                raise PackageError("locked packages are missing required fields")
            if any(not isinstance(item[key], str) or not item[key] for key in ("name", "kind", "registry")):
                raise PackageError("locked package name, kind, and registry must be non-empty strings")
            if not isinstance(item["content_hash"], str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", item["content_hash"]):
                raise PackageError("locked package content_hash must be sha256:<64 lowercase hex digits>")
            if not isinstance(item.get("dependencies", []), list) or not isinstance(item.get("namespaces", []), list):
                raise PackageError("locked package dependencies and namespaces must be arrays")
            packages.append(LockedPackage(
                item["name"], str(Version.parse(item["version"])), item["kind"], item["content_hash"],
                item["registry"], tuple(PackageDependency.from_value(dep) for dep in item.get("dependencies", [])),
                _string_list(item.get("namespaces", [item["name"]]), "locked namespaces"),
            ))
        result = cls.create(roots, packages)
        if result.lock_hash != value.get("lock_hash"):
            raise PackageError("lock hash drift")
        return result


class PackageResolver:
    def __init__(self, registry: CompositeRegistry, *, environment: Mapping[str, Any] | None = None,
                 core_namespaces: Iterable[str] = ("cidx",), policy: PackagePolicy | None = None):
        self.registry = registry
        self.environment = runtime_environment()
        self.environment.update(environment or {})
        self.core_namespaces = frozenset(core_namespaces)
        self.policy = policy or PackagePolicy.local_development()

    def resolve(self, requirements: Iterable[PackageDependency | Mapping[str, Any] | str]) -> Lockfile:
        roots = tuple(PackageDependency.from_value(item) for item in requirements)
        selected: dict[str, tuple[PackageRegistry, PackageManifest]] = {}
        visiting: list[str] = []

        def visit(requirement: PackageDependency) -> None:
            if requirement.name in visiting:
                cycle = " -> ".join(visiting + [requirement.name])
                raise PackageResolutionError(f"dependency cycle: {cycle}")
            current = selected.get(requirement.name)
            if current is not None:
                manifest = current[1]
                if not version_satisfies(manifest.version, requirement.version):
                    raise PackageResolutionError(
                        f"incompatible requirements for {requirement.name}: {manifest.version} does not satisfy {requirement.version}"
                    )
                if requirement.kind is not None and manifest.kind != requirement.kind:
                    raise PackageResolutionError(f"wrong package kind for {requirement.name}")
                if requirement.content_hash is not None and manifest.content_hash != requirement.content_hash:
                    raise PackageResolutionError(f"wrong package hash for {requirement.name}")
                return
            candidates = [row for row in self.registry.candidates(
                requirement.name, self.policy, self.environment
            )
                          if version_satisfies(row[2].version, requirement.version)
                          and (requirement.kind is None or row[2].kind == requirement.kind)
                          and (requirement.content_hash is None or row[2].content_hash == requirement.content_hash)]
            if not candidates:
                raise PackageResolutionError(f"missing dependency: {requirement.name} {requirement.version}")
            highest_version = max(Version.parse(row[2].version) for row in candidates)
            same_version = [row for row in candidates if Version.parse(row[2].version) == highest_version]
            highest_priority = min(row[0] for row in same_version)
            tier = [row for row in same_version if row[0] == highest_priority]
            if requirement.content_hash is None and len({row[2].content_hash for row in tier}) > 1:
                raise PackageResolutionError(
                    f"ambiguous package candidates: {requirement.name}@{highest_version} has multiple content hashes"
                )
            priority, registry, manifest = sorted(tier, key=lambda row: row[2].content_hash)[0]
            validate_package(
                registry.path_for(manifest), environment=self.environment,
                policy=self.policy, registry_identity=registry.identity,
            )
            visiting.append(requirement.name)
            selected[requirement.name] = (registry, manifest)
            for dependency in manifest.dependencies:
                visit(dependency)
            visiting.pop()

        for root in roots:
            visit(root)
        namespaces: dict[str, str] = {}
        for name, (_, manifest) in sorted(selected.items()):
            for namespace in manifest.namespaces:
                if namespace in self.core_namespaces:
                    raise PackageResolutionError(f"package namespace collides with core: {namespace}")
                owner = namespaces.get(namespace)
                if owner is not None and owner != name:
                    raise PackageResolutionError(f"package namespace collision: {namespace} ({owner}, {name})")
                namespaces[namespace] = name
        # Keep machine-local registry paths out of the lock hash.  The origin
        # is a package-declared, reviewable identity; materialization is
        # resolved from the caller's ordered registry set during verification.
        locked = [LockedPackage.from_manifest(manifest, manifest.origin)
                  for _, manifest in selected.values()]
        return Lockfile.create(roots, locked)

    def verify_lock(self, lockfile: Lockfile, *, offline: bool = True) -> dict[str, PackageManifest]:
        manifests: dict[str, PackageManifest] = {}
        for locked in lockfile.packages:
            registry, manifest = self.registry.exact(
                locked.name, locked.version, locked.content_hash, self.policy, self.environment
            )
            if not offline and manifest.origin.startswith("file:"):
                raise PackageResolutionError("network registry access is not supported by the offline SDK")
            if manifest.kind != locked.kind or manifest.namespaces != locked.namespaces:
                raise PackageResolutionError(f"lock metadata drift for {locked.name}")
            validate_package(
                registry.path_for(manifest), environment=self.environment,
                expected_hash=locked.content_hash, policy=self.policy,
                registry_identity=registry.identity,
            )
            manifests[locked.name] = manifest
        for root in lockfile.roots:
            manifest = manifests.get(root.name)
            if manifest is None or not version_satisfies(manifest.version, root.version):
                raise PackageResolutionError(f"lockfile root is missing or incompatible: {root.name}")
        for locked in lockfile.packages:
            for dependency in locked.dependencies:
                dep = manifests.get(dependency.name)
                if (dep is None or not version_satisfies(dep.version, dependency.version)
                        or (dependency.kind is not None and dep.kind != dependency.kind)
                        or (dependency.content_hash is not None and dep.content_hash != dependency.content_hash)):
                    raise PackageResolutionError(f"lockfile dependency is missing or incompatible: {locked.name} -> {dependency.name}")
        visiting: list[str] = []

        def visit(name: str) -> None:
            if name in visiting:
                raise PackageResolutionError(f"dependency cycle in lockfile: {' -> '.join(visiting + [name])}")
            visiting.append(name)
            locked = next(item for item in lockfile.packages if item.name == name)
            for dependency in locked.dependencies:
                visit(dependency.name)
            visiting.pop()

        for locked in lockfile.packages:
            visit(locked.name)
        namespaces: dict[str, str] = {}
        for manifest in manifests.values():
            for namespace in manifest.namespaces:
                if namespace in self.core_namespaces:
                    raise PackageResolutionError(f"package namespace collides with core: {namespace}")
                owner = namespaces.get(namespace)
                if owner is not None and owner != manifest.name:
                    raise PackageResolutionError(f"package namespace collision: {namespace} ({owner}, {manifest.name})")
                namespaces[namespace] = manifest.name
        return manifests


@dataclass(frozen=True)
class ResultProvenance:
    package_name: str
    package_version: str
    package_hash: str
    entry_point: str
    input_fact_sets: tuple[str, ...]
    sandbox_profile: str
    applicability: Mapping[str, Any]
    lock_hash: str

    def as_dict(self) -> dict[str, Any]:
        return {
            "package": {"name": self.package_name, "version": self.package_version, "hash": self.package_hash},
            "entry_point": self.entry_point,
            "input_fact_sets": list(self.input_fact_sets),
            "sandbox_profile": self.sandbox_profile,
            "applicability": dict(self.applicability),
            "lock_hash": self.lock_hash,
            "evidence": "extension-analysis",
        }


def artifact_identity(lockfile: Lockfile, *, artifact_kind: str, entry_point: str,
                      input_identity: str, options: Mapping[str, Any] | None = None) -> str:
    return "sha256:" + _sha256_value({
        "artifact_kind": artifact_kind,
        "entry_point": entry_point,
        "input_identity": input_identity,
        "lock_hash": lockfile.lock_hash,
        "options": dict(options or {}),
    })


@dataclass(frozen=True)
class ConformanceCase:
    case_id: str
    package: str
    expected: str
    checks: Mapping[str, Any] = field(default_factory=dict)
    fixture: str | None = None
    expected_facts: tuple[Mapping[str, Any], ...] = ()
    expected_results: tuple[Mapping[str, Any], ...] = ()
    expected_status: str = "complete"
    environment: Mapping[str, Any] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "ConformanceCase":
        allowed = {
            "id", "package", "expected", "checks", "fixture", "expected_facts",
            "expected_results", "expected_status", "environment",
        }
        unknown = set(value) - allowed
        if unknown:
            raise PackageError(f"unknown conformance case fields: {', '.join(sorted(unknown))}")
        if not all(isinstance(value.get(key), str) and value[key] for key in ("id", "package", "expected")):
            raise PackageError("conformance cases require id, package, and expected")
        if value["expected"] not in {"valid", "invalid"}:
            raise PackageError("conformance expected must be valid or invalid")
        expected_status = value.get("expected_status", "complete")
        if not isinstance(expected_status, str) or not expected_status:
            raise PackageError("conformance expected_status must be a non-empty string")
        fixture = value.get("fixture")
        if fixture is not None and (not isinstance(fixture, str) or not fixture):
            raise PackageError("conformance fixture must be a non-empty path")
        checks = value.get("checks", {})
        environment = value.get("environment", {})
        if not isinstance(checks, dict) or not isinstance(environment, dict):
            raise PackageError("conformance checks and environment must be objects")

        def rows(key: str) -> tuple[Mapping[str, Any], ...]:
            selected = value.get(key, [])
            if not isinstance(selected, list) or any(not isinstance(row, dict) for row in selected):
                raise PackageError(f"conformance {key} must be an array of objects")
            return tuple(dict(row) for row in selected)

        return cls(value["id"], value["package"], value["expected"], dict(checks), fixture,
                   rows("expected_facts"), rows("expected_results"), expected_status,
                   dict(environment))


class ConformanceSDK:
    """Run declarative package contract cases without executing package code."""

    def __init__(self, resolver: PackageResolver):
        self.resolver = resolver

    @staticmethod
    def _normalized_rows(value: Any, label: str) -> list[dict[str, Any]]:
        if not isinstance(value, (list, tuple)) or any(not isinstance(row, dict) for row in value):
            raise PackageError(f"{label} must be an array of objects")
        return sorted((json.loads(canonical_json(row)) for row in value), key=canonical_json)

    @staticmethod
    def _read_json(path: Path, label: str) -> Mapping[str, Any]:
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise PackageError(f"malformed {label}: {path}") from exc
        if not isinstance(value, dict):
            raise PackageError(f"{label} must be an object: {path}")
        return value

    def _materialize_case_package(self, case: ConformanceCase) -> tuple[Path, PackageManifest]:
        candidate = Path(case.package).expanduser()
        if candidate.is_dir():
            return candidate.resolve(), validate_package(
                candidate, environment={**self.resolver.environment, **case.environment},
                policy=self.resolver.policy, registry_identity=str(candidate.resolve()),
            )
        if "@" in case.package:
            name, version = case.package.rsplit("@", 1)
        else:
            name, version = case.package, "*"
        lock = self.resolver.resolve([PackageDependency(name, version)])
        locked = next(item for item in lock.packages if item.name == name)
        registry, manifest = self.resolver.registry.exact(
            locked.name, locked.version, locked.content_hash, self.resolver.policy,
            self.resolver.environment,
        )
        return registry.path_for(manifest), manifest

    @staticmethod
    def _fixture_path(case: ConformanceCase, package_path: Path) -> Path | None:
        if case.fixture is None:
            return None
        requested = Path(case.fixture).expanduser()
        if requested.is_absolute():
            return requested
        package_relative = package_path / requested
        return package_relative if package_relative.exists() else requested

    @staticmethod
    def _render(value: Any, row: Mapping[str, Any]) -> Any:
        if isinstance(value, str):
            if value.startswith("$"):
                key = value[1:]
                if key not in row:
                    raise PackageError(f"analysis rule references missing fact field: {key}")
                return row[key]
            return value
        if isinstance(value, list):
            return [ConformanceSDK._render(item, row) for item in value]
        if isinstance(value, dict):
            return {key: ConformanceSDK._render(item, row) for key, item in value.items()}
        return value

    def _execute_contract(self, case: ConformanceCase, package_path: Path,
                          manifest: PackageManifest,
                          upstream_facts: Sequence[Mapping[str, Any]] = (),
                          check_expected: bool = True,
                          fixture_value: Mapping[str, Any] | None = None) -> dict[str, Any]:
        if fixture_value is None:
            fixture_path = self._fixture_path(case, package_path)
            fixture = self._read_json(fixture_path, "fixture") if fixture_path is not None else {}
        else:
            fixture = fixture_value
        entry_name, entry = next(iter(sorted(manifest.entry_points.items())))
        entry_path = package_path / entry
        if entry_path.suffix.lower() != ".json":
            raise PackageError(f"conformance requires a JSON declarative entry point: {entry}")
        descriptor = self._read_json(entry_path, "entry point")
        result: dict[str, Any] = {"entry_point": entry_name, "execution_status": "complete"}
        if manifest.kind == "cidx.extract":
            source = descriptor.get("input_source")
            if not isinstance(source, str) or not source:
                raise PackageError("extract entry point must declare input_source")
            emits = descriptor.get("emits")
            bindings = descriptor.get("bindings", [])
            if (not isinstance(emits, list) or not emits or
                    any(not isinstance(relation, str) or not relation for relation in emits)):
                raise PackageError("extract entry point must declare non-empty emits")
            if not isinstance(bindings, list) or any(not isinstance(binding, str) for binding in bindings):
                raise PackageError("extract entry point bindings must be an array of strings")
            inputs = self._normalized_rows(fixture.get(source, []), "fixture inputs")
            generated = []
            for row in inputs:
                for relation in emits:
                    output = {"relation": relation}
                    for binding in bindings:
                        if binding not in row:
                            raise PackageError(f"extract binding references missing input field: {binding}")
                        output[binding] = row[binding]
                    generated.append(output)
            result["facts"] = self._normalized_rows(generated, "generated facts")
            if check_expected:
                expected = self._normalized_rows(case.expected_facts, "expected_facts")
                if result["facts"] != expected:
                    raise PackageError("fixture extraction facts do not match expected_facts")
        elif manifest.kind == "cidx.analysis":
            rules = descriptor.get("rules")
            if not isinstance(rules, list) or not rules:
                raise PackageError("analysis entry point must declare non-empty rules")
            facts = list(upstream_facts)
            if not facts:
                facts = self._normalized_rows(fixture.get("facts", []), "analysis input facts")
            generated = []
            for rule in rules:
                if not isinstance(rule, dict) or not isinstance(rule.get("input_relation"), str):
                    raise PackageError("analysis rules require input_relation")
                emit = rule.get("emit")
                if not isinstance(emit, dict) or not emit:
                    raise PackageError("analysis rules require a non-empty emit object")
                for fact in facts:
                    if fact.get("relation") == rule["input_relation"]:
                        generated.append(self._render(emit, fact))
            result["results"] = self._normalized_rows(generated, "generated results")
            if check_expected:
                expected = self._normalized_rows(case.expected_results, "expected_results")
                if result["results"] != expected:
                    raise PackageError("fixture analysis results do not match expected_results")
        elif manifest.kind == "cidx.query":
            if not isinstance(descriptor.get("plan"), list):
                raise PackageError("query entry point must declare a plan")
            result["plan"] = descriptor["plan"]
        else:
            if not isinstance(descriptor.get("clauses"), list):
                raise PackageError("model entry point must declare clauses")
            result["clauses"] = descriptor["clauses"]
        return result

    def run(self, cases: Iterable[ConformanceCase | Mapping[str, Any]]) -> list[dict[str, Any]]:
        results = []
        for item in cases:
            case = item if isinstance(item, ConformanceCase) else ConformanceCase.from_dict(item)
            error = None
            execution: dict[str, Any] = {}
            try:
                package_path, manifest = self._materialize_case_package(case)
                checks = case.checks
                if checks.get("kind") and manifest.kind != checks["kind"]:
                    raise PackageError("package kind mismatch")
                if "max_budget" in checks and any(value > int(checks["max_budget"]) for value in manifest.budgets.values()):
                    raise PackageError("budget exceeds conformance limit")
                if "required_dependency" in checks and checks["required_dependency"] not in {d.name for d in manifest.dependencies}:
                    raise PackageError("required dependency is missing")
                fixture_path = self._fixture_path(case, package_path)
                fixture = self._read_json(fixture_path, "fixture") if fixture_path is not None else {}
                upstream_facts: list[Mapping[str, Any]] = []
                if manifest.dependencies:
                    dependency_lock = self.resolver.resolve(manifest.dependencies)
                    for dependency in manifest.dependencies:
                        locked = next(item for item in dependency_lock.packages if item.name == dependency.name)
                        registry, dependency_manifest = self.resolver.registry.exact(
                            locked.name, locked.version, locked.content_hash,
                            self.resolver.policy, self.resolver.environment,
                        )
                        if dependency_manifest.kind == "cidx.extract":
                            dependency_execution = self._execute_contract(
                                case, registry.path_for(dependency_manifest), dependency_manifest,
                                check_expected=False, fixture_value=fixture,
                            )
                            upstream_facts.extend(dependency_execution.get("facts", []))
                execution = self._execute_contract(
                    case, package_path, manifest, upstream_facts, fixture_value=fixture,
                )
                output_rows = execution.get("facts", execution.get("results", []))
                output_bytes = len(canonical_json(output_rows).encode("utf-8"))
                execution["resource"] = {"output_rows": len(output_rows), "output_bytes": output_bytes}
                if "max_output_rows" in checks and len(output_rows) > int(checks["max_output_rows"]):
                    raise PackageError("conformance output row limit exceeded")
                if "max_output_bytes" in checks and output_bytes > int(checks["max_output_bytes"]):
                    raise PackageError("conformance output byte limit exceeded")
                if execution["execution_status"] != case.expected_status:
                    raise PackageError(
                        f"unexpected conformance status: {execution['execution_status']} (expected {case.expected_status})"
                    )
            except PackageError as exc:
                error = str(exc)
            passed = (error is None) if case.expected == "valid" else (error is not None)
            results.append({"id": case.case_id, "status": "passed" if passed else "failed",
                            "error": error, **execution})
        return results


def pack_package(root: str | os.PathLike[str], output: str | os.PathLike[str]) -> PackageManifest:
    manifest = validate_package(root)
    destination = Path(output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    archive_bytes = io.BytesIO()
    with tarfile.open(fileobj=archive_bytes, mode="w") as archive:
        for item in sorted(Path(root).rglob("*")):
            if item.is_file() and not item.is_symlink():
                data = item.read_bytes()
                info = tarfile.TarInfo(item.relative_to(root).as_posix())
                info.size = len(data)
                info.mode = 0o644
                info.mtime = 0
                archive.addfile(info, io.BytesIO(data))
    destination.write_bytes(gzip.compress(archive_bytes.getvalue(), mtime=0))
    return manifest


def load_conformance_cases(path: str | os.PathLike[str]) -> list[ConformanceCase]:
    value = json.loads(Path(path).read_text(encoding="utf-8"))
    cases = value.get("cases") if isinstance(value, dict) else value
    if not isinstance(cases, list):
        raise PackageError("conformance file must contain a cases array")
    return [ConformanceCase.from_dict(item) for item in cases]
