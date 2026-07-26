#!/usr/bin/env python3
"""Generate the deterministic HSE-71 self-host architecture-policy report.

This is the "self-hosting" half of the CIDX architecture contract: it
indexes CIDX's own repository (via a separately built, throwaway `index.db`,
see scripts/self_host_index.sh) and cross-references the *resolved*
semantic call graph against architecture/cidx-module-manifest.json (HSE-58)
and architecture/cidx-self-host-policy.json (HSE-71).

The bootstrap checks (scripts/check_architecture.py,
scripts/check_platform_contract.py) remain the checker for facts CIDX
cannot yet derive soundly from its own index (CMake target graph, Python
imports, raw #include text). This script adds an independent, second
signal computed from the actual resolved 'calls'/'dispatch_calls' edges of
a self-index, so the two checkers can be compared for agreement on the
facts they both cover (module-to-module coupling).

Every symbol/call-edge/call-site fact -- the graph-sufficient data this
policy is about -- is read exclusively through python/indexer/queryplan.py
(HSE-66's CXQ QueryPlan), never a hand-rolled SQL reader: HSE-71 itself
forbids a new command-specific SQL public path once the graph suffices, and
symbols/calls/sites are exactly that. A small number of *index identity and
coverage* bookkeeping reads (meta, file/diagnostic counts) remain direct
SQL, matching how HSE-32's own `Storage.index_identity()` and this script's
own `_read_identity()` already work: identity/coverage integrity is a
different concern from semantic policy evaluation, not a bypass of it.
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
sys.path.insert(0, str(ROOT / "python"))

from scripts.check_architecture import check_manifest, load_manifest, _sources, _includes  # noqa: E402
from scripts.check_platform_contract import validate_contract  # noqa: E402

from indexer.storage import Storage  # noqa: E402
from indexer.queryplan import (  # noqa: E402
    ENUMERATE_BUDGET, Executor, codebase, limit, nodes, select, sites, start, view,
)
from indexer._version import DATABASE_SCHEMA_VERSION  # noqa: E402
from indexer.generated_catalog import CATALOG_HASH as EXPECTED_CATALOG_HASH  # noqa: E402

REPORT_FORMAT = "cidx.self-host-architecture-report/v1"
REPORT_VERSION = 1

CALL_EDGE_KINDS = {"calls", "dispatch_calls"}
# Only actual translation units are expected to appear as indexed `file`
# rows; headers are only ever indirectly present via #include, so their
# absence from the file table is not, by itself, a coverage gap.
TU_SUFFIXES = {".cpp", ".cc", ".cxx"}
ISSUE_RE = re.compile(r"HSE-\d+\Z")


def _nonempty(value: object) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _stable_label(path: Path, root: Path) -> str:
    """A checkout-location-independent identity label for `path`.

    Two clean checkouts of the same commit live at different absolute
    directories; hashing `str(path)` would make packageHash reflect *where*
    the checkout happens to sit rather than *what* it contains. Labelling by
    the path's position relative to `root` (falling back to just the
    filename when it isn't under `root`, e.g. a synthetic test fixture)
    keeps the label -- and therefore the hash -- identical across runners
    and worktrees for byte-identical content.
    """
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.name


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


def _read_identity(conn: sqlite3.Connection, index_path: str) -> IndexIdentity:
    """Index provenance bookkeeping: `meta`/`component`/count reads only.

    Deliberately not routed through QueryPlan -- this is index *identity*
    (what index is this, what does it claim to be), not a semantic/policy
    fact about the codebase, exactly as HSE-32's own
    `Storage.index_identity()` treats it as a distinct concern from graph
    queries.
    """

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


@dataclass
class SemanticFacts:
    """Symbols, call edges, and every call site -- read exclusively through
    QueryPlan/CXQ (HSE-66), never a hand-rolled SQL reader. `calls` carries
    one row PER SITE (an edge with three call sites yields three rows), so
    no evidence is collapsed away before the policy checks see it.
    """

    symbol_name: dict[int, str]
    symbol_file: dict[int, str]
    symbol_line: dict[int, int]
    calls: list[tuple[int, int, int, int, int]]  # edge_id, src_id, dst_id, site_line, site_col
    truncated: bool
    unwitnessed_call_sites: int = 0


def _run_all_pages(
    executor: Executor, plan: Any, cursor_field_index: int
) -> tuple[list[tuple[Any, ...]], bool]:
    """Run `plan` to genuine completion, not just its first page.

    A single `Executor.run()` call is hard-capped at ENUMERATE_BUDGET /
    TRAVERSE_NODE_BUDGET rows -- a real ceiling on that ONE call, enforced
    inside the enumeration's own SQL before `limit()` ever runs, so raising
    the `limit()` argument can never lift it (see
    python/indexer/queryplan.py's `_enumerate`). To read every row rather
    than silently accepting a capped prefix as if it were the whole result,
    this re-runs the SAME plan with QueryPlan's `after_id` pagination cursor
    (also added for this purpose), advancing it to the strictly-increasing
    id in `cursor_field_index` of the last row of each full page, until a
    page comes back short of a full page -- the only way a plan can end.
    `cursor_field_index` must name a field that is ascending and duplicate-
    free across the whole result (true for both queries below: "id" for the
    symbol enumeration, "edge_id" for the site enumeration, each the
    query's own real, single-column, strictly-ordered enumeration key).
    """
    rows: list[tuple[Any, ...]] = []
    after_id: int | None = None
    while True:
        page = executor.run(plan, after_id=after_id)
        rows.extend(page.rows)
        if len(page.rows) < ENUMERATE_BUDGET:
            return rows, False
        next_after_id = page.rows[-1][cursor_field_index]
        if next_after_id is None or next_after_id == after_id:
            # Defensive: a cursor that fails to strictly advance would loop
            # forever. This can only happen if `cursor_field_index` isn't
            # actually the query's own ascending enumeration key -- a real
            # bug in the caller, not a legitimately large result -- so
            # surface it as truncated rather than hang.
            return rows, True
        after_id = next_after_id


def _read_semantic_facts(db: Storage, root: Path) -> SemanticFacts:
    executor = Executor(db)
    truncated = False

    # Every query below explicitly asks for up to ENUMERATE_BUDGET rows per
    # page (so QueryPlan's *artificial* DEFAULT_RESULT_CAP of 1,000 -- which
    # applies whenever a plan never uses `limit()` -- can never truncate
    # evidence before the *real* per-call budget would), and is run to
    # completion via `_run_all_pages` rather than accepted as a single,
    # possibly-capped page: `truncated` is only ever True here if
    # `_run_all_pages` itself gave up (its cursor-advance defensive check),
    # never merely because the underlying table has more than
    # ENUMERATE_BUDGET rows.
    symbol_plan = (
        start(codebase()) | nodes() | select(["id", "name", "file", "line"])
        | limit(ENUMERATE_BUDGET)
    ).plan
    symbol_rows, symbol_truncated = _run_all_pages(executor, symbol_plan, cursor_field_index=0)
    truncated = truncated or symbol_truncated
    symbol_name: dict[int, str] = {}
    symbol_file: dict[int, str] = {}
    symbol_line: dict[int, int] = {}
    root_resolved = root.resolve()
    for sym_id, name, file_path, line in symbol_rows:
        symbol_name[sym_id] = name
        if file_path is not None:
            # QueryPlan resolves "file" to an ABSOLUTE filesystem path
            # (Storage.file_abs_path); the manifest classifies sources by
            # path RELATIVE TO THE REPO ROOT, so convert here once rather
            # than at every module-classification call site. A path outside
            # `root` (e.g. a system header) has no manifest module and is
            # left unresolved -- never silently mapped to the wrong file.
            try:
                symbol_file[sym_id] = Path(file_path).resolve().relative_to(root_resolved).as_posix()
            except ValueError:
                pass
        if line is not None:
            symbol_line[sym_id] = line

    # The "site" view exposes the owning edge's own stable src_id/dst_id
    # directly (a correlated subquery against edge.id -- see
    # python/indexer/queryplan.py's `_typed_column`), so every call site's
    # caller, callee, relation, and exact location come from ONE public
    # QueryPlan query. No separate "edge" view round-trip, and no private
    # `db._conn` correlation, is needed to recover src/dst. "edge_id" is the
    # underlying edge's own id (the query's cursor key), paginated the same
    # way as the symbol query above.
    site_plan = (
        start(codebase()) | view("edge") | nodes() | sites()
        | select(["edge_id", "src_id", "dst_id", "line", "col", "relation"])
        | limit(ENUMERATE_BUDGET)
    ).plan
    site_rows, site_truncated = _run_all_pages(executor, site_plan, cursor_field_index=0)
    truncated = truncated or site_truncated
    calls: list[tuple[int, int, int, int, int]] = []
    for edge_id, src_id, dst_id, line, col, relation in site_rows:
        if relation not in CALL_EDGE_KINDS:
            continue
        calls.append((edge_id, src_id, dst_id, line or 0, col or 0))

    # A symbol whose file_id was NULLed by an ON DELETE SET NULL cascade
    # (its file row was deleted) is indistinguishable at the symbol level
    # from a symbol that legitimately never had a file (e.g. a builtin) --
    # but a CALL SITE through it becomes silently unwitnessable either way.
    # Count that directly here so the report can fail closed on it instead
    # of the caller/callee simply vanishing from every finding.
    unwitnessed = sum(
        1 for _edge_id, src_id, dst_id, _line, _col in calls
        if src_id not in symbol_file or dst_id not in symbol_file
    )
    return SemanticFacts(symbol_name, symbol_file, symbol_line, calls, truncated, unwitnessed)


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
    src_path = facts.symbol_file.get(src)
    dst_path = facts.symbol_file.get(dst)
    if src_path is None or dst_path is None:
        return None
    return Witness(
        caller_symbol=facts.symbol_name.get(src, "?"),
        caller_file=src_path,
        caller_line=facts.symbol_line.get(src, 0),
        callee_symbol=facts.symbol_name.get(dst, "?"),
        callee_file=dst_path,
        callee_line=facts.symbol_line.get(dst, 0),
        call_site_line=line,
        call_site_col=col,
    )


def find_module_boundary_violations(
    facts: SemanticFacts, graph: ModuleGraph
) -> list[dict[str, Any]]:
    """Cross-module call edges not covered by an allowed dependency or exception.

    Violation identity includes the exact call-SITE (line, col), not just
    (caller_file, from_module, to_module, callee_symbol): the same caller
    calling the same disallowed callee from three different places in the
    source is three distinct pieces of evidence, and collapsing them to one
    finding would silently discard the other two -- exactly the kind of
    evidence loss this report exists to prevent. This matches
    `find_legacy_facade_violations`'s own per-site identity below; the two
    checkers must not disagree about what a violation is.
    """
    violations: list[dict[str, Any]] = []
    seen: set[tuple[str, str, str, str, int, int]] = set()
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
        key = (
            witness.caller_file, from_module, to_module, witness.callee_symbol,
            witness.call_site_line, witness.call_site_col,
        )
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
    """Every call SITE into a legacy facade is checked independently: the
    baseline/violation identity is (callerFile, calleeQualName, siteLine,
    siteCol), not just (callerFile, calleeQualName). A file that already has
    one baselined call into a facade does not thereby grandfather a second,
    later call site added anywhere in that same file.
    """
    violations: list[dict[str, Any]] = []
    for facade in policy.get("legacyFacades", []):
        prefixes = tuple(facade.get("calleeQualNamePrefixes", []))
        if not prefixes:
            continue
        exempt = set(facade.get("exemptModules", []))
        baseline = {
            (entry["callerFile"], entry["calleeQualName"], entry.get("line"), entry.get("col"))
            for entry in facade.get("baseline", [])
            if _nonempty(entry.get("callerFile")) and _nonempty(entry.get("calleeQualName"))
        }
        seen: set[tuple[str, str, int, int]] = set()
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
            key = (
                witness.caller_file,
                witness.callee_symbol,
                witness.call_site_line,
                witness.call_site_col,
            )
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
    # `(` for a paren-call construction (std::pair(8, "function")) and `{`
    # for ordinary brace-init (std::pair{8, "function"}, {8, "function"} in
    # an aggregate/array) are both accepted ways to redeclare the same pair
    # in C++; catching only one lets the other through undetected.
    patterns = [
        re.compile(r"[({]\s*" + str(id_) + r"\s*,\s*\"" + re.escape(name) + r"\"")
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


def _metadata_errors(prefix: str, item: dict, *, require_site: bool = False) -> list[str]:
    """Same required-field/expiry policy as check_architecture.py's manifest
    exceptions, applied to a legacy-facade baseline entry: an exception is a
    reviewed migration item, not a silent grandfather clause. When
    `require_site`, `line`/`col` are required too, so a baseline entry can
    only ever grandfather the exact call site it names -- never every call
    site at the same (file, callee) pair.
    """
    errors: list[str] = []
    required = {"owner", "rationale", "expiresOn", "removalIssue"}
    missing = sorted(field for field in required if not _nonempty(item.get(field)))
    if missing:
        errors.append(f"{prefix}: missing or empty {missing}")
    if require_site:
        missing_site = sorted(
            field for field in ("line", "col") if not isinstance(item.get(field), int)
        )
        if missing_site:
            errors.append(
                f"{prefix}: missing or non-integer {missing_site}; a baseline entry "
                "must pin an exact call site, not every call from this file to this callee"
            )
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
    """Every legacy-facade baseline entry needs owner/rationale/expiry/issue
    plus an exact call-site line/col, exactly like a manifest exception
    (ADR-011's review policy) -- a baseline entry is a tracked migration
    debt item pinned to one call site, not a permanent (file, callee) bypass."""
    errors: list[str] = []
    for facade in policy.get("legacyFacades", []):
        facade_id = facade.get("id", "<unknown>")
        for index, entry in enumerate(facade.get("baseline", [])):
            prefix = f"legacy facade {facade_id} baseline[{index}] ({entry.get('callerFile', '<unknown>')})"
            errors.extend(_metadata_errors(prefix, entry, require_site=True))
    return sorted(errors)


def _portable_source_hash(root: Path, conn: sqlite3.Connection, db: Storage) -> str:
    """A checkout-location-independent content hash of every indexed file.

    HSE-32's own `Storage.index_identity()` keys its source_fingerprint by
    each file's (component_name, **component_path**, ...) tuple --
    `component_path` is an ABSOLUTE filesystem path, by design, since that
    mechanism's job is detecting drift within the SAME checkout over time,
    not comparing two different checkouts. Two byte-identical clean
    checkouts at two different absolute paths therefore get two different
    `source_fingerprint`s from that mechanism -- expected for HSE-32's own
    purpose, but not what a report-reproducibility check wants. This hash
    is keyed by REPO-RELATIVE path (via `_stable_label`) plus content, so
    it is the same for byte-identical checkouts regardless of where either
    one lives on disk.
    """
    digest = hashlib.sha256()
    rows = sorted(
        (f"{dir_path}/{name}" if dir_path else name, file_id)
        for file_id, dir_path, name in conn.execute(
            "SELECT f.id, d.path, f.name FROM file f JOIN directory d ON d.id = f.directory_id"
        )
    )
    for relative_path, file_id in rows:
        abs_path = db.file_abs_path(file_id)
        content_hash = _md5_file(Path(abs_path)) if abs_path else None
        digest.update(relative_path.encode())
        digest.update(b"\0")
        digest.update((content_hash or "<unreadable>").encode())
        digest.update(b"\n")
    return digest.hexdigest()


def _md5_file(path: Path) -> str | None:
    try:
        return hashlib.md5(path.read_bytes()).hexdigest()
    except OSError:
        return None


def check_index_coverage(
    root: Path,
    manifest: dict,
    db: Storage,
    facts: SemanticFacts,
) -> tuple[list[str], dict[str, Any]]:
    """Fail closed on index coverage.

    A self-index that is missing an expected translation unit, has a
    dangling file reference, recorded a fatal parse diagnostic, was built
    against a stale schema/catalog/source snapshot, has an unrecorded build
    configuration, or has an untraversable enumeration must never be
    reported as complete evidence -- absence of a violation proves nothing
    when the underlying coverage itself is not verified. These are index
    *identity/coverage* facts (counts over meta/file/diagnostic, plus
    HSE-32's own `Storage.index_identity()`), not semantic policy facts, so
    they are read directly rather than through QueryPlan, exactly like
    `_read_identity` above.
    """
    conn = db._conn
    issues: list[str] = []

    # HSE-32 source/config/source-snapshot identity: index_identity() reads
    # the source_revision/source_fingerprint/index_config_fingerprint this
    # index stamped at build time (a content hash of every indexed file,
    # NOT a git SHA -- see stamp_index_identity()) and RE-COMPUTES the same
    # fingerprint from the files on disk right now, so "current" is a real,
    # live re-verification against this checkout, not a recorded claim taken
    # on faith.
    identity_info = db.index_identity()
    if identity_info.freshness != "current":
        issues.append(
            f"index source/config identity is {identity_info.freshness!r} "
            "(HSE-32 index_identity()): the self-index's recorded "
            "source_fingerprint/index_config_fingerprint do not match this "
            "checkout's current files and build configuration, or the "
            "required identity metadata was never stamped; this is not "
            "verified evidence of a current source/build snapshot"
        )

    # Build/TU descriptor identity: an indexed file with no recorded
    # compile_options was never given real build flags, so its facts cannot
    # be tied to a verifiable build configuration.
    # Scoped to actual translation units: a header only ever appears via
    # #include and legitimately has no compile_options of its own (it was
    # never separately compiled), so checking every indexed row here would
    # misreport every header as a build-identity gap.
    tu_suffix_placeholders = " OR ".join("f.name LIKE ?" for _ in TU_SUFFIXES)
    missing_build_config = conn.execute(
        f"SELECT COUNT(*) FROM file f WHERE f.indexed = 1 "
        f"AND f.compile_options IS NULL AND ({tu_suffix_placeholders})",
        tuple(f"%{suffix}" for suffix in TU_SUFFIXES),
    ).fetchone()[0]
    if missing_build_config:
        issues.append(
            f"{missing_build_config} indexed translation unit(s) have no "
            "recorded compile_options; their build identity cannot be "
            "verified"
        )

    dangling = conn.execute(
        "SELECT COUNT(*) FROM symbol WHERE file_id IS NOT NULL "
        "AND file_id NOT IN (SELECT id FROM file)"
    ).fetchone()[0]
    if dangling:
        issues.append(
            f"{dangling} symbol(s) reference a file_id with no matching file row; "
            "any call edge through them cannot be witnessed by this pass and must "
            "not be read as evidence of compliance"
        )
    if facts.unwitnessed_call_sites:
        # `file` has ON DELETE SET NULL from `symbol`, so a deleted file row
        # NULLs the referencing symbol's file_id rather than leaving a
        # dangling id -- the query above alone would miss it. Either way the
        # effect is the same: a call site's caller or callee has no
        # resolvable file, so it silently drops out of every finding unless
        # this is surfaced explicitly.
        issues.append(
            f"{facts.unwitnessed_call_sites} call site(s) reference a caller or "
            "callee symbol with no resolvable file (deleted file row or "
            "unresolved symbol); they cannot be witnessed and must not be read "
            "as evidence of compliance"
        )

    sources, _ = _sources(root, manifest)
    expected_tus = {source.path for source in sources if Path(source.path).suffix in TU_SUFFIXES}
    indexed_paths = {
        f"{dir_path}/{name}" if dir_path else name
        for dir_path, name in conn.execute(
            "SELECT d.path, f.name FROM file f JOIN directory d ON d.id = f.directory_id"
        )
    }
    missing_tus = sorted(expected_tus - indexed_paths)
    if missing_tus:
        preview = missing_tus[:10]
        ellipsis = ", ..." if len(missing_tus) > 10 else ""
        issues.append(
            f"{len(missing_tus)} expected translation unit(s) are not present in "
            f"the index: {preview}{ellipsis}"
        )

    pending = conn.execute("SELECT COUNT(*) FROM file WHERE indexed = 0").fetchone()[0]
    if pending:
        issues.append(f"{pending} file(s) are registered but not yet indexed (indexed=0)")

    try:
        failed = conn.execute(
            "SELECT COUNT(DISTINCT file_id) FROM diagnostic WHERE severity >= 3"
        ).fetchone()[0]
    except sqlite3.OperationalError:
        failed = 0
    if failed:
        issues.append(
            f"{failed} file(s) have a fatal/error diagnostic recorded during "
            "indexing; their facts are not reliable evidence"
        )

    meta = {key: value for key, value in conn.execute("SELECT key, value FROM meta")}
    schema_version = meta.get("schema_version")
    if schema_version is not None and str(schema_version) != str(DATABASE_SCHEMA_VERSION):
        issues.append(
            f"index schema_version is {schema_version!r}, this checkout's own "
            f"source expects {DATABASE_SCHEMA_VERSION}; the self-index is stale "
            "relative to the source tree it is meant to describe"
        )
    catalog_hash = meta.get("catalog_hash")
    if catalog_hash is not None and catalog_hash != EXPECTED_CATALOG_HASH:
        issues.append(
            "index catalog_hash does not match this checkout's generated catalog; "
            "the self-index was built against a different catalog version"
        )

    if facts.truncated:
        issues.append(
            "the bulk symbol/edge/site enumeration was truncated by QueryPlan's "
            "traversal budget; this report's semantic findings are incomplete and "
            "must not be treated as a clean bill of health"
        )

    coverage = {
        "expectedTranslationUnits": len(expected_tus),
        "missingTranslationUnits": missing_tus,
        "danglingSymbolFileReferences": dangling,
        "unwitnessedCallSites": facts.unwitnessed_call_sites,
        "pendingFiles": pending,
        "filesWithFatalDiagnostics": failed,
        "enumerationTruncated": facts.truncated,
        "missingBuildConfig": missing_build_config,
        # Only `freshness` is surfaced here: HSE-32's own
        # `source_fingerprint`/`index_config_fingerprint`/`source_revision`
        # are keyed by ABSOLUTE `component_path` by design (see
        # `_portable_source_hash` above), so they are legitimately different
        # across two byte-identical checkouts at different absolute paths.
        # Reporting them here would make this report non-reproducible for
        # a fact this report does not actually depend on -- `freshness` is
        # the live re-verified signal this report needs; the raw
        # fingerprints remain available via `db.index_identity()` itself
        # for anyone debugging HSE-32 drift directly.
        "indexIdentity": {"freshness": identity_info.freshness},
        # Checkout-location-independent: same content, same hash, no matter
        # where either checkout lives on disk.
        "sourceContentHash": _portable_source_hash(root, conn, db),
    }
    return issues, coverage


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


def _package_hash(root: Path, manifest_path: Path, policy_path: Path) -> str:
    """Deterministic, checkout-location-independent hash of the exact
    policy/checker/executor sources this run used: every entry is hashed
    under a root-relative logical label, never an absolute path, so
    byte-identical sources in two different clean checkout directories (or
    worktrees) produce the same hash.

    Includes the HSE-66 QueryPlan/Storage surface this script's semantic
    reads actually execute (indexer.queryplan.Executor and every stage/
    predicate it uses, indexer.storage.Storage.index_identity()) --
    changing that surface changes what this report's findings mean, so the
    package identity must change with it, not just with the four files
    that live directly in scripts/. Also includes catalogs/core.json:
    `check_catalog_containment` reads it directly to build the guarded
    (id, name) pair list, so it literally defines what that check catches --
    editing it changes the gate's behavior and must change packageHash too.
    """
    script_dir = Path(__file__).resolve().parent
    project_root = script_dir.parent
    digest = hashlib.sha256()
    for path in (
        manifest_path,
        policy_path,
        script_dir / "check_architecture.py",
        script_dir / "check_platform_contract.py",
        script_dir / "self_host_architecture_report.py",
        project_root / "python/indexer/queryplan.py",
        project_root / "python/indexer/storage.py",
        root / "catalogs/core.json",
    ):
        label = _stable_label(path, root)
        digest.update(label.encode())
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

    # Read-only: mirrors GraphQuery's own `?mode=ro` wrapping
    # (Storage.from_connection skips __init__'s migrate/schema-write path
    # entirely), so opening this index can never mutate it -- belt and
    # braces alongside always pointing --index-db at a throwaway cache.
    raw_conn = sqlite3.connect(f"file:{index_path}?mode=ro", uri=True)
    open_error: str | None = None
    try:
        # Storage.from_connection validates the catalog identity itself and
        # raises RuntimeError on a mismatch (e.g. a stale/corrupted catalog
        # hash) rather than opening a Storage/Executor over facts it cannot
        # trust. Fail closed with a clear identity issue rather than an
        # unhandled crash -- a hard-to-open index is exactly the kind of
        # incomplete evidence this report must never present as clean.
        db = Storage.from_connection(raw_conn, str(index_path))
        identity = _read_identity(raw_conn, str(index_path))
        facts = _read_semantic_facts(db, root)
        coverage_issues, coverage = check_index_coverage(root, manifest, db, facts)
    except RuntimeError as exc:
        open_error = str(exc)
        identity = IndexIdentity(
            path=str(index_path), schema_version=None, catalog_version=None,
            catalog_hash=None, graph_resolved_at=None, component_paths=[],
            file_count=0, symbol_count=0, edge_count=0,
        )
        facts = SemanticFacts({}, {}, {}, [], truncated=False)
        coverage_issues, coverage = [f"could not open index for analysis: {open_error}"], {}
    finally:
        raw_conn.close()

    identity_issues: list[str] = list(coverage_issues)
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
    # A partial semantic pass (e.g. zero call edges, or any of the coverage/
    # identity issues above) is exactly as disqualifying as a real
    # violation: "no findings" from an incomplete pass is not evidence of
    # compliance, so `status` is derived FROM `semantic_completeness`, not
    # computed independently of it -- the two can never disagree.
    semantic_completeness = "partial" if (identity_issues or not facts.calls) else "complete"
    status = (
        "fail"
        if (
            all_errors
            or module_boundary_violations
            or legacy_facade_violations
            or module_cycles
            or identity_issues
            or semantic_completeness != "complete"
        )
        else "pass"
    )

    report = {
        "format": REPORT_FORMAT,
        "reportVersion": REPORT_VERSION,
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "repository": manifest.get("repository", "husams/cpp-indexer"),
        # NOTE: "sourceRevision" is this report-generating process's git HEAD
        # at `root` -- it says nothing about what the INDEX itself claims to
        # have been built from. Whether the index still matches this
        # checkout's current files and build configuration is
        # `index.coverage.indexIdentity.freshness` (HSE-32's own
        # `Storage.index_identity()`, re-verified live); a
        # checkout-location-independent content hash of everything the index
        # actually indexed is `index.coverage.sourceContentHash`.
        "sourceRevision": _git_revision(root),
        "config": {
            "manifestPath": str(manifest_path.relative_to(root)) if manifest_path.is_relative_to(root) else str(manifest_path),
            "manifestVersion": manifest.get("manifestVersion"),
            "policyPath": str(policy_path.relative_to(root)) if policy_path.is_relative_to(root) else str(policy_path),
            "policyVersion": policy.get("policyVersion"),
            "packageHash": _package_hash(root, manifest_path, policy_path),
        },
        "index": {
            # Relativized via `_stable_label`: the index db and the files it
            # names live outside `root` in the common case (a throwaway
            # build-directory index), so a raw absolute path would make the
            # report differ between two byte-identical checkouts even though
            # nothing about the checkout itself differs. Genuinely
            # unrelated absolute paths (e.g. an index built from a totally
            # different tree) fall back to just the file name, same as
            # `packageHash` above.
            "path": _stable_label(index_path, root),
            "schemaVersion": identity.schema_version,
            "catalogVersion": identity.catalog_version,
            "catalogHash": identity.catalog_hash,
            "graphResolvedAt": identity.graph_resolved_at,
            "componentPaths": [_stable_label(Path(p), root) for p in identity.component_paths],
            "fileCount": identity.file_count,
            "symbolCount": identity.symbol_count,
            "edgeCount": identity.edge_count,
            "coverage": coverage,
        },
        "completeness": {
            "bootstrap": "complete",
            "semantic": semantic_completeness,
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

    repo_root = args.repo_root.resolve()
    index_db = args.index_db.resolve()
    # Standing rule: this script must never open the repository's own
    # checked-in index.db, not even read-only. --index-db must always point
    # at a throwaway cache (see scripts/self_host_index.sh).
    if index_db == (repo_root / "index.db"):
        print(
            "error: --index-db must not point at the repository's checked-in "
            "index.db; build a throwaway self-index (scripts/self_host_index.sh) "
            "instead",
            file=sys.stderr,
        )
        return 2

    report = generate_report(
        repo_root,
        args.manifest.resolve(),
        args.policy.resolve(),
        index_db,
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
