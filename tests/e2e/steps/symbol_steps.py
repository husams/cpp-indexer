"""THEN: the symbol rows themselves -- membership, exhaustiveness and spans."""

from __future__ import annotations

from pytest_bdd import parsers, then

from .symbol_facts import describe, match_one
from .tables import rows
from .workspace import Workspace


@then("the index holds the symbols:")
def index_holds_symbols(workspace: Workspace, datatable: list[list[str]]) -> None:
    """Each row must match exactly one indexed symbol on every stated column."""
    for row in rows(datatable):
        match_one(workspace, row, "symbol table")


@then("the index holds exactly these symbols:")
def index_holds_exactly_symbols(
    workspace: Workspace, datatable: list[list[str]]
) -> None:
    """As above, and the table must account for every symbol in the index."""
    matched: set[int] = set()
    for row in rows(datatable):
        symbol = match_one(workspace, row, "symbol table")
        assert symbol.id not in matched, (
            "the exact symbol table mentions the same indexed symbol more than once: "
            f"{describe(symbol)}"
        )
        matched.add(symbol.id)
    extra = [s for s in workspace.symbols() if s.id not in matched]
    assert not extra, "index holds symbols the table does not mention:\n" + "\n".join(
        f"    {describe(s)}  usr={s.usr}" for s in extra
    )


@then(parsers.parse('symbol "{selector}" spans lines {start:d} to {end:d}'))
def symbol_spans(workspace: Workspace, selector: str, start: int, end: int) -> None:
    sym = workspace.resolve(selector)
    assert (sym.line, sym.end_line) == (start, end), (
        f"{describe(sym)}: expected span {start}..{end}, got {sym.line}..{sym.end_line}"
    )
