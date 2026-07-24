#!/usr/bin/env python3
"""Generate the cross-language CIDX semantic catalog contract.

The input is deliberately JSON so the generator has no third-party runtime
dependency. ``--check`` is the CI gate; ``--write`` is the only mode that
updates generated files or the compatibility lock.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "catalogs/core.json"
LOCK = ROOT / "catalogs/compatibility.json"
EXTENSIONS_DIR = ROOT / "catalogs/extensions"


def canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def load_extensions() -> list[dict[str, Any]]:
    manifests = []
    packages: set[str] = set()
    qualified_ids: set[str] = set()
    for path in sorted(EXTENSIONS_DIR.glob("*.json")):
        manifest = json.loads(path.read_text())
        if manifest.get("format") != "cidx.catalog.extension/v1":
            raise SystemExit(f"extension {path} must declare cidx.catalog.extension/v1")
        package = manifest.get("package", "")
        if not re.fullmatch(r"[a-z][a-z0-9_.-]*", package):
            raise SystemExit(f"extension {path} has invalid package namespace")
        if package in packages:
            raise SystemExit(f"duplicate extension package namespace: {package}")
        packages.add(package)
        relations = manifest.get("relations")
        if not isinstance(relations, list) or not relations:
            raise SystemExit(f"extension {path} must declare relations")
        for relation in relations:
            if not isinstance(relation.get("id"), str) or ":" in relation["id"]:
                raise SystemExit(f"extension {path} relation ids must be package-local strings")
            if not relation["id"].startswith("relation/"):
                raise SystemExit(f"extension {path} relation ids must use relation/<name>")
            qualified_id = f"{package}/{relation['id']}"
            if qualified_id in qualified_ids:
                raise SystemExit(f"duplicate extension relation identity: {qualified_id}")
            qualified_ids.add(qualified_id)
            for key in ("source", "target", "inverse", "traversal", "evidence", "evidence_capabilities", "completeness"):
                if key not in relation:
                    raise SystemExit(f"extension {path} relation missing {key}")
            relation["qualified_name"] = qualified_id
        manifest["path"] = (
            str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else path.name
        )
        manifests.append(manifest)
    return manifests


def load() -> tuple[dict[str, Any], str]:
    data = json.loads(SOURCE.read_text())
    if data.get("format") != "cidx.catalog/v1":
        raise SystemExit("catalog source must declare format cidx.catalog/v1")
    if not isinstance(data.get("catalog_version"), int) or data["catalog_version"] < 1:
        raise SystemExit("catalog_version must be a positive integer")
    for group in ("symbol_kinds", "relations", "entity_kinds", "type_kinds", "type_edge_kinds", "fields", "occurrence_roles", "effect_roles"):
        rows = data.get(group)
        if not isinstance(rows, list) or not rows:
            raise SystemExit(f"catalog group {group} must be a non-empty array")
        names = [row["name"] for row in rows]
        if len(names) != len(set(names)) and group != "relations":
            raise SystemExit(f"duplicate name in catalog group {group}")
        if group != "fields":
            ids = [row["id"] for row in rows]
            if len(ids) != len(set(ids)) and group != "relations":
                raise SystemExit(f"duplicate id in catalog group {group}")
    domains = data.get("storage_enum_domains")
    if not isinstance(domains, list) or not domains:
        raise SystemExit("storage_enum_domains must be a non-empty array")
    domain_names = [domain.get("name") for domain in domains]
    if any(not isinstance(name, str) or not name for name in domain_names):
        raise SystemExit("storage enum domains must have names")
    if len(domain_names) != len(set(domain_names)):
        raise SystemExit("duplicate storage enum domain")
    for domain in domains:
        entries = domain.get("entries")
        if not isinstance(entries, list) or not entries:
            raise SystemExit(f"storage enum domain {domain['name']} must have entries")
        ids = [entry.get("id") for entry in entries]
        names = [entry.get("name") for entry in entries]
        if any(not isinstance(entry_id, int) for entry_id in ids):
            raise SystemExit(f"storage enum domain {domain['name']} ids must be integers")
        if len(ids) != len(set(ids)) or len(names) != len(set(names)):
            raise SystemExit(f"duplicate storage enum entry in {domain['name']}")
    relation_keys = [(row["layer"], row["id"]) for row in data["relations"]]
    if len(relation_keys) != len(set(relation_keys)):
        raise SystemExit("relation ids must be unique within each layer")
    for row in data["relations"]:
        required = ("source", "target", "inverse", "traversal", "evidence", "evidence_capabilities", "completeness")
        if any(key not in row for key in required):
            raise SystemExit(f"relation {row.get('name', '<unnamed>')} must declare endpoint and capability metadata")
        if not row["traversal"] or not row["evidence_capabilities"]:
            raise SystemExit(f"relation {row['name']} must declare traversal/evidence capabilities")
        if row["completeness"] not in data["statuses"]:
            raise SystemExit(f"unknown completeness status: {row['completeness']}")
        if row["evidence"] not in data["evidence_classes"] and row["evidence"] != "call_site" and row["evidence"] != "reference_site" and row["evidence"] != "declaration":
            raise SystemExit(f"unknown evidence class: {row['evidence']}")
    data["extensions"] = load_extensions()
    return data, hashlib.sha256(canonical(data).encode()).hexdigest()


def compatibility_check(data: dict[str, Any]) -> dict[str, Any]:
    current = {
        key: {str(row["id"]): row["name"] for row in data[key] if "id" in row}
        for key in ("symbol_kinds", "relations", "entity_kinds", "type_kinds", "type_edge_kinds")
    }
    if data["relations"]:
        current["relations"] = {f"{row['layer']}:{row['id']}": row["name"] for row in data["relations"]}
    baseline = json.loads(LOCK.read_text()) if LOCK.exists() else {"catalog_version": data["catalog_version"], "entries": current, "tombstones": {}}
    migrations = data.get("migrations", [])

    def migration(group: str, key: str, old: str, new: str | None, action: str | None = None) -> bool:
        return any(
            m.get("catalog") == group
            and str(m.get("id", m.get("key"))) == key
            and m.get("from") == old
            and m.get("to") == new
            and (action is None or m.get("action") == action)
            for m in migrations
        )

    tombstones = {group: dict(rows) for group, rows in baseline.get("tombstones", {}).items()}
    for group, old_rows in baseline.get("entries", {}).items():
        for key, old_name in old_rows.items():
            new_name = current.get(group, {}).get(key)
            if new_name is None:
                if not migration(group, key, old_name, None, "tombstone"):
                    raise SystemExit(f"{group} id {key} deleted without an explicit tombstone migration")
                tombstones.setdefault(group, {})[key] = old_name
            elif new_name != old_name and not migration(group, key, old_name, new_name):
                raise SystemExit(f"{group} id {key} renamed {old_name!r}->{new_name!r} without a migration entry")
    for group, old_rows in tombstones.items():
        for key, old_name in list(old_rows.items()):
            new_name = current.get(group, {}).get(key)
            if new_name is not None:
                if not migration(group, key, old_name, new_name, "reuse"):
                    raise SystemExit(f"{group} id {key} reuses tombstoned identity without an explicit reuse migration")
                del old_rows[key]
    return {"catalog_version": data["catalog_version"], "entries": current, "tombstones": tombstones}


def py_name(name: str) -> str:
    return "".join(part.capitalize() for part in name.replace("-", "_").split("_"))


def render_python(data: dict[str, Any], digest: str) -> str:
    def dict_rows(rows: list[dict[str, Any]]) -> str:
        return "{\n" + "\n".join(f"    {row['name']!r}: {row['id']}," for row in rows) + "\n}"
    relations = [(r["name"], r["layer"], r["id"]) for r in data["relations"]]
    storage_enum_ids = {
        domain["name"]: {entry["name"]: entry["id"] for entry in domain["entries"]}
        for domain in data["storage_enum_domains"]
    }
    return f'''# Generated by scripts/generate_catalogs.py; DO NOT EDIT.\nfrom dataclasses import dataclass\nfrom enum import StrEnum\n\nCATALOG_VERSION = {data["catalog_version"]}\nCATALOG_HASH = {digest!r}\n\nclass CatalogView(StrEnum):\n    SYMBOL = "symbol"\n    ENTITY = "entity"\n\nclass ResultStatus(StrEnum):\n{''.join(f'    {py_name(s).upper()} = "{s}"\n' for s in data["statuses"])}\n\nclass EvidenceClass(StrEnum):\n{''.join(f'    {py_name(s).upper()} = "{s}"\n' for s in data["evidence_classes"])}\n\nSYMBOL_KIND_IDS = {dict_rows(data["symbol_kinds"])}\nSYMBOL_KIND_NAMES = {{v: k for k, v in SYMBOL_KIND_IDS.items()}}\nSYMBOL_KINDS = frozenset(SYMBOL_KIND_IDS)\nENTITY_KIND_NAMES = tuple(row["name"] for row in {data["entity_kinds"]!r})\nTYPE_KIND_NAMES = {{row["id"]: row["name"] for row in {data["type_kinds"]!r}}}\nTYPE_EDGE_KIND_NAMES = {{row["id"]: row["name"] for row in {data["type_edge_kinds"]!r}}}\nEDGE_KINDS = {dict_rows(data["relations"][:20])}\nENTITY_EDGE_KINDS = {dict_rows(data["relations"][20:])}\nRELATION_CATALOG = {relations!r}\nFIELD_CATALOG = {[(f["name"], f["filterable"], f["is_string"]) for f in data["fields"]]!r}\nUNKNOWN_REASONS = {dict_rows(data["unknown_reasons"])}\nARTIFACT_KINDS = {dict_rows(data["artifact_kinds"])}\n\n@dataclass(frozen=True)\nclass ArtifactManifest:\n    artifact_kind: str\n    schema_version: int\n    catalog_version: int\n    catalog_hash: str\n    status: str = "complete"\n    evidence: str = "source"\n\n    def as_dict(self) -> dict[str, object]:\n        return {{"artifact_kind": self.artifact_kind, "schema_version": self.schema_version, "catalog_version": self.catalog_version, "catalog_hash": self.catalog_hash, "status": self.status, "evidence": self.evidence}}\n'''


def render_cpp(data: dict[str, Any], digest: str) -> str:
    def row(names: list[str], values: list[str]) -> str:
        return "    {" + ", ".join(f".{name} = {value}" for name, value in zip(names, values)) + "},\n"

    def named(rows: list[dict[str, Any]]) -> str:
        return "".join(row(["id", "name"], [str(item["id"]), json.dumps(item["name"])]) for item in rows)

    def array(type_name: str, name: str, rows: str, size: int) -> str:
        return (
            f"inline constexpr std::array<{type_name}, {size}> {name} = "
            + "{{\n"
            + rows
            + "}};\n"
        )

    relations = "".join(
        row(
            ["id", "name", "layer", "source", "target", "inverse", "traversal", "evidence", "evidence_capabilities", "completeness"],
            [
                str(item["id"]),
                json.dumps(item["name"]),
                "View::" + ("Symbol" if item["layer"] == "symbol" else "Entity"),
                json.dumps(item.get("source", item["layer"])),
                json.dumps(item.get("target", item["layer"])),
                json.dumps(item["inverse"]),
                json.dumps("|".join(item["traversal"])),
                json.dumps(item["evidence"]),
                json.dumps("|".join(item["evidence_capabilities"])),
                json.dumps(item["completeness"]),
            ],
        )
        for item in data["relations"]
    )
    fields = "".join(
        row(
            ["name", "filterable", "is_string"],
            [
                json.dumps(item["name"]),
                str(item["filterable"]).lower(),
                str(item["is_string"]).lower(),
            ],
        )
        for item in data["fields"]
    )
    lines = [
        "// Generated by scripts/generate_catalogs.py; DO NOT EDIT.",
        "#pragma once",
        "#include <array>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace cidx::catalog {",
        f"inline constexpr int kCatalogVersion = {data['catalog_version']};",
        f"inline constexpr std::string_view kCatalogHash = {json.dumps(digest)};",
        "enum class View : std::uint8_t { Symbol, Entity };",
        "struct NamedId { int64_t id; std::string_view name; };",
        "struct Relation { int64_t id; std::string_view name; View layer; std::string_view source; std::string_view target; std::string_view inverse; std::string_view traversal; std::string_view evidence; std::string_view evidence_capabilities; std::string_view completeness; };",
        "struct Field { std::string_view name; bool filterable; bool is_string; };",
        array("NamedId", "kSymbolKinds", named(data["symbol_kinds"]), len(data["symbol_kinds"])),
        array("NamedId", "kEntityKinds", named(data["entity_kinds"]), len(data["entity_kinds"])),
        array("NamedId", "kTypeKinds", named(data["type_kinds"]), len(data["type_kinds"])),
        array("NamedId", "kTypeEdgeKinds", named(data["type_edge_kinds"]), len(data["type_edge_kinds"])),
        array("Relation", "kRelations", relations, len(data["relations"])),
        array("Field", "kFields", fields, len(data["fields"])),
        array("NamedId", "kOccurrenceRoles", named(data["occurrence_roles"]), len(data["occurrence_roles"])),
        array("NamedId", "kEffectRoles", named(data["effect_roles"]), len(data["effect_roles"])),
        *[
            array(
                "NamedId",
                f"k{py_name(domain['name'])}s",
                named(domain["entries"]),
                len(domain["entries"]),
            )
            for domain in data["storage_enum_domains"]
        ],
        f"inline constexpr std::array<std::string_view, {len(data['statuses'])}> kStatuses = "
        + "{"
        + ", ".join(json.dumps(status) for status in data["statuses"])
        + "};",
        f"inline constexpr std::array<std::string_view, {len(data['evidence_classes'])}> kEvidenceClasses = "
        + "{"
        + ", ".join(json.dumps(evidence) for evidence in data["evidence_classes"])
        + "};",
        f"inline constexpr std::array<std::string_view, {len(data['trust_levels'])}> kTrustLevels = "
        + "{"
        + ", ".join(json.dumps(trust) for trust in data["trust_levels"])
        + "};",
        array("NamedId", "kUnknownReasons", named(data["unknown_reasons"]), len(data["unknown_reasons"])),
        array("NamedId", "kArtifactKinds", named(data["artifact_kinds"]), len(data["artifact_kinds"])),
        "} // namespace cidx::catalog",
        "",
    ]
    return "\n".join(lines)


def render_sql(data: dict[str, Any], digest: str) -> str:
    out = ["-- Generated by scripts/generate_catalogs.py; DO NOT EDIT."]
    for table, rows in (("symbol_kind", data["symbol_kinds"]), ("edge_kind", data["relations"][:20]), ("entity_edge_kind", data["relations"][20:]), ("entity_kind", data["entity_kinds"]), ("type_kind", data["type_kinds"]), ("type_edge_kind", data["type_edge_kinds"])):
        out.append(f"INSERT OR IGNORE INTO {table}(id,name) VALUES")
        values = [f"  ({r['id']},{json.dumps(r['name'])})" for r in rows]
        out.append(",\n".join(values) + ";")
    for relation in data["relations"]:
        key = f"relation:{relation['layer']}:{relation['id']}"
        value = canonical({k: relation[k] for k in ("name", "source", "target", "inverse", "traversal", "evidence", "evidence_capabilities", "completeness")})
        out.append("INSERT OR IGNORE INTO meta(key,value) VALUES (" + json.dumps(key) + ",'" + value.replace("'", "''") + "');")
    for extension in data.get("extensions", []):
        for relation in extension["relations"]:
            value = canonical({"package": extension["package"], **relation})
            key = f"extension:{relation['qualified_name']}"
            out.append("INSERT OR IGNORE INTO meta(key,value) VALUES (" + json.dumps(key) + ",'" + value.replace("'", "''") + "');")
    out.extend([
        "INSERT OR IGNORE INTO meta(key,value) VALUES ('catalog_version'," + str(data["catalog_version"]) + ");",
        "INSERT OR IGNORE INTO meta(key,value) VALUES ('catalog_hash'," + json.dumps(digest) + ");",
        "INSERT OR IGNORE INTO meta(key,value) VALUES ('artifact_kind','semantic-index');",
        "INSERT OR IGNORE INTO meta(key,value) VALUES ('status','complete');",
        "INSERT OR IGNORE INTO meta(key,value) VALUES ('trust','producer-verified');",
        "INSERT OR IGNORE INTO meta(key,value) VALUES ('evidence','source');",
    ])
    return "\n".join(out) + "\n"


def render_sql_header(data: dict[str, Any], digest: str) -> str:
    sql = render_sql(data, digest).replace('\\', '\\\\').replace('"', '\\"')
    return f'''// Generated by scripts/generate_catalogs.py; DO NOT EDIT.
#pragma once
#include <string_view>

namespace cidx::catalog {{
inline constexpr std::string_view kSeedSql = "{sql.replace(chr(10), '\\n')}";
}} // namespace cidx::catalog
'''


def render_schema(data: dict[str, Any], digest: str) -> str:
    return json.dumps({"$schema": "https://json-schema.org/draft/2020-12/schema", "title": "CIDX artifact manifest", "type": "object", "required": ["artifact_kind", "schema_version", "catalog_version", "catalog_hash", "status", "evidence"], "properties": {"artifact_kind": {"enum": [x["name"] for x in data["artifact_kinds"]]}, "schema_version": {"type": "integer", "minimum": 1}, "catalog_version": {"const": data["catalog_version"]}, "catalog_hash": {"const": digest}, "status": {"enum": data["statuses"]}, "evidence": {"enum": data["evidence_classes"]}}}, indent=2) + "\n"


def render_docs(data: dict[str, Any], digest: str) -> str:
    lines = ["# Generated CIDX semantic catalog", "", f"- Catalog version: `{data['catalog_version']}`", f"- Catalog hash: `{digest}`", "", "## Relations", "", "| Layer | ID | Name | Source | Target | Inverse | Traversal | Evidence | Evidence capabilities | Completeness |", "|---|---:|---|---|---|---|---|---|---|---|"]
    lines += [f"| {r['layer']} | {r['id']} | `{r['name']}` | `{r['source']}` | `{r['target']}` | `{r['inverse']}` | {','.join(r['traversal'])} | {r['evidence']} | {','.join(r['evidence_capabilities'])} | {r['completeness']} |" for r in data["relations"]]
    lines += ["", "## Compatibility", "", "Stable IDs are locked in `catalogs/compatibility.json`. A rename or reuse requires a migration entry in the source and a new catalog version.", ""]
    return "\n".join(lines)


def render_souffle(data: dict[str, Any], digest: str) -> str:
    lines = ["// Generated by scripts/generate_catalogs.py; DO NOT EDIT.", f"// catalog_hash = {digest}", ".decl cidx_symbol_kind(id:number, name:symbol)", ".decl cidx_relation(id:number, layer:symbol, name:symbol, source:symbol, target:symbol, inverse:symbol, traversal:symbol, evidence:symbol, evidence_capabilities:symbol, completeness:symbol)", ".decl cidx_extension_relation(qualified_name:symbol, package:symbol, id:symbol, name:symbol, layer:symbol, source:symbol, target:symbol, inverse:symbol, traversal:symbol, evidence:symbol, evidence_capabilities:symbol, completeness:symbol)", ".decl cidx_status(name:symbol)", ".decl cidx_evidence(name:symbol)", ""]
    lines += [f"#define CIDX_CATALOG_HASH \"{digest}\"", ""]
    lines += [f"cidx_symbol_kind({row['id']}, \"{row['name']}\")." for row in data["symbol_kinds"]]
    lines += [f"cidx_status(\"{s}\")." for s in data["statuses"]]
    lines += [f"cidx_evidence(\"{s}\")." for s in data["evidence_classes"]]
    lines += [f"cidx_relation({r['id']}, \"{r['layer']}\", \"{r['name']}\", \"{r['source']}\", \"{r['target']}\", \"{r['inverse']}\", \"{'|'.join(r['traversal'])}\", \"{r['evidence']}\", \"{'|'.join(r['evidence_capabilities'])}\", \"{r['completeness']}\")." for r in data["relations"]]
    lines += [f"cidx_extension_relation(\"{r['qualified_name']}\", \"{extension['package']}\", \"{r['id']}\", \"{r['name']}\", \"{r['layer']}\", \"{r['source']}\", \"{r['target']}\", \"{r['inverse']}\", \"{'|'.join(r['traversal'])}\", \"{r['evidence']}\", \"{'|'.join(r['evidence_capabilities'])}\", \"{r['completeness']}\")." for extension in data.get("extensions", []) for r in extension["relations"]]
    return "\n".join(lines) + "\n"


def render_extensions_python(data: dict[str, Any], digest: str) -> str:
    relations = {}
    for extension in data.get("extensions", []):
        for relation in extension["relations"]:
            relations[relation["qualified_name"]] = {
                "package": extension["package"],
                **{key: relation[key] for key in ("id", "name", "layer", "source", "target", "inverse", "traversal", "evidence", "evidence_capabilities", "completeness")},
            }
    return "# Generated by scripts/generate_catalogs.py; DO NOT EDIT.\n" + f"CATALOG_HASH = {digest!r}\nEXTENSION_RELATIONS = {relations!r}\nEXTENSION_RELATION_CATALOG = tuple(EXTENSION_RELATIONS.items())\n"


def render_extensions_cpp(data: dict[str, Any], digest: str) -> str:
    rows = []
    names = ("qualified_name", "package", "id", "name", "layer", "source", "target", "inverse", "traversal", "evidence", "evidence_capabilities", "completeness")
    for extension in data.get("extensions", []):
        for relation in extension["relations"]:
            values = [
                relation["qualified_name"], extension["package"], relation["id"], relation["name"],
                relation["layer"], relation["source"], relation["target"], relation["inverse"],
                "|".join(relation["traversal"]), relation["evidence"],
                "|".join(relation["evidence_capabilities"]), relation["completeness"],
            ]
            rows.append("    {" + ", ".join(f".{name} = {json.dumps(value)}" for name, value in zip(names, values)) + "},")
    return (
        "// Generated by scripts/generate_catalogs.py; DO NOT EDIT.\n"
        "#pragma once\n"
        "#include <array>\n"
        "#include <string_view>\n"
        "namespace cidx::catalog {\n"
        "struct ExtensionRelation { std::string_view qualified_name; std::string_view package; std::string_view id; std::string_view name; std::string_view layer; std::string_view source; std::string_view target; std::string_view inverse; std::string_view traversal; std::string_view evidence; std::string_view evidence_capabilities; std::string_view completeness; };\n"
        f"inline constexpr std::string_view kExtensionCatalogHash = {json.dumps(digest)};\n"
        f"inline constexpr std::array<ExtensionRelation, {len(rows)}> kExtensionRelations = {{{{\n"
        + "\n".join(rows)
        + "\n}};\n}\n"
    )


def outputs(data: dict[str, Any], digest: str) -> dict[Path, str]:
    python_catalog = render_python(data, digest)
    storage_enum_ids = {
        domain["name"]: {entry["name"]: entry["id"] for entry in domain["entries"]}
        for domain in data["storage_enum_domains"]
    }
    python_catalog = python_catalog.replace(
        "\n@dataclass",
        "\nSTORAGE_ENUM_IDS = " + repr(storage_enum_ids)
        + "\nSTORAGE_ENUM_NAMES = {domain: {value: name for name, value in entries.items()} for domain, entries in STORAGE_ENUM_IDS.items()}"
        + "\nSOURCE_KIND_IDS = STORAGE_ENUM_IDS['source_kind']"
        + "\nIDENTITY_KIND_IDS = STORAGE_ENUM_IDS['identity_kind']"
        + "\n\n@dataclass",
    )
    python_catalog = python_catalog.replace(
        "CATALOG_HASH = " + repr(digest),
        "CATALOG_HASH = " + repr(digest)
        + "\nCATALOG_SEED_SQL = " + repr(render_sql(data, digest)),
    )
    relation_metadata = {
        (r["name"], r["layer"], r["id"]): {
            "source": r.get("source", r["layer"]),
            "target": r.get("target", r["layer"]),
            "inverse": r["inverse"],
            "traversal": r["traversal"],
            "evidence": r["evidence"],
            "evidence_capabilities": r["evidence_capabilities"],
            "completeness": r["completeness"],
        }
        for r in data["relations"]
    }
    python_catalog = python_catalog.replace(
        "\nFIELD_CATALOG = ",
        "\nRELATION_METADATA = " + repr(relation_metadata) + "\nFIELD_CATALOG = ",
    )
    python_catalog = python_catalog.replace(
        "\nARTIFACT_KINDS = ",
        "\nTRUST_LEVELS = " + repr(tuple(data["trust_levels"]))
        + "\nOCCURRENCE_ROLES = "
        + repr({row["name"]: row["id"] for row in data["occurrence_roles"]})
        + "\nEFFECT_ROLES = "
        + repr({row["name"]: row["id"] for row in data["effect_roles"]})
        + "\nARTIFACT_KINDS = ",
    )
    python_catalog = python_catalog.replace(
        '    evidence: str = "source"',
        '    trust: str = "producer-verified"\n    evidence: str = "source"',
    )
    python_catalog = python_catalog.replace(
        '"status": self.status, "evidence": self.evidence',
        '"status": self.status, "trust": self.trust, "evidence": self.evidence',
    )
    artifact_schema = json.loads(render_schema(data, digest))
    artifact_schema["required"].insert(-1, "trust")
    artifact_schema["properties"]["trust"] = {"enum": data["trust_levels"]}
    return {
        ROOT / "python/indexer/generated_catalog.py": python_catalog,
        ROOT / "src/catalogs/generated_catalog.hpp": render_cpp(data, digest),
        ROOT / "src/storage/generated_catalog.sql": render_sql(data, digest),
        ROOT / "src/catalogs/generated_catalog_sql.hpp": render_sql_header(data, digest),
        ROOT / "schemas/semantic-artifact.schema.json": json.dumps(artifact_schema, indent=2) + "\n",
        ROOT / "docs/generated/semantic-catalog.md": render_docs(data, digest),
        ROOT / "src/astgraph/rules/catalog_generated.dl": render_souffle(data, digest),
        ROOT / "python/indexer/generated_extensions.py": render_extensions_python(data, digest),
        ROOT / "src/catalogs/generated_extensions.hpp": render_extensions_cpp(data, digest),
        ROOT / "tests/golden/catalog_manifest.json": json.dumps({"catalog_version": data["catalog_version"], "catalog_hash": digest, "artifact_kinds": data["artifact_kinds"]}, indent=2) + "\n",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    args = parser.parse_args()
    data, digest = load()
    lock = compatibility_check(data)
    expected = outputs(data, digest)
    if args.write:
        LOCK.parent.mkdir(parents=True, exist_ok=True)
        LOCK.write_text(json.dumps(lock, indent=2) + "\n")
        for path, content in expected.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        print(f"generated {len(expected)} files; catalog_hash={digest}")
        return 0
    mismatches = []
    if not LOCK.exists() or json.loads(LOCK.read_text()) != lock:
        mismatches.append(str(LOCK))
    for path, content in expected.items():
        if not path.exists() or path.read_text() != content:
            mismatches.append(str(path))
    if mismatches:
        print("generated catalog drift:")
        print("\n".join(f"- {path}" for path in mismatches))
        return 1
    print(f"catalog clean; catalog_hash={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
