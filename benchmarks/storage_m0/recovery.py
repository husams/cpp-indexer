"""Exercise real SQLite WAL, commit, checkpoint, and recovery boundaries."""

from __future__ import annotations

import argparse
import shutil
import sqlite3
import tempfile
from pathlib import Path
from typing import Any

from .common import canonical_json, semantic_digest


def _wal_connection(path: Path) -> sqlite3.Connection:
    connection = sqlite3.connect(path)
    connection.execute("PRAGMA foreign_keys=ON")
    connection.execute("PRAGMA journal_mode=WAL")
    connection.execute("PRAGMA synchronous=FULL")
    connection.execute("PRAGMA wal_autocheckpoint=0")
    connection.commit()
    return connection


def _wal_bytes(path: Path) -> int:
    wal = Path(str(path) + "-wal")
    return wal.stat().st_size if wal.exists() else 0


def _check(path: Path, expected_current: bool, *, checkpointed: bool | None = None, observed_wal_bytes: int | None = None) -> dict[str, Any]:
    connection = sqlite3.connect(path)
    try:
        connection.execute("PRAGMA foreign_keys=ON")
        integrity = connection.execute("PRAGMA integrity_check").fetchone()[0]
        foreign_keys = connection.execute("PRAGMA foreign_key_check").fetchall()
        state = connection.execute("SELECT value FROM benchmark_meta WHERE key='state'").fetchone()[0]
        stored = connection.execute("SELECT value FROM benchmark_meta WHERE key='semantic_digest'").fetchone()[0]
        generation_row = connection.execute("SELECT value FROM benchmark_meta WHERE key='generation'").fetchone()
        actual_digest = semantic_digest(path)
        try:
            generation = int(generation_row[0])
        except (TypeError, ValueError):
            generation = None
        generation_valid = generation is not None and generation > 0
        digest_matches = stored == actual_digest
        current = integrity == "ok" and not foreign_keys and state == "current" and generation_valid and digest_matches
        wal_bytes = _wal_bytes(path) if observed_wal_bytes is None else observed_wal_bytes
        checkpointed_actual = wal_bytes == 0
        checkpoint_ok = checkpointed is None or checkpointed_actual == checkpointed
        return {
            "integrity_check": integrity, "foreign_key_errors": len(foreign_keys),
            "state": state, "generation": generation, "generation_valid": generation_valid,
            "semantic_digest": actual_digest, "semantic_digest_matches": digest_matches,
            "wal_bytes": wal_bytes, "checkpointed": checkpointed_actual,
            "presented_as_current": current, "expected_current": expected_current,
            "status": "pass" if current == expected_current and integrity == "ok" and not foreign_keys and generation_valid and checkpoint_ok else "fail",
        }
    finally:
        connection.close()


def _copy(source: Path, directory: Path, name: str) -> Path:
    target = directory / name
    shutil.copy2(source, target)
    return target


def _write_building(path: Path, *, commit: bool) -> sqlite3.Connection:
    connection = _wal_connection(path)
    connection.execute("BEGIN IMMEDIATE")
    connection.execute("UPDATE benchmark_meta SET value='building' WHERE key='state'")
    connection.execute("UPDATE benchmark_meta SET value=CAST(value AS INTEGER)+1 WHERE key='generation'")
    connection.execute("UPDATE symbol SET qual_name=qual_name || '-recovery-write' WHERE id=(SELECT MIN(id) FROM symbol)")
    if commit:
        connection.commit()
    # The caller closes with an open transaction for the pre-commit boundary.
    return connection


def simulate(path: Path) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="cidx-storage-m0-recovery-") as directory_name:
        directory = Path(directory_name)

        pre_write_path = _copy(path, directory, "pre-write.db")
        connection = _wal_connection(pre_write_path)
        try:
            pre_write = _check(pre_write_path, True)
        finally:
            connection.close()

        pre_commit_path = _copy(path, directory, "pre-commit.db")
        connection = _write_building(pre_commit_path, commit=False)
        connection.close()
        after_write_before_commit = _check(pre_commit_path, True)

        post_commit_path = _copy(path, directory, "post-commit.db")
        post_commit_connection = _write_building(post_commit_path, commit=True)
        post_commit_wal_bytes = _wal_bytes(post_commit_path)
        after_commit_before_checkpoint = _check(post_commit_path, False, checkpointed=False, observed_wal_bytes=post_commit_wal_bytes)
        post_commit_connection.close()

        post_checkpoint_path = _copy(path, directory, "post-checkpoint.db")
        connection = _wal_connection(post_checkpoint_path)
        try:
            connection.execute("BEGIN IMMEDIATE")
            connection.execute("UPDATE benchmark_meta SET value='building' WHERE key='state'")
            connection.execute("UPDATE benchmark_meta SET value=CAST(value AS INTEGER)+1 WHERE key='generation'")
            connection.execute("UPDATE symbol SET qual_name=qual_name || '-checkpoint-write' WHERE id=(SELECT MIN(id) FROM symbol)")
            connection.commit()
            digest = semantic_digest(post_checkpoint_path)
            connection.execute("UPDATE benchmark_meta SET value='current' WHERE key='state'")
            connection.execute("UPDATE benchmark_meta SET value=? WHERE key='semantic_digest'", (digest,))
            connection.commit()
            checkpoint_result = connection.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
        finally:
            connection.close()
        after_checkpoint = _check(post_checkpoint_path, True, checkpointed=True)
        after_checkpoint["checkpoint_result"] = list(checkpoint_result or ())

        tampered_path = _copy(path, directory, "digest-mismatch.db")
        connection = _wal_connection(tampered_path)
        try:
            connection.execute("UPDATE symbol SET qual_name=qual_name || '-tampered' WHERE id=(SELECT MIN(id) FROM symbol)")
            connection.commit()
        finally:
            connection.close()
        digest_mismatch = _check(tampered_path, False)

    checks = (pre_write, after_write_before_commit, after_commit_before_checkpoint, after_checkpoint, digest_mismatch)
    return {
        "recovery_version": "storage-m0/recovery-v2", "source": str(path),
        "points": {
            "pre_write": pre_write, "pre_commit": after_write_before_commit,
            "post_commit_pre_checkpoint": after_commit_before_checkpoint,
            "post_checkpoint": after_checkpoint,
        },
        "rollback_before_commit": after_write_before_commit,
        "committed_building_state": after_commit_before_checkpoint,
        "repaired": after_checkpoint,
        "digest_mismatch_not_current": digest_mismatch,
        "status": "pass" if all(item["status"] == "pass" for item in checks) else "fail",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", type=Path, required=True)
    result = simulate(parser.parse_args(argv).db)
    print(canonical_json(result))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
