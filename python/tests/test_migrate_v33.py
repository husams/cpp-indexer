"""v32 -> v33 symbol.const_value migration + read-query parity (mirrors the
C++ storage_migration_test case and src/graph records).

v33 adds symbol.const_value: the evaluated constant value of a variable's
initializer or an enumerator, as printed by Clang's constant evaluator. Only
the C++ LibTooling indexer writes it; Python provides storage + read parity.
Values are not backfillable -- a v32 database migrates to NULLs until a
reindex.
"""

import sqlite3

from indexer.query import GraphQuery
from indexer.storage import SCHEMA_VERSION, Storage, Symbol


def _make_v32(path: str) -> None:
    # SQLite < 3.35 can't DROP COLUMN, so (like the older column migrations'
    # tests) simulate v32 by winding the version stamp back; the migration's
    # column guard must be idempotent against the already-present column.
    db = Storage(path)
    db.add_component("c", "/data/c")
    db.close()
    conn = sqlite3.connect(path)
    conn.execute("UPDATE meta SET value = '32' WHERE key = 'schema_version'")
    conn.commit()
    conn.close()


def test_v32_to_v33_adds_column_and_stamps_version(tmp_path):
    path = str(tmp_path / "v32.db")
    _make_v32(path)

    db = Storage(path)  # migration runs on open
    assert db.get_component("/data/c") is not None  # old data intact
    db.close()

    conn = sqlite3.connect(path)
    ver = conn.execute(
        "SELECT value FROM meta WHERE key = 'schema_version'"
    ).fetchone()[0]
    cols = {r[1] for r in conn.execute("PRAGMA table_info(symbol)")}
    conn.close()
    assert ver == str(SCHEMA_VERSION)
    assert "const_value" in cols


def test_const_value_roundtrip_and_decl_keeps_definition_value(tmp_path):
    path = str(tmp_path / "roundtrip.db")
    db = Storage(path)
    db.add_symbol(
        Symbol(
            usr="c:@kAnswer",
            spelling="kAnswer",
            qual_name="kAnswer",
            kind="variable",
            is_definition=True,
            const_value="42",
        )
    )
    # A plain declaration carries no initializer, so no value: the upsert must
    # keep the definition's stored constant (COALESCE, mirrors C++).
    db.add_symbol(
        Symbol(
            usr="c:@kAnswer",
            spelling="kAnswer",
            qual_name="kAnswer",
            kind="variable",
            is_definition=False,
        )
    )
    got = db.lookup_symbol("c:@kAnswer")
    assert got is not None
    assert got.const_value == "42"
    db.close()


def test_graph_query_exposes_const_value(tmp_path):
    path = str(tmp_path / "query.db")
    db = Storage(path)
    db.add_symbol(
        Symbol(
            usr="c:@kMax",
            spelling="kMax",
            qual_name="kMax",
            kind="variable",
            type_info="const int",
            is_definition=True,
            const_value="1024",
        )
    )
    db.add_symbol(
        Symbol(
            usr="c:@F@runtime#",
            spelling="runtime",
            qual_name="runtime()",
            kind="function",
            is_definition=True,
        )
    )
    db.close()

    g = GraphQuery(path)
    sym = g.find("kMax")[0]
    assert sym.const_value == "1024"
    # to_dict key order stays identical-by-spec to the C++ port: const_value
    # sits right after type_info.
    keys = list(sym.to_dict().keys())
    assert keys.index("const_value") == keys.index("type_info") + 1
    assert g.find("runtime()")[0].const_value is None
    g.close()
