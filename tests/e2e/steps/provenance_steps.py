"""THEN: receiver provenance and query-time devirtualization (Phase 2)."""

from __future__ import annotations

from pytest_bdd import parsers, then

from .symbol_facts import describe
from .tables import assert_same, rows
from .workspace import Workspace


def _calls_edge(ws: Workspace, src_sel: str, dst_sel: str):
    """The single `calls` edge between two resolved symbols."""
    src = ws.resolve(src_sel)
    dst = ws.resolve(dst_sel)
    hits = [
        e
        for e in ws.graph.edges_out(src, kinds=("calls",))
        if e.peer.id == dst.id
    ]
    assert len(hits) == 1, (
        f"expected exactly 1 calls edge {src_sel} -> {dst_sel}, found {len(hits)}"
    )
    return hits[0]


def _type_name(ws: Workspace, usr: str | None) -> str | None:
    """A receiver-type USR as a symbol name (falls back to the raw USR)."""
    if usr is None:
        return None
    for s in ws.symbols():
        if s.usr == usr:
            return s.name
    return usr


def _decl_name(usr: str | None) -> str | None:
    """The terminal name of a declaring USR: a local's spelling, or the class."""
    return usr.rsplit("@", 1)[-1] if usr else None


@then(parsers.parse('the call from "{src}" to "{dst}" has receiver provenance:'))
def call_receiver_provenance(
    workspace: Workspace, src: str, dst: str, datatable: list[list[str]]
) -> None:
    """One row per call site of the edge. Table columns: kind | type | decl.

    `kind` is where the receiver came from (`local`, `this`, ...), `type` its
    static record type, `decl` the terminal name of the declaring USR (the
    local variable's spelling, or the class for `this`).
    """
    edge = _calls_edge(workspace, src, dst)
    actual = [
        {
            "kind": s.recv_src_kind,
            "type": _type_name(workspace, s.recv_type_usr),
            "decl": _decl_name(s.recv_decl_usr),
        }
        for s in edge.sites
    ]
    assert_same(rows(datatable), actual, f"receiver provenance of {src} -> {dst}")


@then(parsers.parse('the virtual call to "{selector}" has the selection map:'))
def dispatch_selection_map(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    """The Phase-1 receiver-type -> target map, unpruned.

    Table columns: receiver_type | target.
    """
    sym = workspace.resolve(selector)
    site = workspace.graph.dispatch_selection(sym)
    actual = [
        {
            "receiver_type": c.selecting_type.name if c.selecting_type else None,
            "target": c.target.name,
        }
        for c in site.candidates
    ]
    assert_same(
        rows(datatable), actual, f"selection map of {describe(sym)}", ordered=False
    )


@then(parsers.parse('the virtual call to "{selector}" is prunable'))
def dispatch_is_prunable(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    site = workspace.graph.dispatch_selection(sym)
    assert site.prunable, (
        f"{describe(sym)}: expected a prunable dispatch site, got "
        f"unprunable({','.join(site.unprunable_reasons)})"
    )


@then(
    parsers.parse(
        'threading the receiver from "{outer}" through "{mid}", '
        'the virtual call to "{virt}" devirtualizes to:'
    )
)
def devirtualize_through(
    workspace: Workspace,
    outer: str,
    mid: str,
    virt: str,
    datatable: list[list[str]],
) -> None:
    """The Phase-2 join the graph stores the data for but does not materialize:

    the receiver's concrete type recorded at the outer call site, threaded
    through the callee's `this`-dispatched virtual call, prunes the selection
    map down to the single override a real trace can reach.

    Table columns: qual_name | kind | [line].
    """
    outer_edge = _calls_edge(workspace, outer, mid)
    assert len(outer_edge.sites) == 1, (
        f"{outer} -> {mid}: expected exactly 1 call site, got {len(outer_edge.sites)}"
    )
    outer_site = outer_edge.sites[0]
    recv_type = outer_site.recv_type_usr
    assert recv_type is not None, (
        f"{outer} -> {mid}: the call site records no receiver type, "
        f"nothing to devirtualize with"
    )

    inner_edge = _calls_edge(workspace, mid, virt)
    assert all(s.recv_src_kind == "this" for s in inner_edge.sites), (
        f"{mid} -> {virt}: expected the virtual call to dispatch on `this` so "
        f"the outer receiver flows through, got "
        f"{[s.recv_src_kind for s in inner_edge.sites]}"
    )

    selection = workspace.graph.dispatch_selection(workspace.resolve(virt))
    assert selection.prunable, (
        f"{virt}: expected a prunable dispatch site, got "
        f"unprunable({','.join(selection.unprunable_reasons)})"
    )
    pruned = [
        c.target
        for c in selection.candidates
        if c.selecting_type is not None and c.selecting_type.usr == recv_type
    ]
    actual = [{"qual_name": t.name, "kind": t.kind, "line": t.line} for t in pruned]
    assert_same(
        rows(datatable),
        actual,
        f"devirtualized targets of {virt} for the receiver at {outer} -> {mid}",
        ordered=False,
    )
