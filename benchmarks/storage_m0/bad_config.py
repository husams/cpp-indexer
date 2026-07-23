"""Create the intentionally bad layout used by the regression gate."""

from __future__ import annotations

import argparse
import shutil
import sqlite3
from pathlib import Path

from .common import canonical_json, semantic_digest


HOT_INDEXES = (
    "idx_symbol_qual",
    "idx_symbol_qual_nc",
    "idx_symbol_spelling",
    "idx_symbol_spelling_nc",
    "idx_edge_src",
    "idx_edge_dst",
)


def drop_hot_indexes(source: Path, output: Path) -> dict[str, object]:
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()
    shutil.copy2(source, output)
    connection = sqlite3.connect(output)
    try:
        for index in HOT_INDEXES:
            connection.execute(f'DROP INDEX IF EXISTS "{index}"')
        connection.execute(
            "INSERT INTO benchmark_meta(key, value) VALUES('configuration', 'drop_hot_indexes') "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value"
        )
        connection.commit()
        remaining = [
            row[0] for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type='index' ORDER BY name"
            )
        ]
    finally:
        connection.close()
    return {
        "configuration": "drop_hot_indexes",
        "source": str(source),
        "output": str(output),
        "dropped_indexes": list(HOT_INDEXES),
        "remaining_indexes": remaining,
        "semantic_digest": semantic_digest(output),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    print(canonical_json(drop_hot_indexes(args.source, args.output)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
