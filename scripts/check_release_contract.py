#!/usr/bin/env python3
"""Validate release inputs before packaging C++ and Python products."""

from __future__ import annotations

import ast
import json
import re
import sqlite3
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "spec/contracts/compatibility-manifest.json"
sys.path.insert(0, str(ROOT / "scripts"))


def _schema_type_matches(value: object, expected: str) -> bool:
    return {
        "object": isinstance(value, dict),
        "array": isinstance(value, list),
        "string": isinstance(value, str),
        "integer": isinstance(value, int) and not isinstance(value, bool),
        "number": isinstance(value, (int, float)) and not isinstance(value, bool),
        "boolean": isinstance(value, bool),
        "null": value is None,
    }[expected]


def validate_json_schema(value: object, schema: dict, path: str = "$") -> None:
    if "const" in schema and value != schema["const"]:
        fail(f"{path} must equal {schema['const']!r}")
    if "enum" in schema and value not in schema["enum"]:
        fail(f"{path} has unexpected value {value!r}")
    if "type" in schema and not _schema_type_matches(value, schema["type"]):
        fail(f"{path} must be a {schema['type']}")
    if "minimum" in schema and value < schema["minimum"]:
        fail(f"{path} is below minimum {schema['minimum']}")
    if "pattern" in schema and (
        not isinstance(value, str) or re.fullmatch(schema["pattern"], value) is None
    ):
        fail(f"{path} does not match {schema['pattern']!r}")

    if isinstance(value, dict):
        for required in schema.get("required", []):
            if required not in value:
                fail(f"{path} is missing required property {required!r}")
        properties = schema.get("properties", {})
        if schema.get("additionalProperties") is False:
            unknown = set(value) - set(properties)
            if unknown:
                fail(f"{path} has unexpected properties {sorted(unknown)!r}")
        for key, child in value.items():
            if key in properties:
                validate_json_schema(child, properties[key], f"{path}.{key}")
    elif isinstance(value, list) and "items" in schema:
        for index, child in enumerate(value):
            validate_json_schema(child, schema["items"], f"{path}[{index}]")


def _python_literals(path: Path) -> dict[str, object]:
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    values: dict[str, object] = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
            if isinstance(target, ast.Name):
                try:
                    values[target.id] = ast.literal_eval(node.value)
                except (ValueError, TypeError):
                    pass
    return values


def _cpp_string_constants(path: Path) -> dict[str, str]:
    source = path.read_text(encoding="utf-8")
    return dict(re.findall(r"k([A-Za-z0-9]+)\s*=\s*\"([^\"]*)\"", source))


def _cpp_relation_catalog() -> tuple[tuple[str, str, int], ...]:
    source = (ROOT / "src/catalogs/generated_catalog.hpp").read_text(encoding="utf-8")
    rows = re.findall(
        r'\{\.id = (\d+), \.name = "([^"]+)", \.layer = View::(Symbol|Entity),',
        source,
    )
    return tuple((name, layer.lower(), int(kind_id)) for kind_id, name, layer in rows)


def compare_generated_outputs(data: dict, digest: str) -> None:
    """Compare the generated C++ and Python declarations, not just templates."""
    sys.path.insert(0, str(ROOT / "python"))
    from generate_contracts import full_version

    expected = {
        "ProductVersion": full_version(data),
        "BaseProductVersion": data["product"]["version"],
        "FullProductVersion": full_version(data),
        "CatalogHash": digest,
    }
    cpp = _cpp_string_constants(ROOT / "src/cli/version.hpp")
    for name, value in expected.items():
        if cpp.get(name) != value:
            fail(f"C++ generated {name} is {cpp.get(name)!r}, expected {value!r}")

    python = _python_literals(ROOT / "python/indexer/_version.py")
    python_expected = {
        "BASE_VERSION": data["product"]["version"],
        "__version__": full_version(data),
        "FULL_VERSION": full_version(data),
        "CATALOG_HASH": digest,
    }
    for name, value in python_expected.items():
        if python.get(name) != value:
            fail(f"Python generated {name} is {python.get(name)!r}, expected {value!r}")


def compare_catalog_outputs(golden: dict) -> None:
    """Ensure the declared C++ and Python catalog outputs are identical."""
    sys.path.insert(0, str(ROOT / "python"))
    from indexer.queryplan import relation_catalog

    cpp = _cpp_relation_catalog()
    python = tuple(relation_catalog())
    if cpp != python:
        fail("C++ and Python relation catalogs disagree")
    catalog_rows = {(row["name"], row["layer"], row["id"]) for row in golden["entries"]}
    if not catalog_rows.issubset(set(cpp)):
        fail("catalog golden contains an entry missing from C++/Python outputs")


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

    from generate_contracts import catalog_hash, full_version, load_contract

    data = load_contract()
    product_version = full_version(data)
    digest = catalog_hash(data)
    compare_generated_outputs(data, digest)
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
        schema_path = ROOT / contract["schema"]
        golden_path = ROOT / contract["golden"]
        for key in ("schema", "golden"):
            relative = contract[key]
            if not (ROOT / relative).is_file():
                fail(f"{contract['id']} references missing {relative}")
        schema_data = json.loads(schema_path.read_text(encoding="utf-8"))
        golden_data = json.loads(golden_path.read_text(encoding="utf-8"))
        validate_json_schema(golden_data, schema_data, contract["id"])
        if contract["promise"] in {"byte-identical", "schema-compatible", "stable-codes"} and len(contract["executors"]) != 2:
            fail(f"{contract['id']} must name both C++ and Python executors")
        if contract["id"] == "public-catalogs":
            compare_catalog_outputs(golden_data)

    print(f"release contract OK: product {product_version}, schema {schema}, {len(manifest['contracts'])} compatibility contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
