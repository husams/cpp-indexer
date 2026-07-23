"""Exercise the benchmark generation-state recovery contract."""

from __future__ import annotations

import argparse
import json
import shutil
import sqlite3
import tempfile
from pathlib import Path
from typing import Any

from .common import canonical_json, semantic_digest


def _check(path: Path, expected_current: bool) -> dict[str, Any]:
    connection = sqlite3.connect(path)
    try:
        integrity = connection.execute("PRAGMA integrity_check").fetchone()[0]
        foreign_keys = connection.execute("PRAGMA foreign_key_check").fetchall()
        state = connection.execute(
            "SELECT value FROM benchmark_meta WHERE key='state'"
        ).fetchone()[0]
        digest = connection.execute(
            "SELECT value FROM benchmark_meta WHERE key='semantic_digest'"
        ).fetchone()[0]
        actual_digest = semantic_digest(path)
        current = state == "current" and digest == actual_digest
        return {
            "integrity_check": integrity,
            "foreign_key_errors": len(foreign_keys),
            "state": state,
            "semantic_digest_matches": digest == actual_digest,
            "presented_as_current": current,
            "expected_current": expected_current,
            "status": "pass" if integrity == "ok" and not foreign_keys and current == expected_current else "fail",
        }
    finally:
        connection.close()


def simulate(path: Path) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="cidx-storage-m0-recovery-") as directory:
        trial = Path(directory) / "trial.db"
        shutil.copy2(path, trial)
        connection = sqlite3.connect(trial)
        connection.execute("BEGIN IMMEDIATE")
        connection.execute("UPDATE benchmark_meta SET value='building' WHERE key='state'")
        connection.execute("UPDATE benchmark_meta SET value='2' WHERE key='generation'")
        connection.close()  # uncommitted write is rolled back by SQLite
        rollback = _check(trial, True)

        connection = sqlite3.connect(trial)
        connection.execute("BEGIN IMMEDIATE")
        connection.execute("UPDATE benchmark_meta SET value='building' WHERE key='state'")
        connection.execute("UPDATE benchmark_meta SET value='2' WHERE key='generation'")
        connection.commit()  # a crash after this point must not present as current
        connection.close()
        committed_building = _check(trial, False)

        connection = sqlite3.connect(trial)
        connection.execute("UPDATE benchmark_meta SET value='current' WHERE key='state'")
        connection.execute("UPDATE benchmark_meta SET value='2' WHERE key='generation'")
        connection.commit()
        connection.close()
        repaired = _check(trial, True)
    return {
        "recovery_version": "storage-m0/recovery-v1",
        "source": str(path),
        "rollback_before_commit": rollback,
        "committed_building_state": committed_building,
        "repaired": repaired,
        "status": "pass" if all(
            item["status"] == "pass" for item in (rollback, committed_building, repaired)
        ) else "fail",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", type=Path, required=True)
    args = parser.parse_args(argv)
    result = simulate(args.db)
    print(canonical_json(result))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
