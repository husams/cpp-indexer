"""THEN: the signature/type tier -- types, parameters, templates, definitions."""

from __future__ import annotations

from pytest_bdd import parsers, then

from .symbol_facts import describe
from .tables import assert_same, rows
from .workspace import Workspace


@then(parsers.parse('symbol "{selector}" returns "{type_name}"'))
def symbol_returns(workspace: Workspace, selector: str, type_name: str) -> None:
    sym = workspace.resolve(selector)
    sig = workspace.graph.signature(sym)
    actual = sig.returns.spelling if sig.returns else None
    assert actual == type_name, (
        f"{describe(sym)}: expected return type {type_name!r}, got {actual!r}"
    )


@then(parsers.parse('symbol "{selector}" has type "{type_name}"'))
def symbol_of_type(workspace: Workspace, selector: str, type_name: str) -> None:
    """The declared type of a variable/field (signature tier `of_type`)."""
    sym = workspace.resolve(selector)
    sig = workspace.graph.signature(sym)
    actual = sig.of_type.spelling if sig.of_type else None
    assert actual == type_name, (
        f"{describe(sym)}: expected declared type {type_name!r}, got {actual!r}"
    )


@then(parsers.parse('symbol "{selector}" takes the parameters:'))
def symbol_parameters(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    sig = workspace.graph.signature(sym)
    actual = [
        {
            "position": p.position,
            "name": p.name,
            "type": p.type.spelling if p.type else None,
        }
        for p in sig.params
    ]
    assert_same(rows(datatable), actual, f"parameters of {describe(sym)}")


@then(parsers.parse('symbol "{selector}" takes no parameters'))
def symbol_no_parameters(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    params = workspace.graph.signature(sym).params
    assert not params, f"{describe(sym)}: expected no parameters, got {params}"


@then(parsers.parse('symbol "{selector}" declares the template parameters:'))
def symbol_template_params(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    actual = [
        {"position": p.position, "name": p.name, "kind": p.kind_name}
        for p in workspace.graph.template_params(sym)
    ]
    assert_same(rows(datatable), actual, f"template parameters of {describe(sym)}")


@then(parsers.parse('symbol "{selector}" binds the template arguments:'))
def symbol_template_args(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    actual = [
        {"position": a.position, "kind": a.kind_name, "value": a.literal}
        for a in workspace.graph.template_args(sym)
    ]
    assert_same(rows(datatable), actual, f"template arguments of {describe(sym)}")


@then(parsers.parse('symbol "{selector}" binds no template arguments'))
def symbol_no_template_args(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    args = workspace.graph.template_args(sym)
    assert not args, f"{describe(sym)}: expected no template arguments, got {args}"


@then(parsers.parse('symbol "{selector}" is an instantiation of "{primary}"'))
def symbol_template_of(workspace: Workspace, selector: str, primary: str) -> None:
    sym = workspace.resolve(selector)
    want = workspace.resolve(primary)
    got = workspace.graph.template_of(sym)
    assert got is not None, f"{describe(sym)}: template_of() returned None"
    assert got.id == want.id, (
        f"{describe(sym)}: expected instantiation of {describe(want)}, "
        f"got {describe(got)}"
    )


@then(parsers.parse('symbol "{selector}" has {count:d} instantiation'))
@then(parsers.parse('symbol "{selector}" has {count:d} instantiations'))
def symbol_instantiation_count(workspace: Workspace, selector: str, count: int) -> None:
    sym = workspace.resolve(selector)
    inst = workspace.graph.instantiations(sym)
    assert len(inst) == count, (
        f"{describe(sym)}: expected {count} instantiation(s), got {len(inst)}:\n"
        + "\n".join(f"    {describe(i)}" for i in inst)
    )


@then(parsers.parse('symbol "{selector}" has the definitions:'))
def symbol_definitions(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    actual = [
        {
            "file": d.file.name if d.file else None,
            "line": d.line,
            "end_line": d.end_line,
            "component": d.component,
        }
        for d in workspace.graph.definitions(sym)
    ]
    assert_same(rows(datatable), actual, f"definitions of {describe(sym)}")
