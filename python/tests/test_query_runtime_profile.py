"""GraphQuery's read-only SQLite runtime contract."""

import sqlite3
from pathlib import Path

import pytest

from indexer.query import GraphQuery, NoIndexError
from indexer.storage import CATALOG_HASH


def _facts(path: Path) -> dict[str, tuple[int, int]]:
    result = {}
    for suffix in ("", "-wal", "-shm", "-journal"):
        candidate = Path(str(path) + suffix)
        if candidate.exists():
            stat = candidate.stat()
            result[suffix or "database"] = (stat.st_size, stat.st_mtime_ns)
    return result


def test_graph_query_uses_read_only_profile_without_side_effects(tmp_path):
    path = tmp_path / "index.db"
    writer = sqlite3.connect(path)
    writer.executescript(
        "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE symbol(id INTEGER PRIMARY KEY, spelling TEXT);"
        "INSERT INTO meta VALUES('schema_version', '34');"
        f"INSERT INTO meta VALUES('catalog_hash', '{CATALOG_HASH}');"
        "INSERT INTO symbol VALUES(1, 'before');"
        "PRAGMA journal_mode=WAL;"
    )
    writer.commit()
    writer.execute("INSERT INTO symbol VALUES(2, 'wal-row')")
    writer.commit()
    before = _facts(path)
    assert "-wal" in before
    assert "-shm" in before

    with GraphQuery(str(path)) as graph:
        assert graph._c.execute("PRAGMA foreign_keys").fetchone()[0] == 1
        assert graph._c.execute("PRAGMA query_only").fetchone()[0] == 1
        assert graph._c.execute("PRAGMA busy_timeout").fetchone()[0] == 5000
        with pytest.raises(sqlite3.OperationalError):
            graph._c.execute("CREATE TABLE should_not_exist(id INTEGER)")

    assert _facts(path) == before
    writer.close()


def test_graph_query_never_creates_missing_database(tmp_path):
    with pytest.raises(NoIndexError):
        GraphQuery(str(tmp_path / "missing" / "index.db"))
