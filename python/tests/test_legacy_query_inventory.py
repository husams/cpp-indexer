"""Executable differential coverage for the HSE-27 compatibility boundary."""

from __future__ import annotations

import inspect
from pathlib import Path

import pytest

from indexer.entity_graph import ClassKind, EdgeKind, EntityGraph, EntityKind, EntityQuery
from indexer.query import Definition, GraphQuery
from indexer.queryplan import Executor, canonical_json
from indexer.storage import Storage


def _inventory() -> list[tuple[str, str, str]]:
    path = Path(__file__).parents[2] / "tests" / "golden" / "legacy_query_operations.txt"
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        surface, qualified = line.split(":", 1)
        owner, operation = qualified.split(".", 1)
        entries.append((surface, owner, operation))
    return entries


def _public(cls):
    return {
        name for name, member in inspect.getmembers(cls, inspect.isfunction)
        if not name.startswith("_") and name != "close"
    }


def test_inventory_is_complete_against_reflected_public_surfaces():
    entries = _inventory()
    owners = {"GraphQuery": GraphQuery, "EntityGraph": EntityGraph,
              "EntityQuery": EntityQuery}
    for surface, owner, operation in entries:
        if surface.startswith("python_"):
            assert operation in _public(owners[owner]), f"stale {owner}.{operation}"

    for surface, cls in (
        ("python_graph", GraphQuery),
        ("python_entity_graph", EntityGraph),
        ("python_entity_query", EntityQuery),
    ):
        listed = {operation for s, owner, operation in entries if s == surface}
        assert listed == _public(cls), f"inventory mismatch for {surface}"


def _consume(value):
    if isinstance(value, (str, bytes, int, bool, type(None), dict)):
        return value
    if hasattr(value, "to_dict") and not isinstance(value, (list, tuple)):
        try:
            return value.to_dict()
        except TypeError:
            pass
    if isinstance(value, (list, tuple, set)):
        return [_consume(item) for item in value]
    if hasattr(value, "__iter__"):
        return [_consume(item) for item in value]
    return repr(value)


def _graph_args(name, graph, sym, edge, definition):
    main = graph.get("c:@F@main")
    helper = graph.get("c:@F@helper")
    draw = graph.get("c:@S@Base@F@draw#") or sym
    values = {
        "sym": sym, "method": draw.id, "fn": main.id, "callee": helper.id,
        "inst_member": draw.id, "ident": main.id, "start": main.id, "src": main.id,
        "dst": helper, "edge": edge, "edge_id": edge.edge_id,
        "file_id": 1,
        "line": 12, "col": 5, "type_or_id": 1, "type_id": 1,
        "declared_type_id": 1, "adjusted_type_id": 1,
        "definition": definition, "pattern": "Base", "spelling": "draw",
        "path_substr": "main.c", "kind": "function", "kinds": ("calls",),
        "direction": "out", "limit": 2, "direct": True, "access": None,
        "with_sites": False, "include_instantiations": False,
        "include_overrides": True, "close_subtypes": False,
        "relation": None, "min_depth": 1, "max_depth": 1, "depth": 1,
        "max_nodes": 10, "external": False, "component_name": None,
        "symbol_kinds": ("class",), "entity_kinds": (1,),
    }
    args, kwargs = [], {}
    for parameter in list(inspect.signature(getattr(graph, name)).parameters.values()):
        if parameter.kind is inspect.Parameter.VAR_POSITIONAL:
            args.append("Base")
        elif parameter.kind is inspect.Parameter.KEYWORD_ONLY:
            if parameter.name == "close_subtypes":
                kwargs[parameter.name] = False
        elif parameter.default is inspect.Parameter.empty:
            args.append(values.get(parameter.name))
        elif parameter.name in values:
            args.append(values[parameter.name])
    if name == "reaches":
        args = [main, helper, ("calls",)]
    if name == "walk":
        args = [main, ("calls",)]
    if name == "records_by_name":
        args = ["Base"]
        kwargs.update(symbol_kinds=("class",), entity_kinds=(1,))
    if name == "by_qual_or_spelling":
        args = ["Base"]
    if name == "make_file":
        args = ["main.c"]
    if name == "slot_type_facts_for_ids":
        args = [1, 1]
    return args, kwargs


def test_python_inventory_executes_production_plans_and_legacy_oracles(
    g, monkeypatch: pytest.MonkeyPatch
):
    entries = _inventory()
    calls: list[str] = []
    original_run = Executor.run

    def recording_run(executor, plan, after_id=None):
        calls.append(canonical_json(plan))
        return original_run(executor, plan, after_id=after_id)

    monkeypatch.setattr(Executor, "run", recording_run)
    sym = g.get("c:@F@main")
    assert sym is not None
    edge = g.edges_out(sym, ("calls",))[0]
    definitions = g.definitions(sym)
    definition = (
        definitions[0]
        if definitions
        else Definition(sym=sym, component=None, file=None, line=None, col=None, def_id=-1)
    )

    # Every reflected public operation is invoked on the same seeded fixture.
    # Builder-only methods are executed by the terminal that consumes their
    # returned plan; make_file is record construction, not a read operation.
    builder_only = {"plan_for", "make_file"}
    for surface, owner, operation in entries:
        if surface == "cpp_graph" or owner != "GraphQuery":
            continue
        args, kwargs = _graph_args(operation, g, sym, edge, definition)
        before = len(calls)
        try:
            result = getattr(g, operation)(*args, **kwargs)
            _consume(result)
        except Exception as exc:  # pragma: no cover - identifies a missing case
            pytest.fail(f"{owner}.{operation} did not execute: {exc}")
        if operation not in builder_only:
            assert len(calls) > before, f"{owner}.{operation} bypassed QueryPlan"

    # Differential production oracles for the seeded selection, ordering, and
    # hydration boundary. These are independent SQLite legacy reads, not
    # another invocation of the adapter under test.
    expected_id = g._c.execute(
        "SELECT id FROM symbol WHERE usr = ?", ("c:@F@main",)
    ).fetchone()[0]
    assert sym.id == expected_id
    expected_peers = [
        row[0] for row in g._c.execute(
            "SELECT dst_id FROM edge WHERE src_id = ? AND kind = 1 ORDER BY id",
            (sym.id,),
        )
    ]
    assert [item.peer.id for item in g.edges_out(sym, ("calls",))] == expected_peers
    expected_sites = [
        tuple(row) for row in g._c.execute(
            "SELECT line, col FROM edge_site WHERE edge_id = ? "
            "ORDER BY file_id, line, col", (edge.edge_id,)
        )
    ]
    assert [(site.line, site.col) for site in g.sites(edge)] == expected_sites


def test_entity_inventory_executes_production_plans(g, monkeypatch):
    entries = _inventory()
    calls: list[str] = []
    original_run = Executor.run

    def recording_run(executor, plan, after_id=None):
        calls.append(canonical_json(plan))
        return original_run(executor, plan, after_id=after_id)

    monkeypatch.setattr(Executor, "run", recording_run)
    graph = EntityGraph(g)
    query = graph.query()
    for surface, owner, operation in entries:
        if surface == "python_entity_graph":
            method = getattr(graph, operation)
            if operation in {"abstract_class", "instance", "interface", "klass",
                             "record", "struct", "template"}:
                value = method("Base")
            elif operation == "by_kind":
                value = method(EdgeKind.USES)
            elif operation == "entity":
                value = method(g.get("c:@F@main"))
            elif operation == "find":
                value = method("Base")
            else:
                value = method()
            _consume(value)
        elif surface == "python_entity_query":
            method = getattr(query, operation)
            if operation in {"relation", "step", "then"}:
                value = method(EdgeKind.GENERALIZES)
            elif operation == "where":
                value = method(lambda _node: True)
            elif operation == "of_kind":
                value = method(EntityKind.CLASS)
            elif operation == "of_class_kind":
                value = method(ClassKind.CONCRETE)
            elif operation == "exclude":
                value = method("Base")
            elif operation == "named":
                value = method("Base")
            elif operation == "to_plan":
                value = method()
                Executor(Storage.from_connection(g._c, g.db_path)).run(value)
                continue
            else:
                value = method()
            _consume(value)
    assert calls, "entity production inventory did not execute QueryPlan"
