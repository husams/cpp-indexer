from __future__ import annotations

import copy
import importlib.util
import json
import sqlite3
from pathlib import Path

import pytest

from indexer.generated_catalog import CATALOG_HASH, CATALOG_SEED_SQL, RELATION_METADATA
from indexer.generated_extensions import EXTENSION_RELATIONS
from indexer.query import GraphQuery
from indexer.queryplan import extension_relation_catalog, extension_relation_metadata
from indexer.souffle import SouffleError, _open_ro
from indexer.storage import Storage


ROOT = Path(__file__).parents[2]


def _generator_module():
    path = ROOT / "scripts/generate_catalogs.py"
    spec = importlib.util.spec_from_file_location("generate_catalogs", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_extension_manifest_reaches_generated_consumers():
    relation = EXTENSION_RELATIONS["test.extension/relation/taints"]
    assert relation["source"] == "symbol.declaration"
    assert relation["target"] == "symbol.declaration"
    assert relation["traversal"] == ["out", "in"]
    assert relation["evidence_capabilities"] == ["derived", "proof"]
    assert RELATION_METADATA[("calls", "symbol", 1)]["source"] == "symbol.callable"
    assert extension_relation_catalog()[0][0] == "test.extension/relation/taints"
    assert extension_relation_metadata("test.extension/relation/taints") == relation
    assert 'extension:test.extension/relation/taints' in CATALOG_SEED_SQL

    souffle = (ROOT / "src/astgraph/rules/catalog_generated.dl").read_text()
    assert 'cidx_symbol_kind(2, "struct").' in souffle
    assert 'cidx_relation(1, "symbol", "calls", "symbol.callable", "symbol.callable"' in souffle
    assert 'cidx_extension_relation("test.extension/relation/taints"' in souffle


def test_catalog_compatibility_preserves_tombstones_and_rejects_reuse(tmp_path):
    generator = _generator_module()
    data, _ = generator.load()
    generator.LOCK = tmp_path / "compatibility.json"
    generator.LOCK.write_text(json.dumps({
        "catalog_version": 1,
        "entries": {"symbol_kinds": {"2": "struct"}},
        "tombstones": {},
    }))

    deleted = copy.deepcopy(data)
    deleted["symbol_kinds"] = [row for row in deleted["symbol_kinds"] if row["id"] != 2]
    with pytest.raises(SystemExit, match="tombstone"):
        generator.compatibility_check(deleted)

    deleted["migrations"] = [{
        "catalog": "symbol_kinds", "id": 2, "from": "struct", "to": None,
        "action": "tombstone",
    }]
    locked = generator.compatibility_check(deleted)
    assert locked["tombstones"]["symbol_kinds"]["2"] == "struct"
    generator.LOCK.write_text(json.dumps(locked))

    reused = copy.deepcopy(deleted)
    reused["symbol_kinds"].append({"id": 2, "name": "replacement"})
    with pytest.raises(SystemExit, match="reuse"):
        generator.compatibility_check(reused)
    reused["migrations"].append({
        "catalog": "symbol_kinds", "id": 2, "from": "struct", "to": "replacement",
        "action": "reuse",
    })
    assert generator.compatibility_check(reused)["tombstones"]["symbol_kinds"] == {}


def _meta_db(path: Path, catalog_hash: str | None) -> None:
    conn = sqlite3.connect(path)
    conn.execute("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT)")
    conn.execute("INSERT INTO meta VALUES ('schema_version', '34')")
    if catalog_hash is not None:
        conn.execute("INSERT INTO meta VALUES ('catalog_hash', ?)", (catalog_hash,))
    conn.commit()
    conn.close()


def test_catalog_hash_is_checked_before_writes_and_on_all_readers(tmp_path):
    path = tmp_path / "bad.db"
    _meta_db(path, "wrong")
    with pytest.raises(RuntimeError, match="catalog_hash"):
        Storage(str(path))
    conn = sqlite3.connect(path)
    assert conn.execute("SELECT value FROM meta WHERE key = 'catalog_hash'").fetchone()[0] == "wrong"
    conn.close()

    for factory in (Storage.from_connection, GraphQuery.from_connection):
        missing = sqlite3.connect(":memory:")
        missing.execute("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT)")
        with pytest.raises(RuntimeError, match="missing"):
            factory(missing)
        missing.close()

    for catalog_hash in (None, "wrong"):
        ro_path = tmp_path / ("missing.db" if catalog_hash is None else "wrong-ro.db")
        _meta_db(ro_path, catalog_hash)
        with pytest.raises(SouffleError, match="catalog_hash"):
            _open_ro(str(ro_path))
