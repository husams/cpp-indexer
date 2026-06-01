from __future__ import annotations

import hashlib
import json
import os
import re
import shlex
import sqlite3
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable
from urllib.parse import urlparse


DEFAULT_DB = ".agent-nav.sqlite3"
DEFAULT_SESSION = "default"
DEFAULT_BUDGET = int(os.environ.get("CPP_AGENT_NAV_BUDGET", "12"))
DEFAULT_SNIPPET_CHARS = int(os.environ.get("CPP_AGENT_NAV_SNIPPET_CHARS", "2400"))

ALLOC_HINTS = {
    "new-expression": r"\bnew\b",
    "malloc-family": r"\b(malloc|calloc|realloc|aligned_alloc)\s*\(",
    "make-unique": r"\bmake_unique\s*<",
    "make-shared": r"\bmake_shared\s*<",
    "container-growth": r"\.(push_back|emplace_back|reserve|resize)\s*\(",
    "map-insert": r"\.(insert|emplace|try_emplace)\s*\(",
    "string-growth": r"\.(append|push_back|resize|reserve|\+?=)\s*\(",
    "loop": r"\b(for|while)\s*\(",
}

FRAME_PATTERNS = [
    re.compile(r"(?:at|by)\s+0x[0-9A-Fa-f]+:\s+(?P<function>.+?)\s+\((?P<location>[^)]+)\)"),
    re.compile(r"#\d+\s+(?:0x[0-9A-Fa-f]+\s+in\s+)?(?P<function>.+?)\s+(?:at|from)\s+(?P<location>\S+)"),
]


def now() -> float:
    return time.time()


def stable_key(prefix: str, value: str) -> str:
    digest = hashlib.sha1(value.encode("utf-8", errors="replace")).hexdigest()[:16]
    return f"{prefix}:{digest}"


@dataclass
class NodeRef:
    key: str
    graph_id: str | None = None
    usr: str | None = None
    kind: str | None = None
    qualified_name: str | None = None
    signature: str | None = None
    file_path: str | None = None
    line: int | None = None
    source: str = "unknown"
    payload: dict[str, Any] | None = None

    @classmethod
    def from_libclang(cls, record: dict[str, Any]) -> "NodeRef":
        usr = record.get("usr")
        qn = record.get("qualified_name") or record.get("displayname") or record.get("spelling")
        loc = record.get("location") or {}
        key = f"usr:{usr}" if usr else stable_key("symbol", qn or json.dumps(record, sort_keys=True))
        return cls(
            key=key,
            usr=usr,
            kind=record.get("kind"),
            qualified_name=qn,
            signature=record.get("signature") or record.get("displayname"),
            file_path=loc.get("file"),
            line=loc.get("line") or loc.get("start_line"),
            source="libclang",
            payload=record,
        )

    @classmethod
    def from_indradb(cls, row: dict[str, Any]) -> "NodeRef":
        props = row.get("properties") or {}
        graph_id = row.get("id")
        qn = props.get("qualified_name") or props.get("name")
        key = f"graph:{graph_id}" if graph_id else stable_key("graph-node", json.dumps(row, sort_keys=True))
        return cls(
            key=key,
            graph_id=graph_id,
            usr=props.get("usr"),
            kind=props.get("kind") or row.get("t"),
            qualified_name=qn,
            signature=props.get("signature"),
            file_path=props.get("file_path"),
            line=props.get("line"),
            source="indradb",
            payload={"symbol_id": props.get("symbol_id"), "raw": row},
        )

    def brief(self) -> dict[str, Any]:
        return {
            "key": self.key,
            "graph_id": self.graph_id,
            "usr": self.usr,
            "kind": self.kind,
            "qualified_name": self.qualified_name,
            "signature": self.signature,
            "file_path": self.file_path,
            "line": self.line,
            "source": self.source,
        }


@dataclass
class EdgeRef:
    src_key: str
    dst_key: str
    kind: str | None
    direction: str
    payload: dict[str, Any] | None = None


class NavStore:
    """SQLite bookkeeping for agent navigation state."""

    def __init__(self, db_path: str | Path = DEFAULT_DB, session: str = DEFAULT_SESSION) -> None:
        self.db_path = Path(db_path)
        self.session = session
        self.conn = sqlite3.connect(self.db_path)
        self.conn.row_factory = sqlite3.Row
        self.init_schema()

    def init_schema(self) -> None:
        self.conn.executescript(
            """
            CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS nodes (
              key TEXT PRIMARY KEY,
              graph_id TEXT,
              usr TEXT,
              kind TEXT,
              qualified_name TEXT,
              signature TEXT,
              file_path TEXT,
              line INTEGER,
              source TEXT,
              payload_json TEXT,
              created_at REAL NOT NULL,
              last_seen REAL NOT NULL
            );
            CREATE TABLE IF NOT EXISTS edges (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              src_key TEXT NOT NULL,
              dst_key TEXT NOT NULL,
              kind TEXT,
              direction TEXT,
              payload_json TEXT,
              created_at REAL NOT NULL,
              UNIQUE(src_key, dst_key, kind, direction)
            );
            CREATE TABLE IF NOT EXISTS focus (
              session TEXT PRIMARY KEY,
              node_key TEXT,
              updated_at REAL NOT NULL
            );
            CREATE TABLE IF NOT EXISTS frontier (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              session TEXT NOT NULL,
              from_key TEXT,
              to_key TEXT NOT NULL,
              direction TEXT,
              edge_kind TEXT,
              reason TEXT,
              priority REAL NOT NULL DEFAULT 0,
              state TEXT NOT NULL DEFAULT 'open',
              created_at REAL NOT NULL
            );
            CREATE TABLE IF NOT EXISTS checkpoints (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              session TEXT NOT NULL,
              label TEXT NOT NULL,
              node_key TEXT,
              note TEXT,
              created_at REAL NOT NULL
            );
            CREATE TABLE IF NOT EXISTS observations (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              session TEXT NOT NULL,
              kind TEXT NOT NULL,
              target_key TEXT,
              summary TEXT NOT NULL,
              payload_json TEXT,
              created_at REAL NOT NULL
            );
            CREATE TABLE IF NOT EXISTS snippets (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              session TEXT NOT NULL,
              target_key TEXT,
              file_path TEXT NOT NULL,
              start_line INTEGER,
              end_line INTEGER,
              text TEXT NOT NULL,
              created_at REAL NOT NULL
            );
            CREATE INDEX IF NOT EXISTS nodes_graph_id_idx ON nodes(graph_id);
            CREATE INDEX IF NOT EXISTS nodes_usr_idx ON nodes(usr);
            CREATE INDEX IF NOT EXISTS nodes_qn_idx ON nodes(qualified_name);
            CREATE INDEX IF NOT EXISTS frontier_state_idx ON frontier(session, state, priority);
            """
        )
        self.conn.commit()

    def close(self) -> None:
        self.conn.close()

    def set_meta(self, key: str, value: str) -> None:
        self.conn.execute("INSERT OR REPLACE INTO meta(key, value) VALUES (?, ?)", (key, value))
        self.conn.commit()

    def upsert_node(self, node: NodeRef) -> NodeRef:
        t = now()
        self.conn.execute(
            """
            INSERT INTO nodes
              (key, graph_id, usr, kind, qualified_name, signature, file_path, line,
               source, payload_json, created_at, last_seen)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(key) DO UPDATE SET
              graph_id=COALESCE(excluded.graph_id, nodes.graph_id),
              usr=COALESCE(excluded.usr, nodes.usr),
              kind=COALESCE(excluded.kind, nodes.kind),
              qualified_name=COALESCE(excluded.qualified_name, nodes.qualified_name),
              signature=COALESCE(excluded.signature, nodes.signature),
              file_path=COALESCE(excluded.file_path, nodes.file_path),
              line=COALESCE(excluded.line, nodes.line),
              source=excluded.source,
              payload_json=excluded.payload_json,
              last_seen=excluded.last_seen
            """,
            (
                node.key,
                node.graph_id,
                node.usr,
                node.kind,
                node.qualified_name,
                node.signature,
                node.file_path,
                node.line,
                node.source,
                json.dumps(node.payload or {}, sort_keys=True),
                t,
                t,
            ),
        )
        self.conn.commit()
        return node

    def node(self, key: str) -> NodeRef | None:
        if key == "current":
            key = self.current_key() or ""
        row = self.conn.execute("SELECT * FROM nodes WHERE key = ?", (key,)).fetchone()
        if row is None:
            return None
        return NodeRef(
            key=row["key"],
            graph_id=row["graph_id"],
            usr=row["usr"],
            kind=row["kind"],
            qualified_name=row["qualified_name"],
            signature=row["signature"],
            file_path=row["file_path"],
            line=row["line"],
            source=row["source"],
            payload=json.loads(row["payload_json"] or "{}"),
        )

    def set_focus(self, key: str | None) -> None:
        self.conn.execute(
            """
            INSERT INTO focus(session, node_key, updated_at)
            VALUES (?, ?, ?)
            ON CONFLICT(session) DO UPDATE SET node_key=excluded.node_key, updated_at=excluded.updated_at
            """,
            (self.session, key, now()),
        )
        self.conn.commit()

    def current_key(self) -> str | None:
        row = self.conn.execute("SELECT node_key FROM focus WHERE session = ?", (self.session,)).fetchone()
        return row["node_key"] if row else None

    def add_edge(self, edge: EdgeRef) -> None:
        self.conn.execute(
            """
            INSERT OR IGNORE INTO edges(src_key, dst_key, kind, direction, payload_json, created_at)
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            (
                edge.src_key,
                edge.dst_key,
                edge.kind,
                edge.direction,
                json.dumps(edge.payload or {}, sort_keys=True),
                now(),
            ),
        )
        self.conn.commit()

    def enqueue(
        self,
        from_key: str | None,
        to_key: str,
        edge_kind: str | None,
        direction: str | None,
        reason: str,
        priority: float = 0,
    ) -> int:
        cur = self.conn.execute(
            """
            INSERT INTO frontier(session, from_key, to_key, direction, edge_kind, reason, priority, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (self.session, from_key, to_key, direction, edge_kind, reason, priority, now()),
        )
        self.conn.commit()
        return int(cur.lastrowid)

    def frontier(self, state: str = "open", budget: int = DEFAULT_BUDGET) -> list[dict[str, Any]]:
        rows = self.conn.execute(
            """
            SELECT f.*, n.kind, n.qualified_name, n.signature, n.file_path, n.line
            FROM frontier f
            LEFT JOIN nodes n ON n.key = f.to_key
            WHERE f.session = ? AND f.state = ?
            ORDER BY f.priority DESC, f.id ASC
            LIMIT ?
            """,
            (self.session, state, budget),
        ).fetchall()
        return [dict(r) for r in rows]

    def mark_frontier(self, frontier_id: int, state: str) -> None:
        self.conn.execute("UPDATE frontier SET state = ? WHERE id = ? AND session = ?", (state, frontier_id, self.session))
        self.conn.commit()

    def checkpoint(self, label: str, note: str | None = None) -> int:
        cur = self.conn.execute(
            "INSERT INTO checkpoints(session, label, node_key, note, created_at) VALUES (?, ?, ?, ?, ?)",
            (self.session, label, self.current_key(), note, now()),
        )
        self.conn.commit()
        return int(cur.lastrowid)

    def backtrack(self, checkpoint_id: int) -> dict[str, Any]:
        row = self.conn.execute(
            "SELECT * FROM checkpoints WHERE id = ? AND session = ?",
            (checkpoint_id, self.session),
        ).fetchone()
        if row is None:
            raise RuntimeError(f"No checkpoint {checkpoint_id} for session {self.session!r}")
        self.set_focus(row["node_key"])
        return dict(row)

    def observe(self, kind: str, summary: str, target_key: str | None = None, payload: Any = None) -> int:
        cur = self.conn.execute(
            """
            INSERT INTO observations(session, kind, target_key, summary, payload_json, created_at)
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            (self.session, kind, target_key, summary, json.dumps(payload or {}, sort_keys=True), now()),
        )
        self.conn.commit()
        return int(cur.lastrowid)

    def add_snippet(self, file_path: str, text: str, start_line: int | None, end_line: int | None, target_key: str | None) -> int:
        cur = self.conn.execute(
            """
            INSERT INTO snippets(session, target_key, file_path, start_line, end_line, text, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            """,
            (self.session, target_key, file_path, start_line, end_line, text, now()),
        )
        self.conn.commit()
        return int(cur.lastrowid)

    def snapshot(self, budget: int = DEFAULT_BUDGET) -> dict[str, Any]:
        current = self.node("current")
        obs = self.conn.execute(
            """
            SELECT id, kind, target_key, summary, created_at
            FROM observations
            WHERE session = ?
            ORDER BY id DESC
            LIMIT ?
            """,
            (self.session, budget),
        ).fetchall()
        return {
            "db": str(self.db_path),
            "session": self.session,
            "current": current.brief() if current else None,
            "open_frontier": self.frontier("open", budget),
            "recent_observations": [dict(r) for r in obs],
        }


class LibclangAPI:
    """Compiler-accurate C++ symbol and snippet API.

    The clang Python package is imported lazily, so agents can use NavStore and
    IndraDBAPI without libclang installed.
    """

    def __init__(self, compile_commands: str | Path | None = None) -> None:
        self.compile_commands = compile_commands

    @staticmethod
    def find_compile_commands(start: str | Path | None) -> Path | None:
        if not start:
            return None
        path = Path(start).resolve()
        if path.is_file() and path.name == "compile_commands.json":
            return path
        if path.is_file():
            path = path.parent
        for cur in [path, *path.parents]:
            candidate = cur / "compile_commands.json"
            if candidate.exists():
                return candidate
        return None

    @classmethod
    def load_compile_commands(cls, path: str | Path | None) -> list[dict[str, Any]]:
        found = cls.find_compile_commands(path)
        if not found:
            return []
        return json.loads(found.read_text())

    def compile_args_for_source(self, source: str | Path, extra_args: Iterable[str] = ()) -> list[str]:
        source_path = Path(source).resolve()
        entry: dict[str, Any] | None = None
        for item in self.load_compile_commands(self.compile_commands or source_path):
            file_path = Path(item.get("file", ""))
            if not file_path.is_absolute():
                file_path = Path(item.get("directory", ".")) / file_path
            if file_path.resolve() == source_path:
                entry = item
                break

        if entry is None:
            args = ["-x", "c++", "-std=c++20"]
        elif "arguments" in entry:
            args = list(entry["arguments"])
        else:
            args = shlex.split(entry.get("command", ""))

        cleaned: list[str] = []
        skip_next = False
        for i, arg in enumerate(args):
            if skip_next:
                skip_next = False
                continue
            base = Path(arg).name
            if i == 0 and not arg.startswith("-") and any(x in base for x in ("clang", "gcc", "g++", "c++")):
                continue
            if arg in {"-c", "-o"}:
                skip_next = arg == "-o"
                continue
            if arg == str(source_path) or Path(arg).name == source_path.name:
                continue
            cleaned.append(arg)
        cleaned.extend(extra_args)
        return cleaned

    @staticmethod
    def _load_clang():
        try:
            from clang import cindex
        except Exception as exc:  # pragma: no cover - environment-dependent
            raise RuntimeError(
                "Python clang bindings are not installed. Install the clang Python package "
                "and set LIBCLANG_PATH or LIBCLANG_LIBRARY_FILE."
            ) from exc
        # libclang permits set_library_file at most once per process; guard on
        # the binding's own `loaded` flag so a second parse (e.g. snippet after
        # resolve_symbol) does not raise.
        if getattr(cindex.Config, "loaded", False):
            return cindex
        lib_file = os.environ.get("LIBCLANG_LIBRARY_FILE")
        lib_path = os.environ.get("LIBCLANG_PATH")
        if lib_file:
            cindex.Config.set_library_file(lib_file)
        elif lib_path:
            for name in ("libclang.dylib", "libclang.so", "libclang.so.18", "libclang.dll"):
                candidate = Path(lib_path) / name
                if candidate.exists():
                    cindex.Config.set_library_file(str(candidate))
                    break
        return cindex

    def parse_tu(self, source: str | Path):
        cindex = self._load_clang()
        index = cindex.Index.create()
        return cindex, index.parse(str(Path(source).resolve()), args=self.compile_args_for_source(source))

    @staticmethod
    def walk(cursor: Any) -> Iterable[Any]:
        yield cursor
        for child in cursor.get_children():
            yield from LibclangAPI.walk(child)

    @staticmethod
    def qualified_name(cursor: Any) -> str:
        names: list[str] = []
        cur = cursor
        while cur is not None and str(getattr(cur, "kind", "")) != "CursorKind.TRANSLATION_UNIT":
            spelling = getattr(cur, "spelling", "") or getattr(cur, "displayname", "")
            if spelling:
                names.append(spelling)
            cur = getattr(cur, "semantic_parent", None)
        return "::".join(reversed(names))

    @classmethod
    def cursor_location(cls, cursor: Any) -> dict[str, Any]:
        loc = cursor.location
        extent = cursor.extent
        return {
            "file": str(loc.file) if loc.file else None,
            "line": loc.line,
            "column": loc.column,
            "start_line": extent.start.line,
            "start_column": extent.start.column,
            "end_line": extent.end.line,
            "end_column": extent.end.column,
        }

    @classmethod
    def cursor_signature(cls, cursor: Any) -> str | None:
        try:
            result = cursor.result_type.spelling
            params = ", ".join(arg.type.spelling for arg in cursor.get_arguments())
            return f"{result} {cls.qualified_name(cursor)}({params})"
        except Exception:
            return getattr(cursor, "displayname", None)

    @classmethod
    def cursor_record(cls, cursor: Any) -> dict[str, Any]:
        return {
            "usr": cursor.get_usr() or None,
            "kind": str(cursor.kind).replace("CursorKind.", ""),
            "spelling": cursor.spelling or None,
            "displayname": cursor.displayname or None,
            "qualified_name": cls.qualified_name(cursor),
            "signature": cls.cursor_signature(cursor),
            "type": getattr(cursor.type, "spelling", None),
            "result_type": getattr(getattr(cursor, "result_type", None), "spelling", None),
            "location": cls.cursor_location(cursor),
        }

    def resolve_symbols(self, source: str | Path, name: str, budget: int = DEFAULT_BUDGET) -> list[dict[str, Any]]:
        cindex, tu = self.parse_tu(source)
        kind_names = [
            "FUNCTION_DECL",
            "CXX_METHOD",
            "CONSTRUCTOR",
            "DESTRUCTOR",
            "FUNCTION_TEMPLATE",
            "CLASS_DECL",
            "STRUCT_DECL",
            "FIELD_DECL",
            "VAR_DECL",
            "ENUM_DECL",
            "NAMESPACE",
        ]
        allowed = {getattr(cindex.CursorKind, k) for k in kind_names if hasattr(cindex.CursorKind, k)}
        matches: list[dict[str, Any]] = []
        for cursor in self.walk(tu.cursor):
            if cursor.kind not in allowed:
                continue
            values = [cursor.spelling or "", cursor.displayname or "", self.qualified_name(cursor), cursor.get_usr() or ""]
            if any(v == name or v.endswith(f"::{name}") or name in v for v in values):
                matches.append(self.cursor_record(cursor))
                if len(matches) >= budget:
                    break
        return matches

    def find_function_cursor(self, source: str | Path, name: str) -> Any | None:
        cindex, tu = self.parse_tu(source)
        kind_names = ["FUNCTION_DECL", "CXX_METHOD", "CONSTRUCTOR", "DESTRUCTOR", "FUNCTION_TEMPLATE"]
        allowed = {getattr(cindex.CursorKind, k) for k in kind_names if hasattr(cindex.CursorKind, k)}
        for cursor in self.walk(tu.cursor):
            if cursor.kind not in allowed:
                continue
            values = [cursor.spelling or "", cursor.displayname or "", self.qualified_name(cursor), cursor.get_usr() or ""]
            if any(v == name or v.endswith(f"::{name}") or name in v for v in values):
                return cursor
        return None

    def snippet(self, source: str | Path, name: str, char_budget: int = DEFAULT_SNIPPET_CHARS) -> dict[str, Any]:
        cursor = self.find_function_cursor(source, name)
        if cursor is None:
            raise RuntimeError(f"No function or method matched {name!r}")
        loc = self.cursor_location(cursor)
        file_name = loc.get("file")
        if not file_name:
            raise RuntimeError("Cursor has no source file")
        path = Path(file_name)
        lines = path.read_text(errors="replace").splitlines()
        start = max(int(loc["start_line"]) - 1, 0)
        end = min(int(loc["end_line"]), len(lines))
        text = "\n".join(lines[start:end])
        truncated = len(text) > char_budget
        if truncated:
            text = text[:char_budget] + "\n/* ... snippet truncated by char budget ... */"
        return {
            "file_path": str(path),
            "start_line": start + 1,
            "end_line": end,
            "text": text,
            "truncated": truncated,
            "char_budget": char_budget,
            "allocation_hints": allocation_hints(text),
            "symbol": self.cursor_record(cursor),
        }


def allocation_hints(text: str) -> list[dict[str, Any]]:
    hints: list[dict[str, Any]] = []
    for name, pattern in ALLOC_HINTS.items():
        count = len(re.findall(pattern, text))
        if count:
            hints.append({"kind": name, "count": count})
    return hints


class IndraDBAPI:
    """Small read-only IndraDB navigation wrapper."""

    def __init__(self, uri: str) -> None:
        try:
            import indradb  # type: ignore[import-untyped]
        except Exception as exc:  # pragma: no cover - environment-dependent
            raise RuntimeError(
                "indradb Python driver is not installed. Install cpp-mcp with "
                "the graphdb-indradb extra, then retry."
            ) from exc
        self.indradb = indradb
        parsed = urlparse(uri)
        host = parsed.hostname or "localhost"
        port = parsed.port or 27615
        # indradb.Client builds its gRPC channel internally with the default 4 MB
        # receive cap, which an AllVertexQuery property dump overruns on a large
        # graph (~17 MB at 35k nodes). Inject unbounded message limits via a
        # scoped monkeypatch so we don't depend on the driver's internal stub name.
        import grpc  # type: ignore[import-untyped]

        _orig_channel = grpc.insecure_channel
        _opts = [
            ("grpc.max_receive_message_length", -1),
            ("grpc.max_send_message_length", -1),
        ]
        grpc.insecure_channel = lambda h, **kw: _orig_channel(h, options=_opts)
        try:
            self.client = indradb.Client(host=f"{host}:{port}")
        finally:
            grpc.insecure_channel = _orig_channel
        self.client.ping()

    @staticmethod
    def _flatten(batches: Any) -> list[Any]:
        return [item for batch in batches for item in batch]

    def _vertex_props(self, query: Any, items: list[Any]) -> list[dict[str, Any]]:
        by_id: dict[str, dict[str, Any]] = {}
        for batch in self.client.get(query.properties()):
            for vp in batch:
                props = by_id.setdefault(str(vp.vertex.id), {})
                for np in vp.props:
                    props[np.name] = np.value
        return [by_id.get(str(v.id), {}) for v in items]

    def _edge_props(self, query: Any, items: list[Any]) -> list[dict[str, Any]]:
        by_key: dict[tuple[str, str, str], dict[str, Any]] = {}
        for batch in self.client.get(query.properties()):
            for ep in batch:
                e = ep.edge
                props = by_key.setdefault((str(e.outbound_id), str(e.t), str(e.inbound_id)), {})
                for np in ep.props:
                    props[np.name] = np.value
        return [by_key.get((str(e.outbound_id), str(e.t), str(e.inbound_id)), {}) for e in items]

    @staticmethod
    def _coerce_vertex(vertex: Any, props: dict[str, Any]) -> dict[str, Any]:
        return {"id": str(vertex.id), "t": str(vertex.t), "properties": props}

    def lookup_property(self, name: str, value: Any, budget: int = DEFAULT_BUDGET) -> dict[str, Any]:
        indradb = self.indradb
        all_vertices = self._flatten(self.client.get(indradb.AllVertexQuery()))
        props_list = self._vertex_props(indradb.AllVertexQuery(), all_vertices)
        matched = [(v, p) for v, p in zip(all_vertices, props_list, strict=True) if p.get(name) == value]
        return {
            "rows": [self._coerce_vertex(v, p) for v, p in matched[:budget]],
            "matched_count": len(matched),
            "truncated": len(matched) > budget,
            "budget": budget,
        }

    def neighbors(self, vertex_id: str, direction: str, edge_kind: str | None = None, budget: int = DEFAULT_BUDGET) -> dict[str, Any]:
        indradb = self.indradb
        svq = indradb.SpecificVertexQuery(uuid.UUID(vertex_id))
        pipe = svq.outbound() if direction == "outbound" else svq.inbound()
        if edge_kind:
            pipe = pipe.t(edge_kind)
        raw_edges = self._flatten(self.client.get(pipe))
        edges = raw_edges[:budget]
        edge_props = self._edge_props(indradb.SpecificEdgeQuery(*edges), edges) if edges else []
        neighbor_ids = [e.inbound_id if direction == "outbound" else e.outbound_id for e in edges]
        vertices = self._flatten(self.client.get(indradb.SpecificVertexQuery(*neighbor_ids))) if neighbor_ids else []
        vertex_by_id = {str(v.id): v for v in vertices}
        vertex_props = self._vertex_props(indradb.SpecificVertexQuery(*[v.id for v in vertices]), vertices) if vertices else []
        props_by_id = {str(v.id): p for v, p in zip(vertices, vertex_props, strict=True)}
        rows: list[dict[str, Any]] = []
        for edge, props in zip(edges, edge_props, strict=True):
            nid = str(edge.inbound_id if direction == "outbound" else edge.outbound_id)
            vertex = vertex_by_id.get(nid)
            if vertex:
                rows.append(
                    {
                        "edge": {
                            "outbound_id": str(edge.outbound_id),
                            "inbound_id": str(edge.inbound_id),
                            "t": str(edge.t),
                            "properties": props,
                        },
                        "node": self._coerce_vertex(vertex, props_by_id.get(nid, {})),
                    }
                )
        return {
            "rows": rows,
            "direction": direction,
            "edge_kind": edge_kind,
            "fetched_count": len(rows),
            "truncated": len(raw_edges) > budget,
            "budget": budget,
        }


def profile_frames(profile: str | Path, project_root: str | Path | None = None, budget: int = DEFAULT_BUDGET) -> list[dict[str, Any]]:
    root = Path(project_root).resolve() if project_root else None
    frames: list[dict[str, Any]] = []
    for line_no, line in enumerate(Path(profile).read_text(errors="replace").splitlines(), start=1):
        for pattern in FRAME_PATTERNS:
            match = pattern.search(line.strip())
            if not match:
                continue
            location = match.group("location")
            project_owned = False
            if root and location:
                loc_file = location.split(":")[0]
                try:
                    project_owned = Path(loc_file).resolve().is_relative_to(root)
                except Exception:
                    project_owned = str(root) in loc_file
            frames.append(
                {
                    "line": line_no,
                    "function": match.group("function"),
                    "location": location,
                    "project_owned": project_owned,
                    "raw": line.strip(),
                }
            )
            break
        if len(frames) >= budget:
            break
    return frames


def spec_outline(spec_path: str | Path | None, budget: int = DEFAULT_BUDGET) -> dict[str, Any] | None:
    if not spec_path:
        return None
    text = Path(spec_path).read_text(errors="replace")
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    requirements = [
        re.sub(r"^[-*0-9.)\s]+", "", line)
        for line in lines
        if re.match(r"^([-*]|\d+[.)])\s+", line)
        or re.search(r"\b(shall|must|should|required|when|then)\b", line, re.I)
    ]
    excerpt_budget = min(max(budget, 1) * 400, 4000)
    return {
        "path": str(Path(spec_path).resolve()),
        "excerpt": text[:excerpt_budget],
        "candidate_requirements": requirements[:budget],
        "truncated": len(text) > excerpt_budget or len(requirements) > budget,
    }


class NavigationSession:
    """High-level composable API for agent-built scripts."""

    def __init__(
        self,
        db_path: str | Path = DEFAULT_DB,
        session: str = DEFAULT_SESSION,
        compile_commands: str | Path | None = None,
        default_budget: int = DEFAULT_BUDGET,
    ) -> None:
        self.default_budget = default_budget
        self.store = NavStore(db_path, session)
        self.libclang = LibclangAPI(compile_commands)

    def close(self) -> None:
        self.store.close()

    def init(self, repo_root: str | Path | None = None) -> dict[str, Any]:
        if repo_root:
            self.store.set_meta("repo_root", str(Path(repo_root).resolve()))
        return self.status()

    def status(self, budget: int | None = None) -> dict[str, Any]:
        return self.store.snapshot(budget or self.default_budget)

    def resolve_symbol(
        self,
        source: str | Path,
        name: str,
        focus: bool = False,
        budget: int | None = None,
    ) -> list[NodeRef]:
        records = self.libclang.resolve_symbols(source, name, budget or self.default_budget)
        nodes = [self.store.upsert_node(NodeRef.from_libclang(r)) for r in records]
        if focus and nodes:
            self.store.set_focus(nodes[0].key)
        self.store.observe(
            "libclang.resolve",
            f"Resolved {len(nodes)} candidate(s) for {name}",
            nodes[0].key if nodes else None,
            {"source": str(source), "name": name, "candidate_keys": [n.key for n in nodes]},
        )
        return nodes

    def snippet(
        self,
        source: str | Path,
        name: str,
        char_budget: int = DEFAULT_SNIPPET_CHARS,
        target_key: str | None = None,
    ) -> dict[str, Any]:
        data = self.libclang.snippet(source, name, char_budget)
        if not target_key:
            node = self.store.upsert_node(NodeRef.from_libclang(data["symbol"]))
            target_key = node.key
        snippet_id = self.store.add_snippet(
            data["file_path"], data["text"], data["start_line"], data["end_line"], target_key
        )
        self.store.observe(
            "libclang.snippet",
            f"Stored snippet {snippet_id} for {name}",
            target_key,
            {"snippet_id": snippet_id, "allocation_hints": data.get("allocation_hints", []), "truncated": data["truncated"]},
        )
        return {"snippet_id": snippet_id, "target_key": target_key, **data}

    def graph_lookup(
        self,
        uri: str,
        property_name: str,
        value: Any,
        focus: bool = False,
        budget: int | None = None,
    ) -> list[NodeRef]:
        result = IndraDBAPI(uri).lookup_property(property_name, value, budget or self.default_budget)
        nodes = [self.store.upsert_node(NodeRef.from_indradb(row)) for row in result["rows"]]
        if focus and nodes:
            self.store.set_focus(nodes[0].key)
        self.store.observe(
            "indradb.lookup",
            f"Lookup {property_name}={value!r} stored {len(nodes)} node(s)",
            nodes[0].key if nodes else None,
            {"property": property_name, "value": value, "matched_count": result["matched_count"]},
        )
        return nodes

    def lookahead(
        self,
        uri: str,
        node_key: str = "current",
        direction: str = "outbound",
        edge_kind: str | None = None,
        reason: str | None = None,
        budget: int | None = None,
    ) -> dict[str, Any]:
        node = self.store.node(node_key)
        if node is None:
            raise RuntimeError(f"Unknown node key {node_key!r}")
        if not node.graph_id:
            raise RuntimeError(f"Node {node.key} has no graph_id; use graph_lookup first")
        result = IndraDBAPI(uri).neighbors(node.graph_id, direction, edge_kind, budget or self.default_budget)
        stored: list[dict[str, Any]] = []
        for row in result["rows"]:
            dst = self.store.upsert_node(NodeRef.from_indradb(row["node"]))
            edge_payload = row["edge"]
            if direction == "outbound":
                edge = EdgeRef(node.key, dst.key, edge_payload.get("t"), direction, edge_payload)
            else:
                edge = EdgeRef(dst.key, node.key, edge_payload.get("t"), direction, edge_payload)
            self.store.add_edge(edge)
            frontier_id = self.store.enqueue(
                node.key,
                dst.key,
                edge_payload.get("t"),
                direction,
                reason or f"{direction} {edge_payload.get('t')} from {node.qualified_name or node.key}",
            )
            brief = dst.brief()
            brief["frontier_id"] = frontier_id
            stored.append(brief)
        self.store.observe(
            "indradb.lookahead",
            f"Lookahead from {node.qualified_name or node.key}: {len(stored)} neighbor(s)",
            node.key,
            {"direction": direction, "edge_kind": edge_kind, "truncated": result["truncated"]},
        )
        return {"from": node.brief(), "stored_neighbors": stored, **result}

    def frontier(self, state: str = "open", budget: int | None = None) -> list[dict[str, Any]]:
        return self.store.frontier(state, budget or self.default_budget)

    def step(self, frontier_id: int, checkpoint: str | None = None) -> NodeRef | None:
        row = self.store.conn.execute(
            "SELECT * FROM frontier WHERE id = ? AND session = ?",
            (frontier_id, self.store.session),
        ).fetchone()
        if row is None:
            raise RuntimeError(f"No frontier item {frontier_id}")
        if checkpoint:
            self.store.checkpoint(checkpoint, f"Before stepping to frontier {frontier_id}")
        self.store.set_focus(row["to_key"])
        self.store.mark_frontier(frontier_id, "explored")
        return self.store.node(row["to_key"])

    def checkpoint(self, label: str, note: str | None = None) -> int:
        return self.store.checkpoint(label, note)

    def backtrack(self, checkpoint_id: int) -> dict[str, Any]:
        return self.store.backtrack(checkpoint_id)

    def record_profile(self, profile: str | Path, project_root: str | Path | None = None, budget: int | None = None) -> list[dict[str, Any]]:
        frames = profile_frames(profile, project_root, budget or self.default_budget)
        project_count = sum(1 for f in frames if f.get("project_owned"))
        self.store.observe(
            "profile.frames",
            f"Parsed {len(frames)} profile frame(s); {project_count} project-owned",
            None,
            {"frames": frames},
        )
        return frames

    def record_spec(self, spec_path: str | Path, budget: int | None = None) -> dict[str, Any] | None:
        outline = spec_outline(spec_path, budget or self.default_budget)
        if outline:
            self.store.observe(
                "functional.spec",
                f"Loaded {len(outline['candidate_requirements'])} candidate requirement(s)",
                None,
                outline,
            )
        return outline
