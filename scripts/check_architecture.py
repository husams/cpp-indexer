#!/usr/bin/env python3
"""Validate the checked-in CIDX module and dependency contract.

This is deliberately stdlib-only so it can run before the C++ build exists.
It checks source ownership, CMake target ownership/link declarations, internal
include edges, contract purity, declared dependency cycles, and exception
metadata.  It is a bootstrap guard; the self-indexing CIDX check can replace
or strengthen it after the query surface is ready.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from datetime import date
from pathlib import Path
from typing import Iterable


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
TARGET_RE = re.compile(
    r"\badd_(?:library|executable|custom_target)\s*\(\s*([A-Za-z0-9_.+:-]+)",
    re.MULTILINE,
)
LINK_RE = re.compile(
    r"\btarget_link_libraries\s*\(\s*([A-Za-z0-9_.+:-]+)(.*?)\)",
    re.DOTALL,
)
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[\"<]([^\">]+)[\">]", re.MULTILINE)


@dataclass(frozen=True)
class Source:
    path: str
    module: str


def _posix(path: Path) -> str:
    return path.as_posix()


def _matches(path: str, pattern: str) -> bool:
    return fnmatch.fnmatchcase(path, pattern)


def load_manifest(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _module_for(path: str, modules: list[dict]) -> list[str]:
    matches = []
    for module in modules:
        if any(_matches(path, pattern) for pattern in module.get("paths", [])) and not any(
            _matches(path, pattern) for pattern in module.get("excludePaths", [])
        ):
            matches.append(module["id"])
    return matches


def _sources(root: Path, manifest: dict) -> tuple[list[Source], list[str]]:
    modules = manifest["modules"]
    found = []
    errors = []
    roots = [root / relative for relative in manifest.get("sourceRoots", [])]
    files = sorted(
        path
        for source_root in roots
        if source_root.exists()
        for path in source_root.rglob("*")
        if path.is_file() and path.suffix in SOURCE_SUFFIXES
    )
    for path in files:
        relative = _posix(path.relative_to(root))
        matches = _module_for(relative, modules)
        if len(matches) != 1:
            errors.append(
                f"source {relative}: expected exactly one module, matched {matches or 'none'}"
            )
        else:
            found.append(Source(relative, matches[0]))
    return found, errors


def _cmake_targets(root: Path, manifest: dict) -> tuple[set[str], dict[str, set[str]]]:
    targets: set[str] = set()
    links: dict[str, set[str]] = defaultdict(set)
    for relative in manifest.get("cmakeFiles", ["CMakeLists.txt"]):
        path = root / relative
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        targets.update(TARGET_RE.findall(text))
        for target, body in LINK_RE.findall(text):
            tokens = re.findall(r"\$\{[^}]+\}|[A-Za-z0-9_.+:-]+", body)
            links[target].update(
                token for token in tokens if token not in {"PUBLIC", "PRIVATE", "INTERFACE"}
            )
    return targets, links


def _internal_include_edges(root: Path, sources: Iterable[Source]) -> list[tuple[str, str, str]]:
    by_path = {source.path: source.module for source in sources}
    edges = []
    for source in sources:
        text = (root / source.path).read_text(encoding="utf-8", errors="replace")
        for included in INCLUDE_RE.findall(text):
            normalized = included.removeprefix("./")
            candidate = f"src/{normalized}" if not normalized.startswith("src/") else normalized
            target_module = by_path.get(candidate)
            if target_module:
                edges.append((source.path, source.module, target_module))
    return edges


def _exception_matches(exception: dict, path: str, source_module: str, target_module: str) -> bool:
    if exception.get("fromModule") != source_module or exception.get("toModule") != target_module:
        return False
    return _matches(path, exception.get("source", "**"))


def _declared_cycle_errors(modules: list[dict]) -> list[str]:
    graph = {
        module["id"]: set(module.get("allowedDependencies", [])) - {module["id"]}
        for module in modules
    }
    errors = []
    visiting: list[str] = []
    visited: set[str] = set()

    def visit(node: str) -> None:
        if node in visiting:
            cycle = visiting[visiting.index(node) :] + [node]
            errors.append("declared dependency cycle: " + " -> ".join(cycle))
            return
        if node in visited:
            return
        visiting.append(node)
        for dependency in sorted(graph.get(node, ())):
            if dependency in graph:
                visit(dependency)
        visiting.pop()
        visited.add(node)

    for node in sorted(graph):
        visit(node)
    return errors


def _contract_errors(root: Path, manifest: dict) -> list[str]:
    errors = []
    for contract in manifest.get("contracts", []):
        path = root / contract["path"]
        if not path.exists():
            errors.append(f"contract {contract['path']}: file does not exist")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for line_number, line in enumerate(text.splitlines(), 1):
            match = re.match(r"\s*#\s*include\s*[\"<]([^\">]+)", line)
            if not match:
                continue
            included = match.group(1)
            if any(included.startswith(prefix) for prefix in contract.get("forbiddenIncludePrefixes", [])):
                errors.append(
                    f"contract {contract['path']}:{line_number}: forbidden include {included}"
                )
    return errors


def check_manifest(root: Path, manifest_path: Path) -> list[str]:
    manifest = load_manifest(manifest_path)
    errors: list[str] = []
    modules = manifest.get("modules", [])
    module_ids = {module["id"] for module in modules}
    if len(module_ids) != len(modules):
        errors.append("manifest: module ids must be unique")
    for module in modules:
        unknown = set(module.get("allowedDependencies", [])) - module_ids
        if unknown:
            errors.append(f"module {module['id']}: unknown allowed dependencies {sorted(unknown)}")

    sources, source_errors = _sources(root, manifest)
    errors.extend(source_errors)
    source_by_path = {source.path: source.module for source in sources}
    for contract in manifest.get("contracts", []):
        expected = contract.get("owner")
        actual = source_by_path.get(contract.get("path"))
        if actual != expected:
            errors.append(
                f"contract {contract.get('path')}: owner {expected!r} does not match source module {actual!r}"
            )

    target_names, cmake_links = _cmake_targets(root, manifest)
    target_entries = manifest.get("targets", [])
    target_map = {entry["name"]: entry for entry in target_entries}
    missing_targets = sorted(target_names - set(target_map))
    unknown_targets = sorted(set(target_map) - target_names)
    errors.extend(f"target {name}: missing manifest ownership" for name in missing_targets)
    errors.extend(f"target {name}: manifest target is not declared by CMake" for name in unknown_targets)
    for target, entry in target_map.items():
        actual_links = cmake_links.get(target, set())
        expected_links = set(entry.get("links", []))
        if actual_links != expected_links:
            errors.append(
                f"target {target}: manifest links {sorted(expected_links)} != CMake links {sorted(actual_links)}"
            )
        if entry.get("module") not in module_ids:
            errors.append(f"target {target}: unknown owner module {entry.get('module')}")

    exceptions = manifest.get("exceptions", [])
    for exception in exceptions:
        required = {"owner", "rationale", "expiresOn", "removalIssue", "boundary", "fromModule", "toModule", "source"}
        missing = sorted(required - set(exception))
        if missing:
            errors.append(f"exception {exception.get('source', '<unknown>')}: missing {missing}")
        try:
            if date.fromisoformat(exception["expiresOn"]) < date.today():
                errors.append(f"exception {exception.get('source')}: expired on {exception.get('expiresOn')}")
        except (KeyError, ValueError):
            pass

    target_exceptions = manifest.get("targetExceptions", [])
    for exception in target_exceptions:
        required = {"target", "owner", "rationale", "expiresOn", "removalIssue", "boundary"}
        missing = sorted(required - set(exception))
        if missing:
            errors.append(f"target exception {exception.get('target', '<unknown>')}: missing {missing}")
        if exception.get("target") not in target_map:
            errors.append(f"target exception {exception.get('target')}: target is not in the manifest")
        try:
            if date.fromisoformat(exception["expiresOn"]) < date.today():
                errors.append(
                    f"target exception {exception.get('target')}: expired on {exception.get('expiresOn')}"
                )
        except (KeyError, ValueError):
            pass

    allowed = {module["id"]: set(module.get("allowedDependencies", [])) for module in modules}
    for path, source_module, target_module in _internal_include_edges(root, sources):
        if target_module in allowed.get(source_module, set()):
            continue
        if any(_exception_matches(item, path, source_module, target_module) for item in exceptions):
            continue
        errors.append(f"include {path}: undeclared dependency {source_module} -> {target_module}")

    errors.extend(_contract_errors(root, manifest))
    errors.extend(_declared_cycle_errors(modules))
    return sorted(set(errors))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args(argv)
    root = args.manifest.resolve().parent.parent
    errors = check_manifest(root, args.manifest.resolve())
    if errors:
        print("architecture check failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"architecture check passed: {args.manifest} ({len(list((root / 'src').rglob('*')))} filesystem entries scanned)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
