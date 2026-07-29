"""Executable differential coverage for the HSE-27 compatibility boundary."""

from __future__ import annotations

import inspect
import json
from pathlib import Path

import pytest

from indexer.entity_graph import ClassKind, EdgeKind, EntityGraph, EntityKind, EntityQuery
from indexer.query import EDGE_KINDS, EDGE_NAMES, Definition, GraphQuery
from indexer.queryplan import Executor, canonical_json, select as plan_select
from indexer.storage import SYMBOL_KIND_IDS, Storage


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


def _semantic(value):
    """Reduce a production result to stable identity/cardinality facts."""
    if isinstance(value, dict):
        return {key: _semantic(item) for key, item in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [_semantic(item) for item in value]
    if value is None or isinstance(value, (str, bytes, int, bool, float)):
        return value
    if hasattr(value, "id") and not hasattr(value, "edge_id"):
        return ("symbol", value.id)
    if hasattr(value, "edge_id"):
        if not hasattr(value, "src_id"):
            caller = getattr(value, "caller", None)
            return ("edge_context", value.edge_id,
                    getattr(caller, "id", None))
        return ("edge", value.edge_id, value.src_id, value.dst_id,
                value.kind, value.count)
    if hasattr(value, "def_id") and hasattr(value, "sym") and hasattr(value.sym, "id"):
        return ("definition", value.sym.id, value.def_id)
    if hasattr(value, "nodes_by_id"):
        return ("traversal", tuple(sorted(value.nodes_by_id)))
    if hasattr(value, "__iter__"):
        return [_semantic(item) for item in value]
    return _consume(value)


def _legacy_ids(conn, sql, params=()):
    return [row[0] for row in conn.execute(sql, params)]


def _stable_site(value):
    return {
        **value,
        "file": Path(value["file"]).name if value.get("file") else None,
    }


_GRAPH_FIXTURE_ORACLES = {
    "aliased_by": [],
    "bases": [],
    "by_name": [],
    "by_qual_or_spelling": [("symbol", 7)],
    "call_args": [],
    "call_args_at": [],
    "call_sites_into": [("edge_context", 1, 1)],
    "callees": [("symbol", 2)],
    "callees_of_definition": [],
    "callers": [],
    "definitions": [],
    "dispatch_targets": [("symbol", 12), ("symbol", 14)],
    "edges_in": [],
    "entity_nodes_ready": True,
    "find": [],
    "instantiations": [],
    "members": [],
    "overridden_by": [("symbol", 12)],
    "overrides": [],
    "possible_callees": [],
    "records_by_name": [],
    "redefined": [],
    "references": [],
    "referencing_definitions": [],
    "require_edges": None,
    "signature": "SignatureInfo(returns=None, params=(), of_type=None, underlying=None)",
    "signature_slots": [],
    "slot_type_facts_for_ids": ["value", "other", None],
    "subclasses": [],
    "symbols_in_file": [("symbol", 1)],
    "template_args": [],
    "template_of": None,
    "template_of_member": None,
    "template_params": [],
    "type_layers": [{
        "path": "root", "relation": "root", "position": 0,
        "depth": 0, "status": "unknown",
    }],
    "type_users": [],
    "uses_of_definition": [],
    "virtual_call_sites": [],
    "virtual_callees": [],
}


def _assert_graph_legacy_parity(operation, args, value, graph):
    """Compare each terminal's selection/cardinality to an independent SQL oracle.

    The adapter is deliberately not used by this helper: the SQL reads below
    are the legacy semantic oracle, while the value was produced through the
    QueryPlan-backed public operation.
    """
    actual = _semantic(value)
    conn = graph._c
    sid = getattr(args[0], "id", args[0]) if args else None
    relation = {
        "aliased_by": ("alias_of", "in"), "bases": ("inherits", "out"),
        "callees": ("calls", "out"), "callers": ("calls", "in"),
        "instantiations": ("instantiates", "in"),
        "overridden_by": ("overrides", "in"), "overrides": ("overrides", "out"),
        "subclasses": ("inherits", "in"),
    }
    if operation == "get":
        expected = _legacy_ids(conn, "SELECT id FROM symbol WHERE id = ?", (sid,))
        actual_ids = [actual[1]] if isinstance(actual, tuple) and actual[0] == "symbol" else []
        assert actual_ids == expected
        return
    if operation in {"by_name", "by_qual_or_spelling", "records_by_name"}:
        names = tuple(arg for arg in args if isinstance(arg, str))
        if operation == "by_name":
            kind = args[1] if len(args) > 1 else None
            where = "spelling = ?"
            params = [names[0]]
            if kind is not None:
                where += " AND kind = ?"
                params.append(SYMBOL_KIND_IDS[kind])
            expected = _legacy_ids(
                conn, f"SELECT id FROM symbol WHERE {where} ORDER BY usr", params
            )
        elif operation == "records_by_name":
            expected = _legacy_ids(
                conn,
                "SELECT s.id FROM symbol s JOIN entity_node en ON en.id = s.id "
                "WHERE (s.qual_name = ? OR s.spelling = ?) AND s.kind = ? "
                "AND en.kind = ? ORDER BY s.id",
                (names[0], names[0], SYMBOL_KIND_IDS["class"], int(EntityKind.CLASS)),
            ) if names else []
        else:
            marks = ",".join("?" * len(names))
            expected = _legacy_ids(
                conn,
                f"SELECT id FROM symbol WHERE qual_name IN ({marks}) OR spelling IN ({marks}) ORDER BY id",
                names + names,
            ) if names else []
        assert [item[1] for item in actual if isinstance(item, tuple) and item[0] == "symbol"] == expected
        return
    if operation in relation:
        kind, direction = relation[operation]
        column = "dst_id" if direction == "out" else "src_id"
        expected = _legacy_ids(
            conn,
            f"SELECT {column} FROM edge WHERE {'src_id' if direction == 'out' else 'dst_id'} = ? "
            "AND kind = ? ORDER BY id",
            (sid, EDGE_KINDS[kind]),
        )
        actual_ids = [item[1] for item in actual if isinstance(item, tuple) and item[0] == "symbol"]
        assert actual_ids == expected
        return
    if operation == "dispatch_targets":
        expected = _legacy_ids(
            conn,
            "WITH RECURSIVE reach(id) AS ("
            "SELECT src_id FROM edge WHERE dst_id = ? AND kind = ? UNION "
            "SELECT e.src_id FROM edge e JOIN reach r ON e.dst_id = r.id "
            "WHERE e.kind = ?) SELECT id FROM reach "
            "WHERE id IN (SELECT id FROM symbol WHERE is_pure = 0) ORDER BY id",
            (sid, EDGE_KINDS["overrides"], EDGE_KINDS["overrides"]),
        )
        actual_ids = [item[1] for item in actual if item[0] == "symbol"]
        assert set(actual_ids) == set(expected)
        return
    if operation == "neighbors":
        kind_names = args[1] or tuple(EDGE_KINDS)
        direction = args[2]
        column = "dst_id" if direction == "out" else "src_id"
        source_column = "src_id" if direction == "out" else "dst_id"
        marks = ",".join("?" * len(kind_names))
        expected = _legacy_ids(
            conn,
            f"SELECT {column} FROM edge WHERE {source_column} = ? AND kind IN ({marks}) ORDER BY id",
            (sid, *(EDGE_KINDS[name] for name in kind_names)),
        )
        actual_ids = [item[1] for item in actual if item[0] == "symbol"]
        assert actual_ids == expected
        return
    if operation in {"edges_in", "edges_out", "references"}:
        if operation == "edges_out":
            direction, kinds = "out", ("calls",)
        elif operation == "edges_in":
            direction, kinds = "in", ("calls",)
        else:
            direction, kinds = "in", ("calls", "uses", "alias_of", "of_type")
        source_column = "src_id" if direction == "out" else "dst_id"
        marks = ",".join("?" * len(kinds))
        rows = conn.execute(
            "SELECT e.id, e.src_id, e.dst_id, e.kind, "
            "CASE WHEN e.count <> 0 THEN e.count ELSE 1 END AS effective_count "
            f"FROM edge e WHERE e.{source_column} = ? "
            f"AND e.kind IN ({marks}) "
            "ORDER BY -effective_count, e.kind, e.id",
            (sid, *(EDGE_KINDS[kind] for kind in kinds)),
        )
        expected = [
            ("edge", row[0], row[1], row[2], EDGE_NAMES[row[3]], row[4])
            for row in rows
        ]
        assert actual == expected
        return
    if operation in {"sites", "declaration_sites"}:
        assert all(isinstance(item, dict) for item in actual)
        if operation == "sites":
            edge_id = args[0].edge_id
            rows = conn.execute(
                "SELECT es.file_id, es.line, es.col, es.conditional, es.args_sig "
                "FROM edge_site_read es WHERE es.edge_id = ? "
                "ORDER BY es.file_id, es.line, es.col",
                (edge_id,),
            )
        else:
            rows = conn.execute(
                "SELECT ds.file_id, ds.line, ds.col, 0, NULL "
                "FROM decl_site ds WHERE ds.symbol_id = ? "
                "ORDER BY ds.file_id, ds.line, ds.col",
                (sid,),
            )
        files = graph._files()
        expected = [
            {
                "file": files.get(row[0], (None, None))[0] if row[0] else None,
                "line": row[1],
                "col": row[2],
                "conditional": bool(row[3]),
                "args_sig": row[4],
            }
            for row in rows
        ]
        assert actual == expected
        return
    if operation == "edge_count":
        assert actual == conn.execute("SELECT COUNT(*) FROM edge").fetchone()[0]
        return
    if operation == "stats":
        assert actual["edges"] == conn.execute("SELECT COUNT(*) FROM edge").fetchone()[0]
        assert actual["symbols"] == conn.execute("SELECT COUNT(*) FROM symbol").fetchone()[0]
        return
    if operation == "def_decl_locations":
        definition, declaration = actual
        assert Path(definition[0]).name == "main.c"
        assert definition[1:] == [10, 1]
        assert declaration is None
        return
    if operation == "dispatch_selection":
        assert actual["receiver_static_type"]["id"] == 7
        assert actual["declared_target"]["id"] == 8
        assert [
            (
                candidate["selecting_type"]["id"],
                candidate["target"]["id"],
                candidate["inherited"],
            )
            for candidate in actual["candidates"]
        ] == [(11, 12, False), (13, 14, False)]
        assert actual["prunable"] is True
        assert actual["unprunable_reasons"] == []
        return
    if operation == "is_virtual_method":
        expected = conn.execute(
            "SELECT is_pure OR EXISTS("
            "SELECT 1 FROM edge WHERE kind = ? AND (src_id = ? OR dst_id = ?)"
            ") FROM symbol WHERE id = ?",
            (EDGE_KINDS["overrides"], sid, sid, sid),
        ).fetchone()[0]
        assert actual is bool(expected)
        return
    if operation in {"receiver_provenance"}:
        assert _stable_site(actual) == {
            "file": "main.c", "line": 12, "col": 5,
            "conditional": False, "args_sig": None,
        }
        return
    if operation == "reaches":
        assert actual == [("symbol", 1), ("symbol", 2)]
        return
    if operation == "walk":
        assert actual == ("traversal", (1, 2, 3, 6))
        return
    if operation not in _GRAPH_FIXTURE_ORACLES:
        pytest.fail(f"missing exact legacy oracle for GraphQuery.{operation}")
    assert actual == _GRAPH_FIXTURE_ORACLES[operation]


_ENTITY_FIXTURE_ORACLES = {
    "abstract_class": [("symbol", 7)],
    "by_kind": [],
    "edges": [
        ("Derived", "generalizes", "Base", 1, "protected"),
        ("Derived2", "generalizes", "Derived", 1, "protected"),
    ],
    "entities": [("symbol", 7), ("symbol", 10), ("symbol", 11), ("symbol", 13)],
    "entity": ("symbol", 1),
    "find": [("symbol", 7)],
    "instance": [],
    "interface": [],
    "klass": [],
    "kinds": [1],
    "query": [("symbol", 7), ("symbol", 10), ("symbol", 11), ("symbol", 13)],
    "record": [("symbol", 7)],
    "struct": [],
    "template": [],
    "abstract": [("symbol", 7)],
    "aggregated_in": [],
    "aggregates": [],
    "associated_with": [],
    "associates": [],
    "bases": [("symbol", 7), ("symbol", 11)],
    "befriended_by": [],
    "composed_in": [],
    "composes": [],
    "concrete": [("symbol", 10), ("symbol", 11), ("symbol", 13)],
    "count": 4,
    "created_by": [],
    "creates": [],
    "derived": [("symbol", 11), ("symbol", 13)],
    "destroyed_by": [],
    "destroys": [],
    "displays": ["Base", "Base::Nested", "Derived", "Derived2"],
    "exclude": [("symbol", 10), ("symbol", 11), ("symbol", 13)],
    "first": ("symbol", 7),
    "friends": [],
    "implemented_by": [],
    "implementors": [],
    "implements": [],
    "instances": [],
    "instantiates": [],
    "interfaces": [],
    "named": [("symbol", 7), ("symbol", 10)],
    "names": ["Base", "Base::Nested", "Derived", "Derived2"],
    "nodes": [("symbol", 7), ("symbol", 10), ("symbol", 11), ("symbol", 13)],
    "of_class_kind": [("symbol", 10), ("symbol", 11), ("symbol", 13)],
    "of_kind": [("symbol", 7), ("symbol", 11), ("symbol", 13)],
    "relation": [("symbol", 7), ("symbol", 11)],
    "specialized_by": [],
    "specializes": [],
    "step": [("symbol", 7), ("symbol", 11)],
    "then": [("symbol", 7), ("symbol", 11)],
    "to_dict": [
        (7, "Base", "class", "abstract_class", "abstract"),
        (10, "Base::Nested", "struct", "class", "concrete"),
        (11, "Derived", "class", "class", "concrete"),
        (13, "Derived2", "class", "class", "concrete"),
    ],
    "used_by": [],
    "uses": [],
    "where": [("symbol", 7), ("symbol", 10), ("symbol", 11), ("symbol", 13)],
}


def _assert_entity_legacy_parity(owner, operation, value, graph):
    """Check EntityGraph/EntityQuery terminals against raw entity tables."""
    actual = _semantic(value)
    conn = graph._c
    valid_nodes = {
        row[0] for row in conn.execute(
            "SELECT id FROM entity_node UNION SELECT src_id FROM entity_edge "
            "UNION SELECT dst_id FROM entity_edge"
        )
    }
    if operation == "stats":
        assert actual == {
            "entities": len(valid_nodes),
            "edges": conn.execute("SELECT COUNT(*) FROM entity_edge").fetchone()[0],
            "by_kind": {
                EdgeKind(row[0]).verb: row[1] for row in conn.execute(
                    "SELECT kind, COUNT(*) FROM entity_edge GROUP BY kind"
                )
            },
        }
        return
    if operation == "edges":
        if owner == "EntityQuery":
            assert actual == []
            return
        actual = [
            (
                item["src"], item["kind"], item["dst"], item["count"],
                item.get("access", "public"),
            )
            for item in actual
        ]
    elif operation == "kinds":
        actual = [int(item) for item in actual]
    elif operation == "to_dict":
        actual = [
            (
                item["id"], item["name"], item["kind"],
                item["entity_type"], item["class_kind"],
            )
            for item in actual
        ]
    if operation not in _ENTITY_FIXTURE_ORACLES:
        pytest.fail(f"missing exact legacy oracle for entity operation {operation}")
    assert actual == _ENTITY_FIXTURE_ORACLES[operation]


def _assert_operation_plan(
    calls: list[str], truncated: list[bool], before: int, operation: str,
    *, builder: bool = False,
):
    produced = calls[before:]
    assert produced, f"{operation} did not execute a production QueryPlan"
    assert not any(truncated[before:]), f"{operation} used a truncated QueryPlan"
    if builder:
        return
    meaningful = {
        "select", "out", "in", "where", "sites", "limit", "count", "union",
        "intersect", "except", "order_by", "distinct", "path", "rank",
    }
    assert any(
        meaningful.intersection(stage["op"] for stage in json.loads(plan)["stages"])
        for plan in produced
    ), f"{operation} executed only an existence/touch plan"


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
    truncated: list[bool] = []
    original_run = Executor.run

    def recording_run(executor, plan, after_id=None):
        calls.append(canonical_json(plan))
        result = original_run(executor, plan, after_id=after_id)
        truncated.append(result.truncated)
        return result

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

    # Every reflected public read operation is invoked on the same seeded
    # fixture. Builder methods are immediately consumed through their real
    # terminal; make_file is the sole record-construction operation.
    for surface, owner, operation in entries:
        if surface == "cpp_graph" or owner != "GraphQuery":
            continue
        args, kwargs = _graph_args(operation, g, sym, edge, definition)
        before = len(calls)
        try:
            result = getattr(g, operation)(*args, **kwargs)
            if operation == "plan_for":
                result = Executor(Storage.from_connection(
                    g._c, g.db_path
                )).run((result | plan_select(["id"])).plan)
            _consume(result)
        except Exception as exc:  # pragma: no cover - identifies a missing case
            pytest.fail(f"{owner}.{operation} did not execute: {exc}")
        if operation not in {"make_file", "plan_for"}:
            _assert_graph_legacy_parity(operation, args, result, g)
        if operation != "make_file":
            _assert_operation_plan(calls, truncated, before, operation)

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
    truncated: list[bool] = []
    original_run = Executor.run

    def recording_run(executor, plan, after_id=None):
        calls.append(canonical_json(plan))
        result = original_run(executor, plan, after_id=after_id)
        truncated.append(result.truncated)
        return result

    monkeypatch.setattr(Executor, "run", recording_run)
    graph = EntityGraph(g)
    query = graph.query()
    for surface, owner, operation in entries:
        before = len(calls)
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
            if isinstance(value, EntityQuery):
                value = list(value.nodes())
            _assert_entity_legacy_parity(owner, operation, value, graph)
            _consume(value)
            _assert_operation_plan(
                calls, truncated, before, f"{owner}.{operation}",
                builder=operation in {"query"},
            )
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
                _assert_operation_plan(
                    calls, truncated, before, f"{owner}.{operation}", builder=True
                )
                continue
            else:
                value = method()
            if isinstance(value, EntityQuery):
                value = list(value.nodes())
            _assert_entity_legacy_parity(owner, operation, value, graph)
            _consume(value)
            _assert_operation_plan(
                calls, truncated, before, f"{owner}.{operation}",
                builder=operation in {
                    "query", "relation", "step", "then", "where", "of_kind",
                    "of_class_kind", "exclude", "named", "to_plan",
                },
            )
