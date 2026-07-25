#!/usr/bin/env python3
"""Generate the deterministic HSE-71 self-host architecture-policy report.

This is the "self-hosting" half of the CIDX architecture contract: it
indexes CIDX's own repository (via a separately built `index.db`, see
scripts/self_host_index.sh) and cross-references the *resolved* semantic
call graph against architecture/cidx-module-manifest.json (HSE-58) and
architecture/cidx-self-host-policy.json (HSE-71).

The bootstrap checks (scripts/check_architecture.py,
scripts/check_platform_contract.py) remain the checker for facts CIDX
cannot yet derive soundly from its own index (CMake target graph, Python
imports, raw #include text). This script adds an independent, second
signal computed from the actual resolved 'calls'/'dispatch_calls' edges of
a self-index, so the two checkers can be compared for agreement on the
facts they both cover (module-to-module coupling).

Deliberately stdlib + sqlite3 only, matching scripts/check_architecture.py's
"stdlib-only" policy so the report can run without any project dependency
beyond a built index.db.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sqlite3
import subprocess
import sys
from dataclasses import dataclass
from datetime import date, datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.check_architecture import check_manifest, load_manifest, _sources, _includes  # noqa: E402
from scripts.check_platform_contract import validate_contract  # noqa: E402

REPORT_FORMAT = "cidx.self-host-architecture-report/v1"
REPORT_VERSION = 1

CALL_EDGE_KINDS = {"calls", "dispatch_calls"}


def _nonempty(value: object) -> bool:
    return isinstance(value, str) and bool(value.strip())


@dataclass
class ModuleGraph:
    """Manifest-derived facts needed to interpret a resolved call graph."""

    module_of_path: dict[str, str]
    allowed: dict[str, set[str]]
    file_exceptions: set[tuple[str, str, str]]  # (source, fromModule, toModule)
    unclassified: list[str]

    def module_for(self, path: str) -> str | None:
        return self.module_of_path.get(path)

    def is_allowed(self, source_path: str, from_module: str, to_module: str) -> bool:
        if from_module == to_module:
            return True
        if to_module in self.allowed.get(from_module, set()):
            return True
        return (source_path, from_module, to_module) in self.file_exceptions


def build_module_graph(root: Path, manifest: dict) -> ModuleGraph:
    sources, errors = _sources(root, manifest)
    module_of_path = {source.path: source.module for source in sources}
    allowed = {
        module["id"]: set(module.get("allowedDependencies", []))
        for module in manifest.get("modules", [])
    }
    file_exceptions = {
        (exception["source"], exception["fromModule"], exception["toModule"])
        for exception in manifest.get("exceptions", [])
        if _nonempty(exception.get("source"))
        and _nonempty(exception.get("fromModule"))
        and _nonempty(exception.get("toModule"))
    }
    unclassified = [error for error in errors if "expected exactly one module" in error]
    return ModuleGraph(module_of_path, allowed, file_exceptions, unclassified)


@dataclass
class IndexIdentity:
    path: str
    schema_version: str | None
    catalog_version: str | None
    catalog_hash: str | None
    graph_resolved_at: str | None
    component_paths: list[str]
    file_count: int
    symbol_count: int
    edge_count: int


@dataclass
class SemanticFacts:
    """Layer-0 relations read directly from a built index.db.

    These are exactly the relations `cidx analyze` exposes to Soufflé via
    python/indexer/rules/prelude.dl (symbol, edge, edge_kind, file): the
    same tables the C++ SqliteFactProvider reads. Reading them directly
    keeps this checker dependency-free (no Soufflé binary, no built `cidx`
    executable required) while still exercising CIDX's own resolved graph.
    """

    file_path_by_id: dict[int, str]
    symbol_qual_name: dict[int, str]
    symbol_file: dict[int, int]
    symbol_line: dict[int, int]
    calls: list[tuple[int, int, int, int, int]]  # (edge_id, src, dst, line, col)


def _read_identity(conn: sqlite3.Connection, index_path: str) -> IndexIdentity:
    def meta(key: str) -> str | None:
        row = conn.execute("SELECT value FROM meta WHERE key = ?", (key,)).fetchone()
        return row[0] if row else None

    try:
        component_paths = [row[0] for row in conn.execute("SELECT path FROM component ORDER BY path")]
    except sqlite3.OperationalError:
        component_paths = []
    file_count = conn.execute("SELECT COUNT(*) FROM file").fetchone()[0]
    symbol_count = conn.execute("SELECT COUNT(*) FROM symbol").fetchone()[0]
    edge_count = conn.execute("SELECT COUNT(*) FROM edge").fetchone()[0]
    return IndexIdentity(
        path=index_path,
        schema_version=meta("schema_version"),
        catalog_version=meta("catalog_version"),
        catalog_hash=meta("catalog_hash"),
        graph_resolved_at=meta("graph_resolved_at"),
        component_paths=component_paths,
        file_count=file_count,
        symbol_count=symbol_count,
        edge_count=edge_count,
    )


def _read_semantic_facts(conn: sqlite3.Connection) -> SemanticFacts:
    file_path_by_id: dict[int, str] = {}
    for file_id, dir_path, name in conn.execute(
        "SELECT f.id, d.path, f.name FROM file f JOIN directory d ON d.id = f.directory_id"
    ):
        relative = f"{dir_path}/{name}" if dir_path else name
        file_path_by_id[file_id] = relative

    symbol_qual_name: dict[int, str] = {}
    symbol_file: dict[int, int] = {}
    symbol_line: dict[int, int] = {}
    for sym_id, qual_name, spelling, file_id, line in conn.execute(
        "SELECT id, qual_name, spelling, file_id, line FROM symbol"
    ):
        symbol_qual_name[sym_id] = qual_name or spelling
        if file_id is not None:
            symbol_file[sym_id] = file_id
        if line is not None:
            symbol_line[sym_id] = line

    call_kind_ids = {
        row[0]
        for row in conn.execute(
            f"SELECT id FROM edge_kind WHERE name IN ({','.join('?' * len(CALL_EDGE_KINDS))})",
            tuple(CALL_EDGE_KINDS),
        )
    }
    calls: list[tuple[int, int, int, int, int]] = []
    if call_kind_ids:
        placeholders = ",".join("?" * len(call_kind_ids))
        for edge_id, src_id, dst_id in conn.execute(
            f"SELECT id, src_id, dst_id FROM edge WHERE kind IN ({placeholders})",
            tuple(call_kind_ids),
        ):
            line, col = 0, 0
            site = conn.execute(
                "SELECT line, col FROM edge_site WHERE edge_id = ? ORDER BY line, col LIMIT 1",
                (edge_id,),
            ).fetchone()
            if site:
                line, col = site
            calls.append((edge_id, src_id, dst_id, line, col))
    return SemanticFacts(file_path_by_id, symbol_qual_name, symbol_file, symbol_line, calls)


@dataclass
class Witness:
    caller_symbol: str
    caller_file: str
    caller_line: int
    callee_symbol: str
    callee_file: str
    callee_line: int
    call_site_line: int
    call_site_col: int

    def as_dict(self) -> dict[str, Any]:
        return {
            "caller": {"symbol": self.caller_symbol, "file": self.caller_file, "line": self.caller_line},
            "callee": {"symbol": self.callee_symbol, "file": self.callee_file, "line": self.callee_line},
            "call_site": {"line": self.call_site_line, "col": self.call_site_col},
        }


def _witness(facts: SemanticFacts, src: int, dst: int, line: int, col: int) -> Witness | None:
    src_file_id = facts.symbol_file.get(src)
    dst_file_id = facts.symbol_file.get(dst)
    if src_file_id is None or dst_file_id is None:
        return None
    src_path = facts.file_path_by_id.get(src_file_id)
    dst_path = facts.file_path_by_id.get(dst_file_id)
    if src_path is None or dst_path is None:
        return None
    return Witness(
        caller_symbol=facts.symbol_qual_name.get(src, "?"),
        caller_file=src_path,
        caller_line=facts.symbol_line.get(src, 0),
        callee_symbol=facts.symbol_qual_name.get(dst, "?"),
        callee_file=dst_path,
        callee_line=facts.symbol_line.get(dst, 0),
        call_site_line=line,
        call_site_col=col,
    )


def find_module_boundary_violations(
    facts: SemanticFacts, graph: ModuleGraph
) -> list[dict[str, Any]]:
    """Cross-module call edges not covered by an allowed dependency or exception."""
    violations: list[dict[str, Any]] = []
    seen: set[tuple[str, str, str, str]] = set()
    for edge_id, src, dst, line, col in facts.calls:
        witness = _witness(facts, src, dst, line, col)
        if witness is None:
            continue
        from_module = graph.module_for(witness.caller_file)
        to_module = graph.module_for(witness.callee_file)
        if from_module is None or to_module is None:
            continue
        if graph.is_allowed(witness.caller_file, from_module, to_module):
            continue
        key = (witness.caller_file, from_module, to_module, witness.callee_symbol)
        if key in seen:
            continue
        seen.add(key)
        violations.append(
            {
                "fromModule": from_module,
                "toModule": to_module,
                "witness": witness.as_dict(),
            }
        )
    return sorted(violations, key=lambda item: json.dumps(item, sort_keys=True))


def find_module_cycles(facts: SemanticFacts, graph: ModuleGraph) -> list[dict[str, Any]]:
    """Cycles among modules found in the *resolved* call graph.

    Independent of the declared-dependency cycle check in
    scripts/check_architecture.py: this one only fires if real call edges
    (not just the declared allowedDependencies graph) round-trip across
    modules.
    """
    edges: dict[str, set[str]] = {}
    for edge_id, src, dst, line, col in facts.calls:
        witness = _witness(facts, src, dst, line, col)
        if witness is None:
            continue
        from_module = graph.module_for(witness.caller_file)
        to_module = graph.module_for(witness.callee_file)
        if from_module is None or to_module is None or from_module == to_module:
            continue
        edges.setdefault(from_module, set()).add(to_module)

    visiting: list[str] = []
    visited: set[str] = set()
    cycles: list[list[str]] = []

    def visit(node: str) -> None:
        if node in visiting:
            cycle = visiting[visiting.index(node):] + [node]
            if cycle not in cycles:
                cycles.append(cycle)
            return
        if node in visited:
            return
        visiting.append(node)
        for dependency in sorted(edges.get(node, ())):
            visit(dependency)
        visiting.pop()
        visited.add(node)

    for node in sorted(edges):
        visit(node)
    return [{"cycle": cycle} for cycle in cycles]


def find_legacy_facade_violations(
    facts: SemanticFacts, graph: ModuleGraph, policy: dict
) -> list[dict[str, Any]]:
    violations: list[dict[str, Any]] = []
    for facade in policy.get("legacyFacades", []):
        prefixes = tuple(facade.get("calleeQualNamePrefixes", []))
        if not prefixes:
            continue
        exempt = set(facade.get("exemptModules", []))
        baseline = {
            (entry["callerFile"], entry["calleeQualName"])
            for entry in facade.get("baseline", [])
            if _nonempty(entry.get("callerFile")) and _nonempty(entry.get("calleeQualName"))
        }
        seen: set[tuple[str, str]] = set()
        for edge_id, src, dst, line, col in facts.calls:
            witness = _witness(facts, src, dst, line, col)
            if witness is None or not witness.callee_symbol.startswith(prefixes):
                continue
            from_module = graph.module_for(witness.caller_file)
            to_module = graph.module_for(witness.callee_file)
            if from_module is None or to_module is None or from_module == to_module:
                continue
            if from_module in exempt:
                continue
            key = (witness.caller_file, witness.callee_symbol)
            if key in baseline or key in seen:
                continue
            seen.add(key)
            violations.append(
                {
                    "facade": facade["id"],
                    "fromModule": from_module,
                    "witness": witness.as_dict(),
                }
            )
    return sorted(violations, key=lambda item: json.dumps(item, sort_keys=True))


def check_catalog_containment(root: Path, graph: ModuleGraph, policy: dict) -> list[str]:
    """Reject a hand-authored duplicate of a generated catalog (id, name) pair."""
    catalog_guard = policy.get("catalogGuard", {})
    generated = {str(Path(path).as_posix()) for path in catalog_guard.get("generatedOutputs", [])}
    catalog_source = json.loads((root / "catalogs/core.json").read_text(encoding="utf-8"))
    pairs: list[tuple[int, str]] = []
    for group in catalog_guard.get("guardedGroups", []):
        for row in catalog_source.get(group, []):
            pairs.append((row["id"], row["name"]))

    errors: list[str] = []
    patterns = [
        re.compile(r"\(\s*" + str(id_) + r"\s*,\s*\"" + re.escape(name) + r"\"")
        for id_, name in pairs
    ] + [
        re.compile(r"\"" + re.escape(name) + r"\"\s*:\s*" + str(id_))
        for id_, name in pairs
    ]
    for path in sorted(graph.module_of_path):
        if path in generated:
            continue
        suffix = Path(path).suffix
        if suffix not in {".cpp", ".hpp", ".h", ".cc", ".cxx", ".py", ".dl"}:
            continue
        text = (root / path).read_text(encoding="utf-8", errors="replace")
        if "Generated by scripts/generate_catalogs.py" in text:
            errors.append(f"catalog guard {path}: generated-catalog marker found outside generatedOutputs")
            continue
        for pattern in patterns:
            if pattern.search(text):
                errors.append(f"catalog guard {path}: duplicates a generated catalog declaration ({pattern.pattern})")
    return sorted(set(errors))


ISSUE_RE = re.compile(r"HSE-\d+\Z")


def _metadata_errors(prefix: str, item: dict) -> list[str]:
    """Same required-field/expiry policy as check_architecture.py's manifest
    exceptions, applied to a legacy-facade baseline entry: an exception is a
    reviewed migration item, not a silent grandfather clause."""
    errors: list[str] = []
    required = {"owner", "rationale", "expiresOn", "removalIssue"}
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


def check_policy_metadata(policy: dict) -> list[str]:
    """Every legacy-facade baseline entry needs owner/rationale/expiry/issue,
    exactly like a manifest exception (ADR-011's review policy) -- a baseline
    entry is a tracked migration debt item, not a permanent bypass."""
    errors: list[str] = []
    for facade in policy.get("legacyFacades", []):
        facade_id = facade.get("id", "<unknown>")
        for index, entry in enumerate(facade.get("baseline", [])):
            prefix = f"legacy facade {facade_id} baseline[{index}] ({entry.get('callerFile', '<unknown>')})"
            errors.extend(_metadata_errors(prefix, entry))
    return sorted(errors)


def _git_revision(root: Path) -> str | None:
    try:
        return subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def _package_hash(manifest_path: Path, policy_path: Path) -> str:
    """Deterministic hash of the exact policy/checker sources this run used.

    Hashes the manifest and policy files actually passed in (so a mutation
    test's synthetic fixture is hashed honestly) plus this script and its
    sibling bootstrap checkers, which always live next to it regardless of
    which repository root is being analyzed.
    """
    script_dir = Path(__file__).resolve().parent
    digest = hashlib.sha256()
    for path in (
        manifest_path,
        policy_path,
        script_dir / "check_architecture.py",
        script_dir / "check_platform_contract.py",
        script_dir / "self_host_architecture_report.py",
    ):
        digest.update(str(path).encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()


def generate_report(
    root: Path,
    manifest_path: Path,
    policy_path: Path,
    index_path: Path,
    expected_repo_root: str | None,
) -> dict[str, Any]:
    manifest = load_manifest(manifest_path)
    policy = json.loads(policy_path.read_text(encoding="utf-8"))
    graph = build_module_graph(root, manifest)

    bootstrap_errors = check_manifest(root, manifest_path)
    platform_contract_path = root / "spec/platform/architecture.json"
    platform_errors: list[str] = []
    if platform_contract_path.exists():
        platform_contract = json.loads(platform_contract_path.read_text(encoding="utf-8"))
        platform_errors = validate_contract(platform_contract, manifest)
    catalog_errors = check_catalog_containment(root, graph, policy)
    policy_metadata_errors = check_policy_metadata(policy)

    conn = sqlite3.connect(f"file:{index_path}?mode=ro", uri=True)
    conn.row_factory = None
    try:
        identity = _read_identity(conn, str(index_path))
        facts = _read_semantic_facts(conn)
    finally:
        conn.close()

    identity_issues: list[str] = []
    if expected_repo_root is not None:
        normalized_expected = str(Path(expected_repo_root).resolve())
        normalized_actual = {str(Path(path).resolve()) for path in identity.component_paths if path}
        if normalized_actual and normalized_expected not in normalized_actual:
            identity_issues.append(
                f"index component path {sorted(normalized_actual)} does not match --repo-root {normalized_expected}"
            )
    if identity.graph_resolved_at is None:
        identity_issues.append("index has not run resolve; entity/derived facts are unavailable (graph_resolved_at unset)")

    module_boundary_violations = find_module_boundary_violations(facts, graph)
    module_cycles = find_module_cycles(facts, graph)
    legacy_facade_violations = find_legacy_facade_violations(facts, graph, policy)

    semantic_findings_count = (
        len(module_boundary_violations) + len(module_cycles) + len(legacy_facade_violations)
    )

    # Cross-check: the bootstrap include scan and the semantic call-graph
    # pass only "overlap" -- and so must agree -- on a (caller file, callee
    # module) pair where BOTH channels have actual evidence: a textual
    # #include (what the bootstrap pass reads) AND at least one resolved
    # call edge into that module (what the semantic pass reads). A
    # textual include with no resolved call, or a resolved call with no
    # textual include, is real evidence only one pass could ever have seen
    # and is not compared here.
    sources, _source_errors = _sources(root, manifest)
    includes, _include_errors = _includes(root, sources)
    textual_include_pairs = {
        (include.source, include.target_module)
        for include in includes
        if include.target_module is not None
    }
    called_module_pairs: dict[tuple[str, str], list[str]] = {}
    for edge_id, src, dst, line, col in facts.calls:
        witness = _witness(facts, src, dst, line, col)
        if witness is None:
            continue
        from_module = graph.module_for(witness.caller_file)
        to_module = graph.module_for(witness.callee_file)
        if from_module is None or to_module is None or from_module == to_module:
            continue
        called_module_pairs.setdefault((witness.caller_file, to_module), []).append(witness.callee_symbol)

    semantic_violation_pairs = {
        (violation["witness"]["caller"]["file"], violation["toModule"]) for violation in module_boundary_violations
    }
    disagreements = []
    for caller_file, to_module in sorted(textual_include_pairs & set(called_module_pairs)):
        from_module = graph.module_for(caller_file)
        if from_module is None or from_module == to_module:
            continue
        if to_module in graph.allowed.get(from_module, set()):
            continue  # both passes agree it is architecturally allowed; nothing to compare
        bootstrap_flagged = f"include {caller_file}: undeclared dependency {from_module} -> {to_module}" in bootstrap_errors
        semantic_flagged = (caller_file, to_module) in semantic_violation_pairs
        if bootstrap_flagged != semantic_flagged:
            disagreements.append(
                {
                    "callerFile": caller_file,
                    "toModule": to_module,
                    "bootstrapFlagged": bootstrap_flagged,
                    "semanticFlagged": semantic_flagged,
                    "reason": (
                        "the bootstrap include scan and the semantic call-graph pass both had "
                        "evidence for this exact (file, module) pair but disagreed on whether it "
                        "is a violation; this points at an exception-matching or manifest gap in "
                        "one of the two checkers"
                    ),
                }
            )

    unresolved_limitations = [
        "Virtual/dispatch calls through an unresolved base-class pointer are not attributed "
        "to a concrete callee and cannot be checked by this pass; absence of a violation for "
        "such a call site is not proof of compliance.",
        "Calls made only through function pointers or std::function are not captured as "
        "'calls'/'dispatch_calls' edges and are outside this pass's evidence.",
        "The catalog duplication guard is a textual pattern match, not a semantic check; it "
        "can miss a duplicate expressed through an unusual literal syntax.",
    ]
    if not facts.calls:
        unresolved_limitations.append(
            "The self-index produced zero 'calls'/'dispatch_calls' edges; the semantic pass "
            "ran but has no positive evidence and its findings must be treated as incomplete, "
            "not as a clean bill of health."
        )

    all_errors = bootstrap_errors + platform_errors + catalog_errors + policy_metadata_errors
    status = "fail" if (all_errors or module_boundary_violations or legacy_facade_violations or module_cycles or identity_issues) else "pass"

    report = {
        "format": REPORT_FORMAT,
        "reportVersion": REPORT_VERSION,
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "repository": manifest.get("repository", "husams/cpp-indexer"),
        "sourceRevision": _git_revision(root),
        "config": {
            "manifestPath": str(manifest_path.relative_to(root)) if manifest_path.is_relative_to(root) else str(manifest_path),
            "manifestVersion": manifest.get("manifestVersion"),
            "policyPath": str(policy_path.relative_to(root)) if policy_path.is_relative_to(root) else str(policy_path),
            "policyVersion": policy.get("policyVersion"),
            "packageHash": _package_hash(manifest_path, policy_path),
        },
        "index": {
            "path": str(index_path),
            "schemaVersion": identity.schema_version,
            "catalogVersion": identity.catalog_version,
            "catalogHash": identity.catalog_hash,
            "graphResolvedAt": identity.graph_resolved_at,
            "componentPaths": identity.component_paths,
            "fileCount": identity.file_count,
            "symbolCount": identity.symbol_count,
            "edgeCount": identity.edge_count,
        },
        "completeness": {
            "bootstrap": "complete",
            "semantic": "partial" if (identity_issues or not facts.calls) else "complete",
            "identityIssues": identity_issues,
        },
        "findings": {
            "bootstrap": sorted(bootstrap_errors),
            "platformContract": sorted(platform_errors),
            "catalogGuard": catalog_errors,
            "policyMetadata": policy_metadata_errors,
            "moduleBoundaryViolations": module_boundary_violations,
            "moduleCycles": module_cycles,
            "legacyFacadeViolations": legacy_facade_violations,
        },
        "unclassifiedSources": graph.unclassified,
        "crossCheck": {
            "semanticFindingsChecked": semantic_findings_count,
            "disagreements": disagreements,
            "status": "blocked" if disagreements else "ok",
        },
        "unresolvedLimitations": unresolved_limitations,
        "status": status,
    }
    if disagreements:
        report["status"] = "fail"
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=ROOT)
    parser.add_argument("--manifest", type=Path, default=ROOT / "architecture/cidx-module-manifest.json")
    parser.add_argument("--policy", type=Path, default=ROOT / "architecture/cidx-self-host-policy.json")
    parser.add_argument("--index-db", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument(
        "--expected-repo-root",
        type=str,
        default=None,
        help="Fail closed (report an identity issue) unless the index's component path matches this.",
    )
    args = parser.parse_args(argv)

    if not args.index_db.is_file():
        print(f"error: index not found at {args.index_db}", file=sys.stderr)
        return 2

    report = generate_report(
        args.repo_root.resolve(),
        args.manifest.resolve(),
        args.policy.resolve(),
        args.index_db.resolve(),
        args.expected_repo_root,
    )
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
