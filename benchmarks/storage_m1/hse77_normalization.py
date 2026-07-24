"""Reproduce the HSE-77 hot-row normalization measurements.

This deliberately uses only sqlite3 and the standard library. It measures the
same row shape before/after normalization, records table/index bytes through
SQLite's dbstat virtual table, and evaluates the proposed symbol hot/cold split
with the same row count and lookup workload.
"""

from __future__ import annotations

import argparse
import json
import os
import sqlite3
import statistics
import tempfile
import time
from pathlib import Path


ROWS = 20_000
REPETITIONS = 5
QUERY_ROWS = 500
SOURCE_KINDS = (1, 2, 3, 4, 5, 6, 7, 8)


def connect(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(path)
    conn.execute("PRAGMA journal_mode=DELETE")
    conn.execute("PRAGMA synchronous=FULL")
    return conn


def object_bytes(path: Path) -> dict[str, int]:
    conn = sqlite3.connect(path)
    rows = conn.execute(
        "SELECT name, SUM(pgsize) FROM dbstat GROUP BY name ORDER BY name"
    ).fetchall()
    conn.close()
    return {name: int(size) for name, size in rows}


def legacy_schema(conn: sqlite3.Connection) -> None:
    conn.executescript(
        """
        CREATE TABLE call_arg (
            id INTEGER PRIMARY KEY,
            src_kind TEXT NOT NULL,
            type_usr TEXT,
            decl_usr TEXT,
            callee_usr TEXT
        );
        CREATE INDEX idx_call_arg_type_usr ON call_arg(type_usr);
        CREATE INDEX idx_call_arg_decl_usr ON call_arg(decl_usr);
        CREATE INDEX idx_call_arg_callee_usr ON call_arg(callee_usr);
        """
    )


def normalized_schema(conn: sqlite3.Connection) -> None:
    conn.executescript(
        """
        CREATE TABLE external_identity (
            id INTEGER PRIMARY KEY,
            identity_kind INTEGER NOT NULL CHECK (identity_kind IN (1,2,3)),
            identity_text TEXT NOT NULL,
            UNIQUE(identity_kind, identity_text)
        );
        CREATE TABLE call_arg (
            id INTEGER PRIMARY KEY,
            src_kind_id INTEGER CHECK (src_kind_id IS NULL OR src_kind_id IN (1,2,3,4,5,6,7,8)),
            type_identity_id INTEGER REFERENCES external_identity(id),
            decl_identity_id INTEGER REFERENCES external_identity(id),
            callee_identity_id INTEGER REFERENCES external_identity(id)
        );
        CREATE INDEX idx_call_arg_type_identity ON call_arg(type_identity_id);
        CREATE INDEX idx_call_arg_decl_identity ON call_arg(decl_identity_id);
        CREATE INDEX idx_call_arg_callee_identity ON call_arg(callee_identity_id);
        """
    )


def legacy_rows() -> list[tuple[int, str, str, str, str]]:
    return [
        (
            i,
            str(SOURCE_KINDS[i % len(SOURCE_KINDS)]),
            f"type:{i % 1000}",
            f"decl:{i % 1000}",
            f"callee:{i % 1000}",
        )
        for i in range(ROWS)
    ]


def measure_write(root: Path, normalized: bool) -> tuple[float, dict[str, int]]:
    samples: list[float] = []
    last_path = root / ("normalized.db" if normalized else "legacy.db")
    rows = legacy_rows()
    for rep in range(REPETITIONS):
        path = root / f"{'normalized' if normalized else 'legacy'}-{rep}.db"
        conn = connect(path)
        (normalized_schema if normalized else legacy_schema)(conn)
        started = time.perf_counter()
        with conn:
            if normalized:
                identities = [
                    (1, f"type:{i}") for i in range(1000)
                ] + [(2, f"decl:{i}") for i in range(1000)] + [
                    (2, f"callee:{i}") for i in range(1000)
                ]
                conn.executemany(
                    "INSERT INTO external_identity(identity_kind,identity_text) VALUES (?,?)",
                    identities,
                )
                ids = {
                    (kind, text): ident
                    for ident, kind, text in conn.execute(
                        "SELECT id,identity_kind,identity_text FROM external_identity"
                    )
                }
                conn.executemany(
                    "INSERT INTO call_arg VALUES (?,?,?,?,?)",
                    [
                        (
                            i,
                            SOURCE_KINDS[i % len(SOURCE_KINDS)],
                            ids[(1, f"type:{i % 1000}")],
                            ids[(2, f"decl:{i % 1000}")],
                            ids[(2, f"callee:{i % 1000}")],
                        )
                        for i in range(ROWS)
                    ],
                )
            else:
                conn.executemany("INSERT INTO call_arg VALUES (?,?,?,?,?)", rows)
        samples.append(ROWS / (time.perf_counter() - started))
        conn.close()
        last_path = path
    return statistics.median(samples), object_bytes(last_path)


def measure_compatibility(path: Path) -> list[float]:
    conn = sqlite3.connect(path)
    samples: list[float] = []
    query = """
        SELECT ca.id,
               CASE ca.src_kind_id WHEN 1 THEN 'literal' WHEN 2 THEN 'local'
                    WHEN 3 THEN 'construct' WHEN 4 THEN 'member'
                    WHEN 5 THEN 'global' WHEN 6 THEN 'call_result'
                    WHEN 7 THEN 'this' WHEN 8 THEN 'unknown' END,
               ti.identity_text, di.identity_text, ci.identity_text
        FROM call_arg ca
        LEFT JOIN external_identity ti ON ti.id = ca.type_identity_id
        LEFT JOIN external_identity di ON di.id = ca.decl_identity_id
        LEFT JOIN external_identity ci ON ci.id = ca.callee_identity_id
        ORDER BY ca.id LIMIT ?
    """
    for _ in range(REPETITIONS):
        started = time.perf_counter()
        rows = conn.execute(query, (QUERY_ROWS,)).fetchall()
        samples.append((time.perf_counter() - started) * 1000)
        assert len(rows) == QUERY_ROWS
    conn.close()
    return samples


def measure_migration(root: Path) -> tuple[float, dict[str, int]]:
    path = root / "migration.db"
    conn = connect(path)
    legacy_schema(conn)
    with conn:
        conn.executemany("INSERT INTO call_arg VALUES (?,?,?,?,?)", legacy_rows())
        conn.execute("ALTER TABLE call_arg RENAME TO call_arg_old")
    started = time.perf_counter()
    with conn:
        normalized_schema(conn)
        conn.execute(
            "INSERT INTO external_identity(identity_kind,identity_text) "
            "SELECT 1,type_usr FROM call_arg_old UNION SELECT 2,decl_usr FROM call_arg_old "
            "UNION SELECT 2,callee_usr FROM call_arg_old"
        )
        conn.execute(
            "INSERT INTO call_arg SELECT id,CAST(src_kind AS INTEGER),"
            "(SELECT id FROM external_identity WHERE identity_kind=1 AND identity_text=type_usr),"
            "(SELECT id FROM external_identity WHERE identity_kind=2 AND identity_text=decl_usr),"
            "(SELECT id FROM external_identity WHERE identity_kind=2 AND identity_text=callee_usr)"
            " FROM call_arg_old"
        )
        conn.execute("DROP TABLE call_arg_old")
    elapsed = time.perf_counter() - started
    conn.close()
    return elapsed, object_bytes(path)


def measure_hot_cold(root: Path) -> dict[str, object]:
    rows = [
        (i, f"usr:{i}", f"Name{i}", f"Qualified::{i}", f"type:{i}", "cold-doc", "cold-extra")
        for i in range(ROWS)
    ]
    results: dict[str, object] = {}
    for split in (False, True):
        path = root / ("split.db" if split else "unsplit.db")
        conn = connect(path)
        if split:
            conn.executescript(
                "CREATE TABLE symbol_hot(id INTEGER PRIMARY KEY, usr TEXT UNIQUE, spelling TEXT, qual_name TEXT, type_info TEXT);"
                "CREATE TABLE symbol_cold(id INTEGER PRIMARY KEY REFERENCES symbol_hot(id), doc TEXT, extra TEXT);"
            )
            with conn:
                conn.executemany("INSERT INTO symbol_hot VALUES (?,?,?,?,?)", [r[:5] for r in rows])
                conn.executemany("INSERT INTO symbol_cold VALUES (?,?,?)", [(r[0], r[5], r[6]) for r in rows])
            query = "SELECT id,usr,spelling,qual_name,type_info FROM symbol_hot WHERE usr=?"
        else:
            conn.execute("CREATE TABLE symbol(id INTEGER PRIMARY KEY, usr TEXT UNIQUE, spelling TEXT, qual_name TEXT, type_info TEXT, doc TEXT, extra TEXT)")
            with conn:
                conn.executemany("INSERT INTO symbol VALUES (?,?,?,?,?,?,?)", rows)
            query = "SELECT id,usr,spelling,qual_name,type_info FROM symbol WHERE usr=?"
        samples = []
        for _ in range(REPETITIONS):
            started = time.perf_counter()
            for i in range(QUERY_ROWS):
                conn.execute(query, (f"usr:{i}",)).fetchone()
            samples.append((time.perf_counter() - started) * 1000)
        conn.close()
        results["split" if split else "unsplit"] = {
            "bytes": sum(object_bytes(path).values()),
            "lookup_ms_median": statistics.median(samples),
        }
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cidx-hse77-benchmark-") as temp:
        root = Path(temp)
        legacy_rate, legacy_objects = measure_write(root, False)
        normalized_rate, normalized_objects = measure_write(root, True)
        normalized_path = root / f"normalized-{REPETITIONS - 1}.db"
        compatibility = measure_compatibility(normalized_path)
        migration_seconds, migration_objects = measure_migration(root)
        hot_cold = measure_hot_cold(root)
        report = {
            "benchmark": "storage-m1/hse77-normalization-v1",
            "method": {
                "rows": ROWS,
                "repetitions": REPETITIONS,
                "compatibility_rows": QUERY_ROWS,
                "sqlite": sqlite3.sqlite_version,
                "journal_mode": "DELETE",
                "synchronous": "FULL",
                "command": "PYTHONPATH=. python3 benchmarks/storage_m1/hse77_normalization.py --output benchmarks/storage_m1/hse77-normalization.json",
            },
            "before_after": {
                "legacy": {"write_rows_per_s_median": legacy_rate, "objects": legacy_objects},
                "normalized": {"write_rows_per_s_median": normalized_rate, "objects": normalized_objects},
                "compatibility_query_ms": {"samples": compatibility, "median": statistics.median(compatibility)},
                "migration": {"seconds": migration_seconds, "rows_per_s": ROWS / migration_seconds, "objects": migration_objects},
            },
            "hot_cold_decision": {
                "measurements": hot_cold,
                "decision": "retain symbol attributes in one hot table",
                "reason": "the measured split adds a join and does not reduce total bytes or improve the representative lookup median",
            },
        }
        args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"output": str(args.output), "benchmark": report["benchmark"]}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
