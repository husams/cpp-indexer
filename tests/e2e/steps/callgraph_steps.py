"""THEN: the call graph -- callers and callees through the graph API."""

from __future__ import annotations

from pytest_bdd import parsers, then

from .symbol_facts import describe
from .tables import assert_same, rows
from .workspace import Workspace


@then(parsers.parse('symbol "{selector}" calls:'))
def symbol_calls(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    """Callees via the graph API. Table columns: qual_name | kind | [line]."""
    sym = workspace.resolve(selector)
    callees = workspace.graph.callees(sym)
    actual = [{"qual_name": c.name, "kind": c.kind, "line": c.line} for c in callees]
    assert_same(rows(datatable), actual, f"callees of {describe(sym)}", ordered=False)


@then(parsers.parse('symbol "{selector}" is called by:'))
def symbol_called_by(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    callers = workspace.graph.callers(sym)
    actual = [{"qual_name": c.name, "kind": c.kind, "line": c.line} for c in callers]
    assert_same(rows(datatable), actual, f"callers of {describe(sym)}", ordered=False)


@then(parsers.parse('symbol "{selector}" is called by nothing'))
def symbol_no_callers(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    callers = workspace.graph.callers(sym)
    assert not callers, (
        f"{describe(sym)}: expected no callers, got {[describe(c) for c in callers]}"
    )
