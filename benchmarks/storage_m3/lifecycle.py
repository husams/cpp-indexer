"""Small disposable-sidecar probe used by the Storage M3 contract tests."""

from __future__ import annotations

import hashlib
import json
import sqlite3
from pathlib import Path
from typing import Any


def _canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def authoritative_digest(path: Path) -> str:
    """Hash the logical rows that an accelerator is allowed to project."""

    with sqlite3.connect(path) as connection:
        rows = connection.execute(
            "SELECT id, usr, kind FROM fact ORDER BY id"
        ).fetchall()
    return hashlib.sha256(_canonical(rows)).hexdigest()


def build_accelerator(authoritative: Path, destination: Path) -> str:
    """Build a deterministic disposable projection from authoritative facts."""

    with sqlite3.connect(authoritative) as source:
        rows = source.execute("SELECT id, usr, kind FROM fact ORDER BY id").fetchall()
    content_identity = hashlib.sha256(_canonical(rows)).hexdigest()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(destination) as projection:
        projection.executescript(
            "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
            "CREATE TABLE adjacency(id INTEGER PRIMARY KEY, usr TEXT NOT NULL, "
            "kind TEXT NOT NULL);"
        )
        projection.executemany(
            "INSERT INTO adjacency(id, usr, kind) VALUES (?, ?, ?)", rows
        )
        projection.executemany(
            "INSERT INTO meta(key, value) VALUES (?, ?)",
            [("content_identity", content_identity), ("source", "sqlite")],
        )
    return content_identity


def run_lifecycle_probe(root: Path) -> dict[str, Any]:
    """Prove deletion is safe and a clean rebuild has the same identity."""

    root.mkdir(parents=True, exist_ok=True)
    authoritative = root / "index.db"
    accelerator = root / "accelerator.db"
    with sqlite3.connect(authoritative) as connection:
        connection.execute(
            "CREATE TABLE fact(id INTEGER PRIMARY KEY, usr TEXT NOT NULL, "
            "kind TEXT NOT NULL)"
        )
        connection.executemany(
            "INSERT INTO fact(id, usr, kind) VALUES (?, ?, ?)",
            [(1, "usr:function:one", "function"), (2, "usr:type:two", "type")],
        )
    before = authoritative_digest(authoritative)
    first_identity = build_accelerator(authoritative, accelerator)
    accelerator.unlink()
    after_delete = authoritative_digest(authoritative)
    second_identity = build_accelerator(authoritative, accelerator)
    return {
        "status": "pass"
        if before == after_delete and first_identity == second_identity
        else "fail",
        "authoritative_unchanged_after_delete": before == after_delete,
        "rebuild_content_identity_matches": first_identity == second_identity,
        "content_identity": second_identity,
    }
