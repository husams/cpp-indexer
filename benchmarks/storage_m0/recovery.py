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
        generation = connection.execute(
            "SELECT value FROM benchmark_meta WHERE key='generation'"
        ).fetchone()
        actual_digest = semantic_digest(path)
        try:
            generation_value = int(generation[0])
        except (TypeError, ValueError):
            generation_value = None
        generation_valid = generation_value is not None and generation_value > 0
        digest_matches = digest == actual_digest
        current = integrity == "ok" and not foreign_keys and state == "current" and generation_valid and digest_matches
        return {
            "integrity_check": integrity,
            "foreign_key_errors": len(foreign_keys),
            "state": state,
            "generation_valid": generation_valid,
            "semantic_digest_matches": digest_matches,
            "presented_as_current": current,
            "expected_current": expected_current,
            "status": "pass" if current == expected_current and (current or (integrity == "ok" and not foreign_keys and generation_valid)) else "fail",
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
        before_write = _check(trial, True)

        connection = sqlite3.connect(trial)
        connection.execute("BEGIN IMMEDIATE")
        connection.execute("UPDATE benchmark_meta SET value='building' WHERE key='state'")
        connection.execute("UPDATE benchmark_meta SET value='2' WHERE key='generation'")
        connection.commit()  # a crash after this point must not present as current
        connection.close()
        after_write_before_commit = _check(trial, False)

        after_commit_before_checkpoint = _check(trial, False)

        connection = sqlite3.connect(trial)
        connection.execute("UPDATE benchmark_meta SET value='current' WHERE key='state'")
        connection.execute("UPDATE benchmark_meta SET value='2' WHERE key='generation'")
        connection.commit()
        connection.close()
        digest_repaired = semantic_digest(trial)
        connection = sqlite3.connect(trial)
        connection.execute("UPDATE benchmark_meta SET value=? WHERE key='semantic_digest'", (digest_repaired,))
        connection.commit()
        connection.close()
        after_checkpoint = _check(trial, True)

        connection = sqlite3.connect(trial)
        connection.execute("UPDATE symbol SET qual_name=qual_name || '-tampered' WHERE id=(SELECT MIN(id) FROM symbol)")
        connection.commit()
        connection.close()
        digest_mismatch = _check(trial, False)
    return {
        "recovery_version": "storage-m0/recovery-v1",
        "source": str(path),
        "points": {
            "before_write": before_write,
            "after_write_before_commit": after_write_before_commit,
            "after_commit_before_checkpoint": after_commit_before_checkpoint,
            "after_checkpoint": after_checkpoint,
        },
        "rollback_before_commit": before_write,
        "committed_building_state": after_commit_before_checkpoint,
        "repaired": after_checkpoint,
        "digest_mismatch_not_current": digest_mismatch,
        "status": "pass" if all(
            item["status"] == "pass" for item in (before_write, after_write_before_commit, after_commit_before_checkpoint, after_checkpoint, digest_mismatch)
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
