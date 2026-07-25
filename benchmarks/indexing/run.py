#!/usr/bin/env python3
"""Reproducible semantic-indexing scale benchmark for HSE-95."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import resource
import re
import subprocess
import statistics
import sys
import tempfile
import time
from typing import Any


EXPECTED_SCHEMA_VERSION = 40
EXPECTED_CATALOG_VERSION = 1
EXPECTED_CATALOG_HASH = "21497a89add82fba96293f97b34f9a19c68912b6cc823a915889acf0709c216d"
REQUIRED_CANONICAL_SECTIONS = frozenset(
    {
        "semantic_universe",
        "translation_unit_config",
        "file",
        "file_config",
        "symbol",
        "decl_site",
        "edge",
        "edge_site",
        "call_arg",
        "template_arg",
        "template_param",
        "definition",
        "def_edge",
        "type_node",
        "type_edge",
        "parameter",
        "symbol_type",
        "include_config",
        "include_edge",
        "include_site",
        "include_macro_use",
        "diagnostic",
        "fact_applicability",
    }
)
REQUIRED_NONEMPTY_SECTIONS = frozenset(REQUIRED_CANONICAL_SECTIONS - {
    "translation_unit_config",
})


def _scoped_symbol_key(
    semantic_universe: str | None, identity_key: str | None, usr: str | None
) -> str:
    """Return the stable semantic identity used by every symbol projection."""

    def normalize_component(value: str) -> str:
        return re.sub(r"local:config:[0-9a-f]{40}",
                      "local:config:<config>", value)

    return "\x1f".join(
        (
            normalize_component(semantic_universe or "legacy"),
            normalize_component(identity_key or usr or ""),
        )
    )


def scoped_symbol_fixture() -> dict[str, Any]:
    """Prove that relationship reassignment changes a duplicate-USR digest."""

    usr = "c:@F@duplicate_fixture#"
    first = _scoped_symbol_key("fixture:first", "", usr)
    second = _scoped_symbol_key("fixture:second", "", usr)
    symbols = sorted((first, second))
    before = {"symbols": symbols, "edges": [[first, second, "calls"]]}
    after = {"symbols": symbols, "edges": [[second, first, "calls"]]}

    def digest(value: dict[str, Any]) -> str:
        return hashlib.sha256(
            json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest()

    before_digest = digest(before)
    after_digest = digest(after)
    if before_digest == after_digest:
        raise RuntimeError("scoped symbol fixture failed to detect reassignment")
    return {
        "duplicate_usr": usr,
        "relationship_reassignment_changes_digest": True,
        "before_sha256": before_digest,
        "after_sha256": after_digest,
    }


def _measure_child() -> int:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--measure-child", action="store_true")
    parser.add_argument("--stdout", required=True)
    parser.add_argument("--stderr", required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command or args.command[0] != "--":
        raise SystemExit("measurement command must follow --")

    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.monotonic()
    with open(args.stdout, "w", encoding="utf-8") as stdout, open(
        args.stderr, "w", encoding="utf-8"
    ) as stderr:
        completed = subprocess.run(
            args.command[1:], stdout=stdout, stderr=stderr, check=False
        )
    elapsed = time.monotonic() - started
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    # macOS reports bytes; Linux reports KiB.
    rss = int(after.ru_maxrss)
    if sys.platform != "darwin":
        rss *= 1024
    print(
        json.dumps(
            {
                "returncode": completed.returncode,
                "wall_seconds": elapsed,
                "cpu_seconds": (after.ru_utime - before.ru_utime)
                + (after.ru_stime - before.ru_stime),
                "user_seconds": after.ru_utime - before.ru_utime,
                "system_seconds": after.ru_stime - before.ru_stime,
                "peak_rss_bytes": rss,
            }
        )
    )
    return completed.returncode


def run_timed(
    command: list[str], env: dict[str, str], run_root: Path, label: str
) -> dict[str, Any]:
    stdout_path = run_root / f"{label}.stdout"
    stderr_path = run_root / f"{label}.stderr"
    measurement = subprocess.run(
        [
            sys.executable,
            str(Path(__file__).resolve()),
            "--measure-child",
            "--stdout",
            str(stdout_path),
            "--stderr",
            str(stderr_path),
            "--",
            *command,
        ],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    metrics = json.loads(measurement.stdout)
    metrics["command"] = command
    metrics["stdout"] = stdout_path.read_text(encoding="utf-8")
    metrics["stderr"] = stderr_path.read_text(encoding="utf-8")
    if metrics["returncode"] != 0:
        raise RuntimeError(
            f"{label} failed ({metrics['returncode']}):\n"
            f"{metrics['stdout'][-2000:]}{metrics['stderr'][-2000:]}"
        )
    return metrics


def generate_corpus(root: Path, count: int) -> tuple[Path, list[Path], Path]:
    repeated_declarations = 16
    edge_targets = 8
    root.mkdir(parents=True)
    header = root / "shared.hpp"
    header.write_text(
        "#pragma once\n"
        "namespace shared { inline int value() { return 7; } }\n",
        encoding="utf-8",
    )
    focused_header = root / "coverage.hpp"
    focused_header.write_text(
        "#pragma once\n"
        "#warning hse95 benchmark diagnostic\n"
        "#define HSE95_COVERAGE_VALUE 7\n"
        "template <typename T> inline T shared_template(T value) { return value; }\n",
        encoding="utf-8",
    )
    sources = []
    commands = []
    for index in range(count):
        source = root / f"unit_{index:04d}.cpp"
        focused_facts = ""
        if index == 0:
            focused_facts = (
                '#include "coverage.hpp"\n'
                "int callarg_source_0 = 0;\n"
                "int callarg_target_0(int value) { return value + HSE95_COVERAGE_VALUE; }\n"
                "int callarg_use_0() { return callarg_target_0(callarg_source_0); }\n"
                "template <typename T> T template_helper_0(T value) { return value; }\n"
                "int template_use_0() { return template_helper_0<int>(0); }\n"
                "using int_pointer_0 = int*;\n"
                "int pointer_use_0(int_pointer_0 value) { return *value; }\n"
            )
        source.write_text(
            (
                '#include "shared.hpp"\n'
                + "".join(
                f"int helper_{index}_{target}() {{ return {target}; }}\n"
                for target in range(edge_targets)
                )
                + f"int use_{index}() {{ return "
                + " + ".join(
                f"helper_{index}_{target}() + helper_{index}_{target}()"
                for target in range(edge_targets)
                )
                + "; }\n"
                + focused_facts
                + f"int unit_{index}();\n"
                + f"int unit_{index}() {{ return shared::value() + {index}; }}\n"
                + (f"int unit_{index}();\n" * repeated_declarations)
            ),
            encoding="utf-8",
        )
        sources.append(source)
        commands.append(
            {
                "directory": str(root),
                "file": str(source),
                "command": (
                    f"clang++ -std=c++23 -I{root} -c {source} "
                    f"-o {source.with_suffix('.o')}"
                ),
            }
        )
    compile_commands = root / "compile_commands.json"
    compile_commands.write_text(json.dumps(commands, indent=2), encoding="utf-8")
    return compile_commands, sources, header


def mutate_translation_unit(source: Path, old_value: int, new_value: int) -> None:
    text = source.read_text(encoding="utf-8")
    old = f"shared::value() + {old_value}"
    new = f"shared::value() + {new_value}"
    if text.count(old) != 1:
        raise RuntimeError(f"expected one mutation marker in {source}: {old!r}")
    source.write_text(text.replace(old, new), encoding="utf-8")


def _canonical_rows(
    connection: Any, corpus_root: Path
) -> dict[str, list[list[Any]]]:
    queries = {
        "semantic_universe": """
            SELECT key, name, policy
            FROM semantic_universe ORDER BY key
        """,
        "translation_unit_config": """
            SELECT COALESCE(descriptor_json,''),
                   COALESCE(driver,''), COALESCE(working_dir,''),
                   COALESCE(language,''), COALESCE(standard,''),
                   COALESCE(target,''), abi_options, sysroot,
                   COALESCE(resource_dir,''), include_paths, macro_state,
                   relevant_environment, generated_inputs,
                   COALESCE(diagnostics_policy,''), arguments, state
            FROM translation_unit_config ORDER BY descriptor_hash
        """,
        "file": """
            SELECT f.name, COALESCE(f.compile_options,''),
                   COALESCE(f.driver,''), f.indexed, COALESCE(f.md5,'')
            FROM file f ORDER BY f.name
        """,
        "file_config": """
            SELECT COALESCE(f.name,''), tuc.descriptor_json, fc.role,
                   fc.state, COALESCE(fc.reason,'')
            FROM file_config fc
            JOIN file f ON f.id = fc.file_id
            JOIN translation_unit_config tuc ON tuc.id = fc.config_id
            ORDER BY f.name, tuc.descriptor_hash, fc.role
        """,
        "symbol": """
            SELECT scoped_symbol_key(su.key, s.identity_key, s.usr),
                   COALESCE(su.key,'legacy'),
                   s.spelling,
                   COALESCE(s.qual_name,''),
                   COALESCE(s.display_name,''),
                   COALESCE(sk.name, CAST(s.kind AS TEXT)),
                   COALESCE(s.type_info,''), COALESCE(ff.name,''),
                   COALESCE(s.line,''), COALESCE(s.col,''),
                   COALESCE(df.name,''), COALESCE(s.decl_line,''),
                   COALESCE(s.decl_col,''), s.is_definition, s.is_pure,
                   s.is_static, s.is_instantiation, COALESCE(s.linkage,''),
                   COALESCE(s.access,''),
                   CASE WHEN parent.id IS NOT NULL THEN
                     scoped_symbol_key(parentu.key, parent.identity_key, parent.usr)
                   ELSE COALESCE(s.parent_usr,'') END,
                   s.resolved
            FROM symbol s
            LEFT JOIN symbol_kind sk ON sk.id = s.kind
            LEFT JOIN file ff ON ff.id = s.file_id
            LEFT JOIN file df ON df.id = s.decl_file_id
            LEFT JOIN semantic_universe su ON su.id = s.semantic_universe_id
            LEFT JOIN symbol parent
              ON parent.usr = s.parent_usr
             AND parent.semantic_universe_id = s.semantic_universe_id
            LEFT JOIN semantic_universe parentu
              ON parentu.id = parent.semantic_universe_id
            ORDER BY COALESCE(su.key,'legacy'), s.identity_key, s.usr
        """,
        "decl_site": """
            SELECT scoped_symbol_key(su.key, s.identity_key, s.usr),
                   COALESCE(f.name,''), COALESCE(d.line,''),
                   COALESCE(d.col,''), d.is_definition
            FROM decl_site d JOIN symbol s ON s.id = d.symbol_id
            LEFT JOIN file f ON f.id = d.file_id
            LEFT JOIN semantic_universe su ON su.id = s.semantic_universe_id
            ORDER BY scoped_symbol_key(su.key, s.identity_key, s.usr),
                     f.name, d.line, d.col
        """,
        "edge": """
            SELECT scoped_symbol_key(ssu.key, ss.identity_key, ss.usr),
                   scoped_symbol_key(dsu.key, dst.identity_key, dst.usr),
                   COALESCE(ek.name, CAST(e.kind AS TEXT)),
                   e.count, COALESCE(e.base_access,''), COALESCE(e.is_virtual,'')
            FROM edge e JOIN symbol ss ON ss.id = e.src_id
            JOIN symbol dst ON dst.id = e.dst_id
            LEFT JOIN edge_kind ek ON ek.id = e.kind
            LEFT JOIN semantic_universe ssu ON ssu.id = ss.semantic_universe_id
            LEFT JOIN semantic_universe dsu ON dsu.id = dst.semantic_universe_id
            ORDER BY scoped_symbol_key(ssu.key, ss.identity_key, ss.usr),
                     scoped_symbol_key(dsu.key, dst.identity_key, dst.usr), e.kind
        """,
        "edge_site": """
            SELECT scoped_symbol_key(ssu.key, ss.identity_key, ss.usr),
                   scoped_symbol_key(dsu.key, ds.identity_key, ds.usr),
                   COALESCE(ek.name, CAST(e.kind AS TEXT)),
                   COALESCE(f.name,''), COALESCE(es.line,''),
                   COALESCE(es.col,''), es.conditional, COALESCE(es.args_sig,''),
                   COALESCE(es.recv_src_kind,''),
                   CASE WHEN rts.id IS NOT NULL THEN
                     scoped_symbol_key(rtsu.key, rts.identity_key, rts.usr)
                   ELSE COALESCE(es.recv_type_usr, tn.decl_usr,
                                 eti.identity_text,'') END,
                   CASE WHEN rs.id IS NOT NULL THEN
                     scoped_symbol_key(rsu.key, rs.identity_key, rs.usr)
                   ELSE COALESCE(es.recv_decl_usr, edi.identity_text,'') END,
                   COALESCE(es.recv_param_pos,''),
                   COALESCE(es.recv_type_is_value,'')
            FROM edge_site es JOIN edge e ON e.id = es.edge_id
            JOIN symbol ss ON ss.id = e.src_id
            JOIN symbol ds ON ds.id = e.dst_id
            LEFT JOIN edge_kind ek ON ek.id = e.kind
            LEFT JOIN file f ON f.id = es.file_id
            LEFT JOIN semantic_universe ssu ON ssu.id = ss.semantic_universe_id
            LEFT JOIN semantic_universe dsu ON dsu.id = ds.semantic_universe_id
            LEFT JOIN type_node tn ON tn.id = es.recv_type_id
            LEFT JOIN symbol rts ON rts.id = tn.decl_id
            LEFT JOIN semantic_universe rtsu ON rtsu.id = rts.semantic_universe_id
            LEFT JOIN symbol rs ON rs.id = es.recv_decl_id
            LEFT JOIN semantic_universe rsu ON rsu.id = rs.semantic_universe_id
            LEFT JOIN external_identity eti ON eti.id = es.recv_type_identity_id
            LEFT JOIN external_identity edi ON edi.id = es.recv_decl_identity_id
            ORDER BY scoped_symbol_key(ssu.key, ss.identity_key, ss.usr),
                     scoped_symbol_key(dsu.key, ds.identity_key, ds.usr),
                     e.kind, f.name, es.line, es.col
        """,
        "call_arg": """
            SELECT scoped_symbol_key(ssu.key, ss.identity_key, ss.usr),
                   scoped_symbol_key(dsu.key, edge_dst.identity_key, edge_dst.usr),
                   COALESCE(ek.name, CAST(e.kind AS TEXT)),
                   COALESCE(f.name,''), ca.line, ca.col, ca.position,
                   ca.src_kind,
                   CASE WHEN ats.id IS NOT NULL THEN
                     scoped_symbol_key(atsu.key, ats.identity_key, ats.usr)
                   ELSE COALESCE(ca.type_usr, tn.decl_usr,
                                 eti.identity_text,'') END,
                   CASE WHEN arg_decl.id IS NOT NULL THEN
                     scoped_symbol_key(adsu.key, arg_decl.identity_key,
                                       arg_decl.usr)
                   ELSE COALESCE(ca.decl_usr, edi.identity_text,'') END,
                   CASE WHEN cs.id IS NOT NULL THEN
                     scoped_symbol_key(csu.key, cs.identity_key, cs.usr)
                   ELSE COALESCE(ca.callee_usr, eci.identity_text,'') END,
                   COALESCE(ca.type_is_value,'')
            FROM call_arg ca JOIN edge e ON e.id = ca.edge_id
            JOIN symbol ss ON ss.id = e.src_id
            JOIN symbol edge_dst ON edge_dst.id = e.dst_id
            LEFT JOIN edge_kind ek ON ek.id = e.kind
            LEFT JOIN file f ON f.id = ca.file_id
            LEFT JOIN semantic_universe ssu ON ssu.id = ss.semantic_universe_id
            LEFT JOIN semantic_universe dsu
              ON dsu.id = edge_dst.semantic_universe_id
            LEFT JOIN type_node tn ON tn.id = ca.type_id
            LEFT JOIN symbol ats ON ats.id = tn.decl_id
            LEFT JOIN semantic_universe atsu ON atsu.id = ats.semantic_universe_id
            LEFT JOIN symbol arg_decl ON arg_decl.id = ca.decl_id
            LEFT JOIN semantic_universe adsu
              ON adsu.id = arg_decl.semantic_universe_id
            LEFT JOIN symbol cs ON cs.id = ca.callee_id
            LEFT JOIN semantic_universe csu ON csu.id = cs.semantic_universe_id
            LEFT JOIN external_identity eti ON eti.id = ca.type_identity_id
            LEFT JOIN external_identity edi ON edi.id = ca.decl_identity_id
            LEFT JOIN external_identity eci ON eci.id = ca.callee_identity_id
            ORDER BY scoped_symbol_key(ssu.key, ss.identity_key, ss.usr),
                     scoped_symbol_key(dsu.key, edge_dst.identity_key,
                                       edge_dst.usr), e.kind, f.name,
                     ca.line, ca.col, ca.position
        """,
        "template_arg": """
            SELECT scoped_symbol_key(osu.key, os.identity_key, os.usr),
                   ta.position, ta.pack_index, ta.arg_kind,
                   COALESCE(scoped_symbol_key(rsu.key, rs.identity_key, rs.usr),''),
                   COALESCE(rt.type_key,''),
                   COALESCE(ta.literal,'')
            FROM template_arg ta JOIN symbol os ON os.id = ta.owner_id
            LEFT JOIN symbol rs ON rs.id = ta.ref_id
            LEFT JOIN semantic_universe osu ON osu.id = os.semantic_universe_id
            LEFT JOIN semantic_universe rsu ON rsu.id = rs.semantic_universe_id
            LEFT JOIN type_node rt ON rt.id = ta.type_id
            ORDER BY scoped_symbol_key(osu.key, os.identity_key, os.usr),
                     ta.position, ta.pack_index
        """,
        "template_param": """
            SELECT scoped_symbol_key(osu.key, os.identity_key, os.usr),
                   tp.position, tp.param_kind,
                   COALESCE(tp.name,''), COALESCE(tp.default_txt,''),
                   COALESCE(t.type_key,''), COALESCE(dt.type_key,''),
                   COALESCE(scoped_symbol_key(dsu.key, ds.identity_key, ds.usr),'')
            FROM template_param tp JOIN symbol os ON os.id = tp.owner_id
            LEFT JOIN type_node t ON t.id = tp.type_id
            LEFT JOIN type_node dt ON dt.id = tp.default_type_id
            LEFT JOIN symbol ds ON ds.id = tp.default_ref_id
            LEFT JOIN semantic_universe osu ON osu.id = os.semantic_universe_id
            LEFT JOIN semantic_universe dsu ON dsu.id = ds.semantic_universe_id
            ORDER BY scoped_symbol_key(osu.key, os.identity_key, os.usr),
                     tp.position
        """,
        "definition": """
            SELECT scoped_symbol_key(su.key, s.identity_key, s.usr),
                   COALESCE(c.name,''), COALESCE(f.name,''),
                   COALESCE(d.line,''), COALESCE(d.col,''),
                   COALESCE(d.end_line,''), COALESCE(d.end_col,''),
                   COALESCE(d.init_text,'')
            FROM definition d JOIN symbol s ON s.id = d.symbol_id
            LEFT JOIN component c ON c.id = d.component_id
            LEFT JOIN file f ON f.id = d.file_id
            LEFT JOIN semantic_universe su ON su.id = s.semantic_universe_id
            ORDER BY scoped_symbol_key(su.key, s.identity_key, s.usr),
                     c.name, f.name, d.line, d.col
        """,
        "def_edge": """
            SELECT scoped_symbol_key(ssu.key, ss.identity_key, ss.usr),
                   scoped_symbol_key(sdu.key, sd.identity_key, sd.usr),
                   COALESCE(ek.name, CAST(de.kind AS TEXT)), de.count
            FROM def_edge de
            JOIN definition d ON d.id = de.src_def_id
            JOIN symbol ss ON ss.id = d.symbol_id
            JOIN symbol sd ON sd.id = de.dst_id
            LEFT JOIN edge_kind ek ON ek.id = de.kind
            LEFT JOIN semantic_universe ssu ON ssu.id = ss.semantic_universe_id
            LEFT JOIN semantic_universe sdu ON sdu.id = sd.semantic_universe_id
            ORDER BY scoped_symbol_key(ssu.key, ss.identity_key, ss.usr),
                     scoped_symbol_key(sdu.key, sd.identity_key, sd.usr), de.kind
        """,
        "type_node": """
            SELECT tn.type_key, tn.spelling,
                   COALESCE(tk.name, CAST(tn.kind AS TEXT)), tn.is_const,
                   tn.is_volatile, tn.is_restrict,
                   CASE WHEN tds.id IS NOT NULL THEN
                     scoped_symbol_key(tdsu.key, tds.identity_key, tds.usr)
                   ELSE COALESCE(tn.decl_usr,'') END,
                   COALESCE(cn.type_key,'')
            FROM type_node tn
            LEFT JOIN type_kind tk ON tk.id = tn.kind
            LEFT JOIN type_node cn ON cn.id = tn.canonical_id
            LEFT JOIN symbol tds ON tds.id = tn.decl_id
            LEFT JOIN semantic_universe tdsu ON tdsu.id = tds.semantic_universe_id
            ORDER BY tn.type_key
        """,
        "type_edge": """
            SELECT ss.type_key, COALESCE(tek.name, CAST(te.kind AS TEXT)),
                   te.position, ds.type_key
            FROM type_edge te
            JOIN type_node ss ON ss.id = te.src_id
            JOIN type_node ds ON ds.id = te.dst_id
            LEFT JOIN type_edge_kind tek ON tek.id = te.kind
            ORDER BY ss.type_key, te.kind, te.position, ds.type_key
        """,
        "parameter": """
            SELECT scoped_symbol_key(osu.key, os.identity_key, os.usr),
                   p.position, p.pack_index, COALESCE(p.name,''),
                   COALESCE(t.type_key,''), COALESCE(dt.type_key,''),
                   COALESCE(at.type_key,''), COALESCE(p.default_text,''),
                   COALESCE(p.default_origin,''),
                   COALESCE(p.reference_semantics,''), COALESCE(f.name,''),
                   COALESCE(p.line,''), COALESCE(p.col,'')
            FROM parameter p JOIN symbol os ON os.id = p.owner_id
            LEFT JOIN type_node t ON t.id = p.type_id
            LEFT JOIN type_node dt ON dt.id = p.declared_type_id
            LEFT JOIN type_node at ON at.id = p.adjusted_type_id
            LEFT JOIN file f ON f.id = p.file_id
            LEFT JOIN semantic_universe osu ON osu.id = os.semantic_universe_id
            ORDER BY scoped_symbol_key(osu.key, os.identity_key, os.usr),
                     p.position, p.pack_index
        """,
        "symbol_type": """
            SELECT scoped_symbol_key(su.key, s.identity_key, s.usr),
                   COALESCE(stk.name, CAST(st.kind AS TEXT)),
                   tn.type_key
            FROM symbol_type st JOIN symbol s ON s.id = st.symbol_id
            JOIN type_node tn ON tn.id = st.type_id
            LEFT JOIN symbol_type_kind stk ON stk.id = st.kind
            LEFT JOIN semantic_universe su ON su.id = s.semantic_universe_id
            ORDER BY scoped_symbol_key(su.key, s.identity_key, s.usr),
                     st.kind, tn.type_key
        """,
        "include_config": """
            SELECT COALESCE(tf.name,''), COALESCE(ic.driver,''),
                   COALESCE(ic.working_dir,''),
                   COALESCE(ic.arguments,''), COALESCE(ic.lang_mode,''),
                   COALESCE(ic.resource_dir,''),
                   COALESCE(tuc.descriptor_json,'')
            FROM include_config ic
            LEFT JOIN file tf ON tf.id = ic.tu_file_id
            LEFT JOIN translation_unit_config tuc
              ON tuc.id = ic.translation_unit_config_id
            ORDER BY tf.name, ic.driver, ic.arguments
        """,
        "include_edge": """
            SELECT COALESCE(sf.name,''), COALESCE(df.name,''), ie.dst_path,
                   ie.is_system, ie.is_generated, ie.count,
                   COALESCE(ic.driver,''), COALESCE(ic.working_dir,''),
                   COALESCE(ic.arguments,'')
            FROM include_edge ie
            JOIN file sf ON sf.id = ie.src_file_id
            LEFT JOIN file df ON df.id = ie.dst_file_id
            LEFT JOIN include_config ic ON ic.id = ie.config_id
            ORDER BY sf.name, ie.dst_path, ic.arguments
        """,
        "include_site": """
            SELECT COALESCE(sf.name,''), COALESCE(df.name,''), ie.dst_path,
                   site.line, site.col,
                   site.begin_offset, site.end_offset, site.spelling,
                   site.is_angled, COALESCE(idk.name, CAST(site.directive AS TEXT)),
                   site.cond_fingerprint, site.resolved, site.guarded,
                   COALESCE(ic.arguments,'')
            FROM include_site site
            JOIN include_edge ie ON ie.id = site.edge_id
            JOIN file sf ON sf.id = ie.src_file_id
            LEFT JOIN file df ON df.id = ie.dst_file_id
            LEFT JOIN include_config ic ON ic.id = ie.config_id
            LEFT JOIN include_directive_kind idk ON idk.id = site.directive
            ORDER BY sf.name, ie.dst_path, site.line, site.col, ic.arguments
        """,
        "include_macro_use": """
            SELECT COALESCE(f.name,''), imu.def_path, imu.name,
                   imu.count, COALESCE(ic.arguments,'')
            FROM include_macro_use imu
            JOIN file f ON f.id = imu.src_file_id
            LEFT JOIN include_config ic ON ic.id = imu.config_id
            ORDER BY f.name, imu.name, imu.def_path, ic.arguments
        """,
        "diagnostic": """
            SELECT COALESCE(f.name,''), d.severity, d.spelling,
                   COALESCE(d.file_path,''), COALESCE(d.line,''),
                   COALESCE(d.col,'')
            FROM diagnostic d JOIN file f ON f.id = d.file_id
            ORDER BY f.name, d.severity, d.line, d.col, d.spelling
        """,
        "fact_applicability": """
            SELECT fa.fact_kind,
                   CASE fa.fact_kind
                     WHEN 'symbol' THEN 'symbol|' || scoped_symbol_key(
                       su.key, s.identity_key, s.usr)
                     WHEN 'edge' THEN 'edge|' || scoped_symbol_key(
                       esu.key, es.identity_key, es.usr) || '|' ||
                       scoped_symbol_key(edu.key, ed.identity_key, ed.usr)
                       || '|' || CAST(e.kind AS TEXT)
                     WHEN 'definition' THEN 'definition|' || scoped_symbol_key(
                       dsu.key, ds.identity_key, ds.usr)
                       || '|' || COALESCE(df.name,'') || '|'
                       || COALESCE(CAST(d.line AS TEXT),'') || '|'
                       || COALESCE(CAST(d.col AS TEXT),'')
                     WHEN 'decl_site' THEN 'decl_site|' || scoped_symbol_key(
                       dssu.key, dss.identity_key, dss.usr)
                       || '|' || COALESCE(dsf.name,'') || '|'
                       || COALESCE(CAST(dsite.line AS TEXT),'') || '|'
                       || COALESCE(CAST(dsite.col AS TEXT),'')
                     WHEN 'def_edge' THEN 'def_edge|' || scoped_symbol_key(
                       defsu.key, defs.identity_key, defs.usr) || '|' ||
                       scoped_symbol_key(defdu.key, defd.identity_key, defd.usr)
                       || '|'
                       || CAST(defe.kind AS TEXT)
                     WHEN 'diagnostic' THEN 'diagnostic|' ||
                       COALESCE(diaf.name, f.name, '') || '|' ||
                       COALESCE(dia.spelling, '<stale>') || '|'
                       || COALESCE(CAST(dia.line AS TEXT), '') || '|'
                       || CAST(fa.generation AS TEXT)
                     WHEN 'parameter' THEN 'parameter|' || scoped_symbol_key(
                       psu.key, ps.identity_key, ps.usr)
                     WHEN 'symbol_type' THEN 'symbol_type|' || scoped_symbol_key(
                       stsu.key, sts.identity_key, sts.usr)
                     WHEN 'type_edge' THEN 'type_edge|' || COALESCE(tes.type_key,'')
                     WHEN 'entity_node' THEN 'entity_node|' || scoped_symbol_key(
                       ensu.key, ens.identity_key, ens.usr)
                     WHEN 'entity_edge' THEN 'entity_edge|' || scoped_symbol_key(
                       eesu.key, ees.identity_key, ees.usr) || '|' ||
                       scoped_symbol_key(eedu.key, eed.identity_key, eed.usr)
                       || '|'
                       || CAST(eee.kind AS TEXT)
                     WHEN 'template_param' THEN 'template_param|' || scoped_symbol_key(
                       tpsu.key, tps.identity_key, tps.usr)
                     WHEN 'template_arg' THEN 'template_arg|' || scoped_symbol_key(
                       tasu.key, tas.identity_key, tas.usr)
                     WHEN 'call_arg' THEN 'call_arg|' || scoped_symbol_key(
                       casu.key, cas.identity_key, cas.usr) || '|' ||
                       scoped_symbol_key(cadu.key, cad.identity_key, cad.usr) || '|'
                       || CAST(cae.kind AS TEXT)
                     WHEN 'possible_call' THEN 'possible_call|' || scoped_symbol_key(
                       pcsu.key, pcs.identity_key, pcs.usr) || '|' ||
                       scoped_symbol_key(pcdu.key, pcd.identity_key, pcd.usr)
                     WHEN 'type_node' THEN 'type|' || COALESCE(tn.type_key,'')
                     ELSE NULL
                   END AS fact_key,
                   COALESCE(f.name,''), tuc.descriptor_json, fa.generation
            FROM fact_applicability fa
            JOIN file f ON f.id = fa.file_id
            JOIN translation_unit_config tuc ON tuc.id = fa.config_id
            LEFT JOIN symbol s ON fa.fact_kind = 'symbol' AND s.id = fa.fact_id
            LEFT JOIN semantic_universe su ON su.id = s.semantic_universe_id
            LEFT JOIN edge e ON fa.fact_kind = 'edge' AND e.id = fa.fact_id
            LEFT JOIN symbol es ON es.id = e.src_id
            LEFT JOIN symbol ed ON ed.id = e.dst_id
            LEFT JOIN semantic_universe esu ON esu.id = es.semantic_universe_id
            LEFT JOIN semantic_universe edu ON edu.id = ed.semantic_universe_id
            LEFT JOIN definition d
              ON fa.fact_kind = 'definition' AND d.id = fa.fact_id
            LEFT JOIN symbol ds ON ds.id = d.symbol_id
            LEFT JOIN semantic_universe dsu ON dsu.id = ds.semantic_universe_id
            LEFT JOIN file df ON df.id = d.file_id
            LEFT JOIN decl_site dsite ON fa.fact_kind = 'decl_site'
              AND dsite.rowid = fa.fact_id
            LEFT JOIN symbol dss ON dss.id = dsite.symbol_id
            LEFT JOIN semantic_universe dssu ON dssu.id = dss.semantic_universe_id
            LEFT JOIN file dsf ON dsf.id = dsite.file_id
            LEFT JOIN def_edge defe ON fa.fact_kind = 'def_edge'
              AND defe.rowid = fa.fact_id
            LEFT JOIN definition defd0 ON defd0.id = defe.src_def_id
            LEFT JOIN symbol defs ON defs.id = defd0.symbol_id
            LEFT JOIN symbol defd ON defd.id = defe.dst_id
            LEFT JOIN semantic_universe defsu
              ON defsu.id = defs.semantic_universe_id
            LEFT JOIN semantic_universe defdu
              ON defdu.id = defd.semantic_universe_id
            LEFT JOIN diagnostic dia ON fa.fact_kind = 'diagnostic'
              AND dia.id = fa.fact_id
            LEFT JOIN file diaf ON diaf.id = dia.file_id
            LEFT JOIN symbol ps ON fa.fact_kind = 'parameter'
              AND ps.id = fa.fact_id
            LEFT JOIN semantic_universe psu ON psu.id = ps.semantic_universe_id
            LEFT JOIN symbol sts ON fa.fact_kind = 'symbol_type'
              AND sts.id = fa.fact_id
            LEFT JOIN semantic_universe stsu ON stsu.id = sts.semantic_universe_id
            LEFT JOIN type_node tes ON fa.fact_kind = 'type_edge'
              AND tes.id = fa.fact_id
            LEFT JOIN entity_node en ON fa.fact_kind = 'entity_node'
              AND en.id = fa.fact_id
            LEFT JOIN symbol ens ON ens.id = en.id
            LEFT JOIN semantic_universe ensu ON ensu.id = ens.semantic_universe_id
            LEFT JOIN entity_edge eee ON fa.fact_kind = 'entity_edge'
              AND eee.rowid = fa.fact_id
            LEFT JOIN symbol ees ON ees.id = eee.src_id
            LEFT JOIN symbol eed ON eed.id = eee.dst_id
            LEFT JOIN semantic_universe eesu ON eesu.id = ees.semantic_universe_id
            LEFT JOIN semantic_universe eedu ON eedu.id = eed.semantic_universe_id
            LEFT JOIN symbol tps ON fa.fact_kind = 'template_param'
              AND tps.id = fa.fact_id
            LEFT JOIN semantic_universe tpsu ON tpsu.id = tps.semantic_universe_id
            LEFT JOIN symbol tas ON fa.fact_kind = 'template_arg'
              AND tas.id = fa.fact_id
            LEFT JOIN semantic_universe tasu ON tasu.id = tas.semantic_universe_id
            LEFT JOIN edge cae ON fa.fact_kind = 'call_arg'
              AND cae.id = fa.fact_id
            LEFT JOIN symbol cas ON cas.id = cae.src_id
            LEFT JOIN symbol cad ON cad.id = cae.dst_id
            LEFT JOIN semantic_universe casu ON casu.id = cas.semantic_universe_id
            LEFT JOIN semantic_universe cadu ON cadu.id = cad.semantic_universe_id
            LEFT JOIN definition pcsd ON fa.fact_kind = 'possible_call'
              AND pcsd.id = fa.fact_id
            LEFT JOIN symbol pcs ON pcs.id = pcsd.symbol_id
            LEFT JOIN semantic_universe pcsu ON pcsu.id = pcs.semantic_universe_id
            LEFT JOIN possible_call pc ON fa.fact_kind = 'possible_call'
              AND pc.src_def_id = fa.fact_id
            LEFT JOIN definition pcdd ON pcdd.id = pc.dst_def_id
            LEFT JOIN symbol pcd ON pcd.id = pcdd.symbol_id
            LEFT JOIN semantic_universe pcdu ON pcdu.id = pcd.semantic_universe_id
            LEFT JOIN type_node tn
              ON fa.fact_kind = 'type_node' AND tn.id = fa.fact_id
            ORDER BY fa.fact_kind, fact_key, f.name, tuc.descriptor_hash,
                     fa.generation
        """,
    }

    connection.create_function("scoped_symbol_key", 3, _scoped_symbol_key)
    root_text = str(corpus_root)

    def normalize(value: Any) -> Any:
        if isinstance(value, str):
            return re.sub(
                r"build:[0-9a-f]{40}",
                "build:<build>",
                value.replace(root_text, "<corpus>"),
            )
        return value

    canonical = {
        name: [[normalize(value) for value in row]
               for row in connection.execute(query).fetchall()]
        for name, query in queries.items()
    }
    unresolved = [
        row for row in canonical["fact_applicability"] if row[1] is None
    ]
    if unresolved:
        raise RuntimeError(
            "fact_applicability contains an unresolved fact family: "
            f"{unresolved[0][0]}"
        )
    return canonical


def database_snapshot(
    database: Path,
    corpus_root: Path,
    require_coverage: bool,
    capture_canonical: bool = True,
) -> dict[str, Any]:
    import sqlite3

    with sqlite3.connect(database) as connection:
        integrity = connection.execute("PRAGMA integrity_check").fetchone()[0]
        foreign_key_violations = connection.execute(
            "PRAGMA foreign_key_check"
        ).fetchall()
        metadata = dict(
            connection.execute(
                "SELECT key, value FROM meta WHERE key IN "
                "('schema_version', 'catalog_version', 'catalog_hash')"
            ).fetchall()
        )
        canonical = _canonical_rows(connection, corpus_root) if capture_canonical else {}
        if capture_canonical:
            missing_sections = sorted(
                REQUIRED_CANONICAL_SECTIONS.difference(canonical)
            )
            if missing_sections:
                raise RuntimeError(
                    f"canonical projection is missing sections: {missing_sections}"
                )
            empty_sections = sorted(
                section for section in REQUIRED_NONEMPTY_SECTIONS
                if not canonical[section]
            )
            if require_coverage and empty_sections:
                raise RuntimeError(
                    "benchmark corpus did not exercise canonical sections: "
                    f"{empty_sections}"
                )
        canonical_json = json.dumps(
            canonical, sort_keys=True, separators=(",", ":")
        )
        page_count = connection.execute("PRAGMA page_count").fetchone()[0]
        page_size = connection.execute("PRAGMA page_size").fetchone()[0]
        freelist = connection.execute("PRAGMA freelist_count").fetchone()[0]
        tables = {}
        for table in (
            "file", "symbol", "edge", "edge_site", "call_arg",
            "definition", "def_edge", "file_config", "fact_applicability",
            "include_config", "include_edge", "include_site",
            "include_macro_use", "diagnostic", "template_arg",
            "template_param", "type_edge", "parameter",
        ):
            try:
                tables[table] = connection.execute(
                    f"SELECT COUNT(*) FROM {table}"
                ).fetchone()[0]
            except sqlite3.OperationalError:
                tables[table] = None
    if integrity != "ok" or foreign_key_violations:
        raise RuntimeError(f"SQLite integrity check failed: {integrity}")
    if int(metadata.get("schema_version", -1)) != EXPECTED_SCHEMA_VERSION:
        raise RuntimeError(
            f"unexpected schema version: {metadata.get('schema_version')}"
        )
    if int(metadata.get("catalog_version", -1)) != EXPECTED_CATALOG_VERSION:
        raise RuntimeError(
            f"unexpected catalog version: {metadata.get('catalog_version')}"
        )
    if metadata.get("catalog_hash") != EXPECTED_CATALOG_HASH:
        raise RuntimeError(
            f"unexpected catalog hash: {metadata.get('catalog_hash')}"
        )
    return {
        "integrity_check": integrity,
        "foreign_key_check": "ok" if not foreign_key_violations else "failed",
        "schema_version": int(metadata["schema_version"]),
        "catalog_version": int(metadata["catalog_version"]),
        "catalog_hash": metadata["catalog_hash"],
            "canonical_sections": sorted(canonical),
            "canonical_nonempty_sections": sorted(
                name for name, rows in canonical.items() if rows
            ),
        "canonical_row_counts": {
            name: len(rows) for name, rows in canonical.items()
        },
        "canonical_sha256": hashlib.sha256(
            canonical_json.encode("utf-8")
        ).hexdigest(),
        "page_count": page_count,
        "page_size": page_size,
        "page_bytes": page_count * page_size,
        "freelist_pages": freelist,
        "rows": tables,
    }


def parse_header_counts(output: str) -> dict[str, int]:
    counts = {"indexed": 0, "already": 0, "system": 0, "unowned": 0}
    for line in output.splitlines():
        match = re.search(
            r"headers:\s+(\d+) indexed.*?,\s+(\d+) already,\s+"
            r"(\d+) system,\s+(\d+) unowned",
            line,
        )
        if match:
            counts["indexed"] += int(match.group(1))
            counts["already"] += int(match.group(2))
            counts["system"] += int(match.group(3))
            counts["unowned"] += int(match.group(4))
    return counts


def stage(
    cidx: Path,
    cache: Path,
    run_root: Path,
    corpus_root: Path,
    label: str,
    args: list[str],
    previous_db: dict[str, Any] | None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    env = dict(os.environ)
    env["INDEXER_CACHE"] = str(cache)
    metrics = run_timed([str(cidx), *args], env, run_root, label)
    metrics["label"] = label
    database = cache / "index.db"
    snapshot = database_snapshot(
        database,
        corpus_root,
        require_coverage=label != "import",
        capture_canonical=not label.startswith("tu-"),
    )
    metrics["sqlite"] = {
        "snapshot": snapshot,
        "delta": snapshot if previous_db is None else {
            "page_bytes": snapshot["page_bytes"] - previous_db["page_bytes"],
            "rows": {
                key: snapshot["rows"][key] - previous_db["rows"][key]
                for key in snapshot["rows"]
                if snapshot["rows"][key] is not None
                and previous_db["rows"][key] is not None
            },
        },
    }
    metrics["header_counts"] = parse_header_counts(
        metrics["stdout"] + metrics["stderr"]
    )
    return metrics, snapshot


def run_case(cidx: Path, count: int, per_tu: int, case_root: Path) -> dict[str, Any]:
    source_root = case_root / "corpus"
    compile_commands, sources, header = generate_corpus(source_root, count)
    cache = case_root / "cache"
    cache.mkdir()
    stages = []
    previous_db = None
    for label, args in (
        ("import", ["import", "--db", str(compile_commands), "--name", "hse95"]),
        ("index-cold", ["index"]),
        ("resolve", ["resolve"]),
        ("index-warm", ["index"]),
    ):
        measured, previous_db = stage(
            cidx, cache, case_root, source_root, label, args, previous_db
        )
        stages.append(measured)

    changed = sources[0]
    mutate_translation_unit(changed, 0, 1)
    measured, previous_db = stage(
        cidx,
        cache,
        case_root,
        source_root,
        "index-incremental",
        ["index", str(changed)],
        previous_db,
    )
    stages.append(measured)

    per_tu_results = []
    for index in range(min(per_tu, count)):
        source = sources[index]
        mutate_translation_unit(source, index + 1 if index == 0 else index,
                                index + 2 if index == 0 else index + 1)
        measured, previous_db = stage(
            cidx,
            cache,
            case_root,
            source_root,
            f"tu-{index:04d}",
            ["index", str(source)],
            previous_db,
        )
        per_tu_results.append(
            {
                "file": source.name,
                "wall_seconds": measured["wall_seconds"],
                "cpu_seconds": measured["cpu_seconds"],
                "peak_rss_bytes": measured["peak_rss_bytes"],
            }
        )

    return {
        "files": count,
        "shared_header": header.name,
        "shared_header_fan_in": count,
        "stages": [
            {
                key: value
                for key, value in measured.items()
                if key not in ("stdout", "stderr", "command")
            }
            for measured in stages
        ],
        "per_tu": per_tu_results,
    }


def comparison(
    baseline: dict[str, Any], current: dict[str, Any]
) -> dict[str, Any]:
    result: dict[str, Any] = {}
    failures: list[str] = []

    def snapshots(stage: dict[str, Any]) -> list[dict[str, Any]]:
        return stage["sqlite"].get(
            "snapshot_trials", [stage["sqlite"]["snapshot"]]
        )

    def signature(snapshot: dict[str, Any]) -> tuple[Any, ...]:
        return (
            snapshot["canonical_sha256"],
            tuple(snapshot["canonical_sections"]),
            snapshot["integrity_check"],
            snapshot["foreign_key_check"],
            snapshot["schema_version"],
            snapshot["catalog_version"],
            snapshot["catalog_hash"],
        )

    for stage_name in ("index-cold", "index-warm", "index-incremental"):
        old = next(item for item in baseline["stages"] if item["label"] == stage_name)
        new = next(item for item in current["stages"] if item["label"] == stage_name)
        old_snapshots = snapshots(old)
        new_snapshots = snapshots(new)
        old_repeat = len({signature(item) for item in old_snapshots}) == 1
        new_repeat = len({signature(item) for item in new_snapshots}) == 1
        trial_matches = [
            index < len(new_snapshots)
            and signature(old_snapshot) == signature(new_snapshots[index])
            for index, old_snapshot in enumerate(old_snapshots)
        ]
        same_trial_count = len(old_snapshots) == len(new_snapshots)
        canonical_match = (
            same_trial_count and old_repeat and new_repeat
            and all(trial_matches)
        )
        integrity_match = all(
            item["integrity_check"] == "ok"
            and item["foreign_key_check"] == "ok"
            for item in (*old_snapshots, *new_snapshots)
        )
        schema_catalog_match = all(
            item["schema_version"] == EXPECTED_SCHEMA_VERSION
            and item["catalog_version"] == EXPECTED_CATALOG_VERSION
            and item["catalog_hash"] == EXPECTED_CATALOG_HASH
            for item in (*old_snapshots, *new_snapshots)
        )
        result[stage_name] = {
            "baseline_wall_seconds": old["wall_seconds"],
            "current_wall_seconds": new["wall_seconds"],
            "wall_delta_seconds": new["wall_seconds"] - old["wall_seconds"],
            "wall_improvement_percent": (
                (old["wall_seconds"] - new["wall_seconds"])
                / old["wall_seconds"]
                * 100
                if old["wall_seconds"]
                else None
            ),
            "cpu_utilization": {
                "baseline": old["cpu_seconds"] / old["wall_seconds"],
                "current": new["cpu_seconds"] / new["wall_seconds"],
            },
            "trial_count_match": same_trial_count,
            "baseline_repeat_consistent": old_repeat,
            "current_repeat_consistent": new_repeat,
            "canonical_semantic_trial_matches": trial_matches,
            "canonical_semantic_match": canonical_match,
            "database_integrity_match": integrity_match,
            "schema_catalog_match": schema_catalog_match,
        }
        if not same_trial_count:
            failures.append(f"{stage_name}: baseline/current trial counts differ")
        if not old_repeat:
            failures.append(f"{stage_name}: baseline trials are not repeat-consistent")
        if not new_repeat:
            failures.append(f"{stage_name}: current trials are not repeat-consistent")
        if not all(trial_matches):
            failures.append(f"{stage_name}: one or more baseline/current trials differ")
        if not integrity_match:
            failures.append(f"{stage_name}: database integrity mismatch")
        if not schema_catalog_match:
            failures.append(f"{stage_name}: schema/catalog mismatch")
    result["parity_failures"] = failures
    return result


def aggregate_cases(cases: list[dict[str, Any]]) -> dict[str, Any]:
    first = cases[0]

    def median(values: list[float | int]) -> float | int:
        return statistics.median(values)

    stages = []
    for stage_index, first_stage in enumerate(first["stages"]):
        stage_values = [case["stages"][stage_index] for case in cases]
        first_snapshot = first_stage["sqlite"]["snapshot"]
        stages.append(
            {
                "label": first_stage["label"],
                "returncode": 0,
                "wall_seconds": median(
                    [stage["wall_seconds"] for stage in stage_values]
                ),
                "wall_seconds_trials": [
                    stage["wall_seconds"] for stage in stage_values
                ],
                "cpu_seconds": median(
                    [stage["cpu_seconds"] for stage in stage_values]
                ),
                "cpu_seconds_trials": [
                    stage["cpu_seconds"] for stage in stage_values
                ],
                "user_seconds": median(
                    [stage["user_seconds"] for stage in stage_values]
                ),
                "system_seconds": median(
                    [stage["system_seconds"] for stage in stage_values]
                ),
                "peak_rss_bytes": median(
                    [stage["peak_rss_bytes"] for stage in stage_values]
                ),
                "cpu_utilization": median(
                    [stage["cpu_seconds"] / stage["wall_seconds"]
                     for stage in stage_values]
                ),
                "sqlite": {
                    "snapshot": first_snapshot,
                    "snapshot_trials": [
                        stage["sqlite"]["snapshot"] for stage in stage_values
                    ],
                    "delta": {
                        "page_bytes": median(
                            [stage["sqlite"]["delta"]["page_bytes"]
                             for stage in stage_values]
                        ),
                        "rows": {
                            key: median(
                                [stage["sqlite"]["delta"]["rows"][key]
                                 for stage in stage_values]
                            )
                            for key in stage_values[0]["sqlite"]["delta"]["rows"]
                        },
                    },
                },
                "canonical_sha256_trials": [
                    stage["sqlite"]["snapshot"]["canonical_sha256"]
                    for stage in stage_values
                ],
                "header_counts": {
                    key: median(
                        [stage["header_counts"][key] for stage in stage_values]
                    )
                    for key in stage_values[0]["header_counts"]
                },
            }
        )

    return {
        "files": first["files"],
        "shared_header": first["shared_header"],
        "shared_header_fan_in": first["shared_header_fan_in"],
        "stages": stages,
        "per_tu": [
            {
                "file": first["per_tu"][tu_index]["file"],
                "wall_seconds": median(
                    [case["per_tu"][tu_index]["wall_seconds"]
                     for case in cases]
                ),
                "wall_seconds_trials": [
                    case["per_tu"][tu_index]["wall_seconds"]
                    for case in cases
                ],
                "cpu_seconds": median(
                    [case["per_tu"][tu_index]["cpu_seconds"]
                     for case in cases]
                ),
                "peak_rss_bytes": median(
                    [case["per_tu"][tu_index]["peak_rss_bytes"]
                     for case in cases]
                ),
            }
            for tu_index in range(len(first["per_tu"]))
        ],
    }


def main() -> int:
    if "--measure-child" in sys.argv[1:]:
        return _measure_child()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-cidx", type=Path)
    parser.add_argument("--current-cidx", type=Path, required=True)
    parser.add_argument("--representative-files", type=int, default=32)
    parser.add_argument("--scale-files", type=int, default=1000)
    parser.add_argument("--per-tu", type=int, default=5)
    parser.add_argument("--trials", type=int, default=1)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.trials < 1:
        raise SystemExit("--trials must be positive")

    executables = {"current": args.current_cidx}
    if args.baseline_cidx:
        executables["baseline"] = args.baseline_cidx
    report: dict[str, Any] = {
        "method": "HSE-95",
        "environment": {
            "platform": platform.platform(),
            "python": platform.python_version(),
        },
        "usability_target": {
            "scale_cold_wall_seconds_max": 900,
            "scale_warm_wall_seconds_max": 5,
            "incremental_wall_seconds_max": 2,
        },
        "cases": {},
        "aggregates": {},
        "scoped_symbol_fixture": scoped_symbol_fixture(),
    }
    with tempfile.TemporaryDirectory(prefix="hse95-indexing-") as temporary:
        root = Path(temporary)
        for trial in range(args.trials):
            for name, executable in executables.items():
                for count in dict.fromkeys(
                    (args.representative_files, args.scale_files)
                ):
                    case_root = root / f"trial-{trial + 1}" / name / str(count)
                    case_root.mkdir(parents=True)
                    report["cases"][f"trial-{trial + 1}:{name}:{count}"] = run_case(
                        executable, count, args.per_tu, case_root
                    )

    for name in executables:
        for count in dict.fromkeys(
            (args.representative_files, args.scale_files)
        ):
            report["aggregates"][f"{name}:{count}"] = aggregate_cases(
                [
                    report["cases"][f"trial-{trial + 1}:{name}:{count}"]
                    for trial in range(args.trials)
                ]
            )

    if args.baseline_cidx:
        report["comparison"] = {}
        parity_failures: list[str] = []
        for count in dict.fromkeys(
            (args.representative_files, args.scale_files)
        ):
            count_comparison = comparison(
                report["aggregates"][f"baseline:{count}"],
                report["aggregates"][f"current:{count}"],
            )
            parity_failures.extend(
                f"{count} files: {failure}"
                for failure in count_comparison.pop("parity_failures")
            )
            report["comparison"][str(count)] = count_comparison
        report["parity_failures"] = parity_failures
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(args.output)
    if report.get("parity_failures"):
        print("semantic parity failures:", file=sys.stderr)
        print("\n".join(report["parity_failures"]), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
