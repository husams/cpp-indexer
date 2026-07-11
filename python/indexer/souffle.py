"""Souffle Datalog analyses over the semantic index (`cidx analyze`).

Exports bounded TSV fact files from index.db, prepends the shared prelude
declarations (rules/prelude.dl), runs the standalone ``souffle`` interpreter,
and returns the program's ``.output`` relations.  Behavior must stay
byte-compatible with the C++ implementation in src/cli/analyze.cpp.
"""

from __future__ import annotations

import os
import re
import shutil
import sqlite3
import subprocess
import tempfile

from .storage import SCHEMA_VERSION

RULES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rules")

#: Built-in rules shipped with cidx, in presentation order.  The same names
#: and descriptions are embedded in the C++ binary; keep them in sync.
BUILTIN_RULES = [
    ("callgraph",
     "direct and transitive call graph (outputs: call, call_transitive)"),
    ("cycles",
     "call cycles over calls/dispatch_calls edges "
     "(outputs: cycle_member, cycle_edge)"),
    ("unused",
     "defined functions with no incoming call or override edge "
     "(outputs: unused)"),
]

SOUFFLE_TIMEOUT = 600.0

#: Exported fact relations: (name, SELECT statement).  Column order must
#: match the declarations in rules/prelude.dl; NULL numbers become 0 and
#: NULL strings ''.  ORDER BY keeps fact files deterministic.
FACT_QUERIES = [
    ("symbol",
     "SELECT id, usr, spelling, COALESCE(qual_name, ''), kind, is_definition,"
     " COALESCE(file_id, 0), COALESCE(line, 0) FROM symbol ORDER BY id"),
    ("symbol_kind", "SELECT id, name FROM symbol_kind ORDER BY id"),
    ("edge",
     "SELECT id, src_id, dst_id, kind, count FROM edge ORDER BY id"),
    ("edge_kind", "SELECT id, name FROM edge_kind ORDER BY id"),
    ("edge_site",
     "SELECT edge_id, file_id, COALESCE(line, 0), COALESCE(col, 0)"
     " FROM edge_site ORDER BY edge_id, file_id, line, col"),
    ("entity_node", "SELECT id, kind FROM entity_node ORDER BY id"),
    ("entity_kind", "SELECT id, name FROM entity_kind ORDER BY id"),
    ("entity_edge",
     "SELECT src_id, dst_id, kind, COALESCE(via_member_id, 0), access,"
     " is_virtual FROM entity_edge"
     " ORDER BY src_id, dst_id, kind, COALESCE(via_member_id, 0)"),
    ("entity_edge_kind",
     "SELECT id, name FROM entity_edge_kind ORDER BY id"),
    ("file",
     "SELECT f.id, c.path ||"
     " CASE WHEN d.path = '' THEN '' ELSE '/' || d.path END || '/' || f.name"
     " FROM file f"
     " JOIN directory d ON f.directory_id = d.id"
     " JOIN component c ON d.component_id = c.id"
     " ORDER BY f.id"),
]


class SouffleError(Exception):
    """Analysis failure surfaced to the user as `error: ...`, exit 1."""


def builtin_rule_path(name: str) -> str:
    return os.path.join(RULES_DIR, name + ".dl")


def prelude_text() -> str:
    with open(os.path.join(RULES_DIR, "prelude.dl"), encoding="utf-8") as fh:
        return fh.read()


def find_souffle() -> str | None:
    exe = os.environ.get("CIDX_SOUFFLE")
    if exe:
        return exe
    return shutil.which("souffle")


def _clean(text: str) -> str:
    """TSV fact files cannot carry tabs or newlines; flatten to spaces."""
    return text.replace("\t", " ").replace("\n", " ").replace("\r", " ")


def _open_ro(db_path: str) -> sqlite3.Connection:
    conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        row = conn.execute(
            "SELECT value FROM meta WHERE key = 'schema_version'").fetchone()
    except sqlite3.DatabaseError as e:
        conn.close()
        raise SouffleError(f"cannot read index at {db_path}: {e}")
    if row is None or row[0] != str(SCHEMA_VERSION):
        got = "none" if row is None else row[0]
        conn.close()
        raise SouffleError(
            f"unsupported index schema version: {got}"
            f" (expected {SCHEMA_VERSION}); run 'cidx db migrate'")
    return conn


def export_facts(db_path: str, out_dir: str) -> tuple[int, int]:
    """Write one TSV file per fact relation plus cidx_facts.dl into out_dir.

    Returns (fact file count, total row count).
    """
    conn = _open_ro(db_path)
    try:
        os.makedirs(out_dir, exist_ok=True)
        total = 0
        for name, sql in FACT_QUERIES:
            with open(os.path.join(out_dir, name + ".facts"), "w",
                      encoding="utf-8") as fh:
                for row in conn.execute(sql):
                    cells = [_clean(v) if isinstance(v, str) else str(v)
                             for v in row]
                    fh.write("\t".join(cells) + "\n")
                    total += 1
        with open(os.path.join(out_dir, "cidx_facts.dl"), "w",
                  encoding="utf-8") as fh:
            fh.write(prelude_text())
        return len(FACT_QUERIES), total
    finally:
        conn.close()


def output_relations(program: str) -> list[str]:
    """Names of the program's .output relations, sorted and deduplicated."""
    names = re.findall(r"^\s*\.output\s+([A-Za-z_][A-Za-z0-9_]*)",
                       program, flags=re.MULTILINE)
    return sorted(set(names))


def _read_relation(path: str) -> list[list[str]]:
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return sorted(line.split("\t") for line in lines)


def run_program(db_path: str, program: str, jobs: int) -> dict:
    """Run prelude + program through souffle; return {relation: rows}."""
    exe = find_souffle()
    if exe is None:
        raise SouffleError(
            "souffle executable not found; install souffle or set "
            "CIDX_SOUFFLE")
    full = prelude_text() + "\n" + program
    with tempfile.TemporaryDirectory(prefix="cidx-analyze-") as tmp:
        facts_dir = os.path.join(tmp, "facts")
        out_dir = os.path.join(tmp, "out")
        os.makedirs(out_dir)
        export_facts(db_path, facts_dir)
        program_path = os.path.join(tmp, "program.dl")
        with open(program_path, "w", encoding="utf-8") as fh:
            fh.write(full)
        argv = [exe, "-F", facts_dir, "-D", out_dir, "-j", str(jobs),
                program_path]
        try:
            res = subprocess.run(argv, capture_output=True, text=True,
                                 timeout=SOUFFLE_TIMEOUT)
        except OSError:
            raise SouffleError(
                "souffle executable not found; install souffle or set "
                "CIDX_SOUFFLE")
        except subprocess.SubprocessError:
            raise SouffleError(
                f"souffle timed out after {int(SOUFFLE_TIMEOUT)}s")
        if res.returncode != 0:
            detail = res.stderr.strip() or res.stdout.strip()
            raise SouffleError(
                f"souffle failed (exit {res.returncode}): {detail}")
        relations = {}
        for name in output_relations(full):
            relations[name] = _read_relation(
                os.path.join(out_dir, name + ".csv"))
        return relations
