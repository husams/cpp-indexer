"""Manifest-governed sidecar schema and read-side compatibility coverage."""

from indexer.storage import SCHEMA_VERSION, Storage


def test_fresh_schema_has_artifact_manifest_and_stable_mapping_readers():
    db = Storage(":memory:")
    db._conn.execute(
        "INSERT INTO artifact("
        "logical_id, kind, artifact_schema, catalog_version, producer_version, "
        "engine_version, workspace_identity, completeness, truncation, trust, "
        "attachment_name, relative_path, content_hash, byte_size, state) "
        "VALUES ('tu:1:astgraph', 'astgraph', 'cidx-artifact/v1', "
        "'semantic-catalog/v1', 'producer/1', 'engine/1', 'workspace:1', "
        "'complete', 'none', 'trusted', 'astgraph', 'artifacts/a.db', 'hash', 4, 'current')"
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
    assert SCHEMA_VERSION == 35
    assert migrated._conn.execute(
        "SELECT value FROM meta WHERE key = 'schema_version'"
    ).fetchone()[0] == "35"
    tables = {
        row[0]
        for row in migrated._conn.execute(
            "SELECT name FROM sqlite_master WHERE type = 'table'"
        )
    }
    assert {"artifact", "artifact_relation", "artifact_identity_map", "artifact_lease", "artifact_pin"} <= tables
