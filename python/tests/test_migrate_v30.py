"""v29 -> v30 signature/type tier migration + read-query parity (mirrors the
C++ storage_migration_test case and src/graph/query.cpp signature/type_users).

A v29 database has none of the v30 tables (type_kind/type_node/type_edge_kind/
type_edge/parameter/symbol_type_kind/symbol_type). Opening it must create them
(schema script), stamp v30, and leave existing rows untouched. The tables are
written only by the C++ LibTooling indexer; Python provides storage + read
parity, exercised here with hand-inserted rows.
"""

import sqlite3

from indexer.query import GraphQuery
from indexer.storage import SCHEMA_VERSION, Storage

_V30_TABLES = (
    "symbol_type",
    "symbol_type_kind",
    "parameter",
    "type_edge",
    "type_edge_kind",
    "type_node",
    "type_kind",
)


def _make_v29(path: str) -> None:
    db = Storage(path)
    db.add_component("c", "/data/c")
    db.close()
    conn = sqlite3.connect(path)
    for table in _V30_TABLES:
        conn.execute(f"DROP TABLE {table}")
    conn.execute("UPDATE meta SET value = '29' WHERE key = 'schema_version'")
    conn.commit()
    conn.close()


def test_v29_to_v30_creates_tier_and_stamps_version(tmp_path):
    path = str(tmp_path / "v29.db")
    _make_v29(path)

    db = Storage(path)  # migration runs on open
    assert db.get_component("/data/c") is not None  # old data intact
    db.close()

    conn = sqlite3.connect(path)
    ver = conn.execute(
        "SELECT value FROM meta WHERE key = 'schema_version'"
    ).fetchone()[0]
    tables = {
        r[0]
        for r in conn.execute(
            "SELECT name FROM sqlite_master WHERE type = 'table'"
        )
    }
    seeds = conn.execute("SELECT COUNT(*) FROM type_kind").fetchone()[0]
    conn.close()
    assert ver == str(SCHEMA_VERSION)
    assert set(_V30_TABLES) <= tables
    assert seeds == 14


def _seed_signature_fixture(path: str) -> None:
    """Hand-written rows shaped exactly like the C++ extractor's output for:

        struct Foo {};
        using FooAlias = Foo;
        Foo make_foo(int seed, const Foo &proto);
    """
    Storage(path).close()
    conn = sqlite3.connect(path)
    conn.execute(
        "INSERT INTO symbol (id, usr, spelling, qual_name, kind) VALUES "
        "(1, 'c:@S@Foo', 'Foo', 'Foo', 2),"
        "(2, 'c:@F@make_foo', 'make_foo', 'make_foo', 8),"
        "(3, 'c:test.h@FooAlias', 'FooAlias', 'FooAlias', 36)"
    )
    # edge table must be non-empty for GraphQuery.require_edges consumers;
    # type queries do not need it, so leave it empty here.
    conn.execute(
        "INSERT INTO type_node "
        "(id, type_key, spelling, kind, is_const, decl_usr, canonical_id) "
        "VALUES "
        "(1, 'b:int', 'int', 1, 0, NULL, NULL),"
        "(2, 't:c:@S@Foo', 'Foo', 2, 0, 'c:@S@Foo', NULL),"
        "(3, 'q:c(t:c:@S@Foo)', 'const Foo', 2, 1, 'c:@S@Foo', NULL),"
        "(4, 'l(q:c(t:c:@S@Foo))', 'const Foo &', 6, 0, NULL, NULL),"
        "(5, 'a:c:test.h@FooAlias', 'FooAlias', 4, 0, 'c:test.h@FooAlias', 2)"
    )
    conn.execute(
        "INSERT INTO type_edge (src_id, kind, position, dst_id) VALUES "
        "(4, 1, 0, 3),"  # const Foo & --pointee--> const Foo
        "(5, 3, 0, 2)"   # FooAlias --alias_of--> Foo
    )
    conn.execute(
        "INSERT INTO parameter (owner_id, position, name, type_id) VALUES "
        "(2, 0, 'seed', 1),"
        "(2, 1, 'proto', 4)"
    )
    conn.execute(
        "INSERT INTO symbol_type (symbol_id, kind, type_id) VALUES "
        "(2, 1, 2),"  # make_foo returns Foo
        "(3, 3, 2)"   # FooAlias underlying_type Foo
    )
    conn.commit()
    conn.close()


def test_signature_read_parity(tmp_path):
    path = str(tmp_path / "sig.db")
    _seed_signature_fixture(path)
    g = GraphQuery(path)

    sig = g.signature(2)
    assert sig.returns is not None
    assert sig.returns.spelling == "Foo"
    assert sig.returns.kind == "record"
    assert sig.returns.canonical is None
    assert [(p.position, p.name) for p in sig.params] == [(0, "seed"), (1, "proto")]
    assert sig.params[0].type is not None
    assert sig.params[0].type.spelling == "int"
    assert sig.params[1].type is not None
    assert sig.params[1].type.spelling == "const Foo &"
    assert sig.of_type is None and sig.underlying is None

    alias_sig = g.signature(3)
    assert alias_sig.underlying is not None
    assert alias_sig.underlying.spelling == "Foo"

    # The alias node itself is sugared: canonical resolves to Foo's spelling.
    alias_info = g._type_info(5)
    assert alias_info is not None
    assert alias_info.kind == "alias"
    assert alias_info.canonical == "Foo"


def test_type_users_closure(tmp_path):
    path = str(tmp_path / "users.db")
    _seed_signature_fixture(path)
    g = GraphQuery(path)

    users = g.type_users(1)  # symbol id 1 = Foo
    got = [(u.sym.id, u.role, u.position) for u in users]
    # param rows first (owner, position), then symbol_type rows (symbol, kind):
    # proto reaches Foo through const-ref -> const Foo -> decl_usr, plus the
    # alias node via canonical_id; returns and underlying_type follow.
    assert got == [
        (2, "param", 1),
        (2, "returns", None),
        (3, "underlying_type", None),
    ]

    # A symbol with no type facts yields nothing.
    assert g.type_users(2) == []
