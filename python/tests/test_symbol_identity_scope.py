import sqlite3

import pytest

from indexer.storage import Storage, Symbol


def _file(store, component, name):
    directory = store.add_directory(component, "")
    return store.add_file(directory, name)


def _symbol(usr, file_id, linkage="external"):
    return Symbol(
        usr=usr,
        spelling="Thing",
        kind="struct",
        file_id=file_id,
        linkage=linkage,
    )


def test_unrelated_universes_isolate_and_declared_universe_merges(tmp_path):
    with Storage(":memory:") as store:
        banking = store.add_semantic_universe("program:banking")
        composed = store.add_semantic_universe("program:composed")
        banking_repo = store.add_repository("banking", semantic_universe_id=banking)
        composed_repo = store.add_repository("composed", semantic_universe_id=composed)
        banking_component = store.add_component("banking", str(tmp_path / "banking"))
        composed_component = store.add_component("composed", str(tmp_path / "composed"))
        store.set_component_repository(banking_component, banking_repo)
        store.set_component_repository(composed_component, composed_repo)
        banking_file = _file(store, banking_component, "collision.hpp")
        composed_file = _file(store, composed_component, "collision.hpp")

        banking_id = store.add_symbol(_symbol("c:@N@collision", banking_file))
        composed_id = store.add_symbol(_symbol("c:@N@collision", composed_file))
        assert banking_id != composed_id
        assert [s.id for s in store.lookup_symbols_by_usr("c:@N@collision")] == [
            banking_id,
            composed_id,
        ]

        twin_repo = store.add_repository(
            "composed-twin", semantic_universe_id=banking
        )
        twin_component = store.add_component("composed-twin", str(tmp_path / "twin"))
        store.set_component_repository(twin_component, twin_repo)
        twin_file = _file(store, twin_component, "collision.hpp")
        assert store.add_symbol(_symbol("c:@N@collision", twin_file)) == banking_id

        assert store.add_symbol(_symbol("c:@F@hidden", banking_file, "internal")) != store.add_symbol(
            _symbol("c:@F@hidden", twin_file, "internal")
        )
        assert store.add_symbol(_symbol("c:@F@local", banking_file, "no-linkage")) != store.add_symbol(
            _symbol("c:@F@local", twin_file, "no-linkage")
        )
        with pytest.raises(ValueError, match="ambiguous symbol USR"):
            store.lookup_symbol("c:@N@collision")
        assert store.lookup_symbol("c:@N@collision", banking).id == banking_id
        assert store.lookup_symbol("c:@N@collision", composed).id == composed_id
        with pytest.raises(ValueError, match="ambiguous symbol USR"):
            store.update_symbol("c:@N@collision", spelling="X")
        assert store.update_symbol(
            "c:@N@collision", banking, spelling="B"
        )
        assert store.lookup_symbol_by_id(banking_id).spelling == "B"
        assert store.lookup_symbol_by_id(composed_id).spelling == "Thing"
        assert store.add_repository("composed") == composed_repo
        assert store.get_repository_by_id(composed_repo).semantic_universe_id == composed
        assert store.lookup_symbol_by_id(banking_id).identity_key == (
            "program:banking\x1fc:@N@collision"
        )


def test_local_identity_is_stable_across_file_insertion_order(tmp_path):
    def make_key(add_filler):
        with Storage(":memory:") as store:
            repo = store.add_repository("clone", remote_url="ssh://example/clone")
            component = store.add_component("clone", str(tmp_path / "stable"))
            store.set_component_repository(component, repo)
            directory = store.add_directory(component, "")
            if add_filler:
                store.add_file(directory, "unrelated.cpp")
            file_id = store.add_file(directory, "stable.cpp")
            return store.lookup_symbol_by_id(
                store.add_symbol(_symbol("c:@F@hidden", file_id, "internal"))
            ).identity_key

    assert make_key(False) == make_key(True)


def test_v34_migration_preserves_numeric_and_scoped_identity(tmp_path):
    path = tmp_path / "v34.db"
    conn = sqlite3.connect(path)
    conn.executescript(
        """
        CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
        INSERT INTO meta VALUES ('schema_version', '34');
        CREATE TABLE symbol (
          id INTEGER PRIMARY KEY, usr TEXT NOT NULL UNIQUE, spelling TEXT NOT NULL,
          qual_name TEXT, display_name TEXT, kind INTEGER NOT NULL, type_info TEXT,
          file_id INTEGER, line INTEGER, col INTEGER, end_line INTEGER, end_col INTEGER,
          decl_file_id INTEGER, decl_line INTEGER, decl_col INTEGER, decl_path TEXT,
          is_definition INTEGER NOT NULL DEFAULT 0, is_pure INTEGER NOT NULL DEFAULT 0,
          is_static INTEGER NOT NULL DEFAULT 0, is_instantiation INTEGER NOT NULL DEFAULT 0,
          is_named_instance INTEGER NOT NULL DEFAULT 0, linkage TEXT, access TEXT,
          parent_usr TEXT, resolved INTEGER NOT NULL DEFAULT 0,
          multi_def INTEGER NOT NULL DEFAULT 0, const_value TEXT
        );
        CREATE TABLE repository (
          id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE,
          kind TEXT NOT NULL DEFAULT 'repo', remote_url TEXT,
          active_clone_id INTEGER
        );
        INSERT INTO repository (id, name) VALUES (3, 'legacy-repo');
        CREATE TABLE component (
          id INTEGER PRIMARY KEY, name TEXT NOT NULL, path TEXT NOT NULL,
          kind TEXT NOT NULL DEFAULT 'repo', version TEXT, repository_id INTEGER,
          UNIQUE(path)
        );
        INSERT INTO component (id, name, path, repository_id)
          VALUES (4, 'legacy-component', '.', 3);
        CREATE TABLE edge (
          id INTEGER PRIMARY KEY, src_id INTEGER NOT NULL, dst_id INTEGER NOT NULL,
          kind INTEGER NOT NULL, count INTEGER NOT NULL DEFAULT 1,
          base_access INTEGER, is_virtual INTEGER, vtable_slot INTEGER,
          UNIQUE(src_id, dst_id, kind)
        );
        INSERT INTO symbol (id, usr, spelling, kind, linkage)
          VALUES (7, 'c:@N@legacy', 'legacy', 22, 'external');
        INSERT INTO edge (id, src_id, dst_id, kind) VALUES (11, 7, 7, 1);
        """
    )
    conn.commit()
    conn.close()

    with Storage(str(path)) as store:
        symbol = store.lookup_symbol_by_id(7)
        assert symbol.semantic_universe_id == 1
        assert symbol.identity_key == "legacy\x1fc:@N@legacy"
        assert store.get_repository_by_id(3).semantic_universe_id == 1

    conn = sqlite3.connect(path)
    assert conn.execute(
        "SELECT value FROM meta WHERE key = 'schema_version'"
    ).fetchone()[0] == "35"
    assert conn.execute("SELECT src_id, dst_id FROM edge WHERE id = 11").fetchone() == (7, 7)
    conn.close()
