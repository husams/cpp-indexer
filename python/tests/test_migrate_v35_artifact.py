"""Manifest-governed sidecar schema and read-side compatibility coverage."""

from indexer.storage import SCHEMA_VERSION, Storage


def test_fresh_schema_has_artifact_manifest_and_stable_mapping_readers():
    db = Storage(":memory:")
    db._conn.execute(
        "INSERT INTO artifact("
        "logical_id, kind, artifact_schema, catalog_version, catalog_hash, producer_version, "
        "engine_version, workspace_identity, completeness, truncation, trust, evidence, "
        "attachment_name, relative_path, content_hash, byte_size, state) "
        "VALUES ('tu:1:astgraph', 'astgraph', 'cidx-artifact/v1', "
        "1, 'catalog', 'producer/1', 'engine/1', 'workspace:1', "
        "'complete', 'none', 'producer-verified', 'source', 'astgraph', 'artifacts/a.db', 'hash', 4, 'current')"
    )
    artifact_id = db._conn.execute("SELECT last_insert_rowid()").fetchone()[0]
    db._conn.execute(
        "INSERT INTO artifact_relation(artifact_id, relation_name) VALUES (?, ?)",
        (artifact_id, "node"),
    )
    db._conn.execute(
        "INSERT INTO artifact_identity_map(artifact_id, local_identity, identity_kind, "
        "stable_identity, resolution_state, diagnostic) VALUES (?, ?, ?, ?, ?, ?)",
        (artifact_id, "7", "symbol", "usr:node", "unresolved", "missing core symbol"),
    )
    db._conn.commit()

    row = db.current_artifact("tu:1:astgraph")
    assert row is not None
    assert row["logical_id"] == "tu:1:astgraph"
    assert row["exposed_relations"] == ["node"]
    assert db.artifact_identity_mappings("tu:1:astgraph")[0]["resolution_state"] == "unresolved"


def test_v34_database_gets_artifact_tables_and_version_bump(tmp_path):
    path = tmp_path / "v34.db"
    db = Storage(str(path))
    db._conn.execute("PRAGMA foreign_keys = OFF")
    for table in (
        "artifact_pin",
        "artifact_lease",
        "artifact_identity_map",
        "artifact_relation",
        "artifact",
    ):
        db._conn.execute(f"DROP TABLE {table}")
    db._conn.execute(
        "UPDATE meta SET value = '34' WHERE key = 'schema_version'"
    )
    db._conn.commit()
    db.close()

    migrated = Storage(str(path))
    assert SCHEMA_VERSION == 36
    assert migrated._conn.execute(
        "SELECT value FROM meta WHERE key = 'schema_version'"
    ).fetchone()[0] == "36"
    tables = {
        row[0]
        for row in migrated._conn.execute(
            "SELECT name FROM sqlite_master WHERE type = 'table'"
        )
    }
    assert {"artifact", "artifact_relation", "artifact_identity_map", "artifact_lease", "artifact_pin"} <= tables


def test_v35_artifact_contract_is_rebuilt(tmp_path):
    path = tmp_path / "v35.db"
    db = Storage(str(path))
    db._conn.execute("PRAGMA foreign_keys = OFF")
    for table in (
        "artifact_pin",
        "artifact_lease",
        "artifact_identity_map",
        "artifact_relation",
        "artifact",
    ):
        db._conn.execute(f"DROP TABLE {table}")
    db._conn.executescript(
        """
        CREATE TABLE artifact (
            id INTEGER PRIMARY KEY, logical_id TEXT NOT NULL, kind TEXT NOT NULL,
            artifact_schema TEXT NOT NULL, catalog_version TEXT NOT NULL,
            producer_version TEXT NOT NULL, engine_version TEXT NOT NULL,
            workspace_identity TEXT NOT NULL, tu_identity TEXT NOT NULL DEFAULT '',
            configuration_identity TEXT NOT NULL DEFAULT '',
            input_fact_set_identity TEXT NOT NULL DEFAULT '', completeness TEXT NOT NULL,
            truncation TEXT NOT NULL, trust TEXT NOT NULL, attachment_name TEXT NOT NULL,
            retention_policy TEXT NOT NULL DEFAULT 'retain', relative_path TEXT NOT NULL,
            content_hash TEXT NOT NULL, byte_size INTEGER NOT NULL, state TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, published_at TEXT,
            UNIQUE(logical_id, content_hash)
        );
        CREATE TABLE artifact_relation (artifact_id INTEGER NOT NULL, relation_name TEXT NOT NULL, PRIMARY KEY (artifact_id, relation_name));
        CREATE TABLE artifact_identity_map (artifact_id INTEGER NOT NULL, local_identity TEXT NOT NULL, identity_kind TEXT NOT NULL, stable_identity TEXT NOT NULL, resolution_state TEXT NOT NULL, core_symbol_id INTEGER, diagnostic TEXT NOT NULL DEFAULT '', PRIMARY KEY (artifact_id, local_identity, identity_kind));
        CREATE TABLE artifact_lease (artifact_id INTEGER NOT NULL, lease_id TEXT NOT NULL, purpose TEXT NOT NULL, PRIMARY KEY (artifact_id, lease_id));
        CREATE TABLE artifact_pin (artifact_id INTEGER NOT NULL, pin_id TEXT NOT NULL, reason TEXT NOT NULL, PRIMARY KEY (artifact_id, pin_id));
        INSERT INTO artifact(logical_id, kind, artifact_schema, catalog_version, producer_version, engine_version, workspace_identity, completeness, truncation, trust, attachment_name, relative_path, content_hash, byte_size, state)
        VALUES ('legacy', 'astgraph', 'cidx-artifact/v1', 'semantic-catalog/v1', 'p', 'e', 'workspace:legacy', 'complete', 'none', 'trusted', 'legacy', 'artifacts/a.db', 'hash', 1, 'current');
        """
    )
    db._conn.execute("UPDATE meta SET value = '35' WHERE key = 'schema_version'")
    db._conn.commit()
    db.close()

    migrated = Storage(str(path))
    row = migrated._conn.execute(
        "SELECT catalog_version, catalog_hash, trust, evidence FROM artifact"
    ).fetchone()
    assert tuple(row) == (0, "", "unverified", "assumption")
    state_hash = migrated._conn.execute(
        "SELECT state, content_hash FROM artifact"
    ).fetchone()
    assert tuple(state_hash) == ("stale", "legacy-sha1:hash")
