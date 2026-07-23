"""Shared deterministic helpers for the Storage M0 benchmark."""

from __future__ import annotations

import hashlib
import json
import platform
import sqlite3
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable

from . import BENCHMARK_VERSION, SCHEMA_VERSION


def canonical_json(value: Any) -> str:
    """Return the stable JSON representation used for hashes and artifacts."""
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def sha256(value: str | bytes) -> str:
    payload = value.encode("utf-8") if isinstance(value, str) else value
    return hashlib.sha256(payload).hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError(f"expected an object in {path}")
    return value


def manifest_digest(manifest: dict[str, Any]) -> str:
    return sha256(canonical_json(manifest))


def percentile(values: list[float], quantile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = (len(ordered) - 1) * quantile
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = index - lower
    return round(ordered[lower] + (ordered[upper] - ordered[lower]) * fraction, 6)


def latency_summary(values: list[float]) -> dict[str, Any]:
    if not values:
        return {"count": 0, "min_ms": None, "p50_ms": None, "p95_ms": None,
                "p99_ms": None, "max_ms": None}
    return {
        "count": len(values),
        "min_ms": round(min(values), 6),
        "p50_ms": percentile(values, 0.50),
        "p95_ms": percentile(values, 0.95),
        "p99_ms": percentile(values, 0.99),
        "max_ms": round(max(values), 6),
    }


def system_profile() -> dict[str, Any]:
    """Capture host facts without making them part of deterministic identity."""
    return {
        "os": platform.system(),
        "os_release": platform.release(),
        "architecture": platform.machine(),
        "python": platform.python_version(),
        "sqlite": sqlite3.sqlite_version,
        "cpu_count": __import__("os").cpu_count(),
        "compiler": platform.python_compiler(),
    }


def sqlite_compile_options(connection: sqlite3.Connection) -> list[str]:
    try:
        return sorted(row[0] for row in connection.execute("PRAGMA compile_options"))
    except sqlite3.DatabaseError:
        return []


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def load_cidx_schema() -> tuple[str, dict[str, int]]:
    """Load the checked-in v34 schema and kind mapping without packaging code."""
    python_root = repo_root() / "python"
    if str(python_root) not in sys.path:
        sys.path.insert(0, str(python_root))
    from indexer.storage import SCHEMA_VERSION as current_version
    from indexer.storage import SYMBOL_KIND_IDS, _SCHEMA

    if current_version != SCHEMA_VERSION:
        raise RuntimeError(
            f"benchmark requires schema v{SCHEMA_VERSION}, found v{current_version}"
        )
    return _SCHEMA, SYMBOL_KIND_IDS


def stable_rows(connection: sqlite3.Connection, table: str, columns: Iterable[str]) -> list[tuple[Any, ...]]:
    selected = ", ".join(columns)
    rows = connection.execute(f"SELECT {selected} FROM {table}").fetchall()
    return sorted(tuple(row) for row in rows)


SEMANTIC_TABLES: dict[str, tuple[str, ...]] = {
    "symbol": ("usr", "spelling", "qual_name", "kind", "file_id", "line", "is_definition"),
    "edge": ("src_id", "dst_id", "kind", "count", "base_access", "is_virtual"),
    "edge_site": ("edge_id", "file_id", "line", "col", "conditional", "args_sig"),
    "entity_node": ("id", "kind"),
    "entity_edge": ("src_id", "dst_id", "kind", "count", "via_member_id", "multiplicity", "access"),
    "type_node": ("type_key", "spelling", "kind", "decl_usr"),
    "type_edge": ("src_id", "kind", "position", "dst_id"),
    "include_edge": ("src_file_id", "dst_file_id", "dst_path", "config_id", "count"),
    "include_site": ("edge_id", "line", "col", "spelling", "resolved"),
}


def semantic_digest(path: Path) -> str:
    """Hash canonical semantic rows, excluding surrogate IDs where possible."""
    connection = sqlite3.connect(path)
    try:
        snapshot: dict[str, Any] = {}
        for table, columns in SEMANTIC_TABLES.items():
            exists = connection.execute(
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (table,)
            ).fetchone()
            if exists:
                snapshot[table] = stable_rows(connection, table, columns)
        return sha256(canonical_json(snapshot))
    finally:
        connection.close()


def require_result_version(result: dict[str, Any]) -> None:
    if result.get("result_version") != "storage-m0/result-v1":
        raise ValueError("unsupported Storage M0 result version")
    if result.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("result is not for SQLite schema v34")
    if result.get("benchmark") != BENCHMARK_VERSION:
        raise ValueError("unsupported Storage M0 benchmark version")


def git_revision() -> str | None:
    try:
        return subprocess.run(
            ["git", "rev-parse", "HEAD"], capture_output=True, text=True,
            check=True, cwd=repo_root(),
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None
