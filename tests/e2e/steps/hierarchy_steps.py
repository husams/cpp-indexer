"""THEN: the class hierarchy -- bases, subclasses, overrides, dynamic dispatch."""

from __future__ import annotations

from pytest_bdd import parsers, then

from .symbol_facts import describe
from .tables import assert_same, rows
from .workspace import Workspace


def _hier_rows(syms) -> list[dict[str, object]]:
    return [{"qual_name": s.name, "kind": s.kind, "line": s.line} for s in syms]


@then(parsers.parse('record "{selector}" has the direct bases:'))
def record_direct_bases(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    """Direct bases via the graph API. Table columns: qual_name | kind | [line]."""
    sym = workspace.resolve(selector)
    actual = _hier_rows(workspace.graph.bases(sym))
    assert_same(rows(datatable), actual, f"direct bases of {describe(sym)}", ordered=False)


@then(parsers.parse('record "{selector}" has no bases'))
def record_no_bases(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    bases = workspace.graph.bases(sym)
    assert not bases, (
        f"{describe(sym)}: expected no bases, got {[describe(b) for b in bases]}"
    )


@then(parsers.parse('record "{selector}" has the direct subclasses:'))
def record_direct_subclasses(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    actual = _hier_rows(workspace.graph.subclasses(sym))
    assert_same(
        rows(datatable), actual, f"direct subclasses of {describe(sym)}", ordered=False
    )


@then(parsers.parse('record "{selector}" has no subclasses'))
def record_no_subclasses(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    subs = workspace.graph.subclasses(sym)
    assert not subs, (
        f"{describe(sym)}: expected no subclasses, got {[describe(s) for s in subs]}"
    )


@then(parsers.parse('the full base hierarchy of "{selector}" is:'))
def full_base_hierarchy(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    """Every transitive base, not just the direct ones."""
    sym = workspace.resolve(selector)
    actual = _hier_rows(workspace.graph.bases(sym, direct=False))
    assert_same(
        rows(datatable), actual, f"base hierarchy of {describe(sym)}", ordered=False
    )


@then(parsers.parse('the full derived subtree of "{selector}" is:'))
def full_derived_subtree(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    """Every transitive subclass, not just the direct ones."""
    sym = workspace.resolve(selector)
    actual = _hier_rows(workspace.graph.subclasses(sym, direct=False))
    assert_same(
        rows(datatable), actual, f"derived subtree of {describe(sym)}", ordered=False
    )


@then(parsers.parse('method "{selector}" overrides:'))
def method_overrides(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    actual = _hier_rows(workspace.graph.overrides(sym))
    assert_same(rows(datatable), actual, f"overrides of {describe(sym)}", ordered=False)


@then(parsers.parse('method "{selector}" is overridden by:'))
def method_overridden_by(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    actual = _hier_rows(workspace.graph.overridden_by(sym))
    assert_same(
        rows(datatable), actual, f"overriders of {describe(sym)}", ordered=False
    )


@then(parsers.parse('method "{selector}" is a virtual method'))
def method_is_virtual(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    assert workspace.graph.is_virtual_method(sym), (
        f"{describe(sym)}: expected a virtual method (pure, overriding, or overridden)"
    )


@then(parsers.parse('method "{selector}" is not a virtual method'))
def method_is_not_virtual(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    assert not workspace.graph.is_virtual_method(sym), (
        f"{describe(sym)}: expected a non-virtual method"
    )


@then(parsers.parse('a virtual call to "{selector}" can land on:'))
def virtual_call_targets(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    """The dynamic-dispatch target set: the method itself (unless pure) plus
    every transitive override."""
    sym = workspace.resolve(selector)
    actual = _hier_rows(workspace.graph.dispatch_targets(sym))
    assert_same(
        rows(datatable), actual, f"dispatch targets of {describe(sym)}", ordered=False
    )


@then(parsers.parse('the virtual dispatch points of "{selector}" are:'))
def virtual_dispatch_points(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    """The virtual callees of a function -- where it performs dynamic dispatch."""
    sym = workspace.resolve(selector)
    actual = _hier_rows(workspace.graph.virtual_callees(sym))
    assert_same(
        rows(datatable), actual, f"virtual dispatch points of {describe(sym)}",
        ordered=False,
    )
