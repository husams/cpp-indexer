"""v31 -> v32 preserves signature foreign-key values and adds constraints."""

import sqlite3

from indexer.storage import SCHEMA_VERSION, Storage, Symbol


def test_v31_to_v32_preserves_parameter_and_template_arg_foreign_keys(tmp_path):
    path = str(tmp_path / "v31.db")
    db = Storage(path)
    db.add_component("c", "/data/c")
    file_id = db.add_file_path("/data/c/main.cpp")
    owner_id = db.add_symbol(
        Symbol("c:@F@owner", "owner", "function", file_id=file_id)
    )
    db.close()

    conn = sqlite3.connect(path)
    conn.execute(
        "INSERT INTO type_node(id, type_key, spelling, kind) "
        "VALUES (1, 'b:int', 'int', 1)"
    )
    conn.execute("DROP TABLE parameter")
    conn.execute("DROP TABLE template_arg")
    conn.execute(
        "CREATE TABLE parameter (owner_id INTEGER NOT NULL, position INTEGER NOT NULL, "
        "name TEXT, type_id INTEGER, file_id INTEGER, line INTEGER, col INTEGER, "
        "PRIMARY KEY(owner_id, position)) WITHOUT ROWID"
    )
    conn.execute(
        "CREATE TABLE template_arg (owner_id INTEGER NOT NULL, position INTEGER NOT NULL, "
        "arg_kind INTEGER NOT NULL, ref_id INTEGER, literal TEXT, type_id INTEGER, "
        "PRIMARY KEY(owner_id, position)) WITHOUT ROWID"
    )
    conn.execute(
        "INSERT INTO parameter(owner_id, position, name, type_id, file_id, line, col) "
        "VALUES (?, 0, 'value', 1, ?, 7, 9)",
        (owner_id, file_id),
    )
    conn.execute(
        "INSERT INTO template_arg(owner_id, position, arg_kind, literal, type_id) "
        "VALUES (?, 0, 1, 'int', 1)",
        (owner_id,),
    )
    conn.execute(
        "UPDATE meta SET value = '31' WHERE key = 'schema_version'"
    )
    conn.commit()
    conn.close()

    Storage(path).close()

    conn = sqlite3.connect(path)
    assert conn.execute(
        "SELECT value FROM meta WHERE key = 'schema_version'"
    ).fetchone()[0] == str(SCHEMA_VERSION)
    row = conn.execute(
        "SELECT type_id, declared_type_id, adjusted_type_id, file_id "
        "FROM parameter WHERE owner_id = ? AND position = 0 AND pack_index = -1",
        (owner_id,),
    ).fetchone()
    assert row == (1, None, None, file_id)
    assert conn.execute(
        "SELECT type_id, pack_index FROM template_arg WHERE owner_id = ?",
        (owner_id,),
    ).fetchone() == (1, -1)

    parameter_targets = {
        row[2] for row in conn.execute("PRAGMA foreign_key_list(parameter)")
    }
    argument_targets = {
        row[2] for row in conn.execute("PRAGMA foreign_key_list(template_arg)")
    }
    assert {"type_node", "file"} <= parameter_targets
    assert "type_node" in argument_targets
    conn.close()
