"""THEN: index-level facts -- the database, the resolve pass and its totals."""

from __future__ import annotations

from pytest_bdd import parsers, then

from .symbol_facts import describe
from .workspace import Workspace


@then("the index database exists")
def index_db_exists(workspace: Workspace) -> None:
    assert workspace.db.is_file(), f"expected an index database at {workspace.db}"


@then("the entity graph is resolved")
def graph_is_resolved(workspace: Workspace) -> None:
    stats = workspace.graph.stats()
    assert stats.get("resolved_at"), (
        f"graph_resolved_at is unset -- `cidx resolve` did not complete. stats={stats}"
    )


@then(parsers.parse("the index holds {count:d} indexed file"))
@then(parsers.parse("the index holds {count:d} indexed files"))
def index_file_count(workspace: Workspace, count: int) -> None:
    actual = workspace.graph.stats()["files_indexed"]
    assert actual == count, f"expected {count} indexed file(s), got {actual}"


@then(parsers.parse("the index holds exactly {count:d} symbol"))
@then(parsers.parse("the index holds exactly {count:d} symbols"))
def index_symbol_count(workspace: Workspace, count: int) -> None:
    syms = workspace.symbols()
    assert len(syms) == count, (
        f"expected {count} symbol(s), got {len(syms)}:\n"
        + "\n".join(f"    {describe(s)}" for s in syms)
    )


@then(parsers.parse("the index holds exactly {count:d} unresolved symbol"))
@then(parsers.parse("the index holds exactly {count:d} unresolved symbols"))
def index_unresolved_symbol_count(workspace: Workspace, count: int) -> None:
    """Unresolved == a minted stub: a USR an edge anchors but that no indexed
    file declares or defines (`Sym.is_stub`). A single-file fixture that names
    nothing external must produce none."""
    stubs = [s for s in workspace.symbols() if s.is_stub]
    reported = workspace.graph.stats()["stubs"]
    assert len(stubs) == count, (
        f"expected {count} unresolved (stub) symbol(s), got {len(stubs)} "
        f"[stats reports stubs={reported}]:\n"
        + "\n".join(f"    {describe(s)}  usr={s.usr}" for s in stubs)
    )
