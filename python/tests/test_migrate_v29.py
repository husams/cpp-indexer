"""v28 -> v29 template_arg.arg_kind migration (mirrors the C++
storage_migration_test case; docs/improvements/template-arg-contract.md).

A v28 database written by the retired class-spec extraction path stored raw
CXTemplateArgumentKind values (Pack=8, Template=5, TemplateExpansion=6,
Expression=7, NullPtr=3, Null=0). Opening the DB must remap them to the
canonical codes exactly once, disambiguating the overloaded 3 by owner kind.
"""

import sqlite3

import pytest

from indexer.storage import SCHEMA_VERSION, Storage


def _make_v28(path: str) -> None:
    db = Storage(path)
    db.close()
    conn = sqlite3.connect(path)
    conn.execute(
        "INSERT INTO symbol (id, usr, spelling, kind) VALUES "
        "(1, 'c:@S@Spec', 'Spec', 2),"  # struct owner (class-spec path)
        "(2, 'c:@F@fn', 'fn', 8)"  # function owner (callable path)
    )
    conn.execute(
        "INSERT INTO template_arg (owner_id, position, arg_kind) VALUES "
        "(1, 0, 8),"  # legacy Pack          -> 4
        "(1, 1, 5),"  # legacy Template      -> 3
        "(1, 2, 6),"  # legacy TmplExpansion -> 3
        "(1, 3, 7),"  # legacy Expression    -> 2
        "(1, 4, 3),"  # legacy NullPtr       -> 2 (record owner)
        "(1, 5, 0),"  # legacy Null          -> row deleted
        "(1, 6, 1),"  # in-contract type     -> unchanged
        "(2, 0, 4),"  # callable pack        -> unchanged
        "(2, 1, 3)"  # callable tmpl-tmpl    -> unchanged (NOT NullPtr)
    )
    conn.execute("UPDATE meta SET value = '28' WHERE key = 'schema_version'")
    conn.commit()
    conn.close()


def _kinds(path: str) -> dict[tuple[int, int], int]:
    conn = sqlite3.connect(path)
    rows = conn.execute(
        "SELECT owner_id, position, arg_kind FROM template_arg"
    ).fetchall()
    conn.close()
    return {(o, p): k for o, p, k in rows}


def test_v28_to_v29_remaps_arg_kind(tmp_path):
    path = str(tmp_path / "v28.db")
    _make_v28(path)

    Storage(path).close()  # migration runs on open

    conn = sqlite3.connect(path)
    ver = conn.execute(
        "SELECT value FROM meta WHERE key = 'schema_version'"
    ).fetchone()[0]
    conn.close()
    assert ver == str(SCHEMA_VERSION)  # migrated DB is stamped current

    kinds = _kinds(path)
    assert kinds[(1, 0)] == 4
    assert kinds[(1, 1)] == 3
    assert kinds[(1, 2)] == 3
    assert kinds[(1, 3)] == 2
    assert kinds[(1, 4)] == 2
    assert (1, 5) not in kinds  # Null row deleted
    assert kinds[(1, 6)] == 1
    assert kinds[(2, 0)] == 4
    assert kinds[(2, 1)] == 3


def test_v29_migration_runs_exactly_once(tmp_path):
    path = str(tmp_path / "v28.db")
    _make_v28(path)
    Storage(path).close()
    # Second open: DB is stamped 29; the valid template-template rows the
    # first pass produced must not remap again.
    Storage(path).close()
    kinds = _kinds(path)
    assert kinds[(1, 1)] == 3
    assert kinds[(2, 1)] == 3


def test_v28_migration_rolls_back_if_schema_installation_fails(tmp_path):
    path = str(tmp_path / "v28-atomic.db")
    _make_v28(path)
    conn = sqlite3.connect(path)
    conn.execute("DROP INDEX idx_artifact_state")
    conn.execute("CREATE TABLE idx_artifact_state (value INTEGER)")
    conn.commit()
    conn.close()

    with pytest.raises(sqlite3.OperationalError, match="idx_artifact_state"):
        Storage(path)

    conn = sqlite3.connect(path)
    version = conn.execute(
        "SELECT value FROM meta WHERE key = 'schema_version'"
    ).fetchone()[0]
    arg_kind = conn.execute(
        "SELECT arg_kind FROM template_arg WHERE owner_id = 1 AND position = 0"
    ).fetchone()[0]
    conn.close()
    assert version == "28"
    assert arg_kind == 8


def test_large_v28_template_arg_remap_preserves_canonical_kinds(tmp_path):
    path = str(tmp_path / "v28-large.db")
    _make_v28(path)
    conn = sqlite3.connect(path)
    conn.executemany(
        "INSERT INTO template_arg "
        "(owner_id, position, pack_index, arg_kind) VALUES (1, 20, ?, ?)",
        ((pack_index, 5 + pack_index % 4) for pack_index in range(25_000)),
    )
    conn.commit()
    conn.close()

    Storage(path).close()

    conn = sqlite3.connect(path)
    counts = dict(
        conn.execute(
            "SELECT arg_kind, COUNT(*) FROM template_arg "
            "WHERE owner_id = 1 AND position = 20 GROUP BY arg_kind"
        )
    )
    conn.close()
    assert counts == {2: 6_250, 3: 12_500, 4: 6_250}
