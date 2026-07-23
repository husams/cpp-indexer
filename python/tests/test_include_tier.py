"""v31 include tier: schema, migration, and Python read queries.

Extraction is C++-only (LibTooling PPCallbacks), so these tests write rows
directly and exercise the storage/read-query half that Python owns. The C++
side's executable spec is tests/include_hygiene_test.cpp.
"""

import sqlite3

from indexer.storage import SCHEMA_VERSION, Storage


def _fresh(tmp_path):
    db = Storage(str(tmp_path / "index.db"))
    db.add_component("c", str(tmp_path / "proj"))
    main = db.add_file_path(str(tmp_path / "proj" / "main.cpp"))
    util = db.add_file_path(str(tmp_path / "proj" / "util.hpp"))
    return db, main, util


def _add_config(db, tu_file_id, digest="d0"):
    cur = db._conn.execute(
        "INSERT INTO include_config (tu_file_id, digest, driver, working_dir, "
        "arguments, lang_mode) VALUES (?, ?, 'c++', '.', '[\"-std=c++23\"]', 'c++') "
        "RETURNING id",
        (tu_file_id, digest),
    )
    return cur.fetchone()[0]


def _add_edge(db, src, dst_path, config_id, dst_file_id=None, is_system=0, count=1):
    cur = db._conn.execute(
        "INSERT INTO include_edge (src_file_id, dst_file_id, dst_path, "
        "config_id, is_system, count) VALUES (?, ?, ?, ?, ?, ?) RETURNING id",
        (src, dst_file_id, dst_path, config_id, is_system, count),
    )
    return cur.fetchone()[0]


def test_schema_version_is_32():
    assert SCHEMA_VERSION == 35


def test_fresh_schema_has_the_include_tier(tmp_path):
    db, _, _ = _fresh(tmp_path)
    tables = {
        r[0]
        for r in db._conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        ).fetchall()
    }
    assert {
        "include_config",
        "include_edge",
        "include_site",
        "include_macro_use",
        "include_directive_kind",
    } <= tables

    # The directive seed rows must match the C++ constants in records.hpp.
    kinds = dict(
        db._conn.execute("SELECT id, name FROM include_directive_kind").fetchall()
    )
    assert kinds == {
        1: "include",
        2: "include_next",
        3: "import",
        4: "include_macros",
        5: "unknown",
    }


def test_v30_to_v31_migration_creates_tables_and_stamps(tmp_path):
    """A v30 DB reopened by a v31 build gains the tier and the version stamp.

    Include facts are NOT backfillable -- only a C++ reindex populates them --
    so the upgraded DB starts with an empty include graph.
    """
    path = str(tmp_path / "v30.db")
    db = Storage(path)
    db.add_component("c", "/data/c")
    del db

    raw = sqlite3.connect(path)
    for t in (
        "include_macro_use",
        "include_site",
        "include_directive_kind",
        "include_edge",
        "include_config",
    ):
        raw.execute(f"DROP TABLE {t}")
    raw.execute("UPDATE meta SET value = '30' WHERE key = 'schema_version'")
    raw.commit()
    raw.close()

    db2 = Storage(path)  # migration runs here
    assert db2.get_component_by_name("c") is not None  # old data intact
    assert not db2.include_graph_populated()  # no backfill is possible
    ver = db2._conn.execute(
        "SELECT value FROM meta WHERE key = 'schema_version'"
    ).fetchone()[0]
    assert ver == "35"


def test_include_graph_populated_distinguishes_empty_from_absent(tmp_path):
    db, main, util = _fresh(tmp_path)
    assert not db.include_graph_populated()
    cfg = _add_config(db, main)
    _add_edge(db, main, "/x/util.hpp", cfg, dst_file_id=util)
    assert db.include_graph_populated()


def test_edges_from_and_to(tmp_path):
    db, main, util = _fresh(tmp_path)
    cfg = _add_config(db, main)
    _add_edge(db, main, "/x/util.hpp", cfg, dst_file_id=util)
    _add_edge(db, main, "/usr/include/string", cfg, is_system=1)

    # System targets are filtered by default: their internals are never indexed.
    out = db.include_edges_from(main)
    assert [e.dst_path for e in out] == ["/x/util.hpp"]

    out_sys = db.include_edges_from(main, include_system=True)
    assert [e.dst_path for e in out_sys] == ["/usr/include/string", "/x/util.hpp"]

    into = db.include_edges_to(util)
    assert [e.src_file_id for e in into] == [main]


def test_edge_is_collapsed_and_sites_ordered_by_offset(tmp_path):
    db, main, util = _fresh(tmp_path)
    cfg = _add_config(db, main)
    eid = _add_edge(db, main, "/x/util.hpp", cfg, dst_file_id=util, count=2)
    for off, line in ((42, 3), (0, 1)):  # inserted out of order on purpose
        db._conn.execute(
            "INSERT INTO include_site (edge_id, line, col, begin_offset, "
            "end_offset, spelling, is_angled, guarded) "
            "VALUES (?, ?, 1, ?, ?, 'util.hpp', 0, 1)",
            (eid, line, off, off + 20),
        )
    db._conn.commit()

    sites = db.include_sites_for(eid)
    assert [s.begin_offset for s in sites] == [0, 42]  # by offset, not insertion
    assert all(s.guarded == 1 for s in sites)


def test_macro_uses_are_readable(tmp_path):
    db, main, _ = _fresh(tmp_path)
    cfg = _add_config(db, main)
    for name in ("ZETA", "ALPHA"):
        db._conn.execute(
            "INSERT INTO include_macro_use (src_file_id, def_path, name, "
            "config_id, count) VALUES (?, '/x/macros.hpp', ?, ?, 1)",
            (main, name, cfg),
        )
    db._conn.commit()

    assert db.include_macro_uses(main, "/x/macros.hpp") == ["ALPHA", "ZETA"]
    assert db.include_macro_uses(main, "/x/other.hpp") == []


def test_configs_for_tu(tmp_path):
    db, main, _ = _fresh(tmp_path)
    _add_config(db, main, digest="zzz")
    _add_config(db, main, digest="aaa")
    configs = db.include_configs_for_tu(main)
    assert [c.digest for c in configs] == ["aaa", "zzz"]  # ordered by digest
    assert configs[0].lang_mode == "c++"


def test_deleting_a_config_cascades_to_its_edges(tmp_path):
    db, main, util = _fresh(tmp_path)
    cfg = _add_config(db, main)
    _add_edge(db, main, "/x/util.hpp", cfg, dst_file_id=util)
    db._conn.execute("DELETE FROM include_config WHERE id = ?", (cfg,))
    db._conn.commit()
    assert not db.include_graph_populated()


def test_deleting_an_edge_cascades_to_its_sites(tmp_path):
    db, main, util = _fresh(tmp_path)
    cfg = _add_config(db, main)
    eid = _add_edge(db, main, "/x/util.hpp", cfg, dst_file_id=util)
    db._conn.execute(
        "INSERT INTO include_site (edge_id, line, col, begin_offset, "
        "end_offset, spelling) VALUES (?, 1, 1, 0, 20, 'util.hpp')",
        (eid,),
    )
    db._conn.commit()
    db._conn.execute("DELETE FROM include_edge WHERE id = ?", (eid,))
    db._conn.commit()
    assert db._conn.execute("SELECT COUNT(*) FROM include_site").fetchone()[0] == 0
