#!/usr/bin/env python3
"""Validate release inputs before packaging C++ and Python products."""

from __future__ import annotations

import json
import re
import sqlite3
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION = ROOT / "spec/platform/version.json"
MANIFEST = ROOT / "spec/contracts/compatibility-manifest.json"


def fail(message: str) -> None:
    raise SystemExit(f"release contract: {message}")


def main() -> int:
    generator = subprocess.run(
        [sys.executable, str(ROOT / "scripts/generate_contracts.py"), "--check"],
        cwd=ROOT,
        check=False,
    )
    if generator.returncode:
        return generator.returncode

    data = json.loads(VERSION.read_text(encoding="utf-8"))
    product_version = data["product"]["version"]
    if not re.fullmatch(r"\d+\.\d+\.\d+", product_version):
        fail("product version is not a stable SemVer")
    for relative in data["catalog"]["inputs"]:
        if not (ROOT / relative).is_file():
            fail(f"missing catalog input {relative}")

    pyproject = (ROOT / "python/pyproject.toml").read_text(encoding="utf-8")
    if 'dynamic = ["version"]' not in pyproject or 'path = "indexer/_version.py"' not in pyproject:
        fail("Python packaging is not wired to the generated version")

    schema = str(data["database"]["schema_version"])
    python_storage = (ROOT / "python/indexer/storage.py").read_text(encoding="utf-8")
    cpp_storage = (ROOT / "src/storage/storage.hpp").read_text(encoding="utf-8")
    if f"SCHEMA_VERSION = {schema}" not in python_storage:
        fail("Python storage schema version disagrees with version.json")
    if f"kSchemaVersion = {schema}" not in cpp_storage:
        fail("C++ storage schema version disagrees with version.json")

    database = ROOT / "index.db"
    if database.is_file():
        with sqlite3.connect(database) as connection:
            row = connection.execute(
                "SELECT value FROM meta WHERE key = 'schema_version'"
            ).fetchone()
        if row is None or row[0] != schema:
            fail(f"index.db schema version is {row[0] if row else 'missing'}, expected {schema}")

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    for contract in manifest["contracts"]:
        for key in ("schema", "golden"):
            relative = contract[key]
            if not (ROOT / relative).is_file():
                fail(f"{contract['id']} references missing {relative}")
        if contract["promise"] in {"byte-identical", "schema-compatible", "stable-codes"} and len(contract["executors"]) != 2:
            fail(f"{contract['id']} must name both C++ and Python executors")

    print(f"release contract OK: product {product_version}, schema {schema}, {len(manifest['contracts'])} compatibility contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
