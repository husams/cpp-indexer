#!/usr/bin/env python3
"""Validate the checked-in CIDX module and dependency contract.

The checker is intentionally stdlib-only so it can run before a C++ build.  It
classifies every production source, validates CMake's actual target graph and
source edges, checks include ownership and external-library confinement, and
requires exceptions to correspond to a live violation.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import posixpath
import re
import shlex
import sys
from collections import defaultdict
from dataclasses import dataclass
from datetime import date
from pathlib import Path, PurePosixPath
from typing import Iterable


CXX_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
SOURCE_SUFFIXES = CXX_SUFFIXES | {".py", ".dl"}
VISIBILITY = {"PUBLIC", "PRIVATE", "INTERFACE"}
ISSUE_RE = re.compile(r"HSE-\d+\Z")
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*([\"<])([^\">]+)[\">]", re.MULTILINE)
COMMAND_RE = re.compile(r"\b(add_library|add_executable|add_custom_target|"
                         r"target_link_libraries|target_sources|add_dependencies)\s*\(")


@dataclass(frozen=True)
class Source:
    path: str
    module: str


@dataclass
class BuildGraph:
    targets: set[str]
    links: dict[str, set[str]]
    edges: dict[str, set[str]]
    source_edges: dict[str, set[str]]


@dataclass(frozen=True)
class Include:
    source: str
    source_module: str
    spelling: str
    target_module: str | None
    unresolved_project_path: str | None


def _posix(path: Path) -> str:
    return path.as_posix()


def _matches(path: str, pattern: str) -> bool:
    return fnmatch.fnmatchcase(path, pattern) or PurePosixPath(path).match(pattern)


def load_manifest(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _module_for(path: str, suffix: str, modules: list[dict]) -> list[str]:
    matches = []
    for module in modules:
        suffixes = set(module.get("sourceSuffixes", CXX_SUFFIXES))
        if suffix not in suffixes:
            continue
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
        matches = _module_for(relative, path.suffix, modules)
        if len(matches) != 1:
            errors.append(
                f"source {relative}: expected exactly one module, matched {matches or 'none'}"
            )
        else:
            found.append(Source(relative, matches[0]))
    return found, errors


def _commands(text: str) -> Iterable[tuple[str, str]]:
    """Yield CMake commands while handling nested generator expressions safely."""
    for match in COMMAND_RE.finditer(text):
        depth = 1
        quote: str | None = None
        index = match.end()
        while index < len(text) and depth:
            character = text[index]
            if quote:
                if character == quote and text[index - 1] != "\\":
                    quote = None
            elif character in {"\"", "'"}:
                quote = character
            elif character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
            index += 1
        if depth == 0:
            yield match.group(1), text[match.end(): index - 1]


def _tokens(body: str) -> list[str]:
    try:
        return shlex.split(re.sub(r"#[^\n]*", "", body), comments=False, posix=True)
    except ValueError:
        return re.findall(r"\$<[^>]+>|\$\{[^}]+\}|[A-Za-z0-9_.+:/${}-]+", body)


def _cmake_graph(root: Path, manifest: dict) -> BuildGraph:
    targets: set[str] = set()
    links: dict[str, set[str]] = defaultdict(set)
    edges: dict[str, set[str]] = defaultdict(set)
    source_edges: dict[str, set[str]] = defaultdict(set)
    commands: list[tuple[str, list[str]]] = []
    for relative in manifest.get("cmakeFiles", ["CMakeLists.txt"]):
        path = root / relative
        if path.exists():
            commands.extend((kind, _tokens(body)) for kind, body in _commands(
                path.read_text(encoding="utf-8", errors="replace")
            ))

    for kind, tokens in commands:
        if not tokens:
            continue
        if kind in {"add_library", "add_executable", "add_custom_target"}:
            targets.add(tokens[0])

    for kind, tokens in commands:
        if not tokens:
            continue
        target = tokens[0]
        if kind == "target_link_libraries":
            links[target].update(token for token in tokens[1:] if token not in VISIBILITY)
        elif kind in {"add_library", "add_executable", "target_sources"}:
            values = tokens[1:] if kind == "target_sources" else tokens[1:]
            for token in values:
                object_match = re.fullmatch(r"\$<TARGET_OBJECTS:([^>]+)>", token)
                if object_match:
                    edges[target].add(object_match.group(1))
                elif token.startswith(("src/", "python/")):
                    source_edges[target].add(token)
        elif kind == "add_dependencies":
            edges[target].update(token for token in tokens[1:] if token not in VISIBILITY)

    for target, values in links.items():
        edges[target].update(value for value in values if value in targets)
    return BuildGraph(set(targets), dict(links), dict(edges), dict(source_edges))


def _normalise_include(source_path: str, spelling: str) -> list[str]:
    include = PurePosixPath(spelling)
    candidates = []
    if spelling.startswith((".", "..")):
        candidates.append(PurePosixPath(source_path).parent / include)
    if spelling.startswith("src/"):
        candidates.append(include)
    else:
        candidates.append(PurePosixPath("src") / include)
    return [posixpath.normpath(PurePosixPath(candidate).as_posix()) for candidate in candidates]


def _includes(root: Path, sources: Iterable[Source]) -> tuple[list[Include], list[str]]:
    by_path = {source.path: source.module for source in sources}
    includes: list[Include] = []
    errors: list[str] = []
    for source in sources:
        if Path(source.path).suffix not in CXX_SUFFIXES:
            continue
        text = (root / source.path).read_text(encoding="utf-8", errors="replace")
        for _, spelling in INCLUDE_RE.findall(text):
            target_module = None
            unresolved = None
            for candidate in _normalise_include(source.path, spelling):
                if candidate in by_path:
                    target_module = by_path[candidate]
                    break
                if (root / candidate).is_file():
                    unresolved = candidate
            if unresolved and target_module is None:
                errors.append(f"include {source.path}: unresolved project include {spelling}")
            includes.append(Include(source.path, source.module, spelling, target_module, unresolved))
    return includes, errors


def _external_kind(spelling: str) -> str | None:
    if spelling.startswith("clang/") or spelling.startswith("clang-c/"):
        return "clang"
    if spelling.startswith("llvm/"):
        return "llvm"
    if spelling in {"sqlite3.h", "sqlite3ext.h", "storage/sqlite.hpp"}:
        return "sqlite"
    return None


def _exception_matches(exception: dict, path: str, source_module: str,
                       target_module: str, spelling: str | None = None) -> bool:
    if exception.get("fromModule") != source_module or exception.get("toModule") != target_module:
        return False
    if not _matches(path, exception.get("source", "")):
        return False
    return not exception.get("include") or exception["include"] == spelling


def _target_exception_matches(exception: dict, target: str, dependency: str) -> bool:
    return exception.get("target") == target and dependency in exception.get("dependencies", [])


def _cycle_errors(graph: dict[str, set[str]], label: str) -> list[str]:
    errors = []
    visiting: list[str] = []
    visited: set[str] = set()

    def visit(node: str) -> None:
        if node in visiting:
            cycle = visiting[visiting.index(node):] + [node]
            errors.append(f"{label} cycle: " + " -> ".join(cycle))
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
            if match and any(match.group(1).startswith(prefix)
                             for prefix in contract.get("forbiddenIncludePrefixes", [])):
                errors.append(
                    f"contract {contract['path']}:{line_number}: forbidden include {match.group(1)}"
                )
    return errors


def _nonempty(value: object) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _metadata_errors(prefix: str, item: dict) -> list[str]:
    errors = []
    required = {"owner", "rationale", "expiresOn", "removalIssue", "boundary"}
    missing = sorted(field for field in required if not _nonempty(item.get(field)))
    if missing:
        errors.append(f"{prefix}: missing or empty {missing}")
    issue = item.get("removalIssue")
    if _nonempty(issue) and not ISSUE_RE.fullmatch(issue.strip()):
        errors.append(f"{prefix}: removalIssue must be a valid HSE issue reference")
    try:
        if date.fromisoformat(str(item["expiresOn"])) < date.today():
            errors.append(f"{prefix}: expired on {item.get('expiresOn')}")
    except (KeyError, TypeError, ValueError):
        errors.append(f"{prefix}: expiresOn must be an ISO date")
    return errors


def check_manifest(root: Path, manifest_path: Path) -> list[str]:
    manifest = load_manifest(manifest_path)
    errors: list[str] = []
    modules = manifest.get("modules", [])
    module_ids = {module.get("id") for module in modules}
    if len(module_ids) != len(modules) or None in module_ids:
        errors.append("manifest: module ids must be unique and non-empty")
    allowed = {module["id"]: set(module.get("allowedDependencies", [])) for module in modules}
    for module in modules:
        unknown = allowed.get(module.get("id"), set()) - module_ids
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

    graph = _cmake_graph(root, manifest)
    target_entries = manifest.get("targets", [])
    target_map = {entry.get("name"): entry for entry in target_entries}
    missing_targets = sorted(graph.targets - set(target_map))
    unknown_targets = sorted(set(target_map) - graph.targets)
    errors.extend(f"target {name}: missing manifest ownership" for name in missing_targets)
    errors.extend(f"target {name}: manifest target is not declared by CMake" for name in unknown_targets)
    for target, entry in target_map.items():
        actual_links = graph.links.get(target, set())
        expected_links = set(entry.get("links", []))
        if actual_links != expected_links:
            errors.append(
                f"target {target}: manifest links {sorted(expected_links)} != CMake links {sorted(actual_links)}"
            )
        if entry.get("module") not in module_ids:
            errors.append(f"target {target}: unknown owner module {entry.get('module')}")

    for target, paths in graph.source_edges.items():
        if target not in target_map:
            continue
        owner = target_map[target].get("module")
        for path in paths:
            source_module = source_by_path.get(path)
            if source_module is None:
                errors.append(f"target {target}: source edge {path} is not a classified production source")
            elif source_module not in allowed.get(owner, set()):
                errors.append(
                    f"target {target}: source edge {owner} -> {source_module} ({path}) is not allowed"
                )

    exceptions = manifest.get("exceptions", [])
    for index, exception in enumerate(exceptions):
        prefix = f"exception {exception.get('source', '<unknown>')}[{index}]"
        errors.extend(_metadata_errors(prefix, exception))
        source = exception.get("source")
        source_module = source_by_path.get(source)
        if source_module != exception.get("fromModule"):
            errors.append(f"{prefix}: source is not owned by fromModule")
        if exception.get("toModule") not in module_ids and not str(exception.get("toModule", "")).startswith("external:"):
            errors.append(f"{prefix}: unknown toModule {exception.get('toModule')}")
        if "include" in exception and not _nonempty(exception.get("include")):
            errors.append(f"{prefix}: include must be non-empty when present")

    target_exceptions = manifest.get("targetExceptions", [])
    for index, exception in enumerate(target_exceptions):
        prefix = f"target exception {exception.get('target', '<unknown>')}[{index}]"
        errors.extend(_metadata_errors(prefix, exception))
        if exception.get("target") not in target_map:
            errors.append(f"{prefix}: target is not in the manifest")
        dependencies = exception.get("dependencies")
        if not isinstance(dependencies, list) or not dependencies or not all(_nonempty(value) for value in dependencies):
            errors.append(f"{prefix}: dependencies must be a non-empty list of names")

    includes, include_errors = _includes(root, sources)
    errors.extend(include_errors)
    actual_include_violations: list[Include] = []
    exception_hits: set[int] = set()
    for include in includes:
        target_module = include.target_module
        dependency_violation = target_module is not None and target_module not in allowed.get(include.source_module, set())
        external_kind = _external_kind(include.spelling)
        module = next((item for item in modules if item["id"] == include.source_module), {})
        external_violation = external_kind is not None and external_kind not in set(module.get("allowedExternal", []))
        if not dependency_violation and not external_violation:
            continue
        actual_include_violations.append(include)
        exception_target = target_module or f"external:{external_kind}"
        matching = [index for index, exception in enumerate(exceptions)
                    if _exception_matches(exception, include.source, include.source_module,
                                          exception_target, include.spelling)]
        if matching:
            exception_hits.update(matching)
            continue
        if dependency_violation:
            errors.append(
                f"include {include.source}: undeclared dependency {include.source_module} -> {target_module}"
            )
        if external_violation:
            errors.append(
                f"include {include.source}: {external_kind} include is outside module confinement"
            )

    for index, exception in enumerate(exceptions):
        if index not in exception_hits:
            errors.append(f"exception {exception.get('source', '<unknown>')}: does not match an actual violation")

    actual_target_violations: set[tuple[str, str]] = set()
    target_exception_hits: set[tuple[int, str]] = set()
    target_exceptions_by_target = defaultdict(list)
    for index, exception in enumerate(target_exceptions):
        target_exceptions_by_target[exception.get("target")].append((index, exception))
    for target, dependencies in graph.edges.items():
        if target not in target_map:
            continue
        from_module = target_map[target].get("module")
        for dependency in dependencies:
            if dependency not in target_map:
                continue
            to_module = target_map[dependency].get("module")
            if to_module in allowed.get(from_module, set()):
                continue
            actual_target_violations.add((target, dependency))
            matching = [index for index, exception in target_exceptions_by_target.get(target, [])
                        if _target_exception_matches(exception, target, dependency)]
            if matching:
                target_exception_hits.update((index, dependency) for index in matching)
            else:
                errors.append(f"target {target}: undeclared target dependency {from_module} -> {to_module} ({dependency})")

    for target, links in graph.links.items():
        if target not in target_map:
            continue
        module_id = target_map[target].get("module")
        module = next((item for item in modules if item["id"] == module_id), {})
        allowed_external = set(module.get("allowedExternal", []))
        for dependency in links:
            kind = {"clang-cpp": "clang", "LLVM": "llvm"}.get(dependency)
            if kind is None and ("sqlite" in dependency.lower() or dependency == "${_cidx_sqlite_link}"):
                kind = "sqlite"
            if kind is None or kind in allowed_external:
                continue
            actual_target_violations.add((target, dependency))
            matching = [index for index, exception in target_exceptions_by_target.get(target, [])
                        if _target_exception_matches(exception, target, dependency)]
            if matching:
                target_exception_hits.update((index, dependency) for index in matching)
            else:
                errors.append(f"target {target}: {kind} link is outside module confinement ({dependency})")

    for index, exception in enumerate(target_exceptions):
        dependencies = exception.get("dependencies", [])
        if any((exception.get("target"), dependency) in actual_target_violations
               and (index, dependency) in target_exception_hits for dependency in dependencies):
            continue
        errors.append(f"target exception {exception.get('target', '<unknown>')}: does not match an actual violation")

    errors.extend(_contract_errors(root, manifest))
    module_graph = {module["id"]: set(module.get("allowedDependencies", [])) - {module["id"]}
                    for module in modules}
    errors.extend(_cycle_errors(module_graph, "declared dependency"))
    errors.extend(_cycle_errors({target: values & graph.targets for target, values in graph.edges.items()},
                                "CMake target"))
    return sorted(set(errors))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args(argv)
    manifest_path = args.manifest.resolve()
    root = manifest_path.parent.parent
    errors = check_manifest(root, manifest_path)
    if errors:
        print("architecture check failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"architecture check passed: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
