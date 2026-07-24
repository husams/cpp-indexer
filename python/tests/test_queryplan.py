"""CXQ QueryPlan tier tests (indexer/queryplan.py).

Mirror of tests/query_plan_test.cpp. The canonical-JSON case pins the SAME
golden file (tests/golden/cxq_plans.txt) as the C++ suite -- the cross-language
byte-parity anchor. Regenerate the golden from the C++ side with
CIDX_UPDATE_GOLDEN=1; this suite only verifies.
"""

from __future__ import annotations

import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from indexer.storage import Storage, Symbol  # noqa: E402
from indexer.utils.hashing import md5_of  # noqa: E402
from indexer import queryplan as qp  # noqa: E402
from indexer.queryplan import (  # noqa: E402
    Executor, PlanError, all_of, canonical_json, codebase, count, distinct,
    entity, eq, except_, glob, in_, in_list, intersect, limit, ne, nodes,
    not_, order_by, out, select, start, symbol, union_, validate, view, where,
    parse_cxq,
)

_REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", ".."))
_GOLDEN = os.path.join(_REPO_ROOT, "tests", "golden", "cxq_plans.txt")


def _make_sym(usr, spelling, kind="function", qual_name=None):
    return Symbol(usr=usr, spelling=spelling, kind=kind, qual_name=qual_name,
                  is_definition=True, resolved=True)


def _golden_plans():
    """The named plans shared with tests/query_plan_test.cpp (same order)."""
    return {
        "entity_uses": (
            start(entity("PaymentService")) | out("uses")
            | where(in_list("entity_type", ["class", "interface"]))
            | select(["name", "usr"]) | limit(100)
        ).plan,
        "codebase_abstract": (
            start(codebase()) | view(qp.ENTITY_VIEW)
            | nodes(eq("entity_type", "abstract_class"))
            | where(all_of([eq("is_definition", True),
                            not_(glob("name", "*Legacy*"))]))
            | select(["name", "kind"]) | order_by(["name"]) | limit(50)
        ).plan,
        "symbol_callers": (
            start(symbol("c:@F@normalize#")) | in_("calls")
            | select(["name", "file", "line"]) | order_by(["name"])
        ).plan,
        "union_count": (
            start(symbol("A")) | out("calls", 1, 3)
            | union_(start(symbol("A")) | out("uses")) | distinct() | count()
        ).plan,
        "qualified_relation": (
            start(symbol("Widget")) | out("entity.uses")
            | view(qp.ENTITY_VIEW) | in_("generalizes", 1, 4)
        ).plan,
        "boolean_normalization": (
            start(codebase())
            | nodes(all_of([all_of([eq("kind", "class"),
                                    eq("is_static", False)]),
                            not_(not_(ne("spelling", "x")))]))
            | count()
        ).plan,
    }


@pytest.fixture()
def seeded():
    """Same graph as the C++ Seeded fixture (query_plan_test.cpp)."""
    db = Storage(":memory:")
    ids = {}
    ids["A"] = db.add_symbol(_make_sym("USR::A", "funcA", "function",
                                       "ns::funcA"))
    ids["B"] = db.add_symbol(_make_sym("USR::B", "funcB"))
    ids["C"] = db.add_symbol(_make_sym("USR::C", "funcC"))
    ids["D"] = db.add_symbol(_make_sym("USR::D", "ClassD", "class"))
    ids["E"] = db.add_symbol(_make_sym("USR::E", "ClassE", "class"))
    # AbsS: declaration kind `struct`, classification `abstract_class` --
    # the two must stay separate fields (PR #20 review).
    ids["S"] = db.add_symbol(_make_sym("USR::S", "AbsS", "struct"))
    db.add_edge(ids["A"], ids["B"], 1)  # calls
    db.add_edge(ids["B"], ids["C"], 1)  # calls
    db.add_edge(ids["A"], ids["C"], 7)  # uses
    db.add_edge(ids["E"], ids["D"], 2)  # inherits
    db._conn.execute(
        "INSERT INTO entity_node (id, kind) VALUES (?, 1), (?, 1), (?, 2)",
        (ids["D"], ids["E"], ids["S"]))
    db.add_entity_edge(ids["D"], ids["E"], 8)  # entity uses
    db.add_entity_edge(ids["E"], ids["D"], 1)  # generalizes
    return db, ids


# ---------------------------------------------------------------------------
# Q1: canonical JSON golden (cross-language parity anchor)
# ---------------------------------------------------------------------------
def test_canonical_json_matches_shared_golden():
    rendered = "".join(
        f"== {name} ==\n{canonical_json(plan)}\n"
        for name, plan in sorted(_golden_plans().items()))
    with open(_GOLDEN, "rb") as fh:
        assert rendered.encode() == fh.read()


def test_textual_cxq_lowers_to_the_shared_plan():
    parsed = parse_cxq(
        "codebase() | nodes(kind = class) | "
        "where(is_definition = true and name ~= 'Widget*') | "
        "select(name, usr) | order_by(name) | limit(10)")
    expected = (
        start(codebase()) | nodes(eq("kind", "class"))
        | where(all_of([eq("is_definition", True),
                       glob("name", "Widget*")]))
        | select(["name", "usr"]) | order_by(["name"]) | limit(10)
    ).plan
    assert canonical_json(parsed) == canonical_json(expected)
    with pytest.raises(PlanError, match=r"E_PARSE: limit\(\) requires one integer"):
        parse_cxq("codebase() | limit(nope)")


@pytest.mark.parametrize(
    ("text", "message"),
    [
        ("codebase() | nodes(kind = class, name = Widget)",
         r"E_PARSE: nodes\(\) takes zero or one predicate"),
        ("codebase() | out(calls, mode=static)",
         "E_PARSE: depth must be an integer or depth=min..max"),
        ("codebase() | nodes() | limit(+1)",
         r"E_PARSE: limit\(\) requires one integer"),
        ("codebase() | nodes() | limit(1_0)",
         r"E_PARSE: limit\(\) requires one integer"),
        ("codebase() | nodes() | limit(9223372036854775808)",
         r"E_PARSE: limit\(\) requires one integer"),
        ("codebase() | nodes() | limit(-9223372036854775809)",
         r"E_PARSE: limit\(\) requires one integer"),
        ("codebase() | out(calls, +1)",
         "E_PARSE: depth must be an integer or depth=min..max"),
        ("codebase() | out(calls, 1_0)",
         "E_PARSE: depth must be an integer or depth=min..max"),
        ("codebase() | out(calls, 9223372036854775808)",
         "E_PARSE: depth must be an integer or depth=min..max"),
        ("codebase() | out(calls, depth=+1..2)",
         "E_PARSE: depth must be written as depth=min..max"),
        ("codebase() | count(extra)", r"E_PARSE: count\(\) takes no arguments"),
        ("codebase() | rank(name)", r"E_PARSE: rank\(\) is not available in v1"),
    ],
)
def test_textual_cxq_rejects_unsupported_or_ambiguous_syntax(text, message):
    with pytest.raises(PlanError, match=message):
        parse_cxq(text)


def test_textual_cxq_rejects_oversized_integer_tokens():
    digits = "9" * 5000
    with pytest.raises(PlanError, match=r"E_PARSE: limit\(\) requires one integer"):
        parse_cxq(f"codebase() | nodes() | limit({digits})")
    with pytest.raises(
        PlanError, match="E_PARSE: depth must be an integer or depth=min..max"
    ):
        parse_cxq(f"codebase() | out(calls, {digits})")


# ---------------------------------------------------------------------------
# Q2: normalization
# ---------------------------------------------------------------------------
def test_normalization_qualifies_and_flattens():
    n = validate((start(symbol("A")) | out("calls")).plan)
    assert n.stages[0].relation == "symbol.calls"

    ne_plan = validate((start(entity("X")) | out("uses")).plan)
    assert ne_plan.stages[0].relation == "entity.uses"

    nb = validate(
        (start(symbol("A"))
         | where(all_of([all_of([eq("spelling", "a"), eq("spelling", "b")]),
                         not_(not_(eq("spelling", "c")))]))).plan)
    np = nb.stages[0].pred
    assert np.op == "all_of"
    assert len(np.kids) == 3
    assert np.kids[2].op == "eq"

    # The stream view follows a traversal's layer: after a qualified entity
    # hop, a bare relation resolves in the entity namespace.
    nq = validate((start(symbol("Widget")) | out("entity.uses")
                   | in_("generalizes")).plan)
    assert nq.stages[1].relation == "entity.generalizes"


# ---------------------------------------------------------------------------
# Q3: validation errors -- stable E_* codes
# ---------------------------------------------------------------------------
def _code(plan):
    try:
        validate(plan)
    except PlanError as e:
        return str(e).split(":", 1)[0]
    return "<no-error>"


def test_validation_error_codes():
    assert _code(start(symbol("")).plan) == "E_SOURCE"
    assert _code((start(symbol("A")) | out("bogus")).plan) == "E_RELATION"
    assert _code((start(symbol("A")) | out("generalizes")).plan) == "E_RELATION"
    assert _code((start(symbol("A")) | out("calls", 1, 33)).plan) == "E_DEPTH"
    assert _code((start(symbol("A")) | out("calls", 0, 2)).plan) == "E_DEPTH"
    assert _code((start(symbol("A")) | where(eq("bogus", "x"))).plan) == "E_FIELD"
    assert _code((start(symbol("A")) | where(eq("file", "x"))).plan) == "E_FIELD"
    assert _code((start(symbol("A"))
                  | where(eq("kind", "bogus_kind"))).plan) == "E_KIND"
    assert _code((start(symbol("A"))
                  | where(eq("kind", "abstract_class"))).plan) == "E_KIND"
    assert _code((start(codebase()) | view(qp.ENTITY_VIEW)
                  | nodes(eq("entity_type", "struct"))).plan) == "E_KIND"
    assert _code((start(codebase()) | view(qp.ENTITY_VIEW)
                  | nodes(eq("kind", "struct"))).plan) == "<no-error>"
    assert _code((start(symbol("A")) | limit(0)).plan) == "E_LIMIT"
    assert _code((start(symbol("A"))
                  | union_(start(entity("B")) | out("uses"))).plan) == "E_SETOP"
    assert _code((start(symbol("A")) | select(["name"])
                  | out("calls")).plan) == "E_STAGE"
    assert _code((start(symbol("A")) | count() | limit(1)).plan) == "E_STAGE"
    assert _code((start(symbol("A")) | nodes()).plan) == "E_STAGE"
    assert _code((start(codebase()) | count()).plan) == "E_STAGE"
    assert _code((start(codebase()) | nodes()).plan) == "<no-error>"
    assert _code((start(symbol("A")) | select(["name"])
                  | order_by(["usr"])).plan) == "E_FIELD"
    assert _code((start(codebase()) | view("bogus") | nodes()).plan) == "E_VIEW"


# ---------------------------------------------------------------------------
# Q4: execution (mirrors the C++ cases)
# ---------------------------------------------------------------------------
def test_source_resolution(seeded):
    db, ids = seeded
    ex = Executor(db)
    r = ex.run((start(symbol("USR::A")) | out("calls")).plan)
    assert [row[0] for row in r.rows] == [ids["B"]]
    assert ex.run((start(symbol("ns::funcA")) | out("calls")).plan).rows
    assert ex.run((start(symbol("funcA")) | out("calls")).plan).rows
    assert not ex.run((start(symbol("nope")) | out("calls")).plan).rows


def test_traversal_depth_and_direction(seeded):
    db, ids = seeded
    ex = Executor(db)
    d1 = ex.run((start(symbol("USR::A")) | out("calls")).plan)
    assert [row[0] for row in d1.rows] == [ids["B"]]
    d2 = ex.run((start(symbol("USR::A")) | out("calls", 1, 2)).plan)
    assert len(d2.rows) == 2
    d22 = ex.run((start(symbol("USR::A")) | out("calls", 2, 2)).plan)
    assert [row[0] for row in d22.rows] == [ids["C"]]
    in1 = ex.run((start(symbol("USR::B")) | in_("calls")).plan)
    assert [row[0] for row in in1.rows] == [ids["A"]]


def test_where_and_select(seeded):
    db, ids = seeded
    ex = Executor(db)
    r = ex.run((start(codebase()) | nodes(eq("kind", "class"))
                | select(["name", "kind", "usr"])).plan)
    assert len(r.rows) == 2
    assert r.fields == ("name", "kind", "usr")
    assert r.rows[0][0] == "ClassD"
    assert r.rows[0][1] == "class"

    w = ex.run((start(symbol("USR::A")) | out("calls", 1, 2)
                | where(eq("spelling", "funcB"))).plan)
    assert [row[0] for row in w.rows] == [ids["B"]]


def test_set_operations_distinct_count(seeded):
    db, ids = seeded
    ex = Executor(db)
    base = start(symbol("USR::A")) | out("calls", 1, 2)
    # union is a SET union -- the shared C must not double-count (PR #20).
    u = ex.run((base | union_(start(symbol("USR::A")) | out("uses"))).plan)
    assert len(u.rows) == 2
    uc = ex.run((base | union_(start(symbol("USR::A")) | out("uses"))
                 | count()).plan)
    assert uc.scalar == 2
    ud = ex.run((base | union_(start(symbol("USR::A")) | out("uses"))
                 | distinct()).plan)
    assert len(ud.rows) == 2
    ix = ex.run((base | intersect(start(symbol("USR::A")) | out("uses"))).plan)
    assert [row[0] for row in ix.rows] == [ids["C"]]
    ec = ex.run((base | except_(start(symbol("USR::A")) | out("uses"))).plan)
    assert [row[0] for row in ec.rows] == [ids["B"]]
    c = ex.run((base | count()).plan)
    assert c.shape == "scalar" and c.scalar == 2


def test_entity_view_traversal(seeded):
    db, ids = seeded
    ex = Executor(db)
    uses = ex.run((start(entity("ClassD")) | out("uses")).plan)
    assert [row[0] for row in uses.rows] == [ids["E"]]
    assert uses.view == "entity"
    subs = ex.run((start(entity("ClassD")) | in_("generalizes")).plan)
    assert [row[0] for row in subs.rows] == [ids["E"]]
    q = ex.run((start(symbol("ClassD")) | out("entity.uses")).plan)
    assert len(q.rows) == 1
    assert not ex.run((start(entity("funcA")) | out("uses")).plan).rows


def test_order_limit_default_fields_result_dict(seeded):
    db, _ids = seeded
    ex = Executor(db)
    r = ex.run((start(codebase()) | nodes(eq("kind", "function"))
                | select(["spelling"]) | order_by(["spelling"])
                | limit(2)).plan)
    assert [row[0] for row in r.rows] == ["funcA", "funcB"]
    assert not r.truncated

    d = ex.run((start(symbol("USR::A")) | out("calls")).plan)
    assert d.fields == (
        "id", "usr", "semantic_universe", "identity_key", "name", "kind"
    )
    dd = d.to_dict()
    assert dd["shape"] == "nodes"
    assert dd["view"] == "symbol"
    assert dd["count"] == 1
    assert dd["rows"][0]["name"] == "funcB"

    scoped = ex.run(
        (start(symbol("USR::A"))
         | select(["usr", "semantic_universe", "identity_key"])).plan
    )
    assert scoped.rows == [("USR::A", "legacy", "legacy\x1fUSR::A")]


def test_default_result_cap_reports_truncation():
    db = Storage(":memory:")
    for i in range(1200):
        db.add_symbol(_make_sym(f"USR::f{i}", f"f{i}"))
    ex = Executor(db)
    r = ex.run((start(codebase()) | nodes()).plan)
    assert len(r.rows) == 1000
    assert r.truncated
    c = ex.run((start(codebase()) | nodes() | count()).plan)
    assert c.scalar == 1200
    lim = ex.run((start(codebase()) | nodes() | limit(1100)).plan)
    assert len(lim.rows) == 1100
    assert not lim.truncated


def test_legacy_index_identity_and_result_key_order(tmp_path):
    source = tmp_path / "source.cpp"
    source.write_text("int answer = 1;\n")
    db = Storage(":memory:")
    db.add_component("fixture", str(tmp_path))
    file_id = db.add_file_path(str(source), md5=md5_of(str(source)))
    db.mark_file_indexed(file_id)

    assert db.index_identity().freshness == "unverifiable"
    ex = Executor(db)
    result = ex.run((start(codebase()) | nodes()).plan)
    row_dict = result.to_dict()
    assert row_dict["index"]["freshness"] == "unverifiable"
    assert list(row_dict) == ["shape", "view", "count", "truncated", "index", "rows"]
    scalar = ex.run((start(codebase()) | nodes() | count()).plan)
    assert list(scalar.to_dict()) == ["shape", "view", "count", "truncated", "index"]
    explained = ex.explain((start(codebase()) | nodes()).plan)
    assert explained["index"] == row_dict["index"]
    assert explained["plan"]["cxq"] == 1


def test_identity_is_stable_across_component_insertion_order(tmp_path):
    roots = {name: tmp_path / name for name in ("alpha", "beta")}
    for root in roots.values():
        root.mkdir()
    sources = {
        name: root / f"{name}.cpp" for name, root in roots.items()
    }
    for name, source in sources.items():
        source.write_text(f"int {name}() {{ return 1; }}\n", encoding="utf-8")

    def make_identity(order):
        db = Storage(":memory:")
        for name in order:
            db.add_component(name, str(roots[name]))
        for source in sources.values():
            file_id = db.add_file_path(str(source), md5=md5_of(str(source)))
            db.mark_file_indexed(file_id)
        db.stamp_index_identity()
        return db.index_identity()

    forward = make_identity(("alpha", "beta"))
    reverse = make_identity(("beta", "alpha"))
    assert forward.freshness == "current"
    assert reverse.freshness == "current"
    assert forward.source_revision == reverse.source_revision
    assert forward.source_fingerprint == reverse.source_fingerprint
    assert forward.index_config_fingerprint == reverse.index_config_fingerprint


# ---------------------------------------------------------------------------
# PR #20 review regressions (mirrors of the C++ cases)
# ---------------------------------------------------------------------------
def test_view_entity_drops_non_entities(seeded):
    db, _ids = seeded
    ex = Executor(db)
    fn = ex.run((start(symbol("funcA")) | view(qp.ENTITY_VIEW)).plan)
    assert fn.rows == []
    assert fn.view == "entity"
    cls = ex.run((start(symbol("ClassD")) | view(qp.ENTITY_VIEW)).plan)
    assert len(cls.rows) == 1
    back = ex.run((start(entity("ClassD")) | view(qp.SYMBOL_VIEW)).plan)
    assert len(back.rows) == 1


def test_min_depth_uses_path_length_not_first_discovery():
    # Diamond: P -> Q, P -> R -> Q. out(calls, 2, 2) must emit Q.
    db = Storage(":memory:")
    p = db.add_symbol(_make_sym("USR::P", "p"))
    q = db.add_symbol(_make_sym("USR::Q", "q"))
    r = db.add_symbol(_make_sym("USR::R", "r"))
    db.add_edge(p, q, 1)
    db.add_edge(p, r, 1)
    db.add_edge(r, q, 1)
    ex = Executor(db)
    d2 = ex.run((start(symbol("USR::P")) | out("calls", 2, 2)).plan)
    assert [row[0] for row in d2.rows] == [q]
    d12 = ex.run((start(symbol("USR::P")) | out("calls", 1, 2)).plan)
    assert len(d12.rows) == 2


def test_default_cap_reapplies_after_expanding_stage():
    db = Storage(":memory:")
    hub = db.add_symbol(_make_sym("USR::hub", "hub"))
    for i in range(1200):
        t = db.add_symbol(_make_sym(f"USR::t{i}", f"t{i}"))
        db.add_edge(hub, t, 1)
    ex = Executor(db)
    r = ex.run((start(symbol("USR::hub")) | limit(1) | out("calls")).plan)
    assert len(r.rows) == 1000
    assert r.truncated


def test_kind_vs_entity_type_separation(seeded):
    db, _ids = seeded
    ex = Executor(db)
    decl = ex.run((start(codebase())
                   | nodes(in_list("kind", ["class", "struct"]))
                   | select(["spelling", "kind", "entity_type"])
                   | order_by(["spelling"])).plan)
    assert [row[0] for row in decl.rows] == ["AbsS", "ClassD", "ClassE"]
    assert decl.rows[0][1] == "struct"
    assert decl.rows[0][2] == "abstract_class"

    cls = ex.run((start(codebase()) | view(qp.ENTITY_VIEW)
                  | nodes(eq("entity_type", "abstract_class"))
                  | select(["spelling", "kind"])).plan)
    assert [tuple(row) for row in cls.rows] == [("AbsS", "struct")]

    fn = ex.run((start(symbol("funcA")) | select(["entity_type"])).plan)
    assert [row[0] for row in fn.rows] == [None]
