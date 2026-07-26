"""CXQ QueryPlan tier tests (indexer/queryplan.py).

Mirror of tests/query_plan_test.cpp. The canonical-JSON case pins the SAME
golden file (tests/golden/cxq_plans.txt) as the C++ suite -- the cross-language
byte-parity anchor. Regenerate the golden from the C++ side with
CIDX_UPDATE_GOLDEN=1; this suite only verifies.
"""

from __future__ import annotations

import os
import sys
from dataclasses import replace

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from indexer.storage import Storage, Symbol  # noqa: E402
from indexer.query import GraphQuery  # noqa: E402
from indexer.utils.hashing import md5_of  # noqa: E402
from indexer import queryplan as qp  # noqa: E402
from indexer.queryplan import (  # noqa: E402
    Executor, PlanError, all_of, canonical_json, codebase, count, distinct,
    entity, eq, except_, glob, in_, in_list, intersect, limit, ne, nodes,
    not_, order_by, out, path, rank, reverse_type_use, select, sites, start,
    symbol, union_, validate, view, where,
    all, all_targets, any_target, at_least, exactly, exists, inherits_from,
    is_abstract, is_instance, none,
    parse_cxq,
)
from indexer.result_protocol import Status, from_query_result  # noqa: E402

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
        "path_calls": (
            start(symbol("USR::A"))
            | path(start(symbol("USR::C")), "calls", 1, 8, 1)
            | rank(5) | limit(1)
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
    ids["T"] = db.add_symbol(_make_sym("USR::T", "Thing", "class-template"))
    ids["I"] = db.add_symbol(_make_sym("USR::I", "Thing<int>", "class"))
    db.add_edge(ids["A"], ids["B"], 1)  # calls
    db.add_edge(ids["B"], ids["C"], 1)  # calls
    db.add_edge(ids["A"], ids["C"], 7)  # uses
    db.add_edge(ids["E"], ids["D"], 2)  # inherits
    db.add_edge(ids["E"], ids["C"], 2)  # inherits to a non-entity target
    db.add_edge(ids["I"], ids["T"], 5)  # instantiates
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
    assert len(r.rows) == 3
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
    assert [row[0] for row in decl.rows] == ["AbsS", "ClassD", "ClassE", "Thing<int>"]
    assert decl.rows[0][1] == "struct"
    assert decl.rows[0][2] == "abstract_class"

    cls = ex.run((start(codebase()) | view(qp.ENTITY_VIEW)
                  | nodes(eq("entity_type", "abstract_class"))
                  | select(["spelling", "kind"])).plan)
    assert [tuple(row) for row in cls.rows] == [("AbsS", "struct")]

    fn = ex.run((start(symbol("funcA")) | select(["entity_type"])).plan)
    assert [row[0] for row in fn.rows] == [None]


def test_semantic_macros_expand_to_quantifiers(seeded):
    db, ids = seeded
    ex = Executor(db)
    abstract = ex.run((start(codebase()) | view(qp.ENTITY_VIEW)
                       | nodes(is_abstract())).plan)
    assert [row[0] for row in abstract.rows] == [ids["S"]]

    any_result = ex.run(
        (start(symbol("USR::E"))
         | where(inherits_from(any_target(["ClassD", "Missing"])))
         | select(["usr"])).plan)
    assert [row[0] for row in any_result.rows] == ["USR::E"]
    all_result = ex.run(
        (start(symbol("USR::E"))
         | where(inherits_from(all_targets(["ClassD", "Missing"])))
         | select(["usr"])).plan)
    assert not all_result.rows
    explained = ex.explain(
        (start(symbol("USR::E")) | where(inherits_from("ClassD"))).plan)
    assert explained["plan"]["stages"][0]["pred"]["op"] == "exists"
    instances = ex.run((start(codebase()) | nodes(is_instance())
                         | select(["usr"])).plan)
    assert [row[0] for row in instances.rows] == ["USR::I"]


def test_partial_quantifiers_preserve_unknown(seeded):
    db, ids = seeded
    ex = Executor(db)
    predicate = none("calls", eq("spelling", "missing"))
    assert not ex.run((start(symbol("USR::A")) | where(predicate)).plan).rows
    included = ex.run((start(symbol("USR::A"))
                       | where(predicate, qp.UnknownPolicy.INCLUDE)).plan)
    assert [row[0] for row in included.rows] == [ids["A"]]
    with pytest.raises(PlanError, match="^E_UNKNOWN:"):
        ex.run((start(symbol("USR::A"))
                | where(predicate, qp.UnknownPolicy.ERROR)).plan)
    exact = ex.run((start(symbol("USR::A"))
                    | where(exactly(2, "calls"), qp.UnknownPolicy.INCLUDE)).plan)
    assert [row[0] for row in exact.rows] == [ids["A"]]


def test_all_quantifiers_bind_and_preserve_nested_unknown(seeded):
    db, ids = seeded
    ex = Executor(db)
    assert ex.run((start(symbol("USR::A")) | where(exists("calls"))).plan).rows
    assert not ex.run((start(symbol("USR::A")) | where(none("calls"))).plan).rows
    assert ex.run((start(symbol("USR::A"))
                   | where(all("calls"), qp.UnknownPolicy.INCLUDE)).plan).rows
    assert ex.run((start(symbol("USR::A"))
                   | where(at_least(1, "calls"))).plan).rows
    assert ex.run((start(symbol("USR::A"))
                   | where(exactly(1, "calls"), qp.UnknownPolicy.INCLUDE)).plan).rows

    target = eq("spelling", "funcB")
    assert ex.run((start(symbol("USR::A"))
                   | where(exists("calls", target))).plan).rows
    assert not ex.run((start(symbol("USR::A"))
                      | where(none("calls", target))).plan).rows
    assert ex.run((start(symbol("USR::A"))
                   | where(all("calls", target), qp.UnknownPolicy.INCLUDE)).plan).rows
    assert ex.run((start(symbol("USR::A"))
                   | where(at_least(1, "calls", target))).plan).rows
    assert ex.run((start(symbol("USR::A"))
                   | where(exactly(1, "calls", target), qp.UnknownPolicy.INCLUDE)).plan).rows

    nested = ex.run(
        (start(symbol("USR::A"))
         | where(exists("calls", exists("calls", eq("spelling", "funcC"))))).plan)
    assert [row[0] for row in nested.rows] == [ids["A"]]
    recursive_target = ex.run(
        (start(symbol("USR::A"))
         | where(exists("calls", eq("spelling", "funcC"), 2, 2))).plan)
    assert [row[0] for row in recursive_target.rows] == [ids["A"]]
    nested_unknown = ex.run(
        (start(symbol("USR::A"))
         | where(exists("calls", exists("calls", eq("spelling", "missing"))),
                qp.UnknownPolicy.INCLUDE)).plan)
    assert [row[0] for row in nested_unknown.rows] == [ids["A"]]
    target_unknown = ex.run(
        (start(symbol("USR::E"))
         | where(all("inherits", eq("entity_type", "class")),
                qp.UnknownPolicy.INCLUDE)).plan)
    assert [row[0] for row in target_unknown.rows] == [ids["E"]]

def test_typed_parameter_view_preserves_natural_slot_identity():
    db = Storage(":memory:")
    owner = db.add_symbol(_make_sym("USR::typed", "typed"))
    db._conn.execute(
        "INSERT INTO parameter(owner_id,position,pack_index,name,default_text,"
        "reference_semantics) VALUES (?,?,?,?,?,?)",
        (owner, 0, -1, "value", "0", "lvalue"),
    )
    db._conn.commit()

    result = Executor(db).run(
        (start(symbol("USR::typed")) | out("has_parameter")
         | select(["owner_id", "position", "pack_index", "name",
                   "default_text", "identity_key"])).plan)
    assert result.view == "parameter"
    assert len(result.rows) == 1
    assert result.rows[0][:5] == (owner, 0, -1, "value", "0")
    assert result.rows[0][5].startswith(
        "parameter:legacy\x1fUSR::typed:0:-1")

    db._conn.execute(
        "INSERT INTO parameter(owner_id,position,pack_index,name) "
        "VALUES (?,?,?,?)", (owner, 1, -1, "other"))
    db._conn.commit()
    filtered = Executor(db).run(
        (start(symbol("USR::typed")) | out("has_parameter")
         | where(eq("position", 1)) | select(["name"])).plan)
    assert filtered.rows == [("other",)]

    reverse = Executor(db).run(
        (start(codebase()) | view("parameter") | nodes()
         | in_("has_parameter") | select(["name"])).plan)
    assert reverse.view == "symbol"
    assert reverse.rows == [("typed",)]


def test_named_signature_slots_and_recursive_type_layers_match_cpp_values():
    db = Storage(":memory:")
    owner = db.add_symbol(Symbol(
        usr="USR::typed_views", spelling="typed_views", kind="function",
        is_definition=True, resolved=True, callable_kind="free-function",
        template_origin="typed_views<T>", template_form="pattern"))
    db._conn.execute(
        "INSERT INTO type_node(type_key,spelling,kind,extent) VALUES "
        "('A4(b:int)','int[4]',8,'4'),('b:int','int',1,NULL),"
        "('b:float','float',1,NULL),('b:char','char',1,NULL)"
    )
    type_ids = [row[0] for row in db._conn.execute(
        "SELECT id FROM type_node ORDER BY id")]
    array_id, int_id, float_id, char_id = type_ids
    db._conn.execute(
        "INSERT INTO type_edge(src_id,kind,position,dst_id) VALUES (?,?,?,?)",
        (array_id, 2, 0, int_id))
    db._conn.execute(
        "INSERT INTO symbol_type(symbol_id,kind,type_id) VALUES (?,?,?)",
        (owner, 1, array_id))
    db._conn.execute(
        "INSERT INTO parameter(owner_id,position,pack_index,name,type_id,"
        "declared_type_id,adjusted_type_id) VALUES (?,?,?,?,?,?,?)",
        (owner, 0, -1, "value", int_id, int_id, int_id))
    db._conn.execute(
        "INSERT INTO template_param(owner_id,position,param_kind,name,type_id) "
        "VALUES (?,?,?,?,?)", (owner, 0, 1, "T", float_id)
    )
    db._conn.execute(
        "INSERT INTO template_arg(owner_id,position,pack_index,arg_kind,type_id) "
        "VALUES (?,?,?,?,?),(?,?,?,?,?)",
        (owner, 1, 0, 1, char_id, owner, 1, 1, 1, char_id),
    )
    db._conn.commit()

    def structure():
        symbols = tuple(db._conn.execute(
            "SELECT id,usr FROM symbol ORDER BY id"))
        edges = tuple(db._conn.execute(
            "SELECT src_id,dst_id,kind FROM edge ORDER BY id"))
        return len(symbols), symbols, len(edges), edges

    before = structure()
    ex = Executor(db)
    symbols = ex.run(
        (start(symbol("USR::typed_views"))
         | where(all_of([eq("callable_kind", "free-function"),
                         eq("template_origin", "typed_views<T>"),
                         eq("template_form", "pattern")]))
         | select(["callable_kind", "template_origin", "template_form"])).plan)
    assert symbols.rows == [("free-function", "typed_views<T>", "pattern")]

    slots = ex.run(
        (start(symbol("USR::typed_views")) | out("has_signature_slot")
         | where(eq("slot_kind", "parameter"))
         | select(["slot_kind", "position", "name", "type_id"])).plan)
    assert slots.rows == [("parameter", 0, "value", int_id)]

    callable_roundtrip = ex.run(
        (start(symbol("USR::typed_views")) | out("has_signature_slot")
         | out("of_callable") | select(["usr"])).plan
    )
    assert callable_roundtrip.rows == [("USR::typed_views",)]
    type_roundtrip = ex.run(
        (start(symbol("USR::typed_views")) | out("has_signature_slot")
         | out("of_type") | select(["type_key"])).plan
    )
    assert type_roundtrip.rows == [
        ("A4(b:int)",), ("b:int",), ("b:float",), ("b:char",)
    ]
    callable_inverse = ex.run(
        (start(symbol("USR::typed_views")) | out("has_signature_slot")
         | in_("has_signature_slot") | select(["usr"])).plan
    )
    assert callable_inverse.rows == [("USR::typed_views",)]
    type_inverse = ex.run(
        (start(codebase()) | view("type") | nodes()
             | where(eq("type_key", "b:int"))
             | in_("signature_slot.of_type")
         | select(["slot_kind"])).plan
    )
    assert type_inverse.rows == [("parameter",)]

    layers = ex.run(
        (start(codebase()) | view("type") | nodes() | out("has_layer")
         | where(eq("root_id", array_id))
         | select(["root_id", "path", "relation", "depth", "status",
                  "extent"])).plan)
    assert layers.rows == [
        (array_id, "root", "root", 0, "complete", "4"),
        (array_id, "root.element", "element_type", 1, "complete", None),
    ]
    parent = ex.run(
        (start(codebase()) | view("type") | nodes() | out("has_layer")
         | where(eq("root_id", array_id))
         | where(eq("path", "root.element")) | in_("child")
         | select(["path"])).plan
    )
    assert parent.rows == [("root",)]
    root_type = ex.run(
        (start(codebase()) | view("type") | nodes() | out("has_layer")
         | where(eq("root_id", array_id)) | where(eq("path", "root"))
         | in_("has_layer") | select(["type_key"])).plan
    )
    assert root_type.rows == [("A4(b:int)",)]
    assert structure() == before


def test_exact_recursive_and_pointer_type_acceptance_is_read_only():
    db = Storage(":memory:")
    owner_id = db.add_symbol(_make_sym("cidx::version_re", "version_re"))
    db.add_symbol(_make_sym("USR::std::regex", "regex", "class", "std::regex"))
    db.add_symbol(_make_sym("USR::Owner", "Owner", "struct"))
    db._conn.execute(
        "INSERT INTO type_node(type_key,spelling,kind,decl_usr) VALUES "
        "('alias:A','A',4,NULL),('alias:B','B',4,NULL),"
        "('fn:ret-param','int(float)',9,NULL),('b:int','int',1,NULL),"
        "('b:float','float',1,NULL),('record:Owner','Owner',2,NULL),"
        "('mfp:Owner','int (Owner::*)(float)',13,NULL),"
        "('pack:int','int...',14,NULL),"
        "('ref:regex','const std::regex &',6,'USR::std::regex'),"
        "('record:regex','std::regex',2,'USR::std::regex')"
    )

    def type_id(key):
        return db._conn.execute(
            "SELECT id FROM type_node WHERE type_key=?", (key,)
        ).fetchone()[0]

    alias_a = type_id("alias:A")
    alias_b = type_id("alias:B")
    function = type_id("fn:ret-param")
    integer = type_id("b:int")
    floating = type_id("b:float")
    member_owner = type_id("record:Owner")
    member_function = type_id("mfp:Owner")
    regex_reference = type_id("ref:regex")
    regex_record = type_id("record:regex")
    db._conn.executemany(
        "INSERT INTO type_edge(src_id,kind,position,dst_id) VALUES (?,?,?,?)",
        [(alias_a, 3, 0, alias_b), (alias_b, 3, 0, alias_a),
         (function, 4, 0, integer), (function, 5, 0, floating),
         (member_function, 7, 0, member_owner),
         (member_function, 8, 0, function),
         (regex_reference, 1, 0, regex_record)],
    )
    db._conn.execute(
        "INSERT INTO symbol_type(symbol_id,kind,type_id) VALUES (?,?,?)",
        (owner_id, 1, regex_reference),
    )
    db._conn.execute(
        "INSERT INTO parameter(owner_id,position,pack_index,name) "
        "VALUES (?,?,?,?)", (owner_id, 9, -1, "unknown")
    )
    db._conn.execute(
        "INSERT INTO parameter(owner_id,position,pack_index,name,type_id) "
        "VALUES (?,?,?,?,?)", (owner_id, 10, -1, "type-only", regex_reference)
    )
    db._conn.commit()

    graph = GraphQuery.from_connection(db._conn)
    alias_layers = graph.type_layers(alias_a)
    assert [(row["path"], row["status"]) for row in alias_layers] == [
        ("root", "complete"),
        ("root.alias_of", "complete"),
        ("root.alias_of.alias_of", "cycle"),
    ]
    assert [row["kind"] for row in graph.type_layers(function)] == [
        "function", "builtin", "builtin"
    ]
    assert [row["path"] for row in graph.type_layers(function)] == [
        "root", "root.return_type", "root.param_type[0]"
    ]
    member_layers = graph.type_layers(member_function)
    assert [row["path"] for row in member_layers] == [
        "root", "root.member_owner", "root.member_component",
        "root.member_component.return_type",
        "root.member_component.param_type[0]",
    ]
    assert member_layers[1]["spelling"] == "Owner"
    assert graph.type_layers(999999) == [{
        "path": "root", "relation": "root", "position": 0,
        "depth": 0, "status": "unknown",
    }]

    null_slot = Executor(db).run(
        (start(symbol("cidx::version_re")) | out("has_signature_slot")
         | where(eq("slot_kind", "return"))
         | where(eq("mode", "lvalue-reference"))
         | where(eq("value_kind", "record"))
         | where(eq("named_decl", "std::regex"))
         | select(["mode", "value_kind", "named_decl"])).plan
    )
    assert null_slot.rows == [(
        "lvalue-reference", "record", "std::regex"
    )]
    graph_slot = next(
        slot for slot in graph.signature_slots(graph.get("cidx::version_re"))
        if slot.position == 10
    )
    assert graph_slot.declared_type is not None
    assert graph_slot.adjusted_type is not None
    assert graph_slot.declared_type.id == regex_reference
    assert graph_slot.adjusted_type.id == regex_reference
    assert (graph_slot.mode, graph_slot.value_kind, graph_slot.named_decl) == (
        "lvalue-reference", "record", "std::regex"
    )
    type_only_plan = Executor(db).run(
        (start(symbol("cidx::version_re")) | out("has_signature_slot")
         | where(eq("slot_kind", "parameter"))
         | where(eq("position", 10))
         | select(["type_id", "declared_type_id", "adjusted_type_id",
                  "mode", "value_kind", "named_decl"])).plan
    )
    assert type_only_plan.rows == [(
        regex_reference, None, None, "lvalue-reference", "record", "std::regex"
    )]
    type_only_filtered = Executor(db).run(
        (start(symbol("cidx::version_re")) | out("has_signature_slot")
         | where(eq("slot_kind", "parameter"))
         | where(eq("position", 10))
         | where(eq("mode", "lvalue-reference"))
         | where(eq("value_kind", "record"))
         | where(eq("named_decl", "std::regex"))
         | select(["position"])).plan
    )
    assert type_only_filtered.rows == [(10,)]
    null_slot = Executor(db).run(
        (start(symbol("cidx::version_re")) | out("has_parameter")
         | where(eq("position", 9)) | select(["type_id"])).plan
    )
    assert null_slot.rows == [(None,)]
    before = tuple(db._conn.execute(
        "SELECT (SELECT count(*) FROM symbol), (SELECT count(*) FROM edge)"
    ).fetchone())
    assert graph.type_layers(type_id("pack:int"))[0]["kind"] == "pack-expansion"
    after = tuple(db._conn.execute(
        "SELECT (SELECT count(*) FROM symbol), (SELECT count(*) FROM edge)"
    ).fetchone())
    assert after == before


def test_template_defaults_expose_logical_evidence():
    db = Storage(":memory:")
    owner = db.add_symbol(_make_sym("USR::template", "template", "class"))
    db._conn.execute(
        "INSERT INTO template_param(owner_id,position,param_kind,name,"
        "default_txt) VALUES (?,?,?,?,?)",
        (owner, 0, 1, "T", "int"),
    )
    db._conn.commit()

    result = Executor(db).run(
        (start(symbol("USR::template")) | out("has_template_parameter")
         | out("has_default")
         | select(["owner_id", "position", "default_txt", "identity_key"])).plan)
    assert result.view == "evidence"
    assert result.rows == [
        (owner, 0, "int", "evidence:template_default:legacy\x1fUSR::template:0")
    ]


def _seed_reverse_typed_graph(
    db, component_path, repo_name="repo", caller_suffix="shared", grouped=True
):
    if grouped:
        repo = db.add_repository(
            repo_name, remote_url=f"https://example.test/{repo_name}.git"
        )
    component = db.add_component("project", component_path)
    if grouped:
        db.set_component_repository(component, repo)
        db._conn.execute(
            "UPDATE component SET path = ? WHERE id = ?", ("src", component)
        )
        db._conn.commit()
    directory = db.add_directory(component, "include" if grouped else "src")
    file_id = db.add_file(directory, "unit.cpp" if grouped else "same.cpp")
    caller = db.add_symbol(
        _make_sym(f"USR::{caller_suffix}::caller", "caller")
    )
    callee = db.add_symbol(
        _make_sym(f"USR::{caller_suffix}::callee", "callee")
    )
    edge_id = db.add_edge(caller, callee, 1)
    db.add_edge_site(edge_id, file_id, 10, 2)
    db._conn.execute(
        "INSERT INTO call_arg(edge_id,file_id,line,col,position) "
        "VALUES (?,?,?,?,?)", (edge_id, file_id, 10, 2, 0))
    db._conn.commit()


def test_typed_reverse_relations_are_not_shadowed_and_file_identity_is_portable():
    first = Storage(":memory:")
    _seed_reverse_typed_graph(first, "/tmp/a/cpp-indexer", grouped=False)
    executor = Executor(first)

    reverse_evidence = executor.run(
        (start(codebase()) | view("evidence") | nodes()
         | in_("edge.has_evidence") | count()).plan)
    reverse_argument = executor.run(
        (start(codebase()) | view("call_argument") | nodes()
         | in_("edge.has_argument") | count()).plan)
    reverse_occurrence = executor.run(
        (start(codebase()) | view("call_argument") | nodes()
         | in_("evidence.of_occurrence") | count()).plan)
    assert reverse_evidence.scalar == 1
    assert reverse_argument.scalar == 1
    assert reverse_occurrence.scalar == 1

    second = Storage(":memory:")
    _seed_reverse_typed_graph(second, "/tmp/b/cpp-indexer", grouped=False)
    first_evidence = executor.run(
        (start(codebase()) | view("evidence") | nodes()
         | select(["identity_key"])).plan)
    second_evidence = Executor(second).run(
        (start(codebase()) | view("evidence") | nodes()
         | select(["identity_key"])).plan)
    first_argument = executor.run(
        (start(codebase()) | view("call_argument") | nodes()
         | select(["identity_key"])).plan)
    second_argument = Executor(second).run(
        (start(codebase()) | view("call_argument") | nodes()
         | select(["identity_key"])).plan)
    assert first_evidence.rows == second_evidence.rows
    assert first_argument.rows == second_argument.rows

    collision = Storage(":memory:")
    _seed_reverse_typed_graph(collision, "/repo-a", "repo-a", "repo-a")
    _seed_reverse_typed_graph(collision, "/repo-b", "repo-b", "repo-b")
    collision_executor = Executor(collision)
    collision_evidence = collision_executor.run(
        (start(codebase()) | view("evidence") | nodes()
         | select(["id", "identity_key"])).plan
    )
    collision_arguments = collision_executor.run(
        (start(codebase()) | view("call_argument") | nodes()
         | select(["id", "identity_key"])).plan
    )
    assert len(collision_evidence.rows) == 2
    assert len({row[0] for row in collision_evidence.rows}) == 2
    assert len({row[1] for row in collision_evidence.rows}) == 2
    assert len(collision_arguments.rows) == 2
    assert len({row[0] for row in collision_arguments.rows}) == 2
    assert len({row[1] for row in collision_arguments.rows}) == 2

    ungrouped = Storage(":memory:")
    _seed_reverse_typed_graph(
        ungrouped,
        "/repo/A/project",
        caller_suffix="ungrouped-a",
        grouped=False,
    )
    _seed_reverse_typed_graph(
        ungrouped,
        "/repo/B/project",
        caller_suffix="ungrouped-b",
        grouped=False,
    )
    ungrouped_executor = Executor(ungrouped)
    with pytest.raises(PlanError, match="^E_IDENTITY: ambiguous ungrouped component identity$"):
        ungrouped_executor.run(
            (start(codebase()) | view("evidence") | nodes()
             | select(["id", "identity_key"])).plan
        )
    with pytest.raises(PlanError, match="^E_IDENTITY: ambiguous ungrouped component identity$"):
        ungrouped_executor.run(
            (start(codebase()) | view("evidence") | nodes() | count()).plan
        )
    with pytest.raises(PlanError, match="^E_IDENTITY: ambiguous ungrouped component identity$"):
        ungrouped_executor.run(
            (start(codebase()) | view("call_argument") | nodes()
             | select(["id", "identity_key"])).plan
        )
    with pytest.raises(PlanError, match="^E_IDENTITY: ambiguous ungrouped component identity$"):
        ungrouped_executor.run(
            (start(codebase()) | view("call_argument") | nodes() | count()).plan
        )
    evidence_base = start(codebase()) | view("evidence") | nodes()
    with pytest.raises(PlanError, match="^E_IDENTITY: ambiguous ungrouped component identity$"):
        ungrouped_executor.run((evidence_base | except_(evidence_base)).plan)
    with pytest.raises(PlanError, match="^E_IDENTITY: ambiguous ungrouped component identity$"):
        ungrouped_executor.run(
            (evidence_base | except_(evidence_base) | count()).plan
        )
    argument_base = start(codebase()) | view("call_argument") | nodes()
    with pytest.raises(PlanError, match="^E_IDENTITY: ambiguous ungrouped component identity$"):
        ungrouped_executor.run((argument_base | except_(argument_base)).plan)
    with pytest.raises(PlanError, match="^E_IDENTITY: ambiguous ungrouped component identity$"):
        ungrouped_executor.run(
            (argument_base | except_(argument_base) | count()).plan
        )

    mirrored_first = Storage(":memory:")
    _seed_reverse_typed_graph(
        mirrored_first,
        "/Users/husam/.codex/worktrees/a/cpp-indexer",
        caller_suffix="mirrored",
        grouped=False,
    )
    mirrored_second = Storage(":memory:")
    _seed_reverse_typed_graph(
        mirrored_second,
        "/Users/husam/.codex/worktrees/b/cpp-indexer",
        caller_suffix="mirrored",
        grouped=False,
    )
    mirrored_first_executor = Executor(mirrored_first)
    mirrored_second_executor = Executor(mirrored_second)
    assert mirrored_first_executor.run(
        (start(codebase()) | view("evidence") | nodes()
         | select(["identity_key"])).plan
    ).rows == mirrored_second_executor.run(
        (start(codebase()) | view("evidence") | nodes()
         | select(["identity_key"])).plan
    ).rows
    assert mirrored_first_executor.run(
        (start(codebase()) | view("call_argument") | nodes()
         | select(["identity_key"])).plan
    ).rows == mirrored_second_executor.run(
        (start(codebase()) | view("call_argument") | nodes()
         | select(["identity_key"])).plan
    ).rows

    non_catalogued_first = Storage(":memory:")
    _seed_reverse_typed_graph(
        non_catalogued_first,
        "/tmp/a/cpp-indexer",
        caller_suffix="non-catalogued-mirrored",
        grouped=False,
    )
    non_catalogued_second = Storage(":memory:")
    _seed_reverse_typed_graph(
        non_catalogued_second,
        "/tmp/b/cpp-indexer",
        caller_suffix="non-catalogued-mirrored",
        grouped=False,
    )
    non_catalogued_first_executor = Executor(non_catalogued_first)
    non_catalogued_second_executor = Executor(non_catalogued_second)
    assert non_catalogued_first_executor.run(
        (start(codebase()) | view("evidence") | nodes()
         | select(["identity_key"])).plan
    ).rows == non_catalogued_second_executor.run(
        (start(codebase()) | view("evidence") | nodes()
         | select(["identity_key"])).plan
    ).rows
    assert non_catalogued_first_executor.run(
        (start(codebase()) | view("call_argument") | nodes()
         | select(["identity_key"])).plan
    ).rows == non_catalogued_second_executor.run(
        (start(codebase()) | view("call_argument") | nodes()
         | select(["identity_key"])).plan
    ).rows


def test_site_view_expansion_exposes_deterministic_provenance():
    db = Storage(":memory:")
    _seed_reverse_typed_graph(db, "/tmp/site-view/cpp-indexer", grouped=False)
    caller, callee = db._conn.execute(
        "SELECT src_id,dst_id FROM edge WHERE id=1"
    ).fetchone()
    reverse_edge = db.add_edge(callee, caller, 1)
    file_id = db._conn.execute("SELECT id FROM file WHERE name='same.cpp'").fetchone()[0]
    db.add_edge_site(reverse_edge, file_id, 5, 2)
    result = Executor(db).run(
        (start(codebase()) | view("edge") | nodes() | sites()
         | select(["edge_id", "file", "line", "col", "relation",
                   "evidence", "status", "partial"])).plan
    )
    assert len(result.rows) == 2
    edge_id, path, line, col, relation, evidence, status, partial = result.rows[0]
    assert edge_id == 1
    assert path.endswith("/same.cpp")
    assert (line, col) == (10, 2)
    assert (relation, evidence, status, partial) == ("calls", "call_site", "partial", 1)
    assert result.view == "site"
    assert result.rows[1][0] == reverse_edge
    assert result.rows[1][2] == 5


def test_typed_provenance_preserves_status_through_select():
    def run(kind, unresolved_endpoint=False, unresolved_site=False):
        db = Storage(":memory:")
        component = db.add_component("project", "/tmp/status-view")
        directory = db.add_directory(component, "src")
        file_id = db.add_file(directory, "status.cpp")
        caller = db.add_symbol(_make_sym("USR::status-caller", "caller"))
        callee = db.add_symbol(_make_sym("USR::status-callee", "callee"))
        if unresolved_endpoint:
            db._conn.execute("UPDATE symbol SET resolved=0 WHERE id=?", (callee,))
        edge = db.add_edge(caller, callee, kind)
        db.add_edge_site(
            edge, file_id, 1, 1,
            recv_decl_usr="USR::missing-declaration" if unresolved_site else None,
        )
        return Executor(db).run(
            (start(codebase()) | view("edge") | nodes()
             | select(["status", "partial", "unknown"])).plan
        )

    def check(result, status, partial, unknown, expected_status):
        assert result.rows == [(status, partial, unknown)]
        assert result.partial is (partial == 1)
        assert result.unknown is (unknown == 1)
        result.index = replace(result.index, freshness="current")
        assert from_query_result(result, result.index).status is expected_status

    check(run(2), "complete", 0, 0, Status.COMPLETE)
    check(run(1), "partial", 1, 0, Status.PARTIAL)
    check(run(2, unresolved_endpoint=True), "unknown", 0, 1, Status.UNKNOWN)
    check(run(2, unresolved_site=True), "unknown", 0, 1, Status.UNKNOWN)


def test_site_status_follows_the_full_logical_site_key():
    db = Storage(":memory:")
    component = db.add_component("project", "/tmp/mixed-site-view")
    directory = db.add_directory(component, "src")
    file_id = db.add_file(directory, "mixed.cpp")
    caller = db.add_symbol(_make_sym("USR::mixed-caller", "caller"))
    callee = db.add_symbol(_make_sym("USR::mixed-callee", "callee"))
    edge = db.add_edge(caller, callee, 1)
    db.add_edge_site(edge, file_id, 1, 1)
    db.add_edge_site(
        edge, file_id, 2, 1, recv_decl_usr="USR::missing-declaration"
    )

    def check_rows(result):
        assert result.rows == [
            (1, "partial", 1, 0),
            (2, "unknown", 0, 1),
        ]
        assert result.partial
        assert result.unknown
        result.index = replace(result.index, freshness="current")
        assert from_query_result(result, result.index).status is Status.UNKNOWN

    site_plan = (start(codebase()) | view("site") | nodes()
                 | select(["line", "status", "partial", "unknown"])).plan
    evidence_plan = (start(codebase()) | view("evidence") | nodes()
                     | select(["line", "status", "partial", "unknown"])).plan
    check_rows(Executor(db).run(site_plan))
    check_rows(Executor(db).run(evidence_plan))

    executor = Executor(db)
    ordered = executor.run(
        (start(codebase()) | view("site") | nodes()
         | select(["line", "status", "partial", "unknown"])
         | order_by(["line"]) | limit(1)).plan
    )
    assert ordered.rows == [(1, "partial", 1, 0)]
    assert ordered.partial
    assert not ordered.unknown
    ordered.index = replace(ordered.index, freshness="current")
    assert from_query_result(ordered, ordered.index).status is Status.PARTIAL

    counted = executor.run(
        (start(codebase()) | view("site") | nodes()
         | select(["line", "status", "partial", "unknown"])
         | qp.count()).plan
    )
    assert counted.scalar == 2
    assert counted.partial
    assert counted.unknown
    counted.index = replace(counted.index, freshness="current")
    assert from_query_result(counted, counted.index).status is Status.UNKNOWN

    distinct = executor.run(
        (start(codebase()) | view("site") | nodes()
         | select(["relation"]) | qp.distinct()).plan
    )
    assert distinct.rows == [("calls",)]
    assert distinct.partial
    assert not distinct.unknown
    distinct.index = replace(distinct.index, freshness="current")
    assert from_query_result(distinct, distinct.index).status is Status.PARTIAL


def test_default_cap_recomputes_discarded_site_status():
    db = Storage(":memory:")
    component = db.add_component("project", "/tmp/capped-site-view")
    directory = db.add_directory(component, "src")
    file_id = db.add_file(directory, "capped.cpp")
    caller = db.add_symbol(_make_sym("USR::capped-caller", "caller"))
    callee = db.add_symbol(_make_sym("USR::capped-callee", "callee"))
    edge = db.add_edge(caller, callee, 1)
    db._conn.execute(
        "WITH RECURSIVE lines(line) AS (SELECT 0 UNION ALL SELECT line + 1 "
        "FROM lines WHERE line < 999) INSERT INTO edge_site "
        "(edge_id,file_id,line,col) SELECT ?,?,line,0 FROM lines",
        (edge, file_id),
    )
    db.add_edge_site(
        edge, file_id, 1000, 0, recv_decl_usr="USR::missing-declaration"
    )
    db._conn.commit()

    result = Executor(db).run(
        (start(codebase()) | view("site") | nodes()
         | select(["line", "status", "unknown"])).plan
    )
    assert len(result.rows) == qp.DEFAULT_RESULT_CAP
    assert result.truncated
    assert result.partial
    assert not result.unknown
    result.index = replace(result.index, freshness="current")
    assert from_query_result(result, result.index).status is Status.PARTIAL


@pytest.mark.parametrize(
    ("site_count", "expected", "truncated"),
    [
        (qp.TRAVERSE_NODE_BUDGET - 1, qp.TRAVERSE_NODE_BUDGET - 1, False),
        (qp.TRAVERSE_NODE_BUDGET, qp.TRAVERSE_NODE_BUDGET, False),
        (qp.TRAVERSE_NODE_BUDGET + 1, qp.TRAVERSE_NODE_BUDGET, True),
    ],
)
def test_sites_budget_boundaries_are_exact_and_ordered(site_count, expected, truncated):
    db = Storage(":memory:")
    component = db.add_component("project", "/tmp/budget-view")
    directory = db.add_directory(component, "src")
    file_id = db.add_file(directory, "budget.cpp")
    caller = db.add_symbol(_make_sym("USR::budget-caller", "caller"))
    callee = db.add_symbol(_make_sym("USR::budget-callee", "callee"))
    edge = db.add_edge(caller, callee, 1)
    db._conn.execute(
        "WITH RECURSIVE lines(line) AS (SELECT 0 UNION ALL SELECT line + 1 "
        f"FROM lines WHERE line < {site_count - 1}) "
        "INSERT INTO edge_site(edge_id,file_id,line,col) "
        f"SELECT {edge},{file_id},line,0 FROM lines",
    )
    db._conn.commit()

    result = Executor(db).run(
        (start(codebase()) | view("edge") | nodes() | sites()
         | limit(qp.TRAVERSE_NODE_BUDGET)
         | qp.count()).plan
    )
    assert result.scalar == expected
    assert result.truncated is truncated


def test_typed_view_compositions_are_rejected_before_execution():
    with pytest.raises(PlanError, match="^E_VIEW:"):
        validate((start(codebase()) | view("edge") | nodes()
                  | view("site")).plan)
    with pytest.raises(PlanError, match="^E_VIEW:"):
        validate((start(codebase()) | nodes() | sites()).plan)
    assert '"op": "sites"' in canonical_json(
        (start(codebase()) | view("edge") | nodes() | sites()).plan
    )


# ---------------------------------------------------------------------------
# HSE-31 (CXQ-004): bounded witness paths, ranking, explain
# ---------------------------------------------------------------------------

def _error_code(fn):
    try:
        fn()
    except PlanError as exc:
        return str(exc).split(":", 1)[0]
    return "<no-error>"


def test_path_validation_errors():
    code = lambda p: _error_code(lambda: validate(p))  # noqa: E731

    assert code((start(symbol("A")) | where(eq("kind", "function"))
                 | path(start(symbol("C")), "calls")).plan) == "<no-error>"
    assert code((start(symbol("A"))
                 | path(start(symbol("C")), "bogus")).plan) == "E_RELATION"
    assert code((start(symbol("A"))
                 | path(start(symbol("C")), "has_parameter")).plan) == "E_RELATION"
    assert code((start(symbol("A"))
                 | path(start(symbol("C")), "calls", 1, 40)).plan) == "E_DEPTH"
    assert code((start(symbol("A")) | path(start(symbol("C")), "calls", 1, 8,
                                           -1)).plan) == "E_LIMIT"
    assert code((start(symbol("A"))
                 | path(start(entity("C")), "calls")).plan) == "E_SETOP"
    assert code((start(codebase()) | view("type") | nodes()
                 | path(start(symbol("C")), "calls")).plan) == "E_VIEW"
    assert code((start(symbol("A")) | path(start(symbol("C")), "calls")
                 | rank(-1)).plan) == "E_LIMIT"
    assert code((start(symbol("A")) | rank()).plan) == "E_STAGE"
    assert code((start(symbol("A")) | path(start(symbol("C")), "calls")
                 | out("calls")).plan) == "E_STAGE"
    assert code((start(symbol("A")) | path(start(symbol("C")), "calls")
                 | order_by(["name"])).plan) == "E_STAGE"


def test_reverse_type_use_validation_errors():
    code = lambda p: _error_code(lambda: validate(p))  # noqa: E731

    assert code((start(symbol("A"))
                 | reverse_type_use()).plan) == "E_VIEW"
    assert code((start(codebase()) | view("type") | nodes()
                 | reverse_type_use(0)).plan) == "E_DEPTH"
    assert code((start(codebase()) | view("type") | nodes()
                 | reverse_type_use(33)).plan) == "E_DEPTH"


def test_path_finds_the_shortest_witness_with_sites(seeded):
    db, ids = seeded
    component = db.add_component("project", "/tmp/path-view")
    directory = db.add_directory(component, "src")
    file_id = db.add_file(directory, "path.cpp")
    edge = next(db._conn.execute(
        "SELECT id FROM edge WHERE src_id=? AND dst_id=? AND kind=1",
        (ids["A"], ids["B"])))[0]
    db.add_edge_site(edge, file_id, 1, 1)
    db._conn.commit()

    ex = Executor(db)
    result = ex.run(
        (start(symbol("USR::A"))
         | path(start(symbol("USR::C")), "calls", 1, 8, 1)).plan)
    assert result.shape == "path"
    assert len(result.paths) == 1
    witness = result.paths[0]
    assert witness.length == 2
    assert [s.node_id for s in witness.steps] == [ids["A"], ids["B"], ids["C"]]
    assert witness.steps[0].through == ""
    assert witness.steps[1].through == "calls"
    assert witness.steps[2].through == "calls"
    assert len(witness.steps[1].sites) == 1
    assert witness.steps[1].sites[0]["line"] == 1
    assert not result.truncated


def test_path_reports_no_witness_within_a_narrow_window(seeded):
    # A -[calls]-> B -[calls]-> C, but max_depth=1 only reaches B, which is
    # itself expandable (B -[calls]-> C exists one hop further). This is a
    # finite-depth exhaustion (docs/query-plan.md), not a proven negative,
    # so it must be reported truncated even though no witness is returned.
    db, ids = seeded
    ex = Executor(db)
    result = ex.run(
        (start(symbol("USR::A"))
         | path(start(symbol("USR::C")), "calls", 1, 1)).plan)
    assert result.paths == []
    assert result.truncated


def test_path_ties_are_broken_by_ascending_node_id_order():
    db = Storage(":memory:")
    start_id = db.add_symbol(_make_sym("USR::S", "s"))
    left = db.add_symbol(_make_sym("USR::L", "l"))
    right = db.add_symbol(_make_sym("USR::R", "r"))
    target = db.add_symbol(_make_sym("USR::T", "t"))
    db.add_edge(start_id, left, 1)
    db.add_edge(start_id, right, 1)
    db.add_edge(left, target, 1)
    db.add_edge(right, target, 1)
    db._conn.commit()

    ex = Executor(db)
    result = ex.run(
        (start(symbol("USR::S"))
         | path(start(symbol("USR::T")), "calls", 1, 8)).plan)
    assert len(result.paths) == 2
    assert result.paths[0].steps[1].node_id == min(left, right)
    assert result.paths[1].steps[1].node_id == max(left, right)

    ranked = ex.run(
        (start(symbol("USR::S"))
         | path(start(symbol("USR::T")), "calls", 1, 8) | rank(1)).plan)
    assert len(ranked.paths) == 1
    assert ranked.paths[0].steps[1].node_id == min(left, right)


def test_path_count_distinct_limit_apply_to_witnesses(seeded):
    db, ids = seeded
    ex = Executor(db)
    counted = ex.run(
        (start(symbol("USR::A"))
         | path(start(symbol("USR::C")), "calls", 1, 8) | count()).plan)
    assert counted.shape == "scalar"
    assert counted.scalar == 1

    limited = ex.run(
        (start(symbol("USR::A"))
         | path(start(symbol("USR::C")), "calls", 1, 8) | limit(1)).plan)
    assert len(limited.paths) == 1

    deduped = ex.run(
        (start(symbol("USR::A"))
         | path(start(symbol("USR::C")), "calls", 1, 8) | distinct()).plan)
    assert len(deduped.paths) == 1


@pytest.mark.parametrize(
    "fanout",
    [qp.PATH_NODE_BUDGET, qp.PATH_NODE_BUDGET + 1],
)
def test_path_level_budget_is_exact_at_boundary_and_truncates_without_witness(fanout):
    # A single start node fans out to `fanout` children in one BFS level: the
    # level-discovery query for that one level returns `fanout` rows in one
    # chunk. Before the fix this only checked PATH_NODE_BUDGET after the whole
    # level's rows were read into parent_of; now the row loop itself breaks
    # the moment the budget is exceeded, so the level (and hence any witness
    # reconstruction from it) is abandoned without waiting to finish reading a
    # level far larger than the budget.
    db = Storage(":memory:")
    start_id = db.add_symbol(_make_sym("USR::fan-start", "start"))
    first_child = None
    for i in range(fanout):
        child = db.add_symbol(_make_sym(f"USR::fan-child-{i}", f"child{i}"))
        db.add_edge(start_id, child, 1)
        if i == 0:
            first_child = child
    db._conn.commit()

    ex = Executor(db)
    result = ex.run(
        (start(symbol("USR::fan-start"))
         | path(start(symbol("USR::fan-child-0")), "calls", 1, 1)).plan)
    if fanout > qp.PATH_NODE_BUDGET:
        assert result.paths == []
        assert result.truncated
    else:
        assert len(result.paths) == 1
        assert result.paths[0].steps[-1].node_id == first_child
        assert not result.truncated


def test_reverse_type_use_retains_every_typed_layer():
    db = Storage(":memory:")
    owner = db.add_symbol(_make_sym("USR::owner", "owner"))
    db._conn.execute(
        "INSERT INTO type_node(type_key,spelling,kind,extent) VALUES "
        "('A4(b:int)','int[4]',8,'4'),('b:int','int',1,NULL)")
    array_id, int_id = [row[0] for row in db._conn.execute(
        "SELECT id FROM type_node ORDER BY id")]
    db._conn.execute(
        "INSERT INTO type_edge(src_id,kind,position,dst_id) VALUES "
        "(?,2,0,?)", (array_id, int_id))
    db._conn.execute(
        "INSERT INTO symbol_type(symbol_id,kind,type_id) VALUES (?,1,?)",
        (owner, array_id))
    db._conn.execute(
        "INSERT INTO parameter(owner_id,position,pack_index,name,type_id) "
        "VALUES (?,0,-1,'value',?)", (owner, int_id))
    db._conn.commit()

    ex = Executor(db)
    result = ex.run(
        (start(codebase()) | view("type") | nodes()
         | where(eq("type_key", "b:int")) | reverse_type_use()).plan)
    assert result.shape == "path"
    assert len(result.paths) == 2

    direct = next(w for w in result.paths if w.length == 1)
    assert direct.steps[-1].domain == "parameter"
    assert direct.steps[-1].node_id == owner

    nested = next(w for w in result.paths if w.length == 2)
    assert len(nested.steps) == 3
    assert nested.steps[0].node_id == int_id
    assert nested.steps[1].node_id == array_id
    assert nested.steps[1].through == "element_type"
    assert nested.steps[2].domain == "symbol"
    assert nested.steps[2].node_id == owner


def test_explain_reports_budgets_shape_and_input_relations(seeded):
    db, ids = seeded
    ex = Executor(db)
    plan = (start(symbol("USR::A"))
           | path(start(symbol("USR::C")), "calls", 1, 8, 1)).plan
    explained = ex.explain(plan)
    assert explained["execution_shape"] == "path"
    assert explained["budgets"]["traverse_node_budget"] == 10000
    assert explained["budgets"]["path_node_budget"] == 10000
    assert explained["budgets"]["default_result_cap"] == 1000
    assert {"relation": "symbol.calls", "completeness": "partial"} in \
        explained["input_relations"]
    assert explained["partial_inputs"] is True


# ---------------------------------------------------------------------------
# PR #69 review round: path-window BFS, evidence caps, owner identity,
# explain() completeness/freshness regressions
# ---------------------------------------------------------------------------

def test_path_finds_an_in_window_witness_reached_only_at_a_later_depth():
    # S->T is depth 1 (out of the [2,2] window); S->M->T is the only depth-2
    # route. A permanent cross-level visited set would mark T visited at
    # depth 1 and discard the depth-2 rediscovery via M.
    db = Storage(":memory:")
    s = db.add_symbol(_make_sym("USR::S", "s"))
    t = db.add_symbol(_make_sym("USR::T", "t"))
    m = db.add_symbol(_make_sym("USR::M", "m"))
    db.add_edge(s, t, 1)
    db.add_edge(s, m, 1)
    db.add_edge(m, t, 1)
    db._conn.commit()

    ex = Executor(db)
    result = ex.run(
        (start(symbol("USR::S"))
         | path(start(symbol("USR::T")), "calls", 2, 2)).plan)
    assert len(result.paths) == 1
    witness = result.paths[0]
    assert witness.length == 2
    assert [step.node_id for step in witness.steps] == [s, m, t]
    assert not result.truncated


def test_path_reports_truncation_when_a_hop_has_more_sites_than_the_cap():
    db = Storage(":memory:")
    component = db.add_component("project", "/tmp/path-evidence-cap-py")
    directory = db.add_directory(component, "src")
    file_id = db.add_file(directory, "cap.cpp")
    a = db.add_symbol(_make_sym("USR::CapA", "a"))
    b = db.add_symbol(_make_sym("USR::CapB", "b"))
    edge = db.add_edge(a, b, 1)
    db._conn.execute(
        "WITH RECURSIVE lines(line) AS (SELECT 0 UNION ALL SELECT line + 1 "
        f"FROM lines WHERE line < 1000) INSERT INTO edge_site "
        f"(edge_id,file_id,line,col) SELECT {edge},{file_id},line,0 FROM lines")
    db._conn.commit()

    ex = Executor(db)
    result = ex.run(
        (start(symbol("USR::CapA"))
         | path(start(symbol("USR::CapB")), "calls", 1, 1)).plan)
    assert len(result.paths) == 1
    witness = result.paths[0]
    assert len(witness.steps) == 2
    assert len(witness.steps[1].sites) == qp.DEFAULT_RESULT_CAP
    assert witness.steps[1].status == "partial"
    assert witness.status == "partial"
    assert result.truncated  # incomplete evidence must never look complete


def test_reverse_type_use_preserves_distinct_parameter_slots_on_one_owner():
    db = Storage(":memory:")
    owner = db.add_symbol(_make_sym("USR::two_params", "two_params"))
    db._conn.execute(
        "INSERT INTO type_node(type_key,spelling,kind,extent) VALUES "
        "('b:int','int',1,NULL)")
    int_id = next(db._conn.execute(
        "SELECT id FROM type_node ORDER BY id"))[0]
    for position in (0, 1):
        db._conn.execute(
            "INSERT INTO parameter(owner_id,position,pack_index,name,"
            "type_id) VALUES (?,?,-1,'p',?)", (owner, position, int_id))
    db._conn.commit()

    ex = Executor(db)
    result = ex.run(
        (start(codebase()) | view("type") | nodes()
         | where(eq("type_key", "b:int")) | reverse_type_use()).plan)
    assert len(result.paths) == 2
    positions = []
    for witness in result.paths:
        assert len(witness.steps) == 2
        assert witness.steps[1].node_id == owner
        assert witness.steps[1].domain == "parameter"
        assert witness.steps[1].through == "parameter"
        positions.append(witness.steps[1].position)
    assert sorted(positions) == [0, 1]


def test_explain_reports_reverse_type_use_real_inputs():
    db = Storage(":memory:")
    db._conn.execute(
        "INSERT INTO type_node(type_key,spelling,kind,extent) VALUES "
        "('b:int','int',1,NULL)")
    db._conn.commit()

    ex = Executor(db)
    plan = (start(codebase()) | view("type") | nodes()
           | reverse_type_use()).plan
    explained = ex.explain(plan)
    assert explained["execution_shape"] == "path"
    names = {row["relation"] for row in explained["input_relations"]}
    assert {
        "type.has_type_edge", "type.canonical_id", "symbol.of_type",
        "parameter.of_type", "template_parameter.of_type",
        "template_argument.of_type",
    } <= names
    assert explained["partial_inputs"] is True
    assert explained["unknown_capable_inputs"] is True


def test_explain_exposes_expected_vs_indexed_source_revision_on_stale_index(
        tmp_path):
    source = tmp_path / "answer.cpp"
    source.write_text("int answer = 1;\n")
    db = Storage(":memory:")
    db.add_component("fixture", str(tmp_path))
    file_id = db.add_file_path(str(source), None, md5_of(str(source)))
    db.mark_file_indexed(file_id)
    db.stamp_index_identity()

    current = db.index_identity()
    assert current.freshness == "current"
    assert current.source_revision is not None
    assert current.expected_source_revision is not None
    assert current.source_revision == current.expected_source_revision
    assert current.expected_index_config_fingerprint is not None

    # Change the checkout after stamping: the persisted identity now
    # disagrees with what the current source hashes to.
    source.write_text("int answer = 2;\n")
    stale = db.index_identity()
    assert stale.freshness == "stale"
    assert stale.source_revision is not None
    assert stale.expected_source_revision is not None
    assert stale.source_revision != stale.expected_source_revision
    assert stale.source_revision == current.source_revision  # unchanged

    ex = Executor(db)
    explained = ex.explain((start(codebase()) | nodes()).plan)
    assert explained["index"]["freshness"] == "stale"
    assert "expected_source_revision" in explained["index"]
    assert "expected_source_fingerprint" in explained["index"]
    assert "expected_index_config_fingerprint" in explained["index"]


# ---------------------------------------------------------------------------
# PR #69 review round 2: intermediate type_edge identity and path
# distinctness
# ---------------------------------------------------------------------------

def test_reverse_type_use_retains_each_intermediate_type_edge_position():
    # One function type F has two `param_type` edges to the same pointee
    # (int) at positions 0 and 1. Both climbs share (parent=F,
    # through=param_type); without `position` they would serialize as
    # byte-identical witnesses.
    db = Storage(":memory:")
    owner = db.add_symbol(_make_sym("USR::two_int_params_fn",
                                    "two_int_params_fn"))
    db._conn.execute(
        "INSERT INTO type_node(type_key,spelling,kind,extent) VALUES "
        "('fn(int,int)','void(int,int)',9,NULL),('b:int','int',1,NULL)")
    fn_id, int_id = [row[0] for row in db._conn.execute(
        "SELECT id FROM type_node ORDER BY id")]
    db._conn.execute(
        "INSERT INTO type_edge(src_id,kind,position,dst_id) VALUES "
        "(?,5,0,?),(?,5,1,?)", (fn_id, int_id, fn_id, int_id))
    db._conn.execute(
        "INSERT INTO symbol_type(symbol_id,kind,type_id) VALUES (?,1,?)",
        (owner, fn_id))
    db._conn.commit()

    ex = Executor(db)
    result = ex.run(
        (start(codebase()) | view("type") | nodes()
         | where(eq("type_key", "b:int")) | reverse_type_use()).plan)
    assert len(result.paths) == 2
    positions = []
    for witness in result.paths:
        assert witness.length == 2
        assert len(witness.steps) == 3
        assert witness.steps[0].node_id == int_id
        assert witness.steps[1].node_id == fn_id
        assert witness.steps[1].through == "param_type"
        positions.append(witness.steps[1].position)
        assert witness.steps[2].node_id == owner
    assert sorted(positions) == [0, 1]

    # distinct() must not collapse these two structurally-different
    # witnesses just because the F-hop's `through` label is the same.
    deduped = ex.run(
        (start(codebase()) | view("type") | nodes()
         | where(eq("type_key", "b:int")) | reverse_type_use()
         | distinct()).plan)
    assert len(deduped.paths) == 2


def test_path_distinct_compares_full_typed_slot_identity_not_just_node_id():
    # One owner has two `int` parameters at positions 0 and 1. Both
    # witnesses share (node_id, through, inbound) on the final step -- only
    # position distinguishes them -- so a distinctness key that ignores
    # position would wrongly collapse them to one.
    db = Storage(":memory:")
    owner = db.add_symbol(_make_sym("USR::two_params_distinct",
                                    "two_params"))
    db._conn.execute(
        "INSERT INTO type_node(type_key,spelling,kind,extent) VALUES "
        "('b:int','int',1,NULL)")
    int_id = next(db._conn.execute(
        "SELECT id FROM type_node ORDER BY id"))[0]
    for position in (0, 1):
        db._conn.execute(
            "INSERT INTO parameter(owner_id,position,pack_index,name,"
            "type_id) VALUES (?,?,-1,'p',?)", (owner, position, int_id))
    db._conn.commit()

    ex = Executor(db)
    before_distinct = ex.run(
        (start(codebase()) | view("type") | nodes()
         | where(eq("type_key", "b:int")) | reverse_type_use()).plan)
    assert len(before_distinct.paths) == 2

    after_distinct = ex.run(
        (start(codebase()) | view("type") | nodes()
         | where(eq("type_key", "b:int")) | reverse_type_use()
         | distinct()).plan)
    assert len(after_distinct.paths) == 2
    positions = []
    for witness in after_distinct.paths:
        assert len(witness.steps) == 2
        positions.append(witness.steps[1].position)
    assert sorted(positions) == [0, 1]


# ---------------------------------------------------------------------------
# PR #69 review round 3: finite-depth exhaustion, incomplete chain
# reconstruction, symbol-owner provenance, and a total rank key
# ---------------------------------------------------------------------------

def test_path_reports_truncation_when_depth_window_cuts_off_an_expandable_frontier():
    # S -> M -> T, but max_depth=1 only lets the BFS reach M. M is not a
    # target, and M is itself expandable (M -> T exists one hop further), so
    # this "no witness" is a finite-depth exhaustion, not proof no path
    # exists.
    db = Storage(":memory:")
    s = db.add_symbol(_make_sym("USR::FDE_S", "s"))
    m = db.add_symbol(_make_sym("USR::FDE_M", "m"))
    t = db.add_symbol(_make_sym("USR::FDE_T", "t"))
    db.add_edge(s, m, 1)
    db.add_edge(m, t, 1)
    db._conn.commit()

    ex = Executor(db)
    result = ex.run(
        (start(symbol("USR::FDE_S"))
         | path(start(symbol("USR::FDE_T")), "calls", 1, 1)).plan)
    assert result.paths == []
    assert result.truncated


def test_path_does_not_report_truncation_when_a_start_is_genuinely_exhausted():
    # S has no outgoing edges at all: the BFS dies at depth 1 with an empty
    # frontier, a real dead end, not a depth-limit cutoff.
    db = Storage(":memory:")
    db.add_symbol(_make_sym("USR::FDE_Dead_S", "s"))
    db.add_symbol(_make_sym("USR::FDE_Dead_T", "t"))
    db._conn.commit()

    ex = Executor(db)
    result = ex.run(
        (start(symbol("USR::FDE_Dead_S"))
         | path(start(symbol("USR::FDE_Dead_T")), "calls", 1, 4)).plan)
    assert result.paths == []
    assert not result.truncated


def test_path_never_serializes_a_chain_reconstruction_cut_short_by_the_witness_cap():
    # A depth-2 fan where the middle layer alone has far more than
    # DEFAULT_RESULT_CAP predecessor combinations: reconstruction from the
    # single target must either complete every emitted witness back to the
    # real source, or emit none -- never a short chain that silently starts
    # mid-path.
    db = Storage(":memory:")
    source = db.add_symbol(_make_sym("USR::Chain_Source", "src"))
    target = db.add_symbol(_make_sym("USR::Chain_Target", "tgt"))
    fanout = 1200  # > DEFAULT_RESULT_CAP
    for i in range(fanout):
        node = db.add_symbol(
            _make_sym(f"USR::Chain_Mid_{i}", f"mid{i}"))
        db.add_edge(source, node, 1)
        db.add_edge(node, target, 1)
    db._conn.commit()

    ex = Executor(db)
    result = ex.run(
        (start(symbol("USR::Chain_Source"))
         | path(start(symbol("USR::Chain_Target")), "calls", 2, 2)).plan)
    assert result.truncated
    for witness in result.paths:
        assert witness.length == 2
        assert len(witness.steps) == 3  # source, middle, target -- complete
        assert witness.steps[0].node_id == source
        assert witness.steps[2].node_id == target


def test_reverse_type_use_reports_a_direct_symbol_owners_declaration_site():
    db = Storage(":memory:")
    component = db.add_component("project", "/tmp/owner-site-provenance-py")
    directory = db.add_directory(component, "src")
    file_id = db.add_file(directory, "owner_site.cpp")
    owner = db.add_symbol(_make_sym("USR::OwnerWithSite", "owner_with_site"))
    db._conn.execute(
        "UPDATE symbol SET file_id=?, line=41, col=7 WHERE id=?",
        (file_id, owner))
    db._conn.execute(
        "INSERT INTO type_node(type_key,spelling,kind,extent) VALUES "
        "('b:owner_site','int',1,NULL)")
    type_id = next(db._conn.execute(
        "SELECT id FROM type_node WHERE type_key='b:owner_site'"))[0]
    db._conn.execute(
        "INSERT INTO symbol_type(symbol_id,kind,type_id) VALUES (?,2,?)",
        (owner, type_id))
    db._conn.commit()

    ex = Executor(db)
    result = ex.run(
        (start(codebase()) | view("type") | nodes()
         | where(eq("type_key", "b:owner_site")) | reverse_type_use()).plan)
    assert len(result.paths) == 1
    witness = result.paths[0]
    assert len(witness.steps) == 2
    owner_step = witness.steps[-1]
    assert owner_step.node_id == owner
    assert owner_step.domain == "symbol"
    assert owner_step.through == "of_type"
    assert len(owner_step.sites) == 1
    assert owner_step.sites[0]["file_id"] == file_id
    assert owner_step.sites[0]["line"] == 41
    assert owner_step.sites[0]["col"] == 7


def test_reverse_type_use_rank_orders_witnesses_by_the_full_typed_step_identity():
    # Two type_edge hops (return_type, then element_type) both land on M at
    # position 0 -- an identical (node_id, position, pack_index) key -- but
    # they are structurally distinct witnesses. Only `through` tells them
    # apart, so the rank key must include it to be total.
    #
    # `type_edge` is a WITHOUT ROWID table keyed by (src_id, kind, position),
    # so for a fixed src_id/position its physical/scan order is kind-
    # ascending; reverse_type_use()'s DFS climbs via a LIFO stack, which
    # reverses that to kind-*descending* in the raw, pre-sort witness order.
    # kind 4 (return_type) > kind 2 (element_type), so the raw order here is
    # [return_type, element_type] -- the opposite of the expected alphabetical
    # rank order ("element_type" < "return_type"). A rank key that dropped the
    # `through` tie-break would leave that raw order untouched (both
    # witnesses tie on node_id/domain/position/pack_index) and this test
    # would observe the wrong order; kind ids 7/8 (member_owner/
    # member_component) do NOT work for this purpose since their raw DFS
    # order already happens to coincide with their alphabetical order,
    # silently passing either way.
    db = Storage(":memory:")
    owner = db.add_symbol(_make_sym("USR::RankOwner", "owner"))
    db._conn.execute(
        "INSERT INTO type_node(type_key,spelling,kind,extent) VALUES "
        "('b:rank_a','A',1,NULL),('b:rank_m','M',1,NULL)")
    a_id, m_id = [row[0] for row in db._conn.execute(
        "SELECT id FROM type_node WHERE type_key IN ('b:rank_a','b:rank_m') "
        "ORDER BY type_key")]
    db._conn.execute(
        "INSERT INTO type_edge(src_id,kind,position,dst_id) VALUES "
        "(?,4,0,?)", (m_id, a_id))  # return_type
    db._conn.execute(
        "INSERT INTO type_edge(src_id,kind,position,dst_id) VALUES "
        "(?,2,0,?)", (m_id, a_id))  # element_type
    db._conn.execute(
        "INSERT INTO symbol_type(symbol_id,kind,type_id) VALUES (?,2,?)",
        (owner, m_id))
    db._conn.commit()

    ex = Executor(db)
    result = ex.run(
        (start(codebase()) | view("type") | nodes()
         | where(eq("type_key", "b:rank_a")) | reverse_type_use()
         | distinct() | rank()).plan)
    assert len(result.paths) == 2
    assert len(result.paths[0].steps) == 3
    assert len(result.paths[1].steps) == 3
    assert result.paths[0].steps[1].node_id == m_id
    assert result.paths[1].steps[1].node_id == m_id
    # "element_type" < "return_type" alphabetically.
    assert result.paths[0].steps[1].through == "element_type"
    assert result.paths[1].steps[1].through == "return_type"
