"""CLI freshness regressions using the real Python import/index path."""

from __future__ import annotations

import json

from indexer import cli
from indexer.queryplan import Executor, codebase, count, nodes, start
from indexer.storage import Storage


def _run(argv, capsys):
    rc = cli.main(argv)
    captured = capsys.readouterr()
    return rc, captured.out, captured.err


def test_partial_index_does_not_restamp_stale_identity(tmp_path, monkeypatch, capsys):
    cache = tmp_path / "cache"
    project = tmp_path / "proj"
    project.mkdir()
    a = project / "a.cpp"
    b = project / "b.cpp"
    a.write_text("int old_symbol() { return 1; }\n")
    b.write_text("int other_symbol() { return 2; }\n")
    (project / "compile_commands.json").write_text(
        json.dumps(
            [
                {
                    "directory": str(project),
                    "command": "c++ -std=c++23 -c a.cpp -o a.o",
                    "file": "a.cpp",
                },
                {
                    "directory": str(project),
                    "command": "c++ -std=c++23 -c b.cpp -o b.o",
                    "file": "b.cpp",
                },
            ]
        )
    )
    monkeypatch.setenv("INDEXER_CACHE", str(cache))

    rc, _, err = _run(["import", "--db", str(project)], capsys)
    assert rc == 0
    assert not err
    with Storage(str(cache / "index.db")) as db:
        assert db.index_identity().freshness == "unverifiable"

    rc, _, err = _run(["index"], capsys)
    assert rc == 0
    assert not err
    with Storage(str(cache / "index.db")) as db:
        assert db.index_identity().freshness == "current"
        assert len(db.lookup_symbols_by_name("old_symbol")) == 1
        ex = Executor(db)
        row_dict = ex.run((start(codebase()) | nodes()).plan).to_dict()
        assert list(row_dict) == [
            "shape", "view", "count", "truncated", "index", "rows"
        ]
        scalar_dict = ex.run((start(codebase()) | nodes() | count()).plan).to_dict()
        assert list(scalar_dict) == [
            "shape", "view", "count", "truncated", "index"
        ]

    a.write_text("int new_symbol() { return 3; }\n")
    rc, _, err = _run(["index", str(b)], capsys)
    assert rc == 0
    assert not err
    with Storage(str(cache / "index.db")) as db:
        assert db.index_identity().freshness == "stale"
        assert len(db.lookup_symbols_by_name("old_symbol")) == 1
        assert not db.lookup_symbols_by_name("new_symbol")

    rc, _, err = _run(["index", str(a)], capsys)
    assert rc == 0
    assert not err
    with Storage(str(cache / "index.db")) as db:
        assert db.index_identity().freshness == "current"
        assert not db.lookup_symbols_by_name("old_symbol")
        replacement = db.lookup_symbols_by_name("new_symbol")
        assert len(replacement) == 1
        assert replacement[0].line == 1
