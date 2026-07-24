"""v33 -> v34 alias_of(19) edge-kind migration + read-query parity (mirrors
the C++ storage_migration_test case).

v34 gives the typedef / using-alias -> underlying-type edge its own
``alias_of`` kind and the variable/field -> declared-type edge its own
``of_type`` kind; before, both were stored as the overloaded ``uses(7)``. The
migration rewrites exactly those rows: namespace-qualifier edges
(``using X = ns::Foo``) and ordinary references from functions stay ``uses``.
"""

import sqlite3

from indexer.query import GraphQuery
from indexer.storage import SCHEMA_VERSION, Storage, Symbol


def _sym(usr: str, spelling: str, kind: str) -> Symbol:
    return Symbol(
        usr=usr,
        spelling=spelling,
        qual_name=spelling,
        kind=kind,
        is_definition=True,
    )


def _make_v33(path: str) -> dict[str, int]:
    """A fresh DB wound back to '33': alias_of kind row removed, alias edges
    stored as uses(7) the way the pre-v34 indexer wrote them."""
    db = Storage(path)
    db.add_component("c", "/data/c")
    ids = {
        "record": db.add_symbol(_sym("c:@S@Color", "Color", "struct")),
        "alias": db.add_symbol(_sym("c:@Rgb", "Rgb", "type-alias")),
        "ns": db.add_symbol(_sym("c:@N@ns", "ns", "namespace")),
        "fn": db.add_symbol(_sym("c:@F@paint#", "paint", "function")),
        "var": db.add_symbol(_sym("c:@shade", "shade", "variable")),
        "field": db.add_symbol(_sym("c:@S@Palette@FI@main", "main", "member")),
    }
    with db.transaction():
        db.add_edge(ids["alias"], ids["record"], 7)  # alias -> type: rewrite
        db.add_edge(ids["alias"], ids["ns"], 7)  # ns qualifier: stays uses
        db.add_edge(ids["fn"], ids["record"], 7)  # plain ref: stays uses
        db.add_edge(ids["var"], ids["record"], 7)  # var -> type: of_type(20)
        db.add_edge(ids["field"], ids["record"], 7)  # field -> type: of_type
    db.close()
    conn = sqlite3.connect(path)
    conn.execute("DELETE FROM edge_kind WHERE id IN (19, 20)")
    conn.execute("UPDATE meta SET value = '33' WHERE key = 'schema_version'")
    conn.commit()
    conn.close()
    return ids


def test_v33_to_v34_rewrites_alias_edges_and_stamps_version(tmp_path):
    path = str(tmp_path / "v33.db")
    ids = _make_v33(path)

    db = Storage(path)  # migration runs on open
    assert db.get_component("/data/c") is not None  # old data intact
    assert db.index_identity().freshness == "unverifiable"
    db.close()

    conn = sqlite3.connect(path)
    ver = conn.execute(
        "SELECT value FROM meta WHERE key = 'schema_version'"
    ).fetchone()[0]
    assert ver == str(SCHEMA_VERSION)
    name = conn.execute("SELECT name FROM edge_kind WHERE id = 19").fetchone()
    assert name == ("alias_of",)
    name = conn.execute("SELECT name FROM edge_kind WHERE id = 20").fetchone()
    assert name == ("of_type",)

    def kind_of(src: int, dst: int) -> int:
        return conn.execute(
            "SELECT kind FROM edge WHERE src_id = ? AND dst_id = ?", (src, dst)
        ).fetchone()[0]

    assert kind_of(ids["alias"], ids["record"]) == 19
    assert kind_of(ids["alias"], ids["ns"]) == 7
    assert kind_of(ids["fn"], ids["record"]) == 7
    assert kind_of(ids["var"], ids["record"]) == 20
    assert kind_of(ids["field"], ids["record"]) == 20
    conn.close()


def test_migration_is_idempotent(tmp_path):
    path = str(tmp_path / "twice.db")
    ids = _make_v33(path)
    Storage(path).close()  # first migration
    Storage(path).close()  # reopen: kind 19 present, no second rewrite
    conn = sqlite3.connect(path)
    kinds = [
        r[0]
        for r in conn.execute(
            "SELECT kind FROM edge WHERE src_id = ? ORDER BY kind",
            (ids["alias"],),
        )
    ]
    conn.close()
    assert kinds == [7, 19]


def test_aliased_by_reads_alias_of_edges(tmp_path):
    path = str(tmp_path / "query.db")
    ids = _make_v33(path)
    Storage(path).close()  # migrate

    g = GraphQuery(path)
    record = g.find("Color")[0]
    assert [s.name for s in g.aliased_by(record)] == ["Rgb"]
    # alias_of still counts as a reference to the target.
    ref_kinds = {(e.peer.name, e.kind) for e in g.references(record)}
    assert ("Rgb", "alias_of") in ref_kinds
    assert ("paint", "uses") in ref_kinds
    g.close()
