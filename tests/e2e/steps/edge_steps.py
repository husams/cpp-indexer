"""THEN: the edge graph -- counts, relationship tables and per-kind totals."""

from __future__ import annotations

from pytest_bdd import parsers, then

from .edge_facts import edge_repr
from .tables import rows
from .workspace import Workspace


@then(parsers.parse("the index holds exactly {count:d} edge"))
@then(parsers.parse("the index holds exactly {count:d} edges"))
def index_edge_count(workspace: Workspace, count: int) -> None:
    edges = workspace.edges()
    assert len(edges) == count, (
        f"expected {count} edge(s), got {len(edges)}:\n"
        + "\n".join(f"    {edge_repr(s, e)}" for s, e in edges)
    )


@then("the index holds no edges")
def index_no_edges(workspace: Workspace) -> None:
    edges = workspace.edges()
    assert not edges, "expected no edges, got:\n" + "\n".join(
        f"    {edge_repr(s, e)}" for s, e in edges
    )


@then(parsers.parse("the index holds no {kind} edges"))
def index_no_edges_of_kind(workspace: Workspace, kind: str) -> None:
    hits = [(s, e) for s, e in workspace.edges() if e.kind == kind]
    assert not hits, f"expected no {kind} edges, got:\n" + "\n".join(
        f"    {edge_repr(s, e)}" for s, e in hits
    )


@then("the index holds the edges:")
def index_holds_edges(workspace: Workspace, datatable: list[list[str]]) -> None:
    _check_edges(workspace, datatable, exhaustive=False)


@then("the index holds exactly these edges:")
def index_holds_exactly_edges(workspace: Workspace, datatable: list[list[str]]) -> None:
    _check_edges(workspace, datatable, exhaustive=True)


def _check_edges(
    ws: Workspace, datatable: list[list[str]], *, exhaustive: bool
) -> None:
    """Table columns: src | kind | dst | [count] | [sites]. src/dst are selectors."""
    actual = ws.edges()
    rendered = [edge_repr(s, e) for s, e in actual]
    matched: set[int] = set()

    for row in rows(datatable):
        unknown = set(row) - {"src", "kind", "dst", "count", "sites"}
        assert not unknown, f"unknown edge column(s) {sorted(unknown)}"
        src = ws.resolve(row["src"])
        dst = ws.resolve(row["dst"])
        hits = [
            (s, e)
            for s, e in actual
            if s.id == src.id and e.peer.id == dst.id and e.kind == row["kind"]
        ]
        assert len(hits) == 1, (
            f"expected exactly 1 {row['kind']!r} edge "
            f"{row['src']} -> {row['dst']}, found {len(hits)}.\n"
            "  all edges:\n" + "\n".join(f"    {r}" for r in rendered)
        )
        s, e = hits[0]
        assert e.edge_id not in matched, (
            "the edge table mentions the same indexed edge more than once: "
            f"{edge_repr(s, e)}"
        )
        if row.get("count") is not None:
            assert e.count == row["count"], (
                f"edge {edge_repr(s, e)}: expected count {row['count']}, got {e.count}"
            )
        if "sites" in row:
            want = row["sites"]
            got = ",".join(f"{x.line}:{x.col}" for x in e.sites) or None
            assert got == want, (
                f"edge {edge_repr(s, e)}: expected sites {want!r}, got {got!r}"
            )
        matched.add(e.edge_id)

    if exhaustive:
        extra = [(s, e) for s, e in actual if e.edge_id not in matched]
        assert not extra, "index holds edges the table does not mention:\n" + "\n".join(
            f"    {edge_repr(s, e)}" for s, e in extra
        )


@then("the edge kind totals are:")
def edge_kind_totals(workspace: Workspace, datatable: list[list[str]]) -> None:
    """Table columns: kind | total. Must account for every edge kind present."""
    actual: dict[str, int] = {}
    for _, e in workspace.edges():
        actual[e.kind] = actual.get(e.kind, 0) + 1
    table = rows(datatable)
    for row in table:
        assert set(row) == {"kind", "total"}, (
            "edge kind totals require exactly the columns 'kind' and 'total'; "
            f"got {sorted(row)}"
        )
    kinds = [row["kind"] for row in table]
    assert len(kinds) == len(set(kinds)), "edge kind totals contain a duplicate kind"
    expected = {row["kind"]: row["total"] for row in table}
    assert actual == expected, (
        f"edge kind totals differ\n  expected: {dict(sorted(expected.items()))}\n"
        f"  actual:   {dict(sorted(actual.items()))}"
    )


@then("no edge points at an unresolved symbol")
def no_edge_points_at_stub(workspace: Workspace) -> None:
    """Every endpoint of every edge is a real, indexed symbol."""
    dangling = [(s, e) for s, e in workspace.edges() if s.is_stub or e.peer.is_stub]
    assert not dangling, (
        "expected every edge endpoint to be a resolved symbol, but these edges "
        "touch a stub:\n" + "\n".join(f"    {edge_repr(s, e)}" for s, e in dangling)
    )
