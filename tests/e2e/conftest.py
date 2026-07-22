"""Shared fixtures and step definitions for the cidx BDD end-to-end suite.

Contract of this suite
----------------------
* The index is produced **only** through the `cidx` command line -- the same
  four-command pipeline a user runs (`init` -> `import` -> `index` -> `resolve`).
  No Python indexing, no direct Storage writes, no libclang in-process.
* Every scenario gets its **own clean `index.db`** in its own `tmp_path`
  workspace, so no scenario can observe another's rows.
* Assertions go exclusively through the public Python graph-query API
  (`indexer.query.GraphQuery`).
* The *expected* symbols, edges and signature facts are stated in the Gherkin
  feature files as data tables. This module only knows how to compare; it never
  hard-codes what a fixture should contain.

Table cell conventions (used by every `Then` table below)
---------------------------------------------------------
    true / false      -> bool
    -                 -> None (absent / not set)
    "" (empty cell)   -> None
    123               -> int
    anything else     -> str

Symbol selectors (the quoted name in scalar steps and in edge tables)
---------------------------------------------------------------------
    add                     qualified name, or spelling; must be unique
    add@11                  ... narrowed to the symbol declared on line 11
    add@11:1                ... narrowed to line 11, column 1
    usr:c:@F@add<#d>#d#d#   exact USR (always unambiguous)
"""

from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterator, Optional

import pytest
from pytest_bdd import given, parsers, then, when

E2E_DIR = Path(__file__).resolve().parent
REPO_ROOT = E2E_DIR.parent.parent
FIXTURES_DIR = E2E_DIR / "fixtures"

# The query API lives in the python/ tree and is imported, not installed, so the
# suite runs against the working copy.
sys.path.insert(0, str(REPO_ROOT / "python"))

from indexer.query import GraphQuery  # noqa: E402


# --------------------------------------------------------------------------- #
# Session fixtures: the CLI under test
# --------------------------------------------------------------------------- #


@pytest.fixture(scope="session")
def cidx_bin() -> Path:
    """Path to the cidx binary under test ($CIDX_BIN, else build/cidx)."""
    env = os.environ.get("CIDX_BIN")
    candidate = Path(env) if env else REPO_ROOT / "build" / "cidx"
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        pytest.fail(
            f"cidx binary not found at {candidate} -- build it first "
            f"(cmake -S . -B build && cmake --build build -j) or set $CIDX_BIN"
        )
    return candidate.resolve()


# --------------------------------------------------------------------------- #
# The per-scenario workspace
# --------------------------------------------------------------------------- #


@dataclass
class Workspace:
    """One scenario's isolated indexing workspace and its query handle."""

    root: Path
    cache: Path
    src_dir: Path
    cidx: Path
    fixture: str = ""
    source: Optional[Path] = None
    commands: list[dict[str, Any]] = field(default_factory=list)
    _graph: Optional[GraphQuery] = None
    _symbols: Optional[list] = None

    # -- CLI ---------------------------------------------------------------- #

    @property
    def db(self) -> Path:
        return self.cache / "index.db"

    def run(self, *args: str) -> subprocess.CompletedProcess:
        """Run one cidx subcommand in the workspace with an isolated cache."""
        env = dict(os.environ)
        env["INDEXER_CACHE"] = str(self.cache)
        proc = subprocess.run(
            [str(self.cidx), *args],
            cwd=self.root,
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.commands.append(
            {
                "argv": ["cidx", *args],
                "returncode": proc.returncode,
                "stdout": proc.stdout,
                "stderr": proc.stderr,
            }
        )
        return proc

    def run_ok(self, *args: str) -> subprocess.CompletedProcess:
        proc = self.run(*args)
        assert proc.returncode == 0, (
            f"`cidx {' '.join(args)}` failed with exit {proc.returncode}\n"
            f"--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}"
        )
        return proc

    @property
    def cli_output(self) -> str:
        return "\n".join(c["stdout"] + c["stderr"] for c in self.commands)

    # -- query -------------------------------------------------------------- #

    @property
    def graph(self) -> GraphQuery:
        assert self.db.is_file(), f"no index database at {self.db}"
        if self._graph is None:
            self._graph = GraphQuery(str(self.db))
        return self._graph

    def close(self) -> None:
        if self._graph is not None:
            self._graph.close()
            self._graph = None

    # -- symbol access ------------------------------------------------------ #

    def symbols(self) -> list:
        """Every symbol, ordered by id (cached for the scenario)."""
        if self._symbols is None:
            g = self.graph
            expected_count = g.stats()["symbols"]
            self._symbols = sorted(
                g.find("", limit=expected_count), key=lambda symbol: symbol.id
            )
            assert len(self._symbols) == expected_count, (
                "public graph query returned an incomplete symbol set: "
                f"expected {expected_count}, got {len(self._symbols)}"
            )
        return self._symbols

    def resolve(self, selector: str):
        """Resolve a symbol selector (see module docstring) to exactly one Sym."""
        selector = selector.strip()
        syms = self.symbols()

        if selector.startswith("usr:"):
            want = selector[4:]
            hits = [s for s in syms if s.usr == want]
            if len(hits) == 1:
                return hits[0]
            raise AssertionError(
                f"selector {selector!r} matched {len(hits)} symbols; "
                f"known USRs: {sorted(s.usr for s in syms)}"
            )

        name, _, where = selector.partition("@")
        line = col = None
        if where:
            line_s, _, col_s = where.partition(":")
            line = int(line_s)
            col = int(col_s) if col_s else None

        def narrow(candidates: list) -> list:
            if line is not None:
                candidates = [s for s in candidates if s.line == line]
            if col is not None:
                candidates = [s for s in candidates if s.col == col]
            return candidates

        # Qualified name wins over spelling: a constructor's spelling is its
        # class name, so "PointClass" must mean the class, not PointClass().
        hits: list = []
        for tier in (
            [s for s in syms if s.name == name],
            [s for s in syms if s.spelling == name],
        ):
            hits = narrow(tier)
            if len(hits) == 1:
                return hits[0]
            if hits:
                break

        detail = "\n".join(
            f"    {s.name!r} kind={s.kind} at {s.line}:{s.col} usr={s.usr}"
            for s in syms
            if name in (s.name, s.spelling)
        )
        raise AssertionError(
            f"selector {selector!r} matched {len(hits)} symbols, expected 1.\n"
            f"  candidates with that name:\n{detail or '    (none)'}\n"
            f"  disambiguate with name@line, name@line:col, or usr:<usr>"
        )

    def edges(self) -> list[tuple]:
        """Every edge as (src Sym, Edge), deduplicated by edge id."""
        g = self.graph
        seen: dict[int, tuple] = {}
        for s in self.symbols():
            for e in g.edges_out(s, limit=5000):
                seen[e.edge_id] = (s, e)
        return [seen[k] for k in sorted(seen)]


@pytest.fixture
def workspace(tmp_path: Path, cidx_bin: Path) -> Iterator[Workspace]:
    """A clean, per-scenario workspace holding its own index.db."""
    ws = Workspace(
        root=tmp_path / "ws",
        cache=tmp_path / "ws" / "cache",
        src_dir=tmp_path / "ws" / "src",
        cidx=cidx_bin,
    )
    ws.src_dir.mkdir(parents=True)
    ws.cache.mkdir(parents=True)
    yield ws
    ws.close()


# --------------------------------------------------------------------------- #
# Table helpers
# --------------------------------------------------------------------------- #


def _coerce(cell: str) -> Any:
    """Gherkin cell -> Python value (see module docstring)."""
    text = cell.strip()
    if text in ("", "-"):
        return None
    if text == "true":
        return True
    if text == "false":
        return False
    if text.lstrip("+-").isdigit():
        return int(text)
    return text


def _rows(datatable: list[list[str]]) -> list[dict[str, Any]]:
    """Gherkin data table -> list of {column: coerced value}, header row first."""
    assert datatable and len(datatable) >= 1, "expected a data table"
    header = [h.strip() for h in datatable[0]]
    assert all(header), f"table contains an empty header cell: {datatable[0]!r}"
    duplicates = sorted({h for h in header if header.count(h) > 1})
    assert not duplicates, f"table contains duplicate column(s): {duplicates}"
    out = []
    for raw in datatable[1:]:
        assert len(raw) == len(header), (
            f"row {raw!r} has {len(raw)} cells but the header has {len(header)}"
        )
        out.append({h: _coerce(c) for h, c in zip(header, raw)})
    return out


def _sym_facts(sym) -> dict[str, Any]:
    """The comparable view of a symbol: every column a feature file may assert."""
    return {
        "usr": sym.usr,
        "spelling": sym.spelling,
        "qual_name": sym.name,
        "kind": sym.kind,
        "type_info": sym.type_info,
        "file": sym.file.name if sym.file else None,
        "line": sym.line,
        "col": sym.col,
        "end_line": sym.end_line,
        "end_col": sym.end_col,
        "is_definition": bool(sym.is_definition),
        "is_instantiation": bool(sym.is_instantiation),
        "is_static": bool(sym.is_static),
        "is_pure": bool(sym.is_pure),
        "is_stub": bool(sym.is_stub),
        "access": sym.access,
    }


def _describe(sym) -> str:
    return f"{sym.name} [{sym.kind}] at {sym.file.name if sym.file else '?'}:{sym.line}:{sym.col}"


def _match_one(ws: Workspace, expected: dict[str, Any], what: str):
    """Exactly one symbol must agree with every column stated in the row."""
    known = set(_sym_facts(ws.symbols()[0]).keys()) if ws.symbols() else set()
    unknown = set(expected) - known if known else set()
    assert not unknown, (
        f"unknown symbol column(s) {sorted(unknown)}; known: {sorted(known)}"
    )

    hits = [
        s
        for s in ws.symbols()
        if all(_sym_facts(s)[k] == v for k, v in expected.items())
    ]
    if len(hits) == 1:
        return hits[0]

    # Build a focused diff against the closest candidate (same name, if any).
    name = expected.get("qual_name") or expected.get("spelling") or expected.get("usr")
    near = [
        s for s in ws.symbols() if name in (s.name, s.spelling, s.usr) or name is None
    ]
    detail = "\n".join(
        "      " + json.dumps({k: _sym_facts(s).get(k) for k in expected}, default=str)
        for s in near
    )
    raise AssertionError(
        f"{what}: expected exactly 1 symbol matching\n"
        f"      {json.dumps(expected, default=str)}\n"
        f"    but found {len(hits)}. Actual rows with a matching name:\n"
        f"{detail or '      (no symbol with that name)'}\n"
        f"    all indexed symbols:\n"
        + "\n".join(f"      {_describe(s)}" for s in ws.symbols())
    )


# --------------------------------------------------------------------------- #
# GIVEN -- the workspace and the fixture under test
# --------------------------------------------------------------------------- #


@given(parsers.parse('a clean index workspace for fixture "{fixture}"'))
def a_clean_workspace(workspace: Workspace, fixture: str) -> Workspace:
    src = FIXTURES_DIR / fixture
    assert src.is_file(), (
        f"fixture {fixture!r} not found in {FIXTURES_DIR}; "
        f"available: {sorted(p.name for p in FIXTURES_DIR.glob('*.cpp'))}"
    )
    workspace.fixture = fixture
    workspace.source = workspace.src_dir / fixture
    shutil.copyfile(src, workspace.source)

    compile_commands = [
        {
            "directory": str(workspace.src_dir),
            "file": str(workspace.source),
            "arguments": ["clang++", "-std=c++17", "-c", fixture],
        }
    ]
    (workspace.root / "compile_commands.json").write_text(
        json.dumps(compile_commands, indent=2) + "\n"
    )
    assert not workspace.db.exists(), "workspace must start without an index.db"
    return workspace


# --------------------------------------------------------------------------- #
# WHEN -- drive the CLI
# --------------------------------------------------------------------------- #


@when("I build the index with the cidx CLI")
def build_the_index(workspace: Workspace) -> None:
    """The canonical four-command pipeline, exactly as documented for users."""
    workspace.run_ok("init")
    workspace.run_ok("import", "--db", str(workspace.root), "--name", "fixture")
    workspace.run_ok("index")
    workspace.run_ok("resolve")


# --------------------------------------------------------------------------- #
# THEN -- index-level facts
# --------------------------------------------------------------------------- #


@then("the index database exists")
def index_db_exists(workspace: Workspace) -> None:
    assert workspace.db.is_file(), f"expected an index database at {workspace.db}"


@then("the entity graph is resolved")
def graph_is_resolved(workspace: Workspace) -> None:
    stats = workspace.graph.stats()
    assert stats.get("resolved_at"), (
        f"graph_resolved_at is unset -- `cidx resolve` did not complete. stats={stats}"
    )


@then(parsers.parse("the index holds {count:d} indexed file"))
@then(parsers.parse("the index holds {count:d} indexed files"))
def index_file_count(workspace: Workspace, count: int) -> None:
    actual = workspace.graph.stats()["files_indexed"]
    assert actual == count, f"expected {count} indexed file(s), got {actual}"


@then(parsers.parse("the index holds exactly {count:d} symbol"))
@then(parsers.parse("the index holds exactly {count:d} symbols"))
def index_symbol_count(workspace: Workspace, count: int) -> None:
    syms = workspace.symbols()
    assert len(syms) == count, (
        f"expected {count} symbol(s), got {len(syms)}:\n"
        + "\n".join(f"    {_describe(s)}" for s in syms)
    )


@then(parsers.parse("the index holds exactly {count:d} edge"))
@then(parsers.parse("the index holds exactly {count:d} edges"))
def index_edge_count(workspace: Workspace, count: int) -> None:
    edges = workspace.edges()
    assert len(edges) == count, (
        f"expected {count} edge(s), got {len(edges)}:\n"
        + "\n".join(f"    {_edge_repr(s, e)}" for s, e in edges)
    )


@then("the index holds no edges")
def index_no_edges(workspace: Workspace) -> None:
    edges = workspace.edges()
    assert not edges, "expected no edges, got:\n" + "\n".join(
        f"    {_edge_repr(s, e)}" for s, e in edges
    )


@then(parsers.parse("the index holds no {kind} edges"))
def index_no_edges_of_kind(workspace: Workspace, kind: str) -> None:
    hits = [(s, e) for s, e in workspace.edges() if e.kind == kind]
    assert not hits, f"expected no {kind} edges, got:\n" + "\n".join(
        f"    {_edge_repr(s, e)}" for s, e in hits
    )


# --------------------------------------------------------------------------- #
# THEN -- symbols
# --------------------------------------------------------------------------- #


@then("the index holds the symbols:")
def index_holds_symbols(workspace: Workspace, datatable: list[list[str]]) -> None:
    """Each row must match exactly one indexed symbol on every stated column."""
    for row in _rows(datatable):
        _match_one(workspace, row, "symbol table")


@then("the index holds exactly these symbols:")
def index_holds_exactly_symbols(
    workspace: Workspace, datatable: list[list[str]]
) -> None:
    """As above, and the table must account for every symbol in the index."""
    rows = _rows(datatable)
    matched: set[int] = set()
    for row in rows:
        symbol = _match_one(workspace, row, "symbol table")
        assert symbol.id not in matched, (
            "the exact symbol table mentions the same indexed symbol more than once: "
            f"{_describe(symbol)}"
        )
        matched.add(symbol.id)
    extra = [s for s in workspace.symbols() if s.id not in matched]
    assert not extra, "index holds symbols the table does not mention:\n" + "\n".join(
        f"    {_describe(s)}  usr={s.usr}" for s in extra
    )


@then(parsers.parse('symbol "{selector}" spans lines {start:d} to {end:d}'))
def symbol_spans(workspace: Workspace, selector: str, start: int, end: int) -> None:
    sym = workspace.resolve(selector)
    assert (sym.line, sym.end_line) == (start, end), (
        f"{_describe(sym)}: expected span {start}..{end}, "
        f"got {sym.line}..{sym.end_line}"
    )


@then(parsers.parse('symbol "{selector}" returns "{type_name}"'))
def symbol_returns(workspace: Workspace, selector: str, type_name: str) -> None:
    sym = workspace.resolve(selector)
    sig = workspace.graph.signature(sym)
    actual = sig.returns.spelling if sig.returns else None
    assert actual == type_name, (
        f"{_describe(sym)}: expected return type {type_name!r}, got {actual!r}"
    )


@then(parsers.parse('symbol "{selector}" has type "{type_name}"'))
def symbol_of_type(workspace: Workspace, selector: str, type_name: str) -> None:
    """The declared type of a variable/field (signature tier `of_type`)."""
    sym = workspace.resolve(selector)
    sig = workspace.graph.signature(sym)
    actual = sig.of_type.spelling if sig.of_type else None
    assert actual == type_name, (
        f"{_describe(sym)}: expected declared type {type_name!r}, got {actual!r}"
    )


@then(parsers.parse('symbol "{selector}" takes the parameters:'))
def symbol_parameters(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    sig = workspace.graph.signature(sym)
    actual = [
        {
            "position": p.position,
            "name": p.name,
            "type": p.type.spelling if p.type else None,
        }
        for p in sig.params
    ]
    expected = _rows(datatable)
    _assert_same(expected, actual, f"parameters of {_describe(sym)}")


@then(parsers.parse('symbol "{selector}" takes no parameters'))
def symbol_no_parameters(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    params = workspace.graph.signature(sym).params
    assert not params, f"{_describe(sym)}: expected no parameters, got {params}"


@then(parsers.parse('symbol "{selector}" declares the template parameters:'))
def symbol_template_params(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    actual = [
        {"position": p.position, "name": p.name, "kind": p.kind_name}
        for p in workspace.graph.template_params(sym)
    ]
    _assert_same(_rows(datatable), actual, f"template parameters of {_describe(sym)}")


@then(parsers.parse('symbol "{selector}" binds the template arguments:'))
def symbol_template_args(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    actual = [
        {"position": a.position, "kind": a.kind_name, "value": a.literal}
        for a in workspace.graph.template_args(sym)
    ]
    _assert_same(_rows(datatable), actual, f"template arguments of {_describe(sym)}")


@then(parsers.parse('symbol "{selector}" binds no template arguments'))
def symbol_no_template_args(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    args = workspace.graph.template_args(sym)
    assert not args, f"{_describe(sym)}: expected no template arguments, got {args}"


@then(parsers.parse('symbol "{selector}" is an instantiation of "{primary}"'))
def symbol_template_of(workspace: Workspace, selector: str, primary: str) -> None:
    sym = workspace.resolve(selector)
    want = workspace.resolve(primary)
    got = workspace.graph.template_of(sym)
    assert got is not None, f"{_describe(sym)}: template_of() returned None"
    assert got.id == want.id, (
        f"{_describe(sym)}: expected instantiation of {_describe(want)}, "
        f"got {_describe(got)}"
    )


@then(parsers.parse('symbol "{selector}" has {count:d} instantiation'))
@then(parsers.parse('symbol "{selector}" has {count:d} instantiations'))
def symbol_instantiation_count(workspace: Workspace, selector: str, count: int) -> None:
    sym = workspace.resolve(selector)
    inst = workspace.graph.instantiations(sym)
    assert len(inst) == count, (
        f"{_describe(sym)}: expected {count} instantiation(s), got {len(inst)}:\n"
        + "\n".join(f"    {_describe(i)}" for i in inst)
    )


@then(parsers.parse('symbol "{selector}" has the definitions:'))
def symbol_definitions(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    actual = [
        {
            "file": d.file.name if d.file else None,
            "line": d.line,
            "end_line": d.end_line,
            "component": d.component,
        }
        for d in workspace.graph.definitions(sym)
    ]
    _assert_same(_rows(datatable), actual, f"definitions of {_describe(sym)}")


# --------------------------------------------------------------------------- #
# THEN -- edges and the call graph
# --------------------------------------------------------------------------- #


def _edge_repr(src, edge) -> str:
    sites = ",".join(f"{s.line}:{s.col}" for s in edge.sites)
    return (
        f"{src.name}#{src.id} --{edge.kind}(count={edge.count})--> "
        f"{edge.peer.name}#{edge.peer.id}" + (f"  @[{sites}]" if sites else "")
    )


@then("the index holds the edges:")
def index_holds_edges(workspace: Workspace, datatable: list[list[str]]) -> None:
    _check_edges(workspace, datatable, exhaustive=False)


@then("the index holds exactly these edges:")
def index_holds_exactly_edges(workspace: Workspace, datatable: list[list[str]]) -> None:
    _check_edges(workspace, datatable, exhaustive=True)


def _check_edges(
    ws: Workspace, datatable: list[list[str]], *, exhaustive: bool
) -> None:
    """Table columns: src | kind | dst | [count] | [sites]. src/dst are selectors."""
    actual = ws.edges()
    rendered = [_edge_repr(s, e) for s, e in actual]
    matched: set[int] = set()

    for row in _rows(datatable):
        unknown = set(row) - {"src", "kind", "dst", "count", "sites"}
        assert not unknown, f"unknown edge column(s) {sorted(unknown)}"
        src = ws.resolve(row["src"])
        dst = ws.resolve(row["dst"])
        hits = [
            (s, e)
            for s, e in actual
            if s.id == src.id and e.peer.id == dst.id and e.kind == row["kind"]
        ]
        assert len(hits) == 1, (
            f"expected exactly 1 {row['kind']!r} edge "
            f"{row['src']} -> {row['dst']}, found {len(hits)}.\n"
            "  all edges:\n" + "\n".join(f"    {r}" for r in rendered)
        )
        s, e = hits[0]
        assert e.edge_id not in matched, (
            "the edge table mentions the same indexed edge more than once: "
            f"{_edge_repr(s, e)}"
        )
        if row.get("count") is not None:
            assert e.count == row["count"], (
                f"edge {_edge_repr(s, e)}: expected count {row['count']}, got {e.count}"
            )
        if "sites" in row:
            want = row["sites"]
            got = ",".join(f"{x.line}:{x.col}" for x in e.sites) or None
            assert got == want, (
                f"edge {_edge_repr(s, e)}: expected sites {want!r}, got {got!r}"
            )
        matched.add(e.edge_id)

    if exhaustive:
        extra = [(s, e) for s, e in actual if e.edge_id not in matched]
        assert not extra, "index holds edges the table does not mention:\n" + "\n".join(
            f"    {_edge_repr(s, e)}" for s, e in extra
        )


def _sites(ws: Workspace) -> list[tuple]:
    """Every (src Sym, Edge, Site) triple in the index, edge order preserved."""
    out: list[tuple] = []
    for src, edge in ws.edges():
        for site in edge.sites:
            out.append((src, edge, site))
    return out


def _site_repr(src, edge, site) -> str:
    return f"{src.name} --{edge.kind}--> {edge.peer.name}  @ {site.loc}"


def _source_line(ws: Workspace, site) -> str:
    """The fixture's raw source line a site points at (1-based site.line)."""
    path = Path(site.file) if site.file else None
    if path is None or not path.is_file():
        path = ws.source
    assert path is not None and path.is_file(), (
        f"cannot read the source for site {site.loc}: no such file {path}"
    )
    lines = path.read_text(encoding="utf-8").splitlines()
    assert site.line is not None and 1 <= site.line <= len(lines), (
        f"site {site.loc} points at line {site.line}, but {path.name} has "
        f"{len(lines)} line(s)"
    )
    return lines[site.line - 1]


@then(parsers.parse('the "{kind}" edge sites are:'))
def edge_sites_of_kind(
    workspace: Workspace, kind: str, datatable: list[list[str]]
) -> None:
    """Every SITE of one edge kind -- one row per site, exhaustive for that kind.

    Table columns: src | dst | file | line | col | [token]

    A site is the concrete `file:line:col` where a relationship is *written*,
    which is not the declaration line of either endpoint. `token` is the source
    text the site must point at: it is read back from the fixture on disk, so
    the stated line and column are grounded in real text instead of merely
    echoing the database.
    """
    actual = [t for t in _sites(workspace) if t[1].kind == kind]
    rendered = [_site_repr(*triple) for triple in actual]
    matched: set[int] = set()

    for row in _rows(datatable):
        unknown = set(row) - {"src", "dst", "file", "line", "col", "token"}
        assert not unknown, f"unknown edge site column(s) {sorted(unknown)}"
        for required in ("src", "dst", "line", "col"):
            assert row.get(required) is not None, (
                f"edge site rows require a {required!r} cell; got {row}"
            )
        src = workspace.resolve(row["src"])
        dst = workspace.resolve(row["dst"])
        hits = [
            i
            for i, (s, e, site) in enumerate(actual)
            if s.id == src.id
            and e.peer.id == dst.id
            and site.line == row["line"]
            and site.col == row["col"]
        ]
        assert len(hits) == 1, (
            f"expected exactly 1 {kind!r} site "
            f"{row['src']} -> {row['dst']} at {row['line']}:{row['col']}, "
            f"found {len(hits)}.\n  all {kind!r} sites:\n"
            + "\n".join(f"    {r}" for r in rendered)
        )
        index = hits[0]
        assert index not in matched, (
            "the edge site table mentions the same site more than once: "
            f"{rendered[index]}"
        )
        _, _, site = actual[index]

        if row.get("file") is not None:
            got_file = os.path.basename(site.file) if site.file else None
            assert got_file == row["file"], (
                f"site {rendered[index]}: expected file {row['file']!r}, "
                f"got {got_file!r}"
            )
        if row.get("token") is not None:
            token = str(row["token"])
            text = _source_line(workspace, site)
            start = site.col - 1
            got = text[start : start + len(token)]
            assert got == token, (
                f"site {rendered[index]}: expected column {site.col} to start "
                f"the token {token!r}, got {got!r}\n"
                f"    {os.path.basename(site.file or '')}:{site.line}\n"
                f"    {text}\n"
                f"    {' ' * start}^ col {site.col}"
            )
        matched.add(index)

    extra = [r for i, r in enumerate(rendered) if i not in matched]
    assert not extra, (
        f"index holds {kind!r} edge sites the table does not mention:\n"
        + "\n".join(f"    {r}" for r in extra)
    )


@then(parsers.parse("the index holds exactly {count:d} unresolved symbol"))
@then(parsers.parse("the index holds exactly {count:d} unresolved symbols"))
def index_unresolved_symbol_count(workspace: Workspace, count: int) -> None:
    """Unresolved == a minted stub: a USR an edge anchors but that no indexed
    file declares or defines (`Sym.is_stub`). A single-file fixture that names
    nothing external must produce none."""
    stubs = [s for s in workspace.symbols() if s.is_stub]
    reported = workspace.graph.stats()["stubs"]
    assert len(stubs) == count, (
        f"expected {count} unresolved (stub) symbol(s), got {len(stubs)} "
        f"[stats reports stubs={reported}]:\n"
        + "\n".join(f"    {_describe(s)}  usr={s.usr}" for s in stubs)
    )


@then("no edge points at an unresolved symbol")
def no_edge_points_at_stub(workspace: Workspace) -> None:
    """Every endpoint of every edge is a real, indexed symbol."""
    dangling = [
        (s, e) for s, e in workspace.edges() if s.is_stub or e.peer.is_stub
    ]
    assert not dangling, (
        "expected every edge endpoint to be a resolved symbol, but these edges "
        "touch a stub:\n" + "\n".join(f"    {_edge_repr(s, e)}" for s, e in dangling)
    )


@then("the edge kind totals are:")
def edge_kind_totals(workspace: Workspace, datatable: list[list[str]]) -> None:
    """Table columns: kind | total. Must account for every edge kind present."""
    actual: dict[str, int] = {}
    for _, e in workspace.edges():
        actual[e.kind] = actual.get(e.kind, 0) + 1
    rows = _rows(datatable)
    for row in rows:
        assert set(row) == {"kind", "total"}, (
            "edge kind totals require exactly the columns 'kind' and 'total'; "
            f"got {sorted(row)}"
        )
    kinds = [row["kind"] for row in rows]
    assert len(kinds) == len(set(kinds)), "edge kind totals contain a duplicate kind"
    expected = {row["kind"]: row["total"] for row in rows}
    assert actual == expected, (
        f"edge kind totals differ\n  expected: {dict(sorted(expected.items()))}\n"
        f"  actual:   {dict(sorted(actual.items()))}"
    )


@then(parsers.parse('symbol "{selector}" calls:'))
def symbol_calls(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    """Callees via the graph API. Table columns: qual_name | kind | [line]."""
    sym = workspace.resolve(selector)
    callees = workspace.graph.callees(sym)
    actual = [{"qual_name": c.name, "kind": c.kind, "line": c.line} for c in callees]
    _assert_same(
        _rows(datatable), actual, f"callees of {_describe(sym)}", ordered=False
    )


@then(parsers.parse('symbol "{selector}" is called by:'))
def symbol_called_by(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    callers = workspace.graph.callers(sym)
    actual = [{"qual_name": c.name, "kind": c.kind, "line": c.line} for c in callers]
    _assert_same(
        _rows(datatable), actual, f"callers of {_describe(sym)}", ordered=False
    )


@then(parsers.parse('symbol "{selector}" is called by nothing'))
def symbol_no_callers(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    callers = workspace.graph.callers(sym)
    assert not callers, (
        f"{_describe(sym)}: expected no callers, got {[_describe(c) for c in callers]}"
    )


# --------------------------------------------------------------------------- #
# THEN -- CLI surface
# --------------------------------------------------------------------------- #


@then(parsers.parse('the CLI output contains "{needle}"'))
def cli_output_contains(workspace: Workspace, needle: str) -> None:
    assert needle in workspace.cli_output, (
        f"{needle!r} not found in CLI output:\n{workspace.cli_output}"
    )


@then(parsers.parse("`cidx {argv}` lists:"))
def cidx_lists(workspace: Workspace, argv: str, datatable: list[list[str]]) -> None:
    """Run a read-only cidx subcommand and check each expected line appears."""
    proc = workspace.run_ok(*shlex.split(argv))
    for row in _rows(datatable):
        assert set(row) == {"line"}, (
            f"cidx output tables require exactly one 'line' column; got {sorted(row)}"
        )
        needle = row["line"]
        assert needle in proc.stdout, (
            f"`cidx {argv}` output missing {needle!r}:\n{proc.stdout}"
        )


# --------------------------------------------------------------------------- #
# Comparison helper
# --------------------------------------------------------------------------- #


def _assert_same(
    expected: list[dict[str, Any]],
    actual: list[dict[str, Any]],
    what: str,
    *,
    ordered: bool = True,
) -> None:
    """Compare row lists, restricting actual rows to the stated columns."""
    if not expected:
        assert not actual, f"{what}: expected none, got {actual}"
        return
    cols = list(expected[0].keys())
    for row in expected:
        assert list(row.keys()) == cols, f"{what}: ragged table header"
    if actual:
        known = set().union(*(row.keys() for row in actual))
        unknown = set(cols) - known
        assert not unknown, (
            f"{what}: unknown table column(s) {sorted(unknown)}; known: {sorted(known)}"
        )
    trimmed = [{k: a.get(k) for k in cols} for a in actual]

    if ordered:
        ok = trimmed == expected
    else:
        ok = sorted(map(_key, trimmed)) == sorted(map(_key, expected))
    assert ok, (
        f"{what} differ\n"
        f"  expected ({len(expected)}):\n"
        + "\n".join(f"    {json.dumps(r, default=str)}" for r in expected)
        + f"\n  actual ({len(trimmed)}):\n"
        + "\n".join(f"    {json.dumps(r, default=str)}" for r in trimmed)
    )


def _key(row: dict[str, Any]) -> str:
    return json.dumps(row, sort_keys=True, default=str)
