"""Read/query parity against the checked-in database produced by C++ cidx."""

from __future__ import annotations

from pathlib import Path

from indexer.queryplan import Executor, codebase, count, eq, nodes, start
from indexer.storage import Storage


REPO_ROOT = Path(__file__).resolve().parents[2]


def test_python_reads_cpp_generated_index_and_identity():
    with Storage(str(REPO_ROOT / "index.db")) as db:
        identity = db.index_identity()
        assert identity.freshness == "current"
        assert identity.source_revision is not None

        matches = db.lookup_symbols_by_name("explain")
        assert any(match.spelling == "explain" for match in matches)

        executor = Executor(db)
        result = executor.run(
            (start(codebase()) | nodes(eq("spelling", "explain"))).plan
        )
        assert result.index.freshness == "current"
        assert result.rows
        assert not result.truncated
        assert list(result.to_dict()) == [
            "shape", "view", "count", "truncated", "index", "rows"
        ]

        scalar = executor.run(
            (start(codebase()) | nodes(eq("spelling", "explain")) | count()).plan
        )
        assert scalar.scalar == 1
        assert list(scalar.to_dict()) == [
            "shape", "view", "count", "truncated", "index"
        ]
