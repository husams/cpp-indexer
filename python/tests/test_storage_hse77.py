import sqlite3

import pytest

from indexer.storage import (
    PREVIOUS_CATALOG_HASH,
    PREVIOUS_SCHEMA_VERSION,
    SCHEMA_VERSION,
    Storage,
    Symbol,
)
from benchmarks.storage_m1.hse77_normalization import decide_hot_cold


def test_hse77_v34_migration_backfills_source_and_preserves_reads(tmp_path):
    path = str(tmp_path / "v34.db")
    db = Storage(path)
    component = db.add_component("c", "/repo/c")
    directory = db.add_directory(component, "")
    file_id = db.add_file(directory, "c.cpp")
    caller_id = db.add_symbol(Symbol("legacy:caller", "caller", "function"))
    callee_id = db.add_symbol(Symbol("legacy:callee", "callee", "function"))
    edge_id = db.add_edge(caller_id, callee_id, 1)
    db._conn.execute(
        "INSERT INTO edge_site(edge_id,file_id,line,col,conditional,args_sig,"
        "recv_src_kind,recv_type_usr,recv_decl_usr,recv_param_pos) "
        "VALUES (?,?,?,?,?,?,?,?,?,?)",
        (edge_id, file_id, 10, 2, 0, "legacy", "local", "legacy:type", "legacy:callee", 0),
    )
    db._conn.execute(
        "INSERT INTO call_arg(edge_id,file_id,line,col,position,src_kind,type_usr,decl_usr,callee_usr) "
        "VALUES (?,?,?,?,?,?,?,?,?)",
        (edge_id, file_id, 11, 4, 0, "local", "legacy:missing-type", "legacy:missing-decl", "legacy:callee"),
    )
    db._conn.execute("UPDATE meta SET value='34' WHERE key='schema_version'")
    db._conn.commit()
    db.close()

    db = Storage(path)
    edge = db._conn.execute(
        "SELECT recv_src_kind, recv_src_kind_id, recv_type_usr, recv_decl_usr, recv_decl_id "
        "FROM edge_site"
    ).fetchone()
    assert tuple(edge) == (None, 2, None, None, callee_id)
    arg = db._conn.execute(
        "SELECT src_kind, src_kind_id, type_usr, decl_usr, callee_usr, callee_id FROM call_arg"
    ).fetchone()
    assert tuple(arg) == (None, 2, None, None, None, callee_id)
    assert tuple(db._conn.execute(
        "SELECT recv_src_kind, recv_type_usr, recv_decl_usr FROM edge_site_read"
    ).fetchone()) == ("local", "legacy:type", "legacy:callee")
    assert tuple(db._conn.execute(
        "SELECT src_kind, type_usr, decl_usr, callee_usr FROM call_arg_read"
    ).fetchone()) == ("local", "legacy:missing-type", "legacy:missing-decl", "legacy:callee")
    assert db._conn.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()[0] == "41"
    db.close()


def test_hse77_normalization_is_order_independent():
    db = Storage(":memory:")
    component = db.add_component("c", "/repo/c")
    directory = db.add_directory(component, "")
    file_id = db.add_file(directory, "c.cpp")
    caller_id = db.add_symbol(Symbol("future:caller", "caller", "function"))
    target_id = db.add_symbol(Symbol("future:target", "target", "function"))
    edge_id = db.add_edge(caller_id, target_id, 1)

    db.add_edge_site(
        edge_id,
        file_id,
        10,
        1,
        recv_type_usr="future:type",
        recv_decl_usr="future:symbol",
    )
    db.add_call_arg(
        edge_id,
        file_id,
        10,
        1,
        0,
        "local",
        type_usr="future:type",
        decl_usr="future:symbol",
        callee_usr="future:callee",
    )

    symbol_id = db.add_symbol(Symbol("future:symbol", "symbol", "struct"))
    callee_id = db.add_symbol(Symbol("future:callee", "callee", "function"))
    type_id = db._conn.execute(
        "INSERT INTO type_node(type_key, spelling, kind, decl_usr) VALUES (?, ?, ?, ?) RETURNING id",
        ("record:future:type", "Future", 2, "future:type"),
    ).fetchone()[0]
    db._reconcile_external_identities()

    edge = db._conn.execute(
        "SELECT recv_src_kind, recv_src_kind_id, recv_type_id, recv_decl_id FROM edge_site"
    ).fetchone()
    assert edge[0] is None
    assert edge[1] is None
    assert edge[2] == type_id
    assert edge[3] == symbol_id
    arg = db._conn.execute(
        "SELECT src_kind, src_kind_id, type_id, decl_id, callee_id FROM call_arg"
    ).fetchone()
    assert arg[0] is None
    assert (arg[1], arg[2], arg[3], arg[4]) == (2, type_id, symbol_id, callee_id)
    assert tuple(db._conn.execute(
        "SELECT recv_src_kind, recv_type_usr, recv_decl_usr FROM edge_site_read"
    ).fetchone()) == (None, "future:type", "future:symbol")
    assert tuple(db._conn.execute(
        "SELECT src_kind, type_usr, decl_usr, callee_usr FROM call_arg_read"
    ).fetchone()) == ("local", "future:type", "future:symbol", "future:callee")
    assert db._conn.execute(
        "SELECT COUNT(*) FROM external_identity WHERE resolution_status = 0"
    ).fetchone()[0] == 0
    db.close()


def test_parent_id_backfills_when_parent_arrives_after_child():
    db = Storage(":memory:")
    child_id = db.add_symbol(
        Symbol("late:child", "Child", "struct", parent_usr="late:parent")
    )
    assert db._conn.execute(
        "SELECT parent_id FROM symbol WHERE id=?", (child_id,)
    ).fetchone()[0] is None

    parent_id = db.add_symbol(Symbol("late:parent", "Parent", "struct"))
    assert db._conn.execute(
        "SELECT parent_id FROM symbol WHERE id=?", (child_id,)
    ).fetchone()[0] == parent_id

    db.add_symbol(Symbol("late:parent", "Parent", "struct"))
    assert db._conn.execute(
        "SELECT parent_id FROM symbol WHERE id=?", (child_id,)
    ).fetchone()[0] == parent_id
    db.close()


def test_migrated_enum_columns_keep_domain_checks(tmp_path):
    path = str(tmp_path / "predecessor.db")
    db = Storage(path)
    component = db.add_component("c", "/repo/c")
    directory = db.add_directory(component, "")
    file_id = db.add_file(directory, "c.cpp")
    caller_id = db.add_symbol(Symbol("domain:caller", "caller", "function"))
    callee_id = db.add_symbol(Symbol("domain:callee", "callee", "function"))
    edge_id = db.add_edge(caller_id, callee_id, 1)
    db._conn.execute(
        "UPDATE meta SET value=? WHERE key='schema_version'",
        (str(PREVIOUS_SCHEMA_VERSION),),
    )
    db._conn.execute(
        "UPDATE meta SET value=? WHERE key='catalog_hash'",
        (PREVIOUS_CATALOG_HASH,),
    )
    db._conn.commit()
    db.close()
    db = Storage(path)
    with pytest.raises(sqlite3.IntegrityError):
        db._conn.execute(
            "INSERT INTO edge_site(edge_id,file_id,line,col,recv_src_kind_id) "
            "VALUES(?,?,?,?,99)",
            (edge_id, file_id, 10, 1),
        )
    with pytest.raises(sqlite3.IntegrityError):
        db._conn.execute(
            "INSERT INTO call_arg(edge_id,file_id,line,col,position,src_kind_id) "
            "VALUES(?,?,?,?,?,99)",
            (edge_id, file_id, 11, 1, 0),
        )
    db.close()


@pytest.mark.parametrize(
    "wrong_schema_version",
    [PREVIOUS_SCHEMA_VERSION - 1, SCHEMA_VERSION],
)
def test_predecessor_catalog_hash_requires_the_predecessor_version(
    tmp_path, wrong_schema_version
):
    path = str(tmp_path / f"wrong-v{wrong_schema_version}.db")
    db = Storage(path)
    db._conn.execute(
        "UPDATE meta SET value=? WHERE key='schema_version'",
        (str(wrong_schema_version),),
    )
    db._conn.execute(
        "UPDATE meta SET value=? WHERE key='catalog_hash'",
        (PREVIOUS_CATALOG_HASH,),
    )
    db._conn.commit()
    db.close()

    with pytest.raises(RuntimeError, match="requires schema_version"):
        Storage(path)


def test_current_main_predecessor_database_upgrades_to_the_candidate_schema(
    tmp_path,
):
    """v40 -> v41 normalises a containment count that used to accumulate.

    The count is now the number of (file, configuration) routes that declare
    into the edge, derived from the applicability rows, so re-indexing a file
    can no longer raise it. An existing database cannot recover the per-route
    split of an accumulated total, so the migration re-derives it from the
    routes the database already records.
    """
    path = str(tmp_path / "main-predecessor.db")
    db = Storage(path)
    component = db.add_component("c", "/repo/c")
    directory = db.add_directory(component, "")
    file_ids = [db.add_file(directory, name) for name in ("a.cpp", "b.cpp")]
    config_id = db._conn.execute(
        "SELECT id FROM translation_unit_config LIMIT 1"
    ).fetchone()
    before_id = db.add_symbol(Symbol("main-predecessor:before", "before", "function"))
    scope_id = db.add_symbol(Symbol("main-predecessor:scope", "scope", "namespace"))
    member_id = db.add_symbol(Symbol("main-predecessor:member", "member", "function"))
    edge_id = db.add_edge(scope_id, member_id, 3)
    db._conn.execute("UPDATE edge SET count=5 WHERE id=?", (edge_id,))
    # Foreign keys are off for the fixture only: the migration under test is a
    # data normalisation, and building a fully valid configuration row here
    # would test the schema rather than the migration.
    db._conn.commit()  # PRAGMA foreign_keys is a no-op inside a transaction
    db._conn.execute("PRAGMA foreign_keys = OFF")
    config = config_id[0] if config_id is not None else 1
    for file_id in file_ids:
        db._conn.execute(
            "INSERT INTO fact_applicability(fact_kind, fact_id, file_id, "
            "config_id, generation) VALUES('edge', ?, ?, ?, 1)",
            (edge_id, file_id, config),
        )
    for kind in ("entity_node", "entity_edge"):
        db._conn.execute(
            "INSERT INTO fact_applicability(fact_kind, fact_id, file_id, "
            "config_id, generation) VALUES(?, ?, ?, ?, 1)",
            (kind, edge_id, file_ids[0], config),
        )
    db._conn.execute(
        "UPDATE meta SET value=? WHERE key='schema_version'",
        (str(PREVIOUS_SCHEMA_VERSION),),
    )
    db._conn.execute(
        "UPDATE meta SET value=? WHERE key='catalog_hash'",
        (PREVIOUS_CATALOG_HASH,),
    )
    db._conn.commit()
    db.close()

    db = Storage(path)
    assert db._conn.execute(
        "SELECT value FROM meta WHERE key='schema_version'"
    ).fetchone()[0] == str(SCHEMA_VERSION)
    assert db._conn.execute(
        "SELECT usr FROM symbol WHERE id=?", (before_id,)
    ).fetchone()[0] == "main-predecessor:before"
    # Re-derived from the two routes that actually declare into it.
    assert db._conn.execute(
        "SELECT count FROM edge WHERE id=?", (edge_id,)
    ).fetchone()[0] == 2
    # The write-only derived rows are gone; the edge's own routes are not.
    assert db._conn.execute(
        "SELECT COUNT(*) FROM fact_applicability WHERE fact_kind IN "
        "('entity_node', 'entity_edge')"
    ).fetchone()[0] == 0
    assert db._conn.execute(
        "SELECT COUNT(*) FROM fact_applicability WHERE fact_kind='edge' "
        "AND fact_id=?",
        (edge_id,),
    ).fetchone()[0] == 2
    db.close()


def test_hot_cold_decision_uses_explicit_thresholds():
    decision = decide_hot_cold(
        {
            "unsplit": {"bytes": 1000, "lookup_ms_median": 10.0},
            "split": {"bytes": 1100, "lookup_ms_median": 9.0},
        }
    )
    assert decision["decision"] == "retain symbol attributes in one hot table"
    assert decision["thresholds"]["max_split_byte_overhead_ratio"] == 0.0
    assert decision["thresholds"]["min_split_latency_improvement_ratio"] == 0.05
    assert decision["derived"]["split_byte_overhead_ratio"] == pytest.approx(0.1)
    assert decision["derived"]["split_latency_improvement_ratio"] == pytest.approx(0.1)
    assert "byte overhead" in decision["reason"]
