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
    ENUMERATE_BUDGET, Executor, all_of, codebase, count, eq, limit, nodes, select,
    sites, start, view,
)
from indexer._version import DATABASE_SCHEMA_VERSION  # noqa: E402
from indexer.generated_catalog import CATALOG_HASH as EXPECTED_CATALOG_HASH  # noqa: E402

REPORT_FORMAT = "cidx.self-host-architecture-report/v1"
REPORT_VERSION = 1

CALL_EDGE_KINDS = {"calls", "dispatch_calls"}
# [P1-2 fix] direct-construction/destruction edges (catalogs/core.json kinds
# 10-16): `src/ast/statement_edge_visitor.cpp`'s `emit_construction_form` /
# `VisitCXXNewExpr` / `VisitCXXDeleteExpr` / `emit_factory_edge` resolve
# their destination to the RECORD being constructed/destroyed (e.g.
# `cidx::Storage`), not to a constructor/destructor symbol -- a
# `Storage db(...)` direct construction is real cross-module coupling into
# that record and must be visible to the same module-boundary/legacy-facade
# checks a `calls`/`dispatch_calls` edge would be. See `_witness()` for how
# the callee identity is presented for these kinds (the record's own name,
# not a signature-bearing symbol name, matching how a legacy-facade baseline
# entry names a direct construction: "cidx::Storage::Storage").
CONSTRUCTION_EDGE_KINDS = {
    "construct-value", "construct-temp", "construct-heap", "construct-copy",
    "construct-move", "factory-construct", "destroy",
}
COUPLING_EDGE_KINDS = CALL_EDGE_KINDS | CONSTRUCTION_EDGE_KINDS
# Only actual translation units are expected to appear as indexed `file`
# rows; headers are only ever indirectly present via #include, so their
# absence from the file table is not, by itself, a coverage gap.
TU_SUFFIXES = {".cpp", ".cc", ".cxx"}
ISSUE_RE = re.compile(r"HSE-\d+\Z")


def _nonempty(value: object) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _bare_qual_name(name: str) -> str:
    """Strip a leaf function/method's PARAMETER LIST and cv/ref qualifiers
    from a `qualified_name(...)`-shaped name (as `ast::qualified_name`,
    `src/ast/names.cpp`, renders it with `with_signature=true`), leaving the
    same shape `ast::qualified_name_bare` (`with_signature=false`) would have
    produced. This is a comparison-time normalization only -- QueryPlan's
    `symbol.name` (`COALESCE(qual_name, spelling)`) never stores the bare
    form, and this is never written back to the index.

    [P1-1 fix] Every `legacyFacades[].baseline[].calleeQualName` in
    `architecture/cidx-self-host-policy.json` is authored bare (e.g.
    `"cidx::SqliteStorageService::write"`), but a resolved witness's callee
    name carries the full C++ signature (`"cidx::SqliteStorageService::
    write(const std::string &)"`) -- the two could never compare equal, so
    no baseline entry could ever suppress a real, matching violation. Used
    only for the (callerFile, calleeQualName, line, col) baseline/dedup key
    in `find_legacy_facade_violations`; the full, signature-bearing name is
    still what a witness's `callee.symbol` displays to a human reader.

    A name with no trailing `(...)` (already bare -- e.g. the record-name
    identity `_witness()` synthesizes for a construction-kind edge) round-
    trips unchanged. An unbalanced/unexpected shape is returned unchanged
    rather than guessed at, so a parsing surprise can only ever make this
    MISS a suppression (fail closed to "still a violation"), never wrongly
    grant one.
    """
    s = name
    changed = True
    while changed:
        changed = False
        for suffix in (" const", " volatile", " &&", " &"):
            if s.endswith(suffix):
                s = s[: -len(suffix)]
                changed = True
    if not s.endswith(")"):
        return name
    depth = 0
    for i in range(len(s) - 1, -1, -1):
        if s[i] == ")":
            depth += 1
        elif s[i] == "(":
            depth -= 1
            if depth == 0:
                return s[:i]
    return name  # unbalanced parens: leave unchanged rather than guess


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
    # edge_id, src_id, dst_id, site_line, site_col, relation
    calls: list[tuple[int, int, int, int, int, str]]
    truncated: bool
    unwitnessed_call_sites: int = 0
    partial: bool = False
    unknown: bool = False


def _run_all_pages(
    executor: Executor, plan: Any, cursor_field_index: int
) -> tuple[list[tuple[Any, ...]], bool, bool, bool]:
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

    Only valid when `cursor_field_index` names a field that is itself
    ascending AND duplicate-free across the whole result -- true for the
    symbol enumeration's "id" below, since `after_id` is wired straight into
    that query's own `WHERE s.id > ?` filter (queryplan.py's `_enumerate`,
    SYMBOL_VIEW branch). It is NOT true for the site/evidence enumeration's
    "edge_id" (one edge legitimately owns many sites, so it repeats across
    rows and is not the query's real per-row key) -- that read must go
    through `_run_all_site_pages` instead, which accounts for exactly that.
    """
    rows: list[tuple[Any, ...]] = []
    after_id: int | None = None
    partial = False
    unknown = False
    while True:
        page = executor.run(plan, after_id=after_id)
        partial = partial or page.partial
        unknown = unknown or page.unknown
        rows.extend(page.rows)
        if len(page.rows) < ENUMERATE_BUDGET:
            # [PR #67 internal critic] an earlier, FULL intermediate page's
            # own `page.truncated` is not read here (and must not be): that
            # signal means only "this ONE call's own query hit its
            # ENUMERATE_BUDGET+1 over-fetch cap", which THIS SAME cursor
            # loop resolves completely by continuing to the next page --
            # accumulating it across iterations would misreport a
            # perfectly complete multi-page read as truncated. This final,
            # SHORT page's own `page.truncated` is what actually matters:
            # `cursor_field_index` covers the query's one enumeration
            # point exactly (see this function's own docstring), so if
            # THIS call still reports truncated despite returning fewer
            # than a full page, something else in the plan (not resolved
            # by this cursor) genuinely dropped evidence.
            return rows, page.truncated, partial, unknown
        next_after_id = page.rows[-1][cursor_field_index]
        if next_after_id is None or next_after_id == after_id:
            # Defensive: a cursor that fails to strictly advance would loop
            # forever. This can only happen if `cursor_field_index` isn't
            # actually the query's own ascending enumeration key -- a real
            # bug in the caller, not a legitimately large result -- so
            # surface it as truncated rather than hang.
            return rows, True, partial, unknown
        after_id = next_after_id


def _edge_site_count(
    executor: Executor, edge_id_hint: int, src_id: int, dst_id: int
) -> tuple[int, bool]:
    """The real, QueryPlan-only count of every recorded call SITE owned by
    the edge(s) from `src_id` to `dst_id`, plus whether even counting them
    completed.

    Used solely to verify pagination completeness at a page boundary (see
    `_run_all_site_pages`), never to derive a semantic finding -- it is a
    coverage/bookkeeping read exactly like `_read_identity`/
    `check_index_coverage` below, just expressed through `count()` (HSE-66's
    own scalar-result stage) instead of raw SQL, so it stays on the same
    QueryPlan-only path as every other fact in this module. Scoped with
    `nodes(...)` BEFORE `sites()` (on src_id/dst_id, both real predicate
    fields of the "edge" view) rather than a `where(...)` filter AFTER it:
    the latter would force QueryPlan to materialize and per-row-filter every
    site of every OTHER edge in the whole graph just to answer one edge's
    count. `(src_id, dst_id)` can match more than one edge if the same pair
    is connected by more than one relation kind; that only ever makes this
    count equal to or larger than the true count for the ONE edge being
    checked, so it can never wrongly report a page as complete -- at worst
    it forces an unnecessary retry, never a silently accepted evidence gap.

    [Round-6 internal critic P1] `executor.run(plan)` with no `after_id`
    enumerates the "edge" view's raw ids starting from the BEGINNING of the
    whole table, capped at ENUMERATE_BUDGET, and only THEN applies
    (src_id, dst_id) as a post-fetch filter (queryplan.py's `_enumerate`: a
    typed-view predicate is never pushed into the raw SQL scan, only applied
    to whatever the unconditional cap already admitted). Once the table has
    more than ENUMERATE_BUDGET edges, an edge whose id sorts past that
    global cutoff is invisible to the filter no matter how the predicate is
    phrased, so this silently returned `(0, True)` for a real, populated
    edge -- exactly backwards for a helper whose entire job is to tell the
    caller whether a page boundary is genuinely complete. The caller
    (`_run_all_site_pages`) always already knows `edge_id_hint`: the
    boundary edge's OWN id, the very row this count is being asked to
    verify. Passing `after_id=edge_id_hint - 1` instead of no cursor at all
    reuses QueryPlan's existing `after_id` cursor, which pushes a real
    `WHERE id > ?` into the SQL BEFORE the ENUMERATE_BUDGET cap is applied
    (see `_enumerate`'s `after_id is not None and st.view == "edge"`
    branch) -- so `edge_id_hint` itself is always the FIRST id considered
    and can never be capped out of the window, however large the edge
    table. This stays a single, QueryPlan-only, bounded round trip; no
    full-table pagination loop is needed (and would be far more expensive,
    since this helper runs at every full-page boundary).

    [P1-4 fix, round 2] The returned `truncated` flag fires on ANY codebase
    with more than ENUMERATE_BUDGET total edges remaining past `edge_id_hint`
    -- i.e. on essentially every boundary check the sparse-density case this
    module exists to handle -- because `_enumerate`'s raw fetch-then-filter
    order (see above) cannot distinguish "the whole table is large" from "our
    one guaranteed-first-row edge was affected." `_run_all_site_pages` treats
    it as advisory only, not as proof of ambiguity by itself: the returned
    `count` is exact for `edge_id_hint` regardless (it can never have been
    trimmed away), so callers should compare `count` against their own
    already-observed evidence and use `truncated` only to explain a genuine
    mismatch, never to override a match.
    """
    plan = (
        start(codebase()) | view("edge")
        | nodes(all_of([eq("src_id", src_id), eq("dst_id", dst_id)]))
        | sites() | count()
    ).plan
    result = executor.run(plan, after_id=edge_id_hint - 1)
    return result.scalar, result.truncated


def _run_all_site_pages(
    executor: Executor, plan: Any
) -> tuple[list[tuple[Any, ...]], bool, bool, bool]:
    """Page the site/evidence enumeration (edge_id, src_id, dst_id, line,
    col, relation, ...) to genuine completion.

    Unlike `_run_all_pages`'s symbol read, this plan's natural SQL order is
    (edge_id, file_id, line, col), and QueryPlan's `after_id` cursor for the
    "edge"->sites() pipeline is only ever wired at EDGE granularity (see
    queryplan.py's `_enumerate`, "edge" branch): the SITE rows this plan
    returns are not a reliable proxy for "how far did the edge cursor get" --
    an edge legitimately owns zero, one, or many sites, so counting SITE rows
    can under-count (most starkly: an ENUMERATE_BUDGET-sized window of edges
    where only one has a site at all returns a single-row page, which looks
    exactly like "genuinely almost done" even though 9,999 more edges past
    the cap were never looked at). [P1-4 fix] A prior version of this
    function inferred both "should I keep paging" and "where do I resume"
    from the site page's own row count/last row, which is wrong for exactly
    that sparse-site density -- real, cross-module violations whose edge id
    sorted past the first ENUMERATE_BUDGET-sized window were silently never
    enumerated, even though the report still (correctly, but not usefully)
    refused to claim a clean pass.

    The fix pages the EDGE-id enumeration on its own dedicated, ground-truth
    cursor (`edge_id_plan`, a plain `view("edge")|nodes()|select(["edge_id"])`
    read -- "edge_id", NOT "id": the latter is this view's portable logical
    identity, a hash the `after_id` cursor cannot page on, see `edge_id_plan`'s
    own comment above for the full story -- no `sites()` expansion, so its row
    count/truncation is a direct, unambiguous read of QueryPlan's own edge
    cursor) run with the exact same
    `after_id` as the site plan for this iteration. The two reads enumerate
    identically (same "edge" view, same cursor, same underlying SQL branch
    in `_enumerate`), so `edge_page`'s ids are always the true edge-id window
    this iteration's `page` (the site plan) was scoped to, independent of how
    many of those edges the site plan's OWN row output happened to carry.

    Within one edge-id window, a SEPARATE truncation source remains: the site
    EXPANSION's own row budget (`_expand_sites`'s `TRAVERSE_NODE_BUDGET`) can
    still cut off mid-edge if that window's edges together carry more sites
    than fit in one page. `page.truncated` reflects that (OR'd with the
    edge-cursor's own truncation, so it is checked only as an extra,
    harmless verification, never to decide whether to keep paging). Exactly
    as before, when it fires this verifies (via `_edge_site_count`) whether
    the page's last SITE row's edge is fully represented; if not, that
    edge's partial rows are dropped and the cursor rewound to just before it
    for a fresh retry, unless the entire page belongs to that one
    still-incomplete edge alone, which is provably futile to retry (see
    `_edge_site_count`'s own docstring) and is reported as truncated
    immediately with whatever partial evidence was already collected.

    Once an edge-id window's sites are safely collected, completion is
    decided ONLY from `edge_page`'s own truncation: if the edge cursor read
    fewer than ENUMERATE_BUDGET edge ids past `after_id`, every edge past the
    previous cursor position was genuinely seen (however few, even zero, of
    them carried a site), so this is genuinely done -- regardless of how
    short the resulting site page was. Otherwise the cursor advances to the
    edge window's own true last id (`edge_page`'s last row, never a SITE
    row's edge_id) and another full-budget window is read.
    """
    # [P1-4 fix] MUST select "edge_id", never "id": the "edge" view's "id"
    # field is QueryPlan's PORTABLE logical identity (a hash of the edge's
    # natural key -- see docs/query-plan.md, "physical SQLite row ids are
    # implementation details"), not something the `after_id` cursor can page
    # on (`_enumerate`'s "edge" branch filters the RAW `edge.id` column via
    # `WHERE id > ?`). A prior version of this plan selected "id" and fed the
    # resulting hash straight back in as `after_id` on the next iteration --
    # `WHERE id > <hash>` against small sequential integers matches nothing,
    # so every window past the first silently came back empty and the loop
    # exited after one page, exactly the truncation-hiding bug this function
    # exists to prevent. "edge_id" is the one field QueryPlan exposes on this
    # view that maps straight to the raw `edge.id` column (added alongside
    # this fix in both python/indexer/queryplan.py's `_TYPED_FIELDS["edge"]`
    # and src/query/plan.cpp's `field_available` -- it already had a matching
    # `typed_column`/`typed_column` SQL mapping in both languages, just no
    # field-availability entry to reach it through `select()`).
    edge_id_plan = (
        start(codebase()) | view("edge") | nodes() | select(["edge_id"])
        | limit(ENUMERATE_BUDGET)
    ).plan
    rows: list[tuple[Any, ...]] = []
    after_id: int | None = None
    partial = False
    unknown = False
    while True:
        edge_page = executor.run(edge_id_plan, after_id=after_id)
        page = executor.run(plan, after_id=after_id)
        partial = partial or page.partial
        unknown = unknown or page.unknown
        edge_ids_in_window = [row[0] for row in edge_page.rows]
        page_rows = page.rows
        if not edge_ids_in_window:
            rows.extend(page_rows)
            return rows, page.truncated, partial, unknown
        if page_rows and page.truncated:
            last_edge_id, last_src_id, last_dst_id = page_rows[-1][0], page_rows[-1][1], page_rows[-1][2]
            sites_in_page = sum(1 for row in page_rows if row[0] == last_edge_id)
            # [P1-4 fix, round 2] Only `real_count` is read; `_edge_site_
            # count`'s own `truncated` flag is NOT treated as ambiguous on
            # its own. `_edge_site_count` is always called with
            # `after_id=last_edge_id - 1`, so `last_edge_id` is structurally
            # guaranteed to be the FIRST row QueryPlan's own edge-view cursor
            # considers (`_enumerate`'s trim only ever drops the TAIL of an
            # over-full raw window, never the head) -- it can never be the
            # row that overflow trimming excluded. That flag fires merely
            # because the TABLE has more than ENUMERATE_BUDGET edges
            # remaining past this cursor, unrelated to whether THIS edge's
            # own sites were fully counted; on any codebase past
            # ENUMERATE_BUDGET edges (the entire reason this pagination
            # exists) it fires on EVERY boundary, and an earlier version of
            # this check treated it alone as ambiguous, making the "give up,
            # mark truncated" branch below fire unconditionally at the very
            # first window -- silently dropping every edge past it, the
            # sparse-density regression this function exists to fix. A real
            # mismatch (`real_count != sites_in_page`) is still caught
            # regardless, since it is symptomatic of the one genuine
            # remaining risk `_edge_site_count`'s own docstring names (a
            # second, same-endpoint edge -- a different kind -- sorting past
            # the truncated window).
            real_count = _edge_site_count(
                executor, last_edge_id, last_src_id, last_dst_id
            )[0]
            if real_count != sites_in_page:
                if sites_in_page == len(page_rows):
                    rows.extend(page_rows)
                    return rows, True, partial, unknown
                page_rows = [row for row in page_rows if row[0] != last_edge_id]
                rows.extend(page_rows)
                next_after_id = last_edge_id - 1
                if next_after_id == after_id:
                    return rows, True, partial, unknown
                after_id = next_after_id
                continue
        rows.extend(page_rows)
        window_last_edge_id = edge_ids_in_window[-1]
        if not edge_page.truncated:
            return rows, page.truncated, partial, unknown
        if window_last_edge_id == after_id:
            return rows, True, partial, unknown
        after_id = window_last_edge_id


def _external_decl_path(decl_path: str, root_resolved: Path) -> bool:
    """True when `decl_path` -- a symbol's raw, possibly-external declaration
    path (`Symbol.decl_path`; storage.py/storage.hpp: populated for a target
    whose declaration lives in an unregistered, e.g. system/stdlib, file) --
    resolves to somewhere OUTSIDE `root_resolved`.

    [moduleCallGraphCheck false-positive fix] An implicit instantiation of an
    external-library template (observed: `std::operator+<...>` for a
    `std::string` concatenation) can have its `symbol.file_id` -- the field
    `_read_semantic_facts` otherwise uses for module attribution -- wrongly
    attributed to the REGISTERED project header that triggered the
    instantiation, rather than to the library header the specialization is
    actually declared in (a real extraction-engine defect, not a report-layer
    one; see `docs/self-host-architecture.md`'s moduleCallGraphCheck note).
    `decl_path`, when this same symbol also went through a stub mint before
    its (mis-attributed) definition was recorded, still names its true,
    external declaration site and is never overwritten by that later,
    incorrect `file_id`/`file` update (`add_symbol`'s own UPDATE clause never
    touches `decl_path` -- see `src/ast/storage_symbol_sink.cpp`'s
    `same_add_symbol_result_except_decl_site`). Treating a non-null,
    resolves-outside-root `decl_path` as authoritative over `file` lets this
    class of misattribution fail closed (excluded from module attribution
    entirely, like any other out-of-repo symbol) instead of silently
    fabricating a project-to-project coupling finding out of a callee that
    was never really written in this repository.
    """
    try:
        Path(decl_path).resolve().relative_to(root_resolved)
    except ValueError:
        return True
    return False


def _read_semantic_facts(db: Storage, root: Path) -> SemanticFacts:
    executor = Executor(db)
    truncated = False
    partial = False
    unknown = False

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
        start(codebase()) | nodes()
        | select(["id", "name", "file", "line", "decl_path"])
        | limit(ENUMERATE_BUDGET)
    ).plan
    symbol_rows, symbol_truncated, symbol_partial, symbol_unknown = _run_all_pages(
        executor, symbol_plan, cursor_field_index=0
    )
    truncated = truncated or symbol_truncated
    partial = partial or symbol_partial
    unknown = unknown or symbol_unknown
    symbol_name: dict[int, str] = {}
    symbol_file: dict[int, str] = {}
    symbol_line: dict[int, int] = {}
    root_resolved = root.resolve()
    for sym_id, name, file_path, line, decl_path in symbol_rows:
        symbol_name[sym_id] = name
        if line is not None:
            symbol_line[sym_id] = line
        # [moduleCallGraphCheck false-positive fix] `decl_path`, when it
        # resolves outside the repo, is authoritative over `file`/`file_id`
        # for module attribution -- see `_external_decl_path`'s own
        # docstring for the exact misattribution this guards against. Fail
        # CLOSED: such a symbol is left out of `symbol_file` entirely (same
        # as a symbol whose `file` itself already resolved outside root
        # below), so it can never be classified into a project module by
        # either `find_module_boundary_violations` or
        # `find_legacy_facade_violations`.
        if decl_path is not None and _external_decl_path(decl_path, root_resolved):
            continue
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

    # The "site" view exposes the owning edge's own stable src_id/dst_id
    # directly (a correlated subquery against edge.id -- see
    # python/indexer/queryplan.py's `_typed_column`), so every call site's
    # caller, callee, relation, and exact location come from ONE public
    # QueryPlan query. No separate "edge" view round-trip, and no private
    # `db._conn` correlation, is needed to recover src/dst. Paginated via
    # `_run_all_site_pages` (NOT `_run_all_pages`): "edge_id" repeats across
    # every site of the same edge, so it is not a valid per-row cursor key,
    # only a real per-EDGE one -- see that function's own docstring for why
    # this distinction matters.
    site_plan = (
        start(codebase()) | view("edge") | nodes() | sites()
        | select(["edge_id", "src_id", "dst_id", "line", "col", "relation"])
        | limit(ENUMERATE_BUDGET)
    ).plan
    site_rows, site_truncated, site_partial, site_unknown = _run_all_site_pages(
        executor, site_plan
    )
    truncated = truncated or site_truncated
    partial = partial or site_partial
    unknown = unknown or site_unknown
    calls: list[tuple[int, int, int, int, int, str]] = []
    for edge_id, src_id, dst_id, line, col, relation in site_rows:
        if relation not in COUPLING_EDGE_KINDS:
            continue
        calls.append((edge_id, src_id, dst_id, line or 0, col or 0, relation))

    # A symbol whose file_id was NULLed by an ON DELETE SET NULL cascade
    # (its file row was deleted) is indistinguishable at the symbol level
    # from a symbol that legitimately never had a file (e.g. a builtin) --
    # but a CALL SITE through it becomes silently unwitnessable either way.
    # Count that directly here so the report can fail closed on it instead
    # of the caller/callee simply vanishing from every finding.
    unwitnessed = sum(
        1 for _edge_id, src_id, dst_id, _line, _col, _relation in calls
        if src_id not in symbol_file or dst_id not in symbol_file
    )
    return SemanticFacts(
        symbol_name,
        symbol_file,
        symbol_line,
        calls,
        truncated,
        unwitnessed_call_sites=unwitnessed,
        partial=partial,
        unknown=unknown,
    )


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


def _constructor_style_name(record_qual_name: str) -> str:
    """The "Record::Record" identity a human-authored legacy-facade baseline
    entry names for a direct construction/destruction (e.g.
    `"cidx::Storage::Storage"` for `Storage db(...)`), synthesized from the
    constructed/destroyed RECORD's own (bare) qualified name -- the only
    identity a construction-kind edge's destination carries (see
    `CONSTRUCTION_EDGE_KINDS` above: the destination is the record, never a
    constructor/destructor symbol). A record's leaf name is its last `::`
    component with any template-argument list dropped, matching how a
    constructor is spelled regardless of how its owning specialization
    prints.
    """
    leaf = record_qual_name.rsplit("::", 1)[-1]
    leaf = leaf.split("<", 1)[0]
    return f"{record_qual_name}::{leaf}"


def _witness(
    facts: SemanticFacts, src: int, dst: int, line: int, col: int, relation: str = "calls",
) -> Witness | None:
    src_path = facts.symbol_file.get(src)
    dst_path = facts.symbol_file.get(dst)
    if src_path is None or dst_path is None:
        return None
    callee_symbol = facts.symbol_name.get(dst, "?")
    if relation in CONSTRUCTION_EDGE_KINDS and callee_symbol != "?":
        # [P1-2 fix] the destination is the constructed/destroyed RECORD
        # (e.g. `cidx::Storage`), not a constructor/destructor symbol --
        # present it the way a human-authored baseline names a direct
        # construction, so `find_legacy_facade_violations`'s baseline/prefix
        # matching (and any human reading the witness) sees the same shape
        # the policy file already uses.
        callee_symbol = _constructor_style_name(callee_symbol)
    return Witness(
        caller_symbol=facts.symbol_name.get(src, "?"),
        caller_file=src_path,
        caller_line=facts.symbol_line.get(src, 0),
        callee_symbol=callee_symbol,
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
    for edge_id, src, dst, line, col, relation in facts.calls:
        witness = _witness(facts, src, dst, line, col, relation)
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
    for edge_id, src, dst, line, col, relation in facts.calls:
        witness = _witness(facts, src, dst, line, col, relation)
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

    [P1-1 fix] the identity's `calleeQualName` half is compared BARE
    (`_bare_qual_name`) on both sides: a baseline entry is always authored
    bare (`"cidx::SqliteStorageService::write"`), but a resolved witness's
    `callee_symbol` for a `calls`/`dispatch_calls` edge carries the full C++
    signature. Comparing the raw (signature-bearing) form against a bare
    baseline entry could never match, so no baseline could ever suppress a
    real violation. The `calleeQualNamePrefixes` PREFIX match just above is
    unaffected by this (a signature-bearing name still starts with its own
    bare class-scope prefix), so it stays on the raw name.
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
        for edge_id, src, dst, line, col, relation in facts.calls:
            witness = _witness(facts, src, dst, line, col, relation)
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
                _bare_qual_name(witness.callee_symbol),
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


# A C++ integer-literal token: hex (0x/0X), binary (0b/0B, C++14), leading-
# zero octal, or plain decimal, each with optional single-quote digit
# separators (C++14, e.g. `1'000`). Deliberately captures the TOKEN, not a
# specific decimal spelling, so its numeric VALUE can be evaluated and
# compared -- see `_parse_cpp_int_literal` below for why.
_CPP_INT_LITERAL = r"0[xX][0-9a-fA-F](?:'?[0-9a-fA-F])*|0[bB][01](?:'?[01])*|0(?:'?[0-7])*|[1-9](?:'?[0-9])*"
# An integer-suffix (u/U/l/L/z/Z in any order/case, e.g. `8uLL`), consumed
# but not captured: it changes the literal's TYPE, never its VALUE.
_CPP_INT_SUFFIX = r"(?:[uUlLzZ])*"

# A C++ character literal: `'` + either a single escape sequence (`\` plus
# one more character/digits, e.g. `\b`, `\x08`, `\010`) or one ordinary
# character, + `'`. Deliberately permissive about what follows the `\` --
# `_eval_cpp_char_literal` below is what actually validates and evaluates
# the escape, so an unrecognized escape fails EVALUATION, not matching.
_CPP_CHAR_LITERAL = r"'(?:\\.|[^'\\])'"

# One "atom" this guard can evaluate to a numeric value on its own: a bare
# integer literal (with optional suffix) or a character literal. Wrapped as
# a single non-capturing alternation so it can be embedded into
# `_CPP_CONST_VALUE` below without disturbing any caller's capture-group
# numbering.
_CPP_CONST_ATOM = r"(?:(?:" + _CPP_INT_LITERAL + r")" + _CPP_INT_SUFFIX + r"|" + _CPP_CHAR_LITERAL + r")"

# One "unit" this guard can evaluate to a numeric value without any
# enclosing parentheses: a bare atom, a `static_cast<...>` around one atom,
# or one leading unary `+`/`-` sign on one atom. [Round-6 internal critic
# P1] widens the original bare-atom-only unit to also close
# `static_cast<unsigned>(static_cast<int>(8))` (one cast wrapping another,
# via `_CPP_CONST_TERM` below allowing a UNIT -- including this cast-of-atom
# form -- inside the outer cast) and `+8` (a leading unary sign) -- both
# produced ZERO regex match at all before this, neither an identifier nor
# one of the previously-recognized shapes.
_CPP_CONST_UNIT = r"(?:" + (
    r"static_cast\s*<[^<>]*>\s*\(\s*" + _CPP_CONST_ATOM + r"\s*\)"
    + r"|[+-]\s*" + _CPP_CONST_ATOM
    + r"|" + _CPP_CONST_ATOM
) + r")"

# One enclosing layer of parentheses around a UNIT, or around a single
# UNIT-op-UNIT binary operation. [Round-6 internal critic P1] closes
# `(8)` and `(4 << 1)` -- a parenthesized literal or parenthesized shift/
# arithmetic expression, both of which previously produced zero regex
# match (the top-level shapes never allowed a leading `(` except as part
# of `static_cast<...>(...)`'s own call syntax).
_CPP_CONST_PAREN = (
    r"\(\s*" + _CPP_CONST_UNIT
    + r"\s*(?:(?:<<|>>|\+|-|\*|/)\s*" + _CPP_CONST_UNIT + r"\s*)?\)"
)

# One leading unary `+`/`-` sign directly on a `_CPP_CONST_PAREN`. [Round-7
# internal critic P1] `_CPP_CONST_UNIT` only ever let a sign land on a bare
# ATOM, never on a parenthesized term -- so `+(8)`, `-(8)`, and `-(4 << 1)`
# produced ZERO regex match at all, even though a sign on a bare literal
# (`+8`) and a parenthesized literal/shift (`(8)`, `(4 << 1)`) were each
# already individually closed. This is deliberately its own alternative,
# not a widening of `_CPP_CONST_UNIT` itself: `_CPP_CONST_PAREN`'s own
# definition already depends on the (unsigned) `_CPP_CONST_UNIT`, so folding
# the sign into `_CPP_CONST_UNIT` in turn would make the two patterns
# mutually self-referential, which plain (non-recursive) `re` patterns
# cannot express. Evaluated by `_eval_cpp_const_term` below, which negates
# the fully-folded parenthesized value -- so `-(4 << 1)` evaluates as
# `-(4 << 1)` == -8, never `(-4) << 1`.
_CPP_CONST_SIGNED_PAREN = r"[+-]\s*" + _CPP_CONST_PAREN

# A "term": a UNIT on its own, one layer of parentheses around a UNIT or a
# UNIT-op-UNIT, or one leading sign directly on such a parenthesized form.
# Used both as a bare top-level match and as the object of an outer
# `static_cast<...>` -- so the WHOLE expression, or what a cast wraps, can
# each be signed, parenthesized, or itself one more level of cast (and now,
# per [Round-7 internal critic P1], signed AND parenthesized together, e.g.
# `static_cast<int>(+(8))`). Deliberately NOT used as an operand of the
# top-level binary-op branch below (that branch uses UNIT, not TERM) -- see
# `_CPP_CONST_VALUE`'s own comment for why allowing a parenthesized TERM
# there would be ambiguous to evaluate.
_CPP_CONST_TERM = (
    r"(?:" + _CPP_CONST_UNIT
    + r"|" + _CPP_CONST_PAREN
    + r"|" + _CPP_CONST_SIGNED_PAREN
    + r")"
)

# The full constant-expression SHAPE this guard recognizes textually: a
# `static_cast<...>` around one term, one UNIT shifted/added/subtracted/
# multiplied/divided by another UNIT, or a bare term -- where a "term" is,
# recursively, a bare literal, a signed literal, one `static_cast` around a
# literal, one enclosing layer of parentheses around any of those, or one
# leading sign directly on that parenthesized form (see
# `_CPP_CONST_UNIT`/`_CPP_CONST_PAREN`/`_CPP_CONST_SIGNED_PAREN`/
# `_CPP_CONST_TERM` above for the exact grammar). The binary-operation
# branch deliberately uses UNIT, not the
# wider TERM, on both sides: a UNIT can never itself contain a nested
# operator, so there is exactly one way to read `1 << 3`. Allowing a
# parenthesized TERM as an operand here too (`(4 << 1) + 2`) would let the
# SAME text be split at either the inner or the outer operator, which is
# genuinely ambiguous to evaluate correctly and is intentionally left
# unmatched rather than resolved by guesswork; `(4 << 1)` and `(8)` are
# still fully covered as a bare TERM in their own right (the third
# alternative below). This is deliberately still not a general C++
# expression evaluator: an identifier of any kind (macro, enum constant,
# named constexpr/const variable) never matches any branch here, and
# nesting beyond the one extra layer of parentheses/cast covered above
# remains unmatched too, so a duplicate spelled through one of those
# remains genuinely undetected -- see `unresolvedLimitations` in
# `generate_report` for the exact, currently-accepted grammar and its
# boundary.
_CPP_CONST_VALUE = (
    r"static_cast\s*<[^<>]*>\s*\(\s*" + _CPP_CONST_TERM + r"\s*\)"
    + r"|" + _CPP_CONST_UNIT + r"\s*(?:<<|>>|\+|-|\*|/)\s*" + _CPP_CONST_UNIT
    + r"|" + _CPP_CONST_TERM
)

_CPP_CONST_ATOM_RE = re.compile(
    r"^(?:(" + _CPP_INT_LITERAL + r")" + _CPP_INT_SUFFIX + r"|(" + _CPP_CHAR_LITERAL + r"))$"
)
_CPP_STATIC_CAST_RE = re.compile(r"^static_cast\s*<[^<>]*>\s*\(\s*(.+?)\s*\)$", re.DOTALL)
_CPP_BINARY_OP_RE = re.compile(r"^(.+?)\s*(<<|>>|\+|-|\*|/)\s*(.+)$", re.DOTALL)
# An anchored, exact match for "this whole string is ONE `_CPP_CONST_PAREN`"
# -- used by the eval functions below to tell a genuine single
# parenthesized term (`(4 << 1)`) apart from a top-level binary operation
# whose OPERANDS merely happen to each start/end with their own parens
# (`(4) + (1)`, matchable by `_CPP_CONST_VALUE`'s TERM-op-TERM branch, each
# TERM its own `_CPP_CONST_PAREN`): a naive "starts with `(` and ends with
# `)`" string check cannot distinguish the two, since the OUTER text of the
# latter also starts and ends with a paren character.
_CPP_CONST_PAREN_RE = re.compile(r"^" + _CPP_CONST_PAREN + r"$", re.DOTALL)
# Same anchored-whole-string purpose as `_CPP_CONST_PAREN_RE` immediately
# above, but for a `_CPP_CONST_SIGNED_PAREN` (one leading sign directly on a
# parenthesized term, e.g. `-(4 << 1)`). [Round-7 internal critic P1] the
# eval functions below must check this BEFORE `_CPP_BINARY_OP_RE`, exactly
# as `_CPP_CONST_PAREN_RE` already must: `_CPP_BINARY_OP_RE` would otherwise
# happily (and wrongly) split `-(4 << 1)` into "-(4" and "1)" by treating
# the shift operator INSIDE the parentheses as the top-level operator.
_CPP_CONST_SIGNED_PAREN_RE = re.compile(r"^" + _CPP_CONST_SIGNED_PAREN + r"$", re.DOTALL)

_CHAR_ESCAPES = {
    "a": 7, "b": 8, "f": 12, "n": 10, "r": 13, "t": 9, "v": 11,
    "\\": 92, "'": 39, '"': 34, "?": 63,
}


def _eval_cpp_char_literal(token: str) -> int | None:
    """The numeric VALUE of a C++ character literal (`'\\b'` -> 8, `'A'` ->
    65, ...), or None if `token` is not one / uses an escape this guard
    does not recognize. Supports the common single-letter escapes, `\\xHH`
    (hex), and `\\NNN` (1-3 octal digits, C++'s own octal-escape grammar).
    """
    match = re.match(r"^'(\\.|[^'\\])'$", token)
    if not match:
        return None
    body = match.group(1)
    if not body.startswith("\\"):
        return ord(body)
    escape = body[1:]
    if escape in _CHAR_ESCAPES:
        return _CHAR_ESCAPES[escape]
    if re.fullmatch(r"x[0-9a-fA-F]+", escape):
        return int(escape[1:], 16)
    if re.fullmatch(r"[0-7]{1,3}", escape):
        return int(escape, 8)
    return None


def _eval_cpp_const_atom(token: str) -> int | None:
    """The numeric VALUE of one `_CPP_CONST_ATOM` match: a bare integer
    literal (`_parse_cpp_int_literal`) or a character literal
    (`_eval_cpp_char_literal`)."""
    match = _CPP_CONST_ATOM_RE.match(token.strip())
    if not match:
        return None
    if match.group(1) is not None:
        return _parse_cpp_int_literal(match.group(1))
    return _eval_cpp_char_literal(match.group(2))


def _fold_cpp_binary_op(op: str, left: int, right: int) -> int | None:
    if op == "<<":
        return left << right
    if op == ">>":
        return left >> right
    if op == "+":
        return left + right
    if op == "-":
        return left - right
    if op == "*":
        return left * right
    if op == "/":
        if right == 0:
            return None
        # C++ integer division truncates toward zero; Python's `//` floors
        # toward negative infinity, so negative operands need this
        # explicit correction to match C++ semantics exactly.
        quotient = abs(left) // abs(right)
        return quotient if (left < 0) == (right < 0) else -quotient
    return None


def _eval_cpp_const_unit(token: str) -> int | None:
    """The numeric VALUE of one `_CPP_CONST_UNIT` match: a bare atom, one
    leading unary `+`/`-` sign on a bare atom, or a `static_cast<...>`
    around a bare atom."""
    token = token.strip()
    cast_match = _CPP_STATIC_CAST_RE.match(token)
    if cast_match:
        return _eval_cpp_const_atom(cast_match.group(1))
    if token[:1] in ("+", "-"):
        value = _eval_cpp_const_atom(token[1:])
        if value is None:
            return None
        return -value if token[0] == "-" else value
    return _eval_cpp_const_atom(token)


def _eval_cpp_const_paren(token: str) -> int | None:
    """The numeric VALUE of one bare `_CPP_CONST_PAREN` match (the caller has
    already confirmed `token` matches `_CPP_CONST_PAREN_RE` in full): the
    UNIT inside on its own, or the folded result of the single UNIT-op-UNIT
    binary operation inside."""
    inner = token[1:-1].strip()
    op_match = _CPP_BINARY_OP_RE.match(inner)
    if op_match:
        left = _eval_cpp_const_unit(op_match.group(1))
        right = _eval_cpp_const_unit(op_match.group(3))
        if left is None or right is None:
            return None
        return _fold_cpp_binary_op(op_match.group(2), left, right)
    return _eval_cpp_const_unit(inner)


def _eval_cpp_const_term(token: str) -> int | None:
    """The numeric VALUE of one `_CPP_CONST_TERM` match: a `_CPP_CONST_UNIT`
    on its own, one enclosing layer of parentheses around a UNIT or a single
    UNIT-op-UNIT binary operation (`_CPP_CONST_PAREN`), or one leading sign
    directly on such a parenthesized form (`_CPP_CONST_SIGNED_PAREN`).
    Checked via `_CPP_CONST_PAREN_RE`/`_CPP_CONST_SIGNED_PAREN_RE`'s anchored,
    whole-string match rather than a bare "starts with `(`/`+`/`-` and ends
    with `)`" check -- see `_CPP_CONST_PAREN_RE`'s own comment for why a
    naive prefix/suffix check would misparse something like `(4) + (1)`
    (two independently-parenthesized operands of a top-level binary
    operation, each its own valid TERM) as a single parenthesized group
    instead. [Round-7 internal critic P1] the sign, when present, is applied
    to the FULLY FOLDED parenthesized value, never to just its first
    operand -- `-(4 << 1)` evaluates as `-(4 << 1)` == -8, never
    `(-4) << 1`.
    """
    token = token.strip()
    if _CPP_CONST_SIGNED_PAREN_RE.match(token):
        value = _eval_cpp_const_paren(token[1:].strip())
        if value is None:
            return None
        return -value if token[0] == "-" else value
    if _CPP_CONST_PAREN_RE.match(token):
        return _eval_cpp_const_paren(token)
    return _eval_cpp_const_unit(token)


def _eval_cpp_const_expr(text: str) -> int | None:
    """The numeric VALUE of one `_CPP_CONST_VALUE` match: a `static_cast<...>`
    around a supported term, a single supported binary shift/arithmetic
    operation between two supported UNITS, or a bare supported term -- where
    a "term" (`_eval_cpp_const_term`) is itself a bare, signed, or
    `static_cast`-wrapped atom, optionally wrapped in one further layer of
    parentheses, and a "unit" (`_eval_cpp_const_unit`) is that same atom
    without the extra parentheses (see `_CPP_CONST_UNIT`/`_CPP_CONST_TERM`'s
    own definitions for the exact shapes, and their comments for why the
    binary-operation branch is restricted to UNIT operands). Returns None if
    `text` is not actually one of those shapes (or evaluation itself fails,
    e.g. division by zero) -- this is never called on arbitrary, unvalidated
    text; the caller only ever passes something `_CPP_CONST_VALUE` already
    matched.
    """
    text = text.strip()
    cast_match = _CPP_STATIC_CAST_RE.match(text)
    if cast_match:
        return _eval_cpp_const_term(cast_match.group(1))
    if _CPP_CONST_PAREN_RE.match(text) or _CPP_CONST_SIGNED_PAREN_RE.match(text):
        # The whole match is ONE (optionally signed) parenthesized term
        # (`(4 << 1)`, `-(4 << 1)`) -- must be checked before the generic
        # binary-op split below, which would otherwise happily (and
        # wrongly) split it into e.g. "-(4" and "1)" by treating the
        # shift/arithmetic operator INSIDE the parentheses as if it were
        # the top-level operator.
        return _eval_cpp_const_term(text)
    op_match = _CPP_BINARY_OP_RE.match(text)
    if op_match:
        left = _eval_cpp_const_unit(op_match.group(1))
        right = _eval_cpp_const_unit(op_match.group(3))
        if left is None or right is None:
            return None
        return _fold_cpp_binary_op(op_match.group(2), left, right)
    return _eval_cpp_const_term(text)


def _parse_cpp_int_literal(token: str) -> int | None:
    """The numeric VALUE of a C++ integer-literal token, regardless of which
    of C++'s several equivalent spellings (hex/octal/binary/decimal, with or
    without digit separators) produced it -- e.g. `0x8`, `010`, `0b1000`,
    and `8` are four different SPELLINGS of the exact same value. `token`
    is already known to match `_CPP_INT_LITERAL`.
    """
    text = token.replace("'", "")
    try:
        if text[:2] in ("0x", "0X"):
            return int(text, 16)
        if text[:2] in ("0b", "0B"):
            return int(text, 2)
        if text != "0" and text.startswith("0"):
            return int(text, 8)
        return int(text, 10)
    except ValueError:
        return None


def check_catalog_containment(root: Path, graph: ModuleGraph, policy: dict) -> list[str]:
    """Reject a hand-authored duplicate of a generated catalog (id, name) pair.

    [PR #67 round-4 review, AC5 blocker] a prior version matched each
    guarded id against ONE hard-coded decimal spelling (`re.escape(str(id_))`),
    so `std::pair{0x8, "function"}` (a hex spelling of the exact same id)
    sailed straight through undetected -- and any other equivalent spelling
    (octal, binary, digit separators) would too, since a purely textual
    spelling-list can never enumerate every equivalent spelling of the same
    value. Matching a GENERIC integer-literal token and evaluating its
    numeric VALUE (`_parse_cpp_int_literal`) before comparing against the
    guarded id closes that whole class of bypass at once, rather than
    reactively patching in one more spelling every time a new one is found.

    [PR #67 internal critic, second finding] a bare integer/character
    literal is not the only way to spell the same numeric value: the guard
    also evaluates (`_eval_cpp_const_expr`) a `static_cast<...>` around one,
    a single shift/arithmetic operation between two of them, and a
    character literal on its own (`'\\b'` == 8) -- see `_CPP_CONST_VALUE`'s
    own docstring for exactly which shapes that covers.

    [Round-6 internal critic P1] that first widening still produced ZERO
    regex match at all -- not "matched but evaluated as unsupported",
    genuinely invisible to the pattern -- for `+8` (a leading unary sign),
    `(8)` (a parenthesized literal), `(4 << 1)` (a parenthesized shift
    expression), and `static_cast<unsigned>(static_cast<int>(8))` (one cast
    wrapping another). `_CPP_CONST_UNIT`/`_CPP_CONST_PAREN`/`_CPP_CONST_TERM`
    close exactly those four shapes by additionally accepting one leading
    unary `+`/`-` sign on a literal, one enclosing layer of parentheses
    around a literal or a single binary operation between two literals, and
    one `static_cast` wrapping another `static_cast`-of-a-literal. A guarded
    id spelled through a macro, an enum constant, or a reference to another
    named constexpr/const variable remains genuinely undetected (none of
    those is an identifier this guard resolves), and so does any nesting or
    number of operators beyond the single extra layer covered above: closing
    THOSE would require actually resolving what the identifier/expression
    means (a real semantic parse, or comparing against the generated
    contract itself), not a wider textual pattern -- see
    `unresolvedLimitations` below for the exact, currently-accepted grammar.

    [Round-7 internal critic P1] the Round-6 widening above closed a sign on
    a bare literal and a parenthesized literal/shift-expression as two
    SEPARATE shapes, but never their COMPOSITION: a sign directly in front
    of a parenthesized term (`+(8)`, `-(8)`, `-(4 << 1)`, or
    `static_cast<int>(+(8))`) still produced ZERO regex match, the exact
    same "invisible to the pattern" failure mode as the shapes Round-6
    closed. `_CPP_CONST_SIGNED_PAREN` closes exactly that composition by
    admitting one leading `+`/`-` sign directly on a `_CPP_CONST_PAREN`, with
    `_eval_cpp_const_term` negating the fully-folded parenthesized value
    (never just its first operand, so `-(4 << 1)` is `-(4 << 1)` == -8, not
    `(-4) << 1`). A sign directly in front of a `static_cast<...>` (e.g.
    `-static_cast<int>(8)`) is a structurally identical composition gap that
    remains genuinely undetected -- see `unresolvedLimitations` below.

    Expressions outside that closed evaluator grammar cannot receive a clean
    result when they appear in an explicit `std::pair` with a guarded catalog
    name. The guard fails closed on the hand-authored declaration whenever
    its id expression cannot be evaluated.
    """
    catalog_guard = policy.get("catalogGuard", {})
    generated = {str(Path(path).as_posix()) for path in catalog_guard.get("generatedOutputs", [])}
    catalog_source = json.loads((root / "catalogs/core.json").read_text(encoding="utf-8"))
    guarded: set[tuple[int, str]] = set()
    for group in catalog_guard.get("guardedGroups", []):
        for row in catalog_source.get(group, []):
            guarded.add((row["id"], row["name"]))
    guarded_names = {name for _id, name in guarded}

    errors: list[str] = []
    # `(` for a paren-call construction (std::pair(8, "function")) and `{`
    # for ordinary brace-init (std::pair{8, "function"}, {8, "function"} in
    # an aggregate/array) are both accepted ways to redeclare the same pair
    # in C++; catching only one lets the other through undetected.
    pair_pattern = re.compile(
        r"[({]\s*(" + _CPP_CONST_VALUE + r")\s*,\s*\"([^\"]*)\""
    )
    name_colon_pattern = re.compile(
        r"\"([^\"]*)\"\s*:\s*(" + _CPP_CONST_VALUE + r")"
    )
    unresolved_std_pair_pattern = re.compile(
        r"std::pair\s*[({]\s*([^,{}\n]+?)\s*,\s*\"([^\"]*)\""
    )
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
        for match in pair_pattern.finditer(text):
            value = _eval_cpp_const_expr(match.group(1))
            if value is not None and (value, match.group(2)) in guarded:
                errors.append(
                    f"catalog guard {path}: duplicates a generated catalog declaration "
                    f"(id={value}, name={match.group(2)!r}, spelled {match.group(1)!r})"
                )
        for match in name_colon_pattern.finditer(text):
            value = _eval_cpp_const_expr(match.group(2))
            if value is not None and (value, match.group(1)) in guarded:
                errors.append(
                    f"catalog guard {path}: duplicates a generated catalog declaration "
                    f"(id={value}, name={match.group(1)!r}, spelled {match.group(2)!r})"
                )
        for match in unresolved_std_pair_pattern.finditer(text):
            if (
                match.group(2) in guarded_names
                and _eval_cpp_const_expr(match.group(1)) is None
            ):
                errors.append(
                    f"catalog guard {path}: guarded catalog name "
                    f"{match.group(2)!r} is paired with an unverifiable "
                    f"hand-authored id expression {match.group(1)!r}"
                )
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
    """A checkout-location-independent content+build hash of every indexed
    file.

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

    Also folds in each file's own `compile_options`/`driver` (as recorded on
    the `file` row -- the exact per-TU build descriptor `cidx import` read
    from compile_commands.json), not just its text content: two self-index
    runs over BYTE-IDENTICAL source, one of them built with e.g. an extra
    `-DADVERSARIAL_BUILD=1`, previously produced this exact same hash (and
    therefore a byte-identical report) even though what was actually
    compiled -- and therefore what any preprocessor-conditional code the
    resolved facts could reflect -- genuinely differed (PR #67 round-4
    review, AC10). `compile_options` is already stored pre-serialized
    (`Storage.add_file`'s own `json.dumps`, in argv order, which is itself
    part of a compiler invocation's meaning) as a single canonical string,
    so it can be folded in directly with no further normalization.
    """
    digest = hashlib.sha256()
    # [PR #67 internal critic] LEFT JOIN, not JOIN: `directory_id` is
    # declared `NOT NULL REFERENCES directory(id)`, and a plain `DELETE FROM
    # directory` through a connection with the default `PRAGMA
    # foreign_keys = ON` (Storage's own default) correctly CASCADES and
    # removes the file too -- there is no orphan to find in that case. This
    # is defense-in-depth, not a fix for one known live code path: no
    # `Storage` method today deletes/merges a directory with
    # `PRAGMA foreign_keys = OFF` in scope (every existing OFF/ON toggle in
    # `python/indexer/storage.py`'s `_migrate` rebuilds `symbol`/
    # `parameter`/`template_arg`/`call_arg`/`artifact_*`, never `directory`).
    # But SQLite never retroactively re-validates existing rows once the
    # pragma is switched back ON, so if a directory were ever deleted or
    # merged away with the pragma OFF -- by a future migration, an external
    # tool, or a hand-run `DELETE FROM directory` against this same
    # connection -- its files' `directory_id` would be left pointing at
    # nothing, permanently and silently. An INNER JOIN would then exclude
    # such a file from this hash entirely -- collapsing it to zero
    # contribution, as if it had never been indexed -- rather than
    # surfacing the gap. LEFT JOIN keeps every file row in
    # the computation; `check_index_coverage`'s
    # `danglingFileDirectoryReferences` is what fails the report closed on
    # the orphaned state itself (matching the existing dangling-symbol-
    # file-reference pattern).
    #
    # Note this does NOT make an orphaned file's hash contribution reflect
    # its actual current content: `db.file_abs_path` needs the very same
    # directory row (to know which directory + component root the file
    # lives under) to reconstruct an absolute path, so it returns `None`
    # for an orphaned file regardless of this join -- there is no way to
    # locate the file's bytes on disk once its directory linkage is gone.
    # What LEFT JOIN buys is that the row is no longer silently ABSENT: it
    # is represented (honestly, as unreadable) instead of contributing
    # nothing at all, and `danglingFileDirectoryReferences` + `status:
    # "fail"` is the actual, reliable signal a consumer must act on.
    rows = sorted(
        (_source_hash_label(dir_path, name, file_id), file_id, compile_options, driver)
        for file_id, dir_path, name, compile_options, driver in conn.execute(
            "SELECT f.id, d.path, f.name, f.compile_options, f.driver "
            "FROM file f LEFT JOIN directory d ON d.id = f.directory_id"
        )
    )
    for relative_path, file_id, compile_options, driver in rows:
        abs_path = db.file_abs_path(file_id)
        content_hash = _md5_file(Path(abs_path)) if abs_path else None
        digest.update(relative_path.encode())
        digest.update(b"\0")
        digest.update((content_hash or "<unreadable>").encode())
        digest.update(b"\0")
        digest.update((compile_options or "<no-compile-options>").encode())
        digest.update(b"\0")
        digest.update((driver or "<no-driver>").encode())
        digest.update(b"\n")
    return digest.hexdigest()


def _source_hash_label(dir_path: str | None, name: str, file_id: int) -> str:
    """The path label one `file` row contributes to `_portable_source_hash`.

    `dir_path` comes from a LEFT JOIN, so `None` means precisely "this
    file's `directory_id` no longer resolves to any `directory` row" (an
    orphaned reference), which is NOT the same state as a legitimate
    repo-root file whose directory row exists with an EMPTY path (`""`) --
    both are falsy, so they must be told apart explicitly rather than by
    `if dir_path` alone, or an orphaned file would silently be mislabeled
    as (and could collide with) an ordinary root-level file of the same
    name. `file_id` is folded into the orphaned label only (not the normal
    path, which must stay purely checkout-location-independent) so that
    two DIFFERENT orphaned files sharing a base name within the SAME index
    do not collide into one hash entry.
    """
    if dir_path is None:
        return f"<orphaned-directory-reference>/{file_id}/{name}"
    return f"{dir_path}/{name}" if dir_path else name


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

    # [PR #67 internal critic] `directory_id` is declared `NOT NULL
    # REFERENCES directory(id)` but this connection does not itself enforce
    # that reference at delete time, so a `file` row can end up pointing at
    # a `directory` row that no longer exists -- the exact same shape of
    # gap as the dangling-symbol-file check just above, one level down the
    # chain. `_portable_source_hash` now LEFT JOINs so such a file's
    # content is never silently dropped from the hash, but its PATH still
    # cannot be verified/reconstructed, so surface it here as its own
    # coverage issue rather than only fixing the hash silently.
    dangling_directory = conn.execute(
        "SELECT COUNT(*) FROM file WHERE directory_id NOT IN (SELECT id FROM directory)"
    ).fetchone()[0]
    if dangling_directory:
        issues.append(
            f"{dangling_directory} file(s) reference a directory_id with no matching "
            "directory row; their path cannot be verified and any fact through them "
            "must not be read as evidence of compliance"
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
    # LEFT JOIN, matching `_portable_source_hash`: an orphaned file (see
    # `dangling_directory` above) must not silently vanish from this set
    # either -- an INNER JOIN here would make an orphaned, still-present TU
    # falsely reported as "missing" (compounding the coverage gap instead
    # of the single, clear `dangling_directory` issue already covering it).
    indexed_paths = {
        _source_hash_label(dir_path, name, file_id)
        for file_id, dir_path, name in conn.execute(
            "SELECT f.id, d.path, f.name FROM file f LEFT JOIN directory d ON d.id = f.directory_id"
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
    # `partial` is expected for catalogued partial relations such as calls;
    # preserve it visibly without treating it as a failed concrete read.
    # `unknown` means this result contains unresolved provenance and must
    # fail the architecture proof closed.
    if facts.unknown:
        issues.append(
            "the QueryPlan semantic read reported unknown evidence; this "
            "report cannot treat returned rows or missing "
            "rows as a complete architecture proof"
        )

    coverage = {
        "expectedTranslationUnits": len(expected_tus),
        "missingTranslationUnits": missing_tus,
        "danglingSymbolFileReferences": dangling,
        "danglingFileDirectoryReferences": dangling_directory,
        "unwitnessedCallSites": facts.unwitnessed_call_sites,
        "pendingFiles": pending,
        "filesWithFatalDiagnostics": failed,
        "enumerationTruncated": facts.truncated,
        "queryPartial": facts.partial,
        "queryUnknown": facts.unknown,
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


def _canonical_report_hash(report: dict[str, Any]) -> str:
    """A hash of this report's CONTENT-DERIVED identity -- excluding the two
    fields that legitimately vary between two runs over the exact same,
    byte-identical checkout: `generatedAt` (this run's own wall-clock time)
    and `index.graphResolvedAt` (whenever `cidx resolve` last ran on the
    self-index -- itself a wall-clock stamp of when that command executed,
    not something derived from what the index actually contains; see
    `IndexIdentity.graph_resolved_at`/`_read_identity` above). Two report
    runs over the same content will always disagree on both of those, even
    with identical findings, packageHash, and sourceContentHash -- so a
    consumer diffing two reports (a release gate, or this module's own
    `test_report_is_reproducible_across_byte_identical_checkouts_at_
    different_paths`) needs ONE well-defined, explicit notion of "did
    anything real change" that does not require independently knowing which
    top-level fields are ephemeral. Every other field in the report
    (packageHash, findings, coverage counts, sourceContentHash, ...) is
    already checkout-location- and wall-clock-independent by construction
    (see `_stable_label`/`_portable_source_hash`/`_package_hash` above);
    this hash just makes that existing property directly and mechanically
    checkable, rather than something a caller has to re-derive by hand.
    """
    canonical = json.loads(json.dumps(report))
    canonical["generatedAt"] = None
    canonical["index"]["graphResolvedAt"] = None
    text = json.dumps(canonical, sort_keys=True)
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


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

    [P2-1 fix] also includes `spec/platform/architecture.json`:
    `generate_report` feeds it straight into `validate_contract` and its
    errors into `findings.platformContract` (a real gate verdict input, same
    as `catalogs/core.json`), so editing it must change `packageHash` too.
    It is optional (some fixtures/checkouts have no platform contract at
    all, per `generate_report`'s own `.exists()` guard), so its absence is
    hashed as an explicit sentinel rather than silently skipped -- adding or
    removing the file is itself a real change to what this run validated
    against, not a no-op.
    """
    script_dir = Path(__file__).resolve().parent
    project_root = script_dir.parent
    digest = hashlib.sha256()
    platform_contract_path = root / "spec/platform/architecture.json"
    for path in (
        manifest_path,
        policy_path,
        script_dir / "check_architecture.py",
        script_dir / "check_platform_contract.py",
        script_dir / "self_host_architecture_report.py",
        project_root / "python/indexer/queryplan.py",
        project_root / "python/indexer/storage.py",
        root / "catalogs/core.json",
        platform_contract_path,
    ):
        label = _stable_label(path, root)
        digest.update(label.encode())
        if path == platform_contract_path and not path.exists():
            digest.update(b"<absent>")
            continue
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
    for edge_id, src, dst, line, col, relation in facts.calls:
        witness = _witness(facts, src, dst, line, col, relation)
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
        "The catalog duplication guard is a textual pattern match, not a semantic check. The "
        "exact grammar it evaluates the numeric VALUE of: a bare integer or character literal "
        "(any equivalent hex/octal/binary/digit-separator spelling included); one leading unary "
        "+/- sign on such a literal; a static_cast<...> around such a literal; a single shift/"
        "arithmetic operation (<<, >>, +, -, *, /) between two such literals (each optionally "
        "signed or static_cast-wrapped); one enclosing layer of parentheses around any of the "
        "above or around one such binary operation; one leading unary +/- sign directly on that "
        "parenthesized form (e.g. -(4 << 1)); and one static_cast wrapping another "
        "static_cast-of-a-literal. An explicit std::pair that associates a guarded name with "
        "an expression outside that grammar fails closed when the expression cannot be "
        "evaluated. A duplicate hidden behind a different user-defined container or generated "
        "indirection still requires AST-level/generated-contract comparison to detect.",
        "A callee symbol whose `decl_path` resolves outside the repository root is excluded "
        "from module attribution even when its (possibly misattributed) `file`/`file_id` "
        "resolves inside the repository -- observed for implicit instantiations of external "
        "library templates (e.g. `std::operator+`) that the extraction engine can attribute to "
        "the project header that triggered the instantiation rather than to the library header "
        "the specialization is actually declared in. This closes that specific false-positive "
        "class but is not a fix for the underlying extraction-engine attribution; a symbol with "
        "neither a `decl_path` nor a correctly-attributed `file` remains unclassifiable and "
        "silently invisible to this pass, exactly like any other unresolved callee.",
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
    # Computed LAST, over the fully-finished report (including the
    # disagreements-driven status override just above) but obviously before
    # this key itself exists to be hashed.
    report["canonicalHash"] = _canonical_report_hash(report)
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
