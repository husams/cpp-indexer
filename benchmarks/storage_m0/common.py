"""Shared deterministic helpers for the Storage M0 benchmark."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import sqlite3
import subprocess
import sys
from pathlib import Path
from typing import Any

from . import BENCHMARK_VERSION, SCHEMA_VERSION


def canonical_json(value: Any) -> str:
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
        "count": len(values), "min_ms": round(min(values), 6),
        "p50_ms": percentile(values, 0.50), "p95_ms": percentile(values, 0.95),
        "p99_ms": percentile(values, 0.99), "max_ms": round(max(values), 6),
    }


def system_profile() -> dict[str, Any]:
    """Capture concrete host facts used to bind comparable measurements."""
    root = Path(__file__).resolve().parents[2]
    usage = shutil.disk_usage(root)
    memory = None
    try:
        memory = int(os.sysconf("SC_PAGE_SIZE")) * int(os.sysconf("SC_PHYS_PAGES"))
    except (AttributeError, OSError, ValueError):
        pass
    return {
        "os": platform.system(), "os_release": platform.release(),
        "architecture": platform.machine(), "cpu_model": platform.processor() or None,
        "cpu_count": os.cpu_count(), "memory_bytes": memory,
        "filesystem_total_bytes": usage.total, "filesystem_free_bytes": usage.free,
        "python": platform.python_version(), "sqlite": sqlite3.sqlite_version,
        "compiler": platform.python_compiler(),
    }


def hardware_fingerprint(environment: dict[str, Any]) -> str:
    keys = ("os", "os_release", "architecture", "cpu_model", "cpu_count",
            "memory_bytes", "sqlite", "sqlite_compile_options")
    return sha256(canonical_json({key: environment.get(key) for key in keys}))


def sqlite_compile_options(connection: sqlite3.Connection) -> list[str]:
    try:
        return sorted(row[0] for row in connection.execute("PRAGMA compile_options"))
    except sqlite3.DatabaseError:
        return []


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def load_cidx_schema() -> tuple[str, dict[str, int]]:
    python_root = repo_root() / "python"
    if str(python_root) not in sys.path:
        sys.path.insert(0, str(python_root))
    from indexer.storage import SCHEMA_VERSION as current_version
    from indexer.storage import SYMBOL_KIND_IDS, _SCHEMA
    if current_version != SCHEMA_VERSION:
        raise RuntimeError(f"benchmark requires schema v{SCHEMA_VERSION}, found v{current_version}")
    return _SCHEMA, SYMBOL_KIND_IDS


# Every row is projected onto external semantic keys before hashing.  The SQL
# ORDER BY clauses make the digest independent of SQLite rowids and insertion
# order while the fetchmany loop keeps memory bounded for the 500M plan.
SEMANTIC_QUERIES: dict[str, str] = {
    "repository": "SELECT name, kind, remote_url FROM repository ORDER BY name, kind, remote_url",
    "clone": "SELECT r.name, c.path, c.label FROM clone c JOIN repository r ON r.id=c.repository_id ORDER BY r.name,c.path,c.label",
    "component": "SELECT COALESCE(r.name,''), c.name, c.path, c.kind, c.version FROM component c LEFT JOIN repository r ON r.id=c.repository_id ORDER BY 1,2,3,4,5",
    "directory": "SELECT COALESCE(r.name,''), c.name, c.path, d.path FROM directory d JOIN component c ON c.id=d.component_id LEFT JOIN repository r ON r.id=c.repository_id ORDER BY 1,2,3,4",
    "file": "SELECT COALESCE(r.name,''), c.name, c.path, d.path, f.name, f.mtime, f.md5, f.compile_options, f.driver, f.indexed, f.indexed_at, f.args_overridden FROM file f JOIN directory d ON d.id=f.directory_id JOIN component c ON c.id=d.component_id LEFT JOIN repository r ON r.id=c.repository_id ORDER BY 1,2,3,4,5",
    "symbol": "SELECT s.usr,s.spelling,s.qual_name,s.display_name,s.kind,s.type_info,COALESCE(r.name,''),c.name,c.path,d.path,f.name,s.line,s.col,s.end_line,s.end_col,COALESCE(dr.name,''),COALESCE(dc.name,''),COALESCE(dc.path,''),COALESCE(dd.path,''),COALESCE(df.name,''),s.decl_line,s.decl_col,s.decl_path,s.is_definition,s.is_pure,s.is_static,s.is_instantiation,s.is_named_instance,s.linkage,s.access,s.parent_usr,s.resolved,s.multi_def,s.const_value FROM symbol s LEFT JOIN file f ON f.id=s.file_id LEFT JOIN directory d ON d.id=f.directory_id LEFT JOIN component c ON c.id=d.component_id LEFT JOIN repository r ON r.id=c.repository_id LEFT JOIN file df ON df.id=s.decl_file_id LEFT JOIN directory dd ON dd.id=df.directory_id LEFT JOIN component dc ON dc.id=dd.component_id LEFT JOIN repository dr ON dr.id=dc.repository_id ORDER BY s.usr",
    "decl_site": "SELECT s.usr,COALESCE(r.name,''),c.name,c.path,d.path,f.name,ds.line,ds.col,ds.end_line,ds.end_col,ds.is_definition FROM decl_site ds JOIN symbol s ON s.id=ds.symbol_id LEFT JOIN file f ON f.id=ds.file_id LEFT JOIN directory d ON d.id=f.directory_id LEFT JOIN component c ON c.id=d.component_id LEFT JOIN repository r ON r.id=c.repository_id ORDER BY 1,2,3,4,5,6,7,8",
    "edge": "SELECT ss.usr,ds.usr,e.kind,e.count,e.base_access,e.is_virtual,e.vtable_slot FROM edge e JOIN symbol ss ON ss.id=e.src_id JOIN symbol ds ON ds.id=e.dst_id ORDER BY 1,2,3",
    "edge_site": "SELECT ss.usr,ds.usr,e.kind,COALESCE(r.name,''),c.name,c.path,d.path,f.name,es.line,es.col,es.conditional,es.args_sig,es.recv_src_kind,es.recv_type_usr,es.recv_decl_usr,es.recv_param_pos,es.recv_type_is_value FROM edge_site es JOIN edge e ON e.id=es.edge_id JOIN symbol ss ON ss.id=e.src_id JOIN symbol ds ON ds.id=e.dst_id JOIN file f ON f.id=es.file_id JOIN directory d ON d.id=f.directory_id JOIN component c ON c.id=d.component_id LEFT JOIN repository r ON r.id=c.repository_id ORDER BY 1,2,3,4,5,6,7,8,9,10",
    "definition": "SELECT s.usr,COALESCE(r.name,''),c.name,c.path,f.name,d.line,d.col,d.end_line,d.end_col,d.init_text FROM definition d JOIN symbol s ON s.id=d.symbol_id LEFT JOIN component c ON c.id=d.component_id LEFT JOIN repository r ON r.id=c.repository_id LEFT JOIN file f ON f.id=d.file_id ORDER BY 1,2,3,4,5,6,7",
    "def_edge": "SELECT sd.usr,COALESCE(r.name,''),c.name,c.path,f.name,df.line,df.col,df.end_line,df.end_col,dd.usr,de.kind,de.count FROM def_edge de JOIN definition df ON df.id=de.src_def_id JOIN symbol sd ON sd.id=df.symbol_id JOIN symbol dd ON dd.id=de.dst_id LEFT JOIN component c ON c.id=df.component_id LEFT JOIN repository r ON r.id=c.repository_id LEFT JOIN file f ON f.id=df.file_id ORDER BY 1,2,3,4,5,6,7,10",
    "possible_call": "SELECT ss.usr,COALESCE(cc.name,''),cc.path,cf.name,cs.line,cs.col,cs.end_line,cs.end_col,ds.usr,COALESCE(dc.name,''),dc.path,df.name,dd.line,dd.col,dd.end_line,dd.end_col,pc.count FROM possible_call pc JOIN definition cs ON cs.id=pc.src_def_id JOIN symbol ss ON ss.id=cs.symbol_id LEFT JOIN component cc ON cc.id=cs.component_id LEFT JOIN file cf ON cf.id=cs.file_id LEFT JOIN definition dd ON dd.id=pc.dst_def_id JOIN symbol ds ON ds.id=dd.symbol_id LEFT JOIN component dc ON dc.id=dd.component_id LEFT JOIN file df ON df.id=dd.file_id ORDER BY 1,2,3,4,5,6,9,10",
    "call_arg": "SELECT ss.usr,ds.usr,e.kind,COALESCE(r.name,''),c.name,c.path,d.path,f.name,ca.line,ca.col,ca.position,ca.src_kind,ca.type_usr,ca.decl_usr,ca.callee_usr,ca.type_is_value FROM call_arg ca JOIN edge e ON e.id=ca.edge_id JOIN symbol ss ON ss.id=e.src_id JOIN symbol ds ON ds.id=e.dst_id JOIN file f ON f.id=ca.file_id JOIN directory d ON d.id=f.directory_id JOIN component c ON c.id=d.component_id LEFT JOIN repository r ON r.id=c.repository_id ORDER BY 1,2,3,4,5,6,7,8,9,10,11",
    "type_node": "SELECT t.type_key,t.spelling,t.kind,t.is_const,t.is_volatile,t.is_restrict,t.decl_usr,ct.type_key FROM type_node t LEFT JOIN type_node ct ON ct.id=t.canonical_id ORDER BY 1",
    "type_edge": "SELECT st.type_key,te.kind,te.position,dt.type_key FROM type_edge te JOIN type_node st ON st.id=te.src_id JOIN type_node dt ON dt.id=te.dst_id ORDER BY 1,2,3,4",
    "symbol_type": "SELECT s.usr,st.kind,t.type_key FROM symbol_type st JOIN symbol s ON s.id=st.symbol_id JOIN type_node t ON t.id=st.type_id ORDER BY 1,2,3",
    "parameter": "SELECT s.usr,p.position,p.pack_index,p.name,COALESCE(t.type_key,''),COALESCE(dt.type_key,''),COALESCE(at.type_key,''),p.default_text,p.default_origin,p.reference_semantics,COALESCE(r.name,''),c.name,c.path,d.path,f.name,p.line,p.col FROM parameter p JOIN symbol s ON s.id=p.owner_id LEFT JOIN type_node t ON t.id=p.type_id LEFT JOIN type_node dt ON dt.id=p.declared_type_id LEFT JOIN type_node at ON at.id=p.adjusted_type_id LEFT JOIN file f ON f.id=p.file_id LEFT JOIN directory d ON d.id=f.directory_id LEFT JOIN component c ON c.id=d.component_id LEFT JOIN repository r ON r.id=c.repository_id ORDER BY 1,2,3",
    "template_param": "SELECT s.usr,tp.position,tp.param_kind,tp.name,tp.default_txt,COALESCE(t.type_key,''),COALESCE(dt.type_key,''),COALESCE(ds.usr,'') FROM template_param tp JOIN symbol s ON s.id=tp.owner_id LEFT JOIN type_node t ON t.id=tp.type_id LEFT JOIN type_node dt ON dt.id=tp.default_type_id LEFT JOIN symbol ds ON ds.id=tp.default_ref_id ORDER BY 1,2",
    "template_arg": "SELECT s.usr,ta.position,ta.pack_index,ta.arg_kind,COALESCE(rs.usr,''),ta.literal,COALESCE(t.type_key,'') FROM template_arg ta JOIN symbol s ON s.id=ta.owner_id LEFT JOIN symbol rs ON rs.id=ta.ref_id LEFT JOIN type_node t ON t.id=ta.type_id ORDER BY 1,2,3",
    "entity_node": "SELECT s.usr,en.kind FROM entity_node en JOIN symbol s ON s.id=en.id ORDER BY 1",
    "entity_edge": "SELECT ss.usr,ds.usr,ee.kind,ee.count,COALESCE(ms.usr,''),ee.multiplicity,ee.access,ee.is_virtual,ee.create_form,ee.partial FROM entity_edge ee JOIN symbol ss ON ss.id=ee.src_id JOIN symbol ds ON ds.id=ee.dst_id LEFT JOIN symbol ms ON ms.id=ee.via_member_id ORDER BY 1,2,3",
    "include_config": "SELECT COALESCE(r.name,''),c.name,c.path,d.path,f.name,ic.digest,ic.driver,ic.working_dir,ic.arguments,ic.lang_mode,ic.resource_dir FROM include_config ic JOIN file f ON f.id=ic.tu_file_id JOIN directory d ON d.id=f.directory_id JOIN component c ON c.id=d.component_id LEFT JOIN repository r ON r.id=c.repository_id ORDER BY 1,2,3,4,5,6",
    "include_edge": "SELECT COALESCE(r.name,''),c.name,c.path,d.path,sf.name,COALESCE(dr.name,''),dc.name,dc.path,dd.path,df.name,ie.dst_path,ic.digest,ie.is_system,ie.is_generated,ie.count FROM include_edge ie JOIN file sf ON sf.id=ie.src_file_id JOIN directory d ON d.id=sf.directory_id JOIN component c ON c.id=d.component_id LEFT JOIN repository r ON r.id=c.repository_id LEFT JOIN file df ON df.id=ie.dst_file_id LEFT JOIN directory dd ON dd.id=df.directory_id LEFT JOIN component dc ON dc.id=dd.component_id LEFT JOIN repository dr ON dr.id=dc.repository_id JOIN include_config ic ON ic.id=ie.config_id ORDER BY 1,2,3,4,5,11,12",
    "include_site": "SELECT COALESCE(r.name,''),c.name,c.path,d.path,sf.name,ie.dst_path,ic.digest,ins.line,ins.col,ins.begin_offset,ins.end_offset,ins.spelling,ins.is_angled,ins.directive,ins.cond_fingerprint,ins.resolved,ins.guarded FROM include_site ins JOIN include_edge ie ON ie.id=ins.edge_id JOIN file sf ON sf.id=ie.src_file_id JOIN directory d ON d.id=sf.directory_id JOIN component c ON c.id=d.component_id LEFT JOIN repository r ON r.id=c.repository_id JOIN include_config ic ON ic.id=ie.config_id ORDER BY 1,2,3,4,5,6,7,8,9,10",
    "include_macro_use": "SELECT COALESCE(r.name,''),c.name,c.path,d.path,f.name,im.def_path,im.name,ic.digest,im.count FROM include_macro_use im JOIN file f ON f.id=im.src_file_id JOIN directory d ON d.id=f.directory_id JOIN component c ON c.id=d.component_id LEFT JOIN repository r ON r.id=c.repository_id JOIN include_config ic ON ic.id=im.config_id ORDER BY 1,2,3,4,5,6,7,8",
    "diagnostic": "SELECT COALESCE(r.name,''),c.name,c.path,d.path,f.name,di.severity,di.spelling,di.file_path,di.line,di.col FROM diagnostic di JOIN file f ON f.id=di.file_id JOIN directory d ON d.id=f.directory_id JOIN component c ON c.id=d.component_id LEFT JOIN repository r ON r.id=c.repository_id ORDER BY 1,2,3,4,5,6,7,8,9,10",
    "label": "SELECT name,path FROM label ORDER BY name",
}

CATALOG_QUERIES = {
    "edge_kind": "SELECT id,name FROM edge_kind ORDER BY id,name",
    "entity_edge_kind": "SELECT id,name FROM entity_edge_kind ORDER BY id,name",
    "entity_kind": "SELECT id,name FROM entity_kind ORDER BY id,name",
    "include_directive_kind": "SELECT id,name FROM include_directive_kind ORDER BY id,name",
    "symbol_kind": "SELECT id,name FROM symbol_kind ORDER BY id,name",
    "symbol_type_kind": "SELECT id,name FROM symbol_type_kind ORDER BY id,name",
    "type_edge_kind": "SELECT id,name FROM type_edge_kind ORDER BY id,name",
    "type_kind": "SELECT id,name FROM type_kind ORDER BY id,name",
}


def _table_exists(connection: sqlite3.Connection, name: str) -> bool:
    return connection.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (name,)
    ).fetchone() is not None


def semantic_digest(path: Path, *, chunk_size: int = 512) -> str:
    """Hash the complete v34 semantic contract with stable keys, streaming rows."""
    digest = hashlib.sha256()
    connection = sqlite3.connect(path)
    try:
        for table, query in (*SEMANTIC_QUERIES.items(), *CATALOG_QUERIES.items()):
            if not _table_exists(connection, table):
                continue
            digest.update(canonical_json({"table": table}).encode("utf-8"))
            digest.update(b"\n")
            cursor = connection.execute(query)
            while rows := cursor.fetchmany(chunk_size):
                for row in rows:
                    digest.update(canonical_json(list(row)).encode("utf-8"))
                    digest.update(b"\n")
        return digest.hexdigest()
    finally:
        connection.close()


def require_result_version(result: dict[str, Any]) -> None:
    if result.get("result_version") != "storage-m0/result-v2":
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
