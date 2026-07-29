"""indexer.query -- read-only, ground-the-LLM graph API over a cidx index.db.

This module lets an agent (or human) REASON over a large code graph without
loading source files into context. You ask graph questions -- who calls X, what
does X call, X's class hierarchy, the real run-time targets of a virtual call --
and get back compact dataclasses that always carry a resolved `file:line` so the
answer can be grounded.

It is strictly read-only: the database is opened with `?mode=ro`, no schema is
created or migrated, and no write methods exist. It depends only on the standard
library (sqlite3), matching storage.py's zero-dependency style.

The schema it reads is cidx schema v7 (see storage.py):

    component / directory / file   location of every symbol (abs path is rebuilt
                                   by joining component.path / directory.path / name)
    symbol                         one decl/def, keyed by clang USR
    edge (+ edge_kind, edge_site)  typed relationships between symbols

Edge kinds (edge.kind -> edge_kind.name):
    1 calls   2 inherits   3 contains   4 specializes   5 instantiates
    6 overrides   7 uses   8 field_of   9 method_of

Edge-direction conventions (the gotchas the traversals get right):
    calls       src=caller, dst=callee.   callers(X) = inbound;  callees(X) = outbound.
    overrides   src=derived, dst=base.    overridden_by(base) = inbound `overrides`.
    inherits    src=derived, dst=base.    subclasses(base) = inbound.
    field_of /  src=member,  dst=record.  a record's members are INbound, while
    method_of                             `contains` is OUTbound (scope->child);
                                          members() unions both.

Quick start:

    from indexer.query import GraphQuery, open_query
    g = open_query()                       # standard DB (INDEXER_CACHE/index.db)
    fn = g.find("rd_kafka_new")[0]         # fuzzy lookup
    for s in g.callers(fn):                # who calls it (inbound `calls`)
        print(s)
    for t in g.dispatch_targets(method):   # virtual method -> run-time targets
        print(t)
"""

from __future__ import annotations

import os
import re
import sqlite3
from dataclasses import dataclass, replace
from typing import Any, Iterable, Literal, Optional, Sequence, overload

# symbol.kind is stored as a CXCursorKind int (v16); these recover the name and
# convert a name filter back to the stored int. Single source of truth = storage.
from indexer.storage import (
    SYMBOL_KIND_IDS,
    SYMBOL_KIND_NAMES,
    _validate_catalog_hash,
)
from indexer.generated_catalog import (
    CATALOG_HASH,
    EDGE_KINDS as _GENERATED_EDGE_KINDS,
    TYPE_KIND_NAMES as _GENERATED_TYPE_KIND_NAMES,
)

# Generated relation identifiers are the read-side contract shared with C++.
EDGE_KINDS = dict(_GENERATED_EDGE_KINDS)
EDGE_NAMES = {v: k for k, v in EDGE_KINDS.items()}

#: C++ access specifiers, used to validate the members(access=...) filter.
_ACCESS = ("public", "protected", "private")

_CACHE_ENV = "INDEXER_CACHE"
_DEFAULT_CACHE = "~/.cache/cidx"
_INDEX_NAME = "index.db"


class NoIndexError(FileNotFoundError):
    """No index database at the requested path."""


class NoEdgesError(RuntimeError):
    """The index has no graph edges (indexed with --no-graph, or never resolved).

    Graph queries are meaningless without edges, so they must not silently fall
    back to another database -- they raise this instead.
    """


class _AdapterPlanTruncated(RuntimeError):
    """The QueryPlan candidate set was capped and is not complete."""


def _legacy_find_glob(pattern: str, *, prefix: bool = False) -> str:
    """Build a GLOB that preserves legacy ASCII case-insensitive matching.

    QueryPlan owns the match operation for compatibility reads.  The optional
    prefix form is the middle tier of the historical ``find`` lookup; the
    default form is the segmented fuzzy tier.
    """
    out = [] if prefix else ["*"]
    segments = [segment for segment in pattern.split("::") if segment]
    for segment_index, segment in enumerate(segments):
        if segment_index:
            out.append("*")
        for char in segment:
            if char == "*" or char == "?":
                out.extend(("[", char, "]"))
            elif char == "[":
                out.extend(("[", "[", "]"))
            elif "a" <= char <= "z" or "A" <= char <= "Z":
                out.extend(("[", char.lower(), char.upper(), "]"))
            else:
                out.append(char)
    out.append("*")
    return "".join(out)


def default_db_path() -> str:
    """The standard cidx index path: $INDEXER_CACHE/index.db else ~/.cache/cidx/index.db.

    Mirrors indexer.cli.index_path() so the library and the CLI agree on the one
    canonical location.
    """
    cache = os.environ.get(_CACHE_ENV) or _DEFAULT_CACHE
    return os.path.join(os.path.expanduser(cache), _INDEX_NAME)


def open_query(
    db_path: Optional[str] = None, require_edges: bool = False
) -> "GraphQuery":
    """Open the standard cidx index read-only. `db_path` overrides discovery."""
    return GraphQuery(db_path or default_db_path(), require_edges=require_edges)


# --------------------------------------------------------------------------- #
# Compact value types -- terse __repr__ so dumping a list stays token-cheap.
# Each carries the resolved file path + line so a caller can ground its claims.
# --------------------------------------------------------------------------- #


class File:
    """A source file in (or referenced by) the index -- a *smart path*.

    ``Sym.file`` is a ``File`` (or ``None`` for a stub). It is the absolute path
    PLUS a back-reference to the read-only :class:`GraphQuery`, so a caller can go
    straight from a symbol to the file's owning ``component`` / ``repo``, its
    ``compile_options``, a ``source()`` slice, or the parsed ``tu()`` / ``walk()``
    AST -- without re-deriving any of it.

    It behaves like its path string so existing callers stay unchanged: it is
    ``os.PathLike`` (``os.path.basename(sym.file)`` works), stringifies to the
    path, and compares equal to a plain ``str`` of the same path
    (``sym.file == "/usr/include/.../foo.h"``). Heavy collaborators
    (``Storage`` / ``astcache`` / ``astcmd`` / ``compiledb``) are imported lazily
    inside the methods that need them.
    """

    __slots__ = (
        "path",
        "external",
        "_q",
        "_component_name",
        "_store",
        "_row",
        "_row_loaded",
        "_tu",
        "_tu_loaded",
    )

    def __init__(
        self,
        path: str,
        query: "GraphQuery",
        *,
        external: bool = False,
        component_name: Optional[str] = None,
    ):
        self.path = path
        self.external = external  # raw path in an UNREGISTERED file (no component)
        self._q = query
        self._component_name = component_name  # cheap name (no Storage round-trip)
        self._store = None  # lazily-wrapped read-only Storage view
        self._row = None  # cached file row (None once loaded = unregistered)
        self._row_loaded = False
        self._tu = None  # memoized TranslationUnit (parsed/loaded once)
        self._tu_loaded = False

    # -- identity / path-like ------------------------------------------------ #

    @property
    def name(self) -> str:
        """Basename of the path (e.g. ``shapes.c``)."""
        return os.path.basename(self.path)

    def __fspath__(self) -> str:
        return self.path

    def __str__(self) -> str:
        return self.path

    def __repr__(self) -> str:
        tag = " external" if self.external else ""
        return f"File({self.name!r}{tag})"

    def __eq__(self, other: object) -> bool:
        if isinstance(other, File):
            return self.path == other.path
        if isinstance(other, str):
            return self.path == other
        return NotImplemented

    def __hash__(self) -> int:
        return hash(self.path)

    # -- DB-backed metadata -------------------------------------------------- #

    def _storage(self):
        """A read-only :class:`indexer.storage.Storage` over the SAME connection
        the :class:`GraphQuery` holds -- no new connection, no migration."""
        if self._store is None:
            from indexer.storage import Storage

            self._store = Storage.from_connection(self._q._c, self._q.db_path)
        return self._store

    def _file_row(self):
        """The ``file`` table row for this path, or ``None`` (unregistered)."""
        if not self._row_loaded:
            self._row = self._storage().get_file(self.path)
            self._row_loaded = True
        return self._row

    @property
    def component(self):
        """The owning :class:`indexer.storage.Component`, or ``None`` for an
        unregistered (system/stdlib) file."""
        if self.external:
            return None
        return self._storage().component_for_path(self.path)

    @property
    def repo(self):
        """The owning :class:`indexer.storage.Repository` (v23 grouping), or
        ``None`` when the file's component is ungrouped/unregistered."""
        comp = self.component
        if comp is None or comp.repository_id is None:
            return None
        return self._storage().get_repository_by_id(comp.repository_id)

    @property
    def compile_options(self) -> Optional[list[str]]:
        """The stored (already-stripped) parse args for this file's TU, or
        ``None`` if the file is not a registered TU."""
        row = self._file_row()
        return row.compile_options if row is not None else None

    @property
    def driver(self) -> Optional[str]:
        """argv[0] of the original compile command, or ``None``."""
        row = self._file_row()
        return row.driver if row is not None else None

    # -- source text --------------------------------------------------------- #

    def source(
        self,
        start: tuple[int, int],
        end: tuple[int, int],
        *,
        encoding: str = "utf-8",
    ) -> str:
        """The on-disk text between ``start`` and ``end``, each a 1-based
        ``(line, col)`` tuple (clang convention). ``end`` is inclusive of the
        character at ``end[1]``. Raises ``OSError`` if the file is unreadable."""
        sl, sc = start
        el, ec = end
        if sl < 1 or el < sl or (el == sl and ec < sc):
            raise ValueError(f"invalid range {start}..{end}")
        with open(self.path, "r", encoding=encoding) as fh:
            lines = fh.readlines()
        if sl > len(lines):
            return ""
        el = min(el, len(lines))
        if sl == el:
            return lines[sl - 1][sc - 1 : ec]
        out = [lines[sl - 1][sc - 1 :]]
        out.extend(lines[sl : el - 1])
        out.append(lines[el - 1][:ec])
        return "".join(out)

    # -- AST ----------------------------------------------------------------- #

    def _target(self):
        """An :class:`indexer.astcmd.Target` for this file -- resolved flags +
        driver, ready for :func:`indexer.astcache.load_or_parse`."""
        from indexer import compiledb
        from indexer.astcmd import Target

        opts = compiledb.resolve_options(
            compiledb.sanitize(self.compile_options or []),
            self._storage().get_alias,
        )
        return Target(abspath=self.path, flags=opts, driver=self.driver)

    def tu(self, cache: bool = True):
        """The file's ``clang.cindex.TranslationUnit``, **memoized on this File**.

        The first call parses (or, when ``cache`` is True, loads the on-disk
        ``.ast``, writing it on a miss) and stores the TU; later calls return the
        SAME object with no disk or parse cost -- so ``f.tu()`` then ``f.walk()``
        is one parse, not two. Pass ``cache=False`` to force a fresh parse from
        source (e.g. after the file changed on disk); that result replaces the
        memo. Returns ``None`` only if the parse itself fails (not memoized)."""
        if cache and self._tu_loaded:
            return self._tu
        from indexer import astcache

        tu = astcache.load_or_parse(self._target(), use_cache=cache)
        if tu is not None:
            self._tu = tu
            self._tu_loaded = True
        return tu

    def walk(self, cache: bool = True):
        """Generator over EVERY cursor in this file's AST (pre-order), so a caller
        can traverse the tree without juggling the raw TU. Reuses the memoized
        :meth:`tu` (``cache`` is forwarded). Yields nothing if the parse fails."""
        tu = self.tu(cache=cache)
        if tu is None:
            return

        def _walk(cursor):
            yield cursor
            for child in cursor.get_children():
                yield from _walk(child)

        for child in tu.cursor.get_children():
            yield from _walk(child)

    def symbols(self, limit: int = 500) -> "list[Sym]":
        """The indexed symbols declared in this file (low-level ``Sym`` values)."""
        return self._q.symbols_in_file(self.path, limit=limit)

    # -- deferred ------------------------------------------------------------ #

    def index(self, force: bool = False):
        """Re-index this file. Deferred: the query layer is strictly read-only
        (``?mode=ro``); indexing needs a writable path that is designed
        separately. Use ``cidx index <path>`` for now."""
        raise NotImplementedError(
            "File.index() is not available from the read-only query layer; "
            "run `cidx index <path>` (optionally after re-import) instead."
        )


@dataclass(frozen=True)
class Sym:
    """A symbol (declaration/definition). `name` is the qualified name."""

    id: int
    usr: str
    spelling: str
    name: str  # qual_name, else spelling
    kind: str
    type_info: Optional[str]
    is_definition: bool
    is_pure: bool  # C++ pure-virtual (= 0): no own body exists
    access: Optional[str]  # public/protected/private (C++)
    parent_usr: Optional[str]
    resolved: bool
    component: Optional[str]
    file: Optional[File]  # best-known location as a File (smart path), None=stub
    line: Optional[int]
    col: Optional[int]
    end_line: Optional[int] = None  # v25: end of the symbol's own extent at
    end_col: Optional[int] = None   # (line, col) -- (line..end_line) slices the
    # whole entity (function/method body, class/struct/union/enum/typedef region).
    # None for decl-only / stub fallbacks that carry no stored end.
    external: bool = False  # `file` is a raw path in an UNREGISTERED file
    # (system/stdlib header no component owns), not a
    # location in any indexed file -- see is_stub
    is_static: bool = False  # C++ static member function (free functions are
    # False; a file-scope `static` free function is reflected by linkage)
    is_instantiation: bool = False  # v13: implicit template-instantiation node
    # (X<int> type node or X<int>::member); definition via instantiates edge
    callable_kind: Optional[str] = None
    template_origin: Optional[str] = None
    template_form: Optional[str] = None
    display_name: Optional[str] = None  # spelling WITH template arguments, e.g.
    # ``Wrapper<int>`` for an instantiation/specialization (``Wrapper<T>`` for the
    # primary template); None/equal-to-spelling for a non-templated symbol. This
    # is the template-argument-bearing name that tells two instantiations apart
    # (``qual_name``/``spelling`` are both the bare ``Wrapper``). Deliberately NOT
    # surfaced in ``to_dict`` -- that view stays byte-identical to the C++ port.
    multi_def: int = 0  # v27: number of definitions (bodies). >1 == redefined
    # per backend (library method left undefined, each server reimplements it).
    const_value: Optional[str] = None  # v33: the evaluated constant value of a
    # variable's initializer or an enumerator, as printed by Clang's constant
    # evaluator (constexpr/consteval arithmetic included); None when the
    # initializer needs runtime evaluation or there is none.
    semantic_universe_id: int = -1  # v35: database-local scope row
    identity_key: str = ""  # v35: portable scope-keyed semantic identity
    semantic_universe: str = ""  # portable universe key

    @property
    def is_redefined(self) -> bool:
        """True when this symbol has more than one definition (a per-backend
        redefinition). See GraphQuery.definitions()/possible_callees()."""
        return self.multi_def > 1

    @property
    def loc(self) -> str:
        if not self.file:
            return "<no-location>"
        base = self.file.name
        return f"{base}:{self.line}" if self.line else base

    @property
    def span(self) -> Optional[str]:
        """`file:line-end_line` -- the line range that slices the whole entity,
        or None when no end is known (decl-only / stub). Lets a reader jump to
        (line..end_line) and read the function/class/enum/typedef without
        scanning the file."""
        if not self.file or not self.line or not self.end_line:
            return None
        return f"{self.file.name}:{self.line}-{self.end_line}"

    @property
    def is_stub(self) -> bool:
        """A minted placeholder for a target that was never indexed: a call/base
        /override/primary USR anchored by an edge but with no definition or
        declaration in any *indexed* file. Keyed on the absence of a registered
        location plus resolved=0: a row is a stub when it is unresolved AND it
        has no location in an indexed file -- either none at all, or only an
        `external` raw path (a system/stdlib target whose file no component
        owns). The external path lets the stub PRINT a location (e.g.
        `stl_iterator.h:NNNN`) without making it a real indexed symbol. NOT keyed
        on spelling -- stubs are minted NAMED from the reference cursor."""
        return not self.resolved and (self.file is None or self.external)

    def source(self, default_lines: int = 10, *, encoding: str = "utf-8") -> str:
        """The symbol's own source text, read straight off disk.

        Slices ``(line, col)..(end_line, end_col)`` -- its stored extent -- via
        :meth:`File.source`. When no extent is stored (decl-only / stub), falls
        back to ``default_lines`` whole lines starting at ``line``. Returns
        ``""`` when the symbol has no file/line; raises ``OSError`` if the file
        can't be read (mirrors :meth:`File.source`)."""
        if not self.file or not self.line:
            return ""
        start = (self.line, self.col or 1)
        if self.end_line is not None:
            return self.file.source(
                start, (self.end_line, self.end_col or 1), encoding=encoding
            )
        end_line = self.line + max(default_lines - 1, 0)
        text = self.file.source(start, (end_line, 1 << 30), encoding=encoding)
        return text[:-1] if text.endswith("\n") else text

    def to_dict(self) -> dict[str, Any]:
        """Stable JSON-serializable view. Identical-by-spec to the C++ port."""
        return {
            "id": self.id,
            "usr": self.usr,
            "semantic_universe": self.semantic_universe,
            "identity_key": self.identity_key,
            "spelling": self.spelling,
            "qual_name": self.name,
            "kind": self.kind,
            "type_info": self.type_info,
            "const_value": self.const_value,
            "file": self.file.path if self.file else None,
            "line": self.line,
            "col": self.col,
            "end_line": self.end_line,
            "end_col": self.end_col,
            "is_definition": self.is_definition,
            "is_pure": self.is_pure,
            "is_static": self.is_static,
            "is_instantiation": self.is_instantiation,
            "is_stub": self.is_stub,
        }

    def __repr__(self) -> str:
        nm = self.name or self.usr
        tag = " stub" if self.is_stub else ""
        return f"Sym(#{self.id} {self.kind} {nm} @{self.loc}{tag})"


@dataclass(frozen=True)
class Definition:
    """One backend body of a (possibly redefined) symbol -- a `definition` row.

    `sym` is the symbol being defined; `file`/`line`/`col` locate THIS body
    (a redefined symbol has several, one per backend file/component)."""

    sym: Sym
    component: Optional[str]
    file: Optional[File]
    line: Optional[int]
    col: Optional[int]
    end_line: Optional[int] = None
    end_col: Optional[int] = None
    init_text: Optional[str] = None  # v28: a (static member) variable's
    # initializer source text for THIS backend (`= seed_a()` -> 'seed_a()'); None
    # for functions and uninitialized variables.
    def_id: int = -1  # `definition` row id -- the handle for this body's own
    # per-backend call/use edges (GraphQuery.callees_of_definition). NOT in
    # to_dict (keeps the JSON view byte-identical to the C++ port).

    @property
    def loc(self) -> str:
        if not self.file:
            return "<no-location>"
        return f"{self.file.name}:{self.line}" if self.line else self.file.name

    def to_dict(self) -> dict[str, Any]:
        return {
            "usr": self.sym.usr,
            "semantic_universe": self.sym.semantic_universe,
            "identity_key": self.sym.identity_key,
            "name": self.sym.name,
            "kind": self.sym.kind,
            "component": self.component,
            "file": self.file.name if self.file else None,
            "line": self.line,
            "col": self.col,
            "end_line": self.end_line,
            "end_col": self.end_col,
            "init_text": self.init_text,
        }


# Generated type identifiers are the read-side contract shared with C++.
TYPE_KIND_NAMES = dict(_GENERATED_TYPE_KIND_NAMES)


@dataclass(frozen=True)
class TypeInfo:
    """Display info for one `type_node` row (v30 signature/type tier).

    `canonical` is the canonical shape's spelling when this node is sugared
    (an alias layer anywhere inside), else None."""

    id: int
    spelling: str
    kind: str
    canonical: Optional[str] = None
    decl_usr: Optional[str] = None
    is_const: bool = False
    is_volatile: bool = False
    is_restrict: bool = False
    extent: Optional[str] = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "spelling": self.spelling,
            "kind": self.kind,
            "canonical": self.canonical,
            "decl_usr": self.decl_usr,
            "const": self.is_const,
            "volatile": self.is_volatile,
            "restrict": self.is_restrict,
            "extent": self.extent,
        }


@dataclass(frozen=True)
class ParamInfo:
    """One parameter of a callable (a `parameter` row)."""

    position: int
    name: Optional[str]
    type: Optional[TypeInfo]

    def to_dict(self) -> dict[str, Any]:
        return {
            "position": self.position,
            "name": self.name,
            "type": self.type.to_dict() if self.type else None,
        }


@dataclass(frozen=True)
class SignatureInfo:
    """Everything the v30 tier knows about one symbol: returns/params for
    callables, of_type for variables/fields, underlying for typedef/alias."""

    returns: Optional[TypeInfo] = None
    params: tuple[ParamInfo, ...] = ()
    of_type: Optional[TypeInfo] = None
    underlying: Optional[TypeInfo] = None

    @property
    def empty(self) -> bool:
        return (self.returns is None and not self.params
                and self.of_type is None and self.underlying is None)


@dataclass(frozen=True)
class SignatureSlot:
    """One public callable signature slot, including source/adjusted types."""

    role: str
    position: Optional[int]
    pack_index: Optional[int]
    name: Optional[str]
    declared_type: Optional[TypeInfo]
    adjusted_type: Optional[TypeInfo]
    mode: str
    value_kind: str
    named_decl: Optional[str]
    reference_semantics: Optional[str] = None
    default: Optional[str] = None
    default_origin: Optional[str] = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "role": self.role,
            "position": self.position,
            "pack_index": self.pack_index,
            "name": self.name,
            "declared_type": self.declared_type.to_dict() if self.declared_type else None,
            "adjusted_type": self.adjusted_type.to_dict() if self.adjusted_type else None,
            "mode": self.mode,
            "value_kind": self.value_kind,
            "named_decl": self.named_decl,
            "reference_semantics": self.reference_semantics,
            "default": self.default,
            "default_origin": self.default_origin,
        }


@dataclass(frozen=True)
class TypeUser:
    """One symbol whose signature/type facts reach a queried type: role is
    'param' (with position), 'returns', 'of_type', or 'underlying_type'."""

    sym: Sym
    role: str
    position: Optional[int] = None


@dataclass(frozen=True)
class Edge:
    """A typed relationship. `peer` is the symbol at the other end."""

    edge_id: int
    kind: str  # edge_kind name
    src_id: int
    dst_id: int
    peer: Sym  # the neighbor reached by following this edge
    count: int  # call/use multiplicity
    base_access: Optional[int]
    is_virtual: Optional[int]
    sites: Sequence["Site"] = ()  # WHERE the edge occurs (use/call locations);
    # eager-loaded by _edges so to_dict() always
    # surfaces the reference site, not just the peer

    def to_dict(self, sites: Optional[Sequence["Site"]] = None) -> dict[str, Any]:
        """Stable JSON view: the peer symbol's fields, plus edge metadata.

        The result is the *peer* (id/usr/qual_name/kind/file/line) augmented with
        the edge `kind`, `count`, and `sites[]`. This is the shape the cidx-graph
        skill consumes for callers/callees/refs/neighbors.

        `sites[]` records WHERE the relationship occurs (e.g. the line a type is
        used, distinct from the peer symbol's own declaration line). It defaults
        to the edge's eager-loaded `self.sites`; pass `sites=` to override.
        """
        d = self.peer.to_dict()
        d["edge_kind"] = self.kind
        d["count"] = self.count
        if self.base_access is not None:
            d["base_access"] = self.base_access
        if self.is_virtual is not None:
            d["is_virtual"] = bool(self.is_virtual)
        effective = sites if sites is not None else self.sites
        d["sites"] = [s.to_dict() for s in effective]
        return d

    def __repr__(self) -> str:
        extra = f" x{self.count}" if self.count and self.count != 1 else ""
        return f"Edge({self.kind}{extra} -> {self.peer!r})"


@dataclass(frozen=True)
class Site:
    """A concrete source location where an edge occurs -- the grounding."""

    file: Optional[str]
    line: Optional[int]
    col: Optional[int]
    conditional: bool  # inside an #if / template that may not compile
    args_sig: Optional[str]
    # Phase 2: receiver provenance for virtual dispatch (NULL for non-member calls)
    recv_src_kind: Optional[str] = None
    recv_type_usr: Optional[str] = None
    recv_decl_usr: Optional[str] = None
    recv_param_pos: Optional[int] = None  # 0-based index of receiver in callee params
    # Phase 3: value-ness flag (1=by-value exact, 0/None=not-value -> TOP)
    recv_type_is_value: Optional[int] = None

    @property
    def loc(self) -> str:
        if not self.file:
            return "<no-location>"
        base = os.path.basename(self.file)
        return f"{base}:{self.line}:{self.col}" if self.line else base

    def to_dict(self) -> dict[str, Any]:
        return {
            "file": self.file,
            "line": self.line,
            "col": self.col,
            "conditional": self.conditional,
            "args_sig": self.args_sig,
        }

    def __repr__(self) -> str:
        c = " (conditional)" if self.conditional else ""
        return f"Site({self.loc}{c})"


@dataclass(frozen=True)
class CallArg:
    """Provenance of one positional argument at a call site (Phase 2).

    Keyed by (edge_id, file_id, line, col, position); one row per
    non-literal positional arg of a ``calls`` edge_site."""

    position: int
    src_kind: str  # local|construct|member|global|call_result|unknown
    type_usr: Optional[str] = None  # USR of the arg's static record type
    decl_usr: Optional[str] = None  # USR of the named local/param/field
    callee_usr: Optional[str] = None  # USR of callee for call_result
    # Phase 3: value-ness flag (1=by-value exact, 0/None=not-value -> TOP)
    type_is_value: Optional[int] = None

    def __repr__(self) -> str:
        return (
            f"CallArg(#{self.position} {self.src_kind}"
            f"{f' decl={self.decl_usr!r}' if self.decl_usr else ''}"
            f"{f' type={self.type_usr!r}' if self.type_usr else ''}"
            f"{f' callee={self.callee_usr!r}' if self.callee_usr else ''})"
        )


@dataclass(frozen=True)
class CallContext:
    """One incoming ``calls`` edge to a callee, with its argument provenance —
    the unit the closed-world param-Γ union consumes (Phase 3b)."""

    caller: "Sym"
    edge_id: int
    site: Optional[Site]
    args: list[CallArg]


#: template_param.param_kind / template_arg.arg_kind code -> readable name.
TEMPLATE_PARAM_KINDS = {
    1: "type", 2: "non-type", 3: "template-template",
    4: "type-pack", 5: "non-type-pack", 6: "template-template-pack",
}
TEMPLATE_ARG_KINDS = {1: "type", 2: "non-type", 3: "template", 4: "pack"}


@dataclass(frozen=True)
class TemplateParam:
    """A formal template parameter declared by a template entity.

    `position` is 0-based, in declaration order. `param_kind` is 1=type,
    2=non-type, 3=template-template, 4=pack (see `TEMPLATE_PARAM_KINDS`).
    `name` is the parameter spelling (``T``), `default` the default argument
    text when one was recorded."""

    position: int
    param_kind: int
    name: Optional[str]
    default: Optional[str] = None
    type: Optional[TypeInfo] = None
    default_type: Optional[TypeInfo] = None
    default_ref: Optional[Sym] = None

    @property
    def kind_name(self) -> str:
        return TEMPLATE_PARAM_KINDS.get(self.param_kind, str(self.param_kind))

    def to_dict(self) -> dict[str, Any]:
        return {
            "position": self.position,
            "param_kind": self.param_kind,
            "kind_name": self.kind_name,
            "name": self.name,
            "default": self.default,
            "type": self.type.to_dict() if self.type else None,
            "default_type": self.default_type.to_dict() if self.default_type else None,
            "default_ref": self.default_ref.to_dict() if self.default_ref else None,
        }

    def __repr__(self) -> str:
        return f"TemplateParam(#{self.position} {self.kind_name} {self.name})"


@dataclass(frozen=True)
class TemplateArg:
    """A concrete template argument bound by a specialization or instantiation.

    `arg_kind` is 1=type, 2=non-type value, 3=template-template, 4=pack. For a
    TYPE arg, `literal` holds the type spelling (``int``, ``std::string``) and
    `ref_id` the indexed symbol id when the argument type is itself indexed
    (None for builtins/unindexed types). For a non-type (INTEGRAL) arg,
    `literal` holds the value text."""

    position: int
    arg_kind: int
    ref_id: Optional[int] = None
    literal: Optional[str] = None
    pack_index: Optional[int] = None
    type: Optional[TypeInfo] = None

    @property
    def kind_name(self) -> str:
        return TEMPLATE_ARG_KINDS.get(self.arg_kind, str(self.arg_kind))

    def to_dict(self) -> dict[str, Any]:
        return {
            "position": self.position,
            "arg_kind": self.arg_kind,
            "kind_name": self.kind_name,
            "ref_id": self.ref_id,
            "literal": self.literal,
            "pack_index": self.pack_index,
            "type": self.type.to_dict() if self.type else None,
        }

    def __repr__(self) -> str:
        what = self.literal if self.literal is not None else f"#{self.ref_id}"
        return f"TemplateArg(#{self.position} {self.kind_name} {what})"


@dataclass(frozen=True)
class Selection:
    """One entry of a virtual call's selection map: if the receiver's run-time
    type is ``selecting_type``, the call dispatches to ``target``.

    ``inherited`` is False for a type that declares its own override, True for a
    subtype that inherits an ancestor's override (only produced when
    ``dispatch_selection(close_subtypes=True)``)."""

    selecting_type: Optional[Sym]
    target: Sym
    inherited: bool = False

    def to_dict(self) -> dict[str, Any]:
        return {
            "selecting_type": (
                self.selecting_type.to_dict()
                if self.selecting_type is not None
                else None
            ),
            "target": self.target.to_dict() if self.target is not None else None,
            "inherited": self.inherited,
        }

    def __repr__(self) -> str:
        st = self.selecting_type.name if self.selecting_type else "?"
        tg = self.target.name if self.target else "?"
        tag = " inherited" if self.inherited else ""
        return f"Selection({st} -> {tg}{tag})"


@dataclass(frozen=True)
class CallerWithContext:
    """A caller (or callee) reached via a template instantiation rollup.

    Returned by ``GraphQuery.callers(sym, include_instantiations=True)`` and the
    matching ``callees`` overload when the opt-in path is active.

    Attributes:
        sym                 The caller/callee symbol.
        via_instantiation   The instantiation node (``X<int>::print``) through
                            which this peer was reached.  ``None`` for direct
                            callers/callees (not reached via any instantiation).
        via_template_args   The concrete template arguments of the instantiation
                            TYPE node that owns ``via_instantiation`` (e.g.
                            ``[TemplateArg(0, type, 'int')]`` for ``X<int>``).
                            Empty list for direct callers/callees or when the
                            instantiation type node carries no stored arguments.

    Usage example::

        for r in g.callers(x_print, include_instantiations=True):
            args = [a.literal for a in r.via_template_args]
            print(r.sym.name, "via", args or "direct")
            # -> caller_int via ['int']
            # -> caller_double via ['double']
    """

    sym: Sym
    via_instantiation: Optional[Sym]
    via_template_args: list[TemplateArg]

    def __repr__(self) -> str:
        targs = (
            "<" + ", ".join(a.literal or "?" for a in self.via_template_args) + ">"
            if self.via_template_args
            else ""
        )
        tag = f" via{targs}" if self.via_instantiation else ""
        return f"CallerWithContext({self.sym!r}{tag})"


@dataclass(frozen=True)
class DispatchSite:
    """The Phase-1 over-approximation of one virtual call.

    ``declared_target`` is the statically-recorded callee (what the `calls` edge
    points at); ``receiver_static_type`` is the class that declares it.
    ``candidates`` is the full selection map -- every concrete type the call
    could land on, paired with its target. ``prunable`` is True when Phase 2 may
    safely narrow this site; when False, ``unprunable_reasons`` says why it must
    stay fully expanded ('not-virtual' | 'no-receiver-type' | 'target-stub' |
    'pure-no-targets' | 'unknown-symbol'). Phase 1 performs NO pruning -- it only
    records this data so Phase 2 can act on it later."""

    receiver_static_type: Optional[Sym]
    declared_target: Optional[Sym]
    candidates: tuple[Selection, ...] = ()
    prunable: bool = False
    unprunable_reasons: tuple[str, ...] = ()

    @property
    def targets(self) -> tuple[Sym, ...]:
        """Every concrete target method this call could reach."""
        return tuple(c.target for c in self.candidates)

    def to_dict(self) -> dict[str, Any]:
        return {
            "receiver_static_type": (
                self.receiver_static_type.to_dict()
                if self.receiver_static_type
                else None
            ),
            "declared_target": (
                self.declared_target.to_dict() if self.declared_target else None
            ),
            "candidates": [c.to_dict() for c in self.candidates],
            "prunable": self.prunable,
            "unprunable_reasons": list(self.unprunable_reasons),
        }

    def __repr__(self) -> str:
        tg = self.declared_target.name if self.declared_target else "?"
        state = (
            "prunable"
            if self.prunable
            else f"unprunable({','.join(self.unprunable_reasons)})"
        )
        return f"DispatchSite({tg}: {len(self.candidates)} candidate(s), {state})"


# --------------------------------------------------------------------------- #
# The query handle.
# --------------------------------------------------------------------------- #

_SYM_COLS = (
    "s.id, s.usr, s.spelling, s.qual_name, s.display_name, s.kind, s.type_info, "
    "s.file_id, s.line, s.col, s.end_line, s.end_col, "
    "s.decl_file_id, s.decl_line, s.decl_col, "
    "s.decl_path, s.is_definition, s.is_pure, s.is_static, s.is_instantiation, "
    "s.access, s.parent_usr, s.resolved, s.multi_def, s.const_value, "
    "s.semantic_universe_id, s.identity_key, s.callable_kind, "
    "s.template_origin, s.template_form, "
    "(SELECT key FROM semantic_universe su "
    " WHERE su.id = s.semantic_universe_id) AS semantic_universe"
)


class GraphQuery:
    """Read-only handle on a cidx index database.

    Every method returns compact `Sym` / `Edge` / `Site` values or lists of them.
    Traversals are bounded by `limit`/`depth` -- never unbounded -- so a query
    over a million-edge graph still hands back a small, reasonable result.

    Construct from a path (opened read-only) or wrap an existing sqlite3
    connection with `GraphQuery.from_connection(conn)` (used by tests that seed
    an in-memory DB and by code that already holds a Storage connection).
    """

    def __init__(self, db_path: str, *, require_edges: bool = False):
        if not os.path.exists(db_path):
            raise NoIndexError(
                f"no cidx index at {db_path!r}. Build one with:\n"
                "    cd <repo> && cidx component add --path . && cidx import "
                "--db <build> && cidx index && cidx resolve\n"
                "or pass --db PATH / set $INDEXER_CACHE."
            )
        # Read-only: file:...?mode=ro guards against accidental writes.
        uri = f"file:{os.path.abspath(db_path)}?mode=ro"
        self._c = sqlite3.connect(uri, uri=True)
        # Keep the Python graph surface aligned with the C++ read-only replay
        # profile. These are connection-local settings and do not mutate the
        # database or create journal/WAL sidecars.
        self._c.execute("PRAGMA busy_timeout = 5000")
        self._c.execute("PRAGMA foreign_keys = ON")
        self._c.execute("PRAGMA query_only = ON")
        self._c.row_factory = sqlite3.Row
        self.db_path = db_path
        self._owns_conn = True
        _validate_catalog_hash(self._c, db_path, require_present=True)
        self._file_cache: Optional[dict[int, tuple[str, Optional[str]]]] = None
        self._resolved: Optional[bool] = None
        self._entity_nodes_ready: Optional[bool] = None
        if require_edges:
            self.require_edges()

    @classmethod
    def from_connection(
        cls, conn: sqlite3.Connection, db_path: str = "<connection>"
    ) -> "GraphQuery":
        """Wrap an already-open sqlite3 connection (does not take ownership)."""
        self = cls.__new__(cls)
        conn.row_factory = sqlite3.Row
        self._c = conn
        self.db_path = db_path
        self._owns_conn = False
        _validate_catalog_hash(conn, db_path, require_present=True)
        self._file_cache = None
        self._resolved = None
        self._entity_nodes_ready = None
        return self

    # -- lifecycle ----------------------------------------------------------- #

    def close(self) -> None:
        if self._owns_conn:
            self._c.close()

    def __enter__(self) -> "GraphQuery":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    # -- guards -------------------------------------------------------------- #

    def edge_count(self) -> int:
        """Total number of edges. 0 means the graph layer is empty."""
        from .queryplan import Executor, codebase, count, nodes, start, view
        from .storage import Storage

        plan = (start(codebase()) | view("edge") | nodes() | count()).plan
        result = Executor(Storage.from_connection(self._c, self.db_path)).run(plan)
        return int(result.scalar)

    def require_edges(self) -> None:
        """Raise NoEdgesError unless the index has at least one edge.

        The standard-DB discipline: a graph query against an edge-less index is a
        hard error, never a silent fall-back to another database.
        """
        if self.edge_count() == 0:
            raise NoEdgesError(
                f"index {self.db_path!r} has no graph edges -- it was built with "
                "`cidx index --no-graph`, or the graph was cleared. Re-run "
                "`cidx index` (without --no-graph) then `cidx resolve`."
            )

    def plan_for(
        self,
        sym,
        relation: Optional[str] = None,
        direction: str = "out",
        min_depth: int = 1,
        max_depth: int = 1,
    ):
        """Build the canonical QueryPlan for a legacy symbol-graph read."""
        from .queryplan import (
            all_of,
            codebase,
            eq,
            in_ as plan_in,
            nodes,
            out as plan_out,
            start,
            symbol,
        )

        resolved = self.get(sym)
        if resolved is None:
            return start(codebase()) | nodes(all_of([eq("id", -1), eq("id", -2)]))
        query = start(symbol(resolved.usr))
        if relation is None:
            return query
        if direction == "out":
            return query | plan_out(relation, min_depth, max_depth)
        if direction == "in":
            return query | plan_in(relation, min_depth, max_depth)
        raise ValueError("direction must be 'in' or 'out'")

    def _adapter_ids(self, plan) -> list[int]:
        """Execute a legacy read's canonical plan and return its node ids.

        The compatibility methods below still materialize their historical
        records (including evidence and ordering), but the candidate set is
        selected by the shared QueryPlan executor.  This keeps the old result
        shape while removing direct SQL traversal from the semantic boundary.
        """
        from .queryplan import Executor, select as plan_select
        from .storage import Storage

        store = Storage.from_connection(self._c, self.db_path)
        result = Executor(store).run((plan | plan_select(["id"])).plan)
        if result.truncated:
            raise _AdapterPlanTruncated(
                "QueryPlan candidate set was truncated; use the legacy read"
            )
        return [
            int(row[0] if not isinstance(row, dict) else row["id"])
            for row in result.rows
        ]

    def _adapter_ids_complete(self, plan) -> list[int]:
        """Execute an adapter plan completely without corrupting its order.

        The executor cursor advances the physical symbol-id enumeration.  It
        cannot be used directly on a plan that orders by another field: each
        page would be independently sorted and a cursor based on the last
        emitted row could skip or repeat rows.  Such plans are collected in
        physical-id pages, then globally ordered and limited using the same
        plan field values before compatibility hydration.
        """
        from .queryplan import Executor, Plan, select as plan_select, validate
        from .storage import Storage

        store = Storage.from_connection(self._c, self.db_path)
        normalized = validate(plan.plan if hasattr(plan, "plan") else plan)
        order_fields = tuple(
            field
            for stage in normalized.stages
            if stage.op == "order_by"
            for field in stage.fields
        )
        if order_fields:
            limit_values = [
                stage.n for stage in normalized.stages if stage.op == "limit"
            ]
            collection_stages = tuple(
                stage for stage in normalized.stages
                if stage.op not in ("select", "order_by", "limit")
            )
            fields = tuple(dict.fromkeys(("id", *order_fields)))
            collection_plan = Plan(
                source=normalized.source,
                stages=collection_stages + (plan_select(fields),),
            )
            collected: list[tuple[int, tuple[object, ...]]] = []
            after_id: Optional[int] = None
            while True:
                result = Executor(store).run(collection_plan, after_id=after_id)
                for row in result.rows:
                    values = tuple(row)
                    if values:
                        collected.append((int(values[0]), values[1:]))
                if not result.truncated or not result.rows:
                    break
                after_id = max(int(row[0]) for row in result.rows)

            def cell_key(value: object) -> tuple[int, object]:
                if value is None:
                    return (2, "")
                if isinstance(value, (int, float)):
                    return (0, value)
                return (1, str(value))

            collected.sort(
                key=lambda item: (
                    tuple(cell_key(value) for value in item[1]), item[0]
                )
            )
            if limit_values:
                del collected[limit_values[-1]:]
            return [sid for sid, _values in collected]

        ids: list[int] = []
        after_id: Optional[int] = None
        while True:
            result = Executor(store).run(normalized, after_id=after_id)
            ids.extend(
                int(row[0] if not isinstance(row, dict) else row["id"])
                for row in result.rows
            )
            if not result.truncated or not result.rows:
                return ids
            after_id = max(
                int(row[0] if not isinstance(row, dict) else row["id"])
                for row in result.rows
            )

    def _adapter_symbols(self, plan) -> list[Sym]:
        """Hydrate symbols in the exact order emitted by a QueryPlan."""
        plan = plan.plan if hasattr(plan, "plan") else plan
        ids = self._adapter_ids_complete(plan)
        out: list[Sym] = []
        for sid in ids:
            row = self._c.execute(
                f"SELECT {_SYM_COLS} FROM symbol s WHERE s.id = ?", (sid,)
            ).fetchone()
            if row is not None:
                out.append(self._sym(row))
        return out

    def _adapter_site_keys(self, edge_id: int, limit: int) -> list[tuple[int, int, int, int]]:
        """Select site identities through the typed QueryPlan edge view."""
        from .queryplan import (
            Executor, codebase, eq, limit as plan_limit, nodes, order_by,
            select as plan_select, sites as plan_sites, start, view,
        )
        from .storage import Storage

        if limit == 0:
            return []
        query = (
            start(codebase())
            | view("edge")
            | nodes(eq("edge_id", edge_id))
            | plan_sites()
            | plan_select(["edge_id", "file_id", "line", "col"])
            | order_by(["file_id", "line", "col"])
        )
        # SQLite historically treats LIMIT 0 as an empty result and LIMIT -1
        # as unbounded.  QueryPlan deliberately rejects non-positive limits,
        # so preserve that public boundary before constructing a plan.
        if limit > 0:
            query = query | plan_limit(limit)
        store = Storage.from_connection(self._c, self.db_path)
        result = Executor(store).run(query.plan)
        return [tuple(int(value) for value in row) for row in result.rows]

    def _adapter_symbol_rows(self, sym_id: int, fields: Sequence[str]) -> list[tuple]:
        """Select compatibility-owned symbol fields through QueryPlan."""
        from .queryplan import Executor, codebase, eq, nodes, select as plan_select, start
        from .storage import Storage

        plan = (
            start(codebase()) | nodes(eq("id", sym_id)) | plan_select(fields)
        ).plan
        result = Executor(Storage.from_connection(self._c, self.db_path)).run(plan)
        return [tuple(row) for row in result.rows]

    def _adapter_signature_slot_rows(self, sym_id: int) -> list[tuple]:
        """Select all signature slots, including order and result fields."""
        from .queryplan import Executor, codebase, eq, nodes, order_by
        from .queryplan import out as plan_out, select as plan_select, start
        from .storage import Storage

        fields = [
            "slot_kind", "position", "pack_index", "name", "type_id",
            "declared_type_id", "adjusted_type_id", "default_text",
            "default_origin", "reference_semantics", "mode", "value_kind",
            "named_decl",
        ]
        plan = (
            start(codebase()) | nodes(eq("id", sym_id))
            | plan_out("has_signature_slot")
            | plan_select(fields)
            | order_by(["position", "pack_index", "slot_kind"])
        ).plan
        result = Executor(Storage.from_connection(self._c, self.db_path)).run(plan)
        if result.truncated:
            raise _AdapterPlanTruncated("signature slot plan was truncated")
        return [tuple(row) for row in result.rows]

    def _adapter_rows(self, plan, fields: Sequence[str], *, order: Sequence[str] = (),
                      limit: int = 0) -> list[tuple]:
        """Run a typed compatibility selection before legacy row hydration."""
        from .queryplan import Executor, limit as plan_limit, order_by, select as plan_select, validate
        from .storage import Storage

        query = plan | plan_select(fields)
        if order:
            query = query | order_by(order)
        if limit:
            query = query | plan_limit(limit)
        store = Storage.from_connection(self._c, self.db_path)
        result = Executor(store).run(validate(query.plan))
        if result.truncated:
            raise _AdapterPlanTruncated("typed QueryPlan candidate set was truncated")
        return [tuple(row) for row in result.rows]

    def _adapter_type_ids(self, type_id: int) -> list[int]:
        from .queryplan import Executor, codebase, eq, nodes, select as plan_select, start, view
        from .storage import Storage

        type_row = self._c.execute(
            "SELECT type_key FROM type_node WHERE id = ?", (type_id,)
        ).fetchone()
        type_key = type_row[0] if type_row is not None else f"__missing_type_{type_id}__"
        plan = (
            start(codebase()) | view("type") | nodes(eq("type_key", type_key))
            | plan_select(["id", "type_key", "kind", "spelling", "decl_usr",
                          "canonical_id", "extent"])
        ).plan
        result = Executor(Storage.from_connection(self._c, self.db_path)).run(plan)
        return [type_id for _row in result.rows]

    def _adapter_edge_ids(self, edge_id: int) -> list[int]:
        from .queryplan import Executor, codebase, eq, nodes, select as plan_select, start, view
        from .storage import Storage

        plan = (
            start(codebase()) | view("edge") | nodes(eq("edge_id", edge_id))
            | plan_select(["edge_id"])
        ).plan
        result = Executor(Storage.from_connection(self._c, self.db_path)).run(plan)
        return [int(row[0]) for row in result.rows]

    def _adapter_edge_rows(
        self, sym_id: int, direction: str,
        kind_ids: Optional[Sequence[int]], limit: int,
    ) -> list[tuple]:
        """Select the complete edge page through the typed plan boundary."""
        from .queryplan import (
            Executor, all_of, codebase, eq, limit as plan_limit,
            nodes, order_by, select as plan_select, start, view,
        )
        from .storage import Storage

        if limit == 0:
            return []
        field = "dst_id" if direction == "in" else "src_id"
        predicates = [eq(field, sym_id)]
        query = (
            start(codebase()) | view("edge") |
            nodes(all_of(predicates)) |
            plan_select([
                "edge_id", "src_id", "dst_id", "kind", "count",
                "base_access", "is_virtual", "negative_count",
            ]) |
            order_by(["negative_count", "kind", "edge_id"])
        )
        if limit > 0:
            query = query | plan_limit(limit)
        result = Executor(Storage.from_connection(self._c, self.db_path)).run(query.plan)
        if result.truncated:
            raise _AdapterPlanTruncated("edge QueryPlan candidate set was truncated")
        return [tuple(row) for row in result.rows]

    @staticmethod
    def _symbol_plan(pred, *, kind: Optional[str] = None, limit: int = 0,
                     order: Sequence[str] = ("name_length", "name")):
        from .queryplan import all_of, codebase, eq, limit as plan_limit
        from .queryplan import nodes, order_by, select as plan_select, start

        predicates = [] if pred is None else [pred]
        if kind is not None:
            predicates.append(eq("kind", kind))
        stage = nodes(all_of(predicates)) if predicates else nodes()
        plan = start(codebase()) | stage
        # Selecting id before ordering makes the plan's row shape explicit;
        # the compatibility layer never reorders or applies a second limit.
        plan = plan | plan_select(["id", *order]) | order_by(order)
        return plan | plan_limit(limit) if limit else plan

    def _seed_ids(self, ident) -> list[int]:
        from .queryplan import codebase, eq, nodes, start

        field, value = ("id", ident) if isinstance(ident, int) else ("usr", ident)
        return self._adapter_ids(start(codebase()) | nodes(eq(field, value)))

    def _peer_ids(self, sym, direction: str, kinds) -> Optional[set[int]]:
        from .queryplan import PlanError

        if not kinds:
            # The legacy all-kinds read has no single relation to lower. The
            # seed still goes through the canonical adapter; SQL retains the
            # historical all-relations ordering/evidence expansion.
            self.plan_for(sym)
            return None
        relation_names = tuple(kinds) if kinds else tuple(EDGE_KINDS)
        ids: set[int] = set()
        for relation in relation_names:
            try:
                ids.update(
                    self._adapter_ids(
                        self.plan_for(sym, relation=relation, direction=direction)
                    )
                )
            except (PlanError, _AdapterPlanTruncated):
                # Typed cross-view legacy relations (notably symbol.of_type)
                # retain their historical SQL/evidence adapter until the
                # typed-view executor exposes that transition.  Do not apply
                # the successful relations' partial candidate set: doing so
                # would silently drop the failed relation's legacy rows.
                return None
        return ids

    def _is_resolved(self) -> bool:
        """True once `cidx resolve` has rolled up edge counts (meta flag set).

        When unset, edge.count is not authoritative, so multiplicity falls back
        to COUNT(edge_site)."""
        if self._resolved is None:
            row = self._c.execute(
                "SELECT value FROM meta WHERE key = 'graph_resolved_at'"
            ).fetchone()
            self._resolved = bool(row and row[0])
        return self._resolved

    # -- internal: file path / Sym construction ------------------------------ #

    def _files(self) -> dict[int, tuple[str, Optional[str]]]:
        """{file_id: (abs_path, component_name)} -- loaded once, cached.

        Routes each distinct component through ``Storage.component_abs_base``
        (the single resolution choke point, v24) rather than joining
        ``component.path`` in raw -- a grouped component's stored path is
        RELATIVE to its repository's active clone root, so using it as-is
        would hand back a clone-relative (unopenable) path."""
        if self._file_cache is None:
            from indexer.storage import Component, Storage

            store = Storage.from_connection(self._c, self.db_path)
            abs_base_cache: dict[tuple, str] = {}
            cache: dict[int, tuple[str, Optional[str]]] = {}
            for r in self._c.execute(
                "SELECT f.id AS fid, c.name AS cname, c.path AS root, "
                "       c.version AS version, c.repository_id AS repo_id, "
                "       d.path AS rel, f.name AS name "
                "FROM file f JOIN directory d ON d.id = f.directory_id "
                "JOIN component c ON c.id = d.component_id"
            ):
                key = (r["root"], r["version"], r["repo_id"])
                eff = abs_base_cache.get(key)
                if eff is None:
                    comp_stub = Component(
                        name="",
                        path=r["root"],
                        version=r["version"],
                        repository_id=r["repo_id"],
                    )
                    eff = store.component_abs_base(comp_stub)
                    abs_base_cache[key] = eff
                path = (
                    os.path.join(eff, r["rel"], r["name"])
                    if r["rel"]
                    else os.path.join(eff, r["name"])
                )
                cache[r["fid"]] = (path, r["cname"])
            self._file_cache = cache
        return self._file_cache

    def make_file(
        self,
        path: Optional[str],
        *,
        external: bool = False,
        component_name: Optional[str] = None,
    ) -> "Optional[File]":
        """Wrap an absolute path into a :class:`File` bound to this query, or
        ``None`` for a falsy path. The single construction choke point so every
        ``Sym.file`` / ``Location.file`` carries the same DB-aware handle."""
        if not path:
            return None
        return File(
            path, self, external=external, component_name=component_name
        )

    def _sym(self, r: sqlite3.Row) -> Sym:
        files = self._files()
        fid, line, col = r["file_id"], r["line"], r["col"]
        # end_line/end_col are the end of the extent that (line, col) starts.
        # Only the best-known site (file_id) carries an end; the decl-site and
        # stub fallbacks below have no stored end, so they read None.
        end_line, end_col = r["end_line"], r["end_col"]
        if fid is None:  # decl-only: fall back to decl site
            fid, line, col = r["decl_file_id"], r["decl_line"], r["decl_col"]
            end_line = end_col = None
        if fid is not None:
            path, comp = files.get(fid, (None, None))
            external = False
        else:
            # No registered location. A stub for a target in an unregistered
            # (system/stdlib) file still carries the raw decl path + line/col.
            path, comp = r["decl_path"], None
            line, col = r["decl_line"], r["decl_col"]
            external = path is not None
        return Sym(
            id=r["id"],
            usr=r["usr"],
            semantic_universe_id=(r["semantic_universe_id"]
                                  if "semantic_universe_id" in r.keys() else -1),
            identity_key=(r["identity_key"] if "identity_key" in r.keys() else ""),
            semantic_universe=(r["semantic_universe"]
                               if "semantic_universe" in r.keys() else ""),
            spelling=r["spelling"],
            name=r["qual_name"] or r["spelling"],
            display_name=r["display_name"],
            kind=SYMBOL_KIND_NAMES.get(r["kind"], r["kind"]),
            type_info=r["type_info"],
            is_definition=bool(r["is_definition"]),
            is_pure=bool(r["is_pure"]),
            is_static=bool(r["is_static"]),
            is_instantiation=bool(r["is_instantiation"]),
            callable_kind=(r["callable_kind"] if "callable_kind" in r.keys() else None),
            template_origin=(r["template_origin"] if "template_origin" in r.keys() else None),
            template_form=(r["template_form"] if "template_form" in r.keys() else None),
            access=r["access"],
            parent_usr=r["parent_usr"],
            resolved=bool(r["resolved"]),
            component=comp,
            file=self.make_file(path, external=external, component_name=comp),
            line=line,
            col=col,
            end_line=end_line,
            end_col=end_col,
            external=external,
            multi_def=(r["multi_def"] if "multi_def" in r.keys() else 0),
            const_value=(r["const_value"] if "const_value" in r.keys() else None),
        )

    def _resolve_id(self, sym) -> int:
        """Resolve a compatibility symbol only after plan-owned selection."""
        sid = sym.id if isinstance(sym, Sym) else int(sym)
        # Every symbol-domain public read enters through this adapter gate.
        # The legacy operation may still hydrate richer rows below, but it
        # cannot select an unvalidated symbol id directly from SQLite.
        self._seed_ids(sid)
        return sid

    def _kind_ids(self, kinds: Optional[Iterable[str]]) -> Optional[list[int]]:
        if kinds is None:
            return None
        out = []
        for k in kinds:
            if k not in EDGE_KINDS:
                raise ValueError(
                    f"unknown edge kind {k!r}; valid: {sorted(EDGE_KINDS)}"
                )
            out.append(EDGE_KINDS[k])
        return out

    # ===================================================================== #
    # 1. LOOKUP SYMBOLS
    # ===================================================================== #

    def get(self, ident) -> Optional[Sym]:
        """Fetch one symbol by integer id, USR string, or pass-through Sym."""
        if isinstance(ident, Sym):
            return ident
        candidate_ids = self._seed_ids(ident)
        if not candidate_ids:
            return None
        col = "id" if isinstance(ident, int) else "usr"
        order = " ORDER BY s.semantic_universe_id, s.identity_key" if col == "usr" else ""
        placeholders = ",".join("?" for _ in candidate_ids)
        rows = self._c.execute(
            f"SELECT {_SYM_COLS} FROM symbol s WHERE s.id IN ({placeholders}){order}",
            candidate_ids,
        ).fetchall()
        if not rows:
            return None
        if col == "usr" and len(rows) > 1:
            raise ValueError(
                f"ambiguous symbol USR; pass a scoped symbol id: {ident}"
            )
        return self._sym(rows[0])

    def def_decl_locations(
        self, sym
    ) -> "tuple[Optional[tuple[Optional[str], Optional[int], Optional[int]]], Optional[tuple[Optional[str], Optional[int], Optional[int]]]]":
        """Return ``(definition_loc, declaration_loc)`` for `sym`.

        The compact `Sym` collapses a symbol to a single best-known location
        (definition, else declaration). This additive accessor surfaces BOTH so
        a caller can distinguish "declared here, defined there". Each loc is
        ``(abs_path, line, col)`` or None when that site is unknown. The
        declaration loc is returned even when it coincides with the definition;
        callers that only want the *distinct* declaration should compare them.

        Read-only and additive -- it does not alter any existing behaviour.
        """
        sid = self._resolve_id(sym)
        selected = self._adapter_symbol_rows(
            sid, ["id", "file", "line", "col", "decl_path"]
        )
        if not selected:
            return None, None
        r = self._c.execute(
            "SELECT file_id, line, col, decl_file_id, decl_line, decl_col, "
            "decl_path FROM symbol s WHERE s.id = ?",
            (sid,),
        ).fetchone()
        if r is None:
            return None, None
        files = self._files()

        def _loc(fid, line, col):
            if fid is None:
                return None
            path = files.get(fid, (None, None))[0]
            return (path, line, col)

        decl = _loc(r["decl_file_id"], r["decl_line"], r["decl_col"])
        if decl is None and r["decl_path"] is not None:
            # external/unregistered (system/stdlib) decl: raw path, no file row
            decl = (r["decl_path"], r["decl_line"], r["decl_col"])
        return (_loc(r["file_id"], r["line"], r["col"]), decl)

    def find(
        self, pattern: str, kind: Optional[str] = None, limit: int = 50
    ) -> list[Sym]:
        """Lookup by qualified name, fast path first. `kind` filters by
        symbol.kind; results are shortest-name-first (the closest matches).

        Three tiers, each returning as soon as it finds anything so the common
        case never touches the slow scan:

          1. **exact** ``qual_name = p OR spelling = p`` -- an indexed point
             lookup (idx_symbol_qual / idx_symbol_spelling). Most ``find`` calls
             pass an exact or fully-qualified name and stop here.
          2. **prefix** ``qual_name LIKE 'p%' OR spelling LIKE 'p%'`` -- a range
             SEARCH on the NOCASE indexes (idx_symbol_qual_nc / _spelling_nc).
          3. **fuzzy** ``COALESCE(qual_name, spelling) LIKE '%a%b%'`` -- the
             original '::'-segmented infix match (find('conf::set') matches
             'RdKafka::ConfImpl::set'), over COALESCE so C symbols with no
             qual_name are still found. This is a FULL TABLE SCAN, reached only
             when neither exact nor prefix matched (or the pattern is empty).

        Mirrors storage.search_symbols (the fuzzy tier is identical)."""
        if limit < 1:
            return []
        from .queryplan import any_of, eq, glob

        if kind is not None and kind not in SYMBOL_KIND_IDS:
            return []

        def run(pred) -> list[Sym]:
            return self._adapter_symbols(
                self._symbol_plan(pred, kind=kind, limit=limit)
            )

        if pattern:
            # Each historical tier is a complete plan. The first non-empty
            # plan wins; compatibility code only hydrates those selected ids.
            hits = run(any_of([eq("qual_name", pattern), eq("spelling", pattern)]))
            if hits:
                return hits
            hits = run(glob("name", _legacy_find_glob(pattern, prefix=True)))
            if hits:
                return hits
            return run(glob("name", _legacy_find_glob(pattern)))

        return self._adapter_symbols(
            self._symbol_plan(None, kind=kind, limit=limit)
        )

    def by_name(self, spelling: str, kind: Optional[str] = None) -> list[Sym]:
        """Exact-spelling lookup (overloads/statics yield several rows)."""
        from .queryplan import eq
        if kind is not None and kind not in SYMBOL_KIND_IDS:
            return []
        return self._adapter_symbols(
            self._symbol_plan(eq("spelling", spelling), kind=kind, order=("usr",))
        )

    def by_qual_or_spelling(self, *names: str, limit: int = 200) -> list[Sym]:
        """Exact, INDEX-backed lookup over qual_name and spelling for any of
        ``names`` -- uses idx_symbol_qual / idx_symbol_spelling (an OR of two
        equality IN-clauses), so it never scans the full symbol table the way
        the fuzzy LIKE ``find`` must. Shortest-name-first, same ordering as
        ``find`` so callers can swap one for the other. Empty / falsy names are
        dropped; returns [] when none remain."""
        uniq = list(dict.fromkeys(n for n in names if n))
        if not uniq or limit < 1:
            return []
        from .queryplan import any_of, eq
        return self._adapter_symbols(
            self._symbol_plan(
                any_of(
                    [eq("qual_name", name) for name in uniq]
                    + [eq("spelling", name) for name in uniq]
                ),
                limit=limit,
            )
        )

    def entity_nodes_ready(self) -> bool:
        """True once the materialized ``entity_node`` classification table holds
        at least one row (populated by ``resolve`` / the v22 backfill). Cached.

        The single-query record selectors (``records_by_name``) use it to decide
        whether the fast ``entity_node`` JOIN is usable, or whether they must
        fall back to per-record classification on a not-yet-materialized index."""
        if self._entity_nodes_ready is None:
            from .queryplan import Executor, codebase, count, nodes, start, view
            from .storage import Storage

            plan = (start(codebase()) | view("entity") | nodes() | count()).plan
            result = Executor(Storage.from_connection(self._c, self.db_path)).run(plan)
            self._entity_nodes_ready = result.scalar > 0
        return self._entity_nodes_ready

    def records_by_name(
        self,
        *names: str,
        symbol_kinds: Iterable[str],
        entity_kinds: Iterable[int],
        limit: int = 200,
    ) -> list[Sym]:
        """Records named in ``names`` whose C++ keyword (``symbol.kind``:
        ``"class"`` / ``"struct"`` / ``"union"``) AND materialized design type
        (``entity_node.kind`` ids: 1 class, 2 abstract_class, 3 interface,
        4 union) both match -- in ONE indexed query.

        Replaces "name lookup, then a per-candidate ``class_kind`` query": the
        JOIN onto ``entity_node`` (PK = ``symbol.id``) reads the abstractness
        classification that ``resolve`` already materialized, so
        ``CodeBase.klass`` / ``struct`` / ``record`` / ``interface`` /
        ``abstract_class`` are a single point lookup instead of N+1 queries.
        Exact/index-backed on ``idx_symbol_qual`` / ``idx_symbol_spelling``;
        shortest-name-first, same ordering as ``by_qual_or_spelling``."""
        uniq = list(dict.fromkeys(n for n in names if n))
        skids = [SYMBOL_KIND_IDS[k] for k in symbol_kinds if k in SYMBOL_KIND_IDS]
        eks = list(entity_kinds)
        if not uniq or not skids or not eks or limit < 1:
            return []
        from .queryplan import all_of, any_of, eq, in_list
        entity_names = (
            "other", "class", "abstract_class", "interface", "union",
            "enum", "class_template", "abstract_class_template",
            "interface_template", "namespace",
        )
        pred = all_of([
            any_of(
                [eq("qual_name", name) for name in uniq]
                + [eq("spelling", name) for name in uniq]
            ),
            in_list("kind", [
                name for name, value in SYMBOL_KIND_IDS.items()
                if value in skids
            ]),
            in_list("entity_type", [
                entity_names[value] if 0 <= value < len(entity_names) else "other"
                for value in eks
            ]),
        ])
        return [
            sym for sym in self._adapter_symbols(
                self._symbol_plan(pred, limit=limit)
            ) if not sym.is_instantiation
        ]

    def symbols_in_file(self, path_substr: str, limit: int = 500) -> list[Sym]:
        """Symbols whose definition file path contains `path_substr`. Useful to
        enumerate a file's API without opening it."""
        file_ids = [fid for fid, (p, _) in self._files().items() if path_substr in p]
        if not file_ids or limit < 1:
            return []
        from .queryplan import any_of, eq
        return self._adapter_symbols(
            self._symbol_plan(
                any_of([eq("file", fid) for fid in file_ids]),
                limit=limit,
                order=("line", "col"),
            )
        )

    # ===================================================================== #
    # 2. LOOKUP REFERENCES
    # ===================================================================== #

    def edges_in(
        self, sym, kinds: Optional[Sequence[str]] = None, limit: int = 500
    ) -> list[Edge]:
        """Incoming edges: who points AT this symbol (peer = the source)."""
        return self._edges(sym, "in", kinds, limit)

    def edges_out(
        self, sym, kinds: Optional[Sequence[str]] = None, limit: int = 500
    ) -> list[Edge]:
        """Outgoing edges: what this symbol points to (peer = the destination)."""
        return self._edges(sym, "out", kinds, limit)

    def _edges(
        self, sym, direction: str, kinds, limit: int, with_sites: bool = True
    ) -> list[Edge]:
        sid = self._resolve_id(sym)
        kids = self._kind_ids(kinds)
        if direction not in {"in", "out"}:
            raise ValueError(f"direction must be 'in' or 'out', got {direction!r}")
        peer_ids = self._peer_ids(sid, direction, kinds)
        if peer_ids is not None and not peer_ids:
            return []
        rows = self._adapter_edge_rows(
            sid, direction, kids, limit if peer_ids is None else -1
        )
        out = []
        for row in rows:
            edge_id, src_id, dst_id, kind_id, count, base_access, is_virtual, _ = row
            peer_id = src_id if direction == "in" else dst_id
            if peer_ids is not None and peer_id not in peer_ids:
                continue
            if kids and kind_id not in kids:
                continue
            peer = self.get(src_id if direction == "in" else dst_id)
            if peer is None:
                continue
            cnt = count
            site_count = self._c.execute(
                "SELECT COUNT(*) FROM edge_site WHERE edge_id = ?", (edge_id,)
            ).fetchone()[0]
            if not self._is_resolved() or not cnt:
                cnt = site_count or cnt or 1
            elif site_count > cnt:
                cnt = site_count
            out.append(Edge(edge_id=edge_id, kind=EDGE_NAMES[kind_id],
                            src_id=src_id, dst_id=dst_id, peer=peer,
                            count=cnt, base_access=base_access,
                            is_virtual=is_virtual))
            if limit > 0 and len(out) >= limit:
                break
        # Eager-load the reference sites (WHERE each edge occurs) so a serialized
        # edge always carries the use/call location, not just the peer's decl
        # line. Internal traversals that discard sites pass with_sites=False.
        if with_sites and out:
            smap = self._sites_for([e.edge_id for e in out])
            out = [replace(e, sites=tuple(smap.get(e.edge_id, ()))) for e in out]
        return out

    def _sites_for(self, edge_ids: Sequence[int]) -> dict[int, list["Site"]]:
        """Batch-fetch edge_site rows for many edges in one query (avoids the
        N+1 a per-edge sites() call would incur). Returns {edge_id: [Site, ...]}."""
        if not edge_ids:
            return {}
        files = self._files()
        q = ",".join("?" * len(edge_ids))
        out: dict[int, list[Site]] = {}
        for r in self._c.execute(
            "SELECT edge_id, file_id, line, col, conditional, args_sig, "
            "       recv_src_kind, recv_type_usr, recv_decl_usr, recv_param_pos,"
            "       recv_type_is_value "
            f"FROM edge_site_read WHERE edge_id IN ({q}) "
            "ORDER BY edge_id, file_id, line, col",
            list(edge_ids),
        ):
            p = files.get(r["file_id"], (None, None))[0] if r["file_id"] else None
            out.setdefault(r["edge_id"], []).append(
                Site(
                    file=p,
                    line=r["line"],
                    col=r["col"],
                    conditional=bool(r["conditional"]),
                    args_sig=r["args_sig"],
                    recv_src_kind=r["recv_src_kind"],
                    recv_type_usr=r["recv_type_usr"],
                    recv_decl_usr=r["recv_decl_usr"],
                    recv_param_pos=r["recv_param_pos"],
                    recv_type_is_value=r["recv_type_is_value"],
                )
            )
        return out

    def references(self, sym, limit: int = 500) -> list[Edge]:
        """All incoming `calls` + `uses` + `alias_of` + `of_type` edges --
        "who references this symbol". Each Edge.peer is the referrer;
        Edge.count is how many times; follow with sites() for exact file:line
        locations."""
        return self.edges_in(
            sym, kinds=("calls", "uses", "alias_of", "of_type"), limit=limit
        )

    def aliased_by(self, sym, limit: int = 500) -> list[Sym]:
        """Type aliases / typedefs whose underlying type directly names ``sym``.

        This is the inverse of the alias ``alias_of`` edge consumed by the model
        layer's ``Typedef.aliased()``. For ``using IntBox = Box<int>;`` it returns
        the ``IntBox`` alias when called on the concrete ``Box<int>`` instance.

        The relationship is direct by design: an alias-of-alias is returned for
        the intermediate alias entity, not for the final record it eventually
        resolves to."""
        if limit < 1:
            return []
        from .queryplan import in_list, select as plan_select, order_by, where as plan_where

        plan = self.plan_for(sym, relation="alias_of", direction="in")
        plan = plan | plan_where(in_list("kind", ["typedef", "type-alias"]))
        plan = plan | plan_select(["id", "name_length", "name"])
        plan = plan | order_by(["name_length", "name"])
        from .queryplan import limit as plan_limit
        return self._adapter_symbols(plan | plan_limit(limit))

    def sites(self, edge, limit: int = 200) -> list[Site]:
        """Concrete source locations for an edge (the file:line grounding).
        Accepts an Edge or a raw edge_id."""
        eid = edge.edge_id if isinstance(edge, Edge) else int(edge)
        if limit == 0:
            return []
        files = self._files()
        selected = self._adapter_site_keys(eid, limit)
        if not selected:
            return []
        out = []
        for edge_id, file_id, line, col in selected:
            r = self._c.execute(
                "SELECT edge_id, file_id, line, col, conditional, args_sig, "
                "       recv_src_kind, recv_type_usr, recv_decl_usr, recv_param_pos,"
                "       recv_type_is_value FROM edge_site_read "
                "WHERE edge_id = ? AND file_id = ? AND COALESCE(line,0) = ? "
                "AND COALESCE(col,0) = ?",
                (edge_id, file_id, line, col),
            ).fetchone()
            if r is None:
                return []
            p = files.get(r["file_id"], (None, None))[0] if r["file_id"] else None
            out.append(
                Site(
                    file=p,
                    line=r["line"],
                    col=r["col"],
                    conditional=bool(r["conditional"]),
                    args_sig=r["args_sig"],
                    recv_src_kind=r["recv_src_kind"],
                    recv_type_usr=r["recv_type_usr"],
                    recv_decl_usr=r["recv_decl_usr"],
                    recv_param_pos=r["recv_param_pos"],
                    recv_type_is_value=r["recv_type_is_value"],
                )
            )
        return out

    def declaration_sites(self, sym, limit: int = 500) -> list[Site]:
        """Every declaration/reopen site of a symbol (v26 `decl_site`).

        The symbol row keeps only the winning definition + one declaration; this
        returns ALL physical sites. For an OPEN symbol -- a namespace reopened
        `namespace ABC { ... }` across many files/components/repos -- this is the
        list of reopenings, the declaration half of `references()`. Ordinary
        symbols usually return their single site. Empty on a pre-v26 (un-
        reindexed) DB."""
        sid = self._resolve_id(sym)
        if not self._adapter_symbol_rows(sid, ["id", "kind", "is_definition"]):
            return []
        files = self._files()
        out: list[Site] = []
        for r in self._c.execute(
            "SELECT file_id, line, col FROM decl_site WHERE symbol_id = ? "
            "ORDER BY file_id, line, col LIMIT ?",
            (sid, limit),
        ):
            p = files.get(r["file_id"], (None, None))[0] if r["file_id"] else None
            out.append(
                Site(
                    file=p,
                    line=r["line"],
                    col=r["col"],
                    conditional=False,
                    args_sig=None,
                )
            )
        return out

    # ===================================================================== #
    # 3. NAVIGATION (walk the graph)
    # ===================================================================== #

    @overload
    def neighbors(
        self,
        sym,
        kinds: Optional[Sequence[str]] = ...,
        direction: str = ...,
        limit: int = ...,
        with_kind: Literal[False] = ...,
    ) -> list[Sym]: ...
    @overload
    def neighbors(
        self,
        sym,
        kinds: Optional[Sequence[str]] = ...,
        direction: str = ...,
        limit: int = ...,
        *,
        with_kind: Literal[True],
    ) -> list[tuple[Sym, str]]: ...

    def neighbors(
        self,
        sym,
        kinds: Optional[Sequence[str]] = None,
        direction: str = "out",
        limit: int = 500,
        with_kind: bool = False,
    ) -> "list[Sym] | list[tuple[Sym, str]]":
        """One hop. direction='out'|'in'. Returns the peer symbols.

        with_kind=False (default) returns a plain list[Sym] -- the peers.
        with_kind=True returns list[tuple[Sym, str]] -- each peer paired with
        the edge kind it was reached by ('calls'/'uses'/'contains'/...), so the
        caller knows the RELATIONSHIP type, not just the neighbour. A peer
        reachable by two kinds appears once per kind. For the full edge object
        (count + sites) use edges_in()/edges_out() instead.
        """
        edges = self._edges(sym, direction, kinds, limit, with_sites=False)
        if with_kind:
            return [(e.peer, e.kind) for e in edges]
        return [e.peer for e in edges]

    def _peers(
        self,
        sym,
        kinds: Optional[Sequence[str]],
        direction: str = "out",
        limit: int = 500,
    ) -> list[Sym]:
        """Internal one-hop peers (no edge kind). Typed plain list[Sym] so the
        graph-internal callers don't inherit neighbors()'s with_kind union."""
        return [
            e.peer for e in self._edges(sym, direction, kinds, limit, with_sites=False)
        ]

    def walk(
        self,
        start,
        kinds: Sequence[str],
        direction: str = "out",
        depth: int = 3,
        max_nodes: int = 500,
    ) -> "Traversal":
        """Bounded BFS from `start` over edges of `kinds` in one `direction`.

        Returns a Traversal recording each reached symbol with its minimum depth
        and the parent it was first reached from -- so you can reconstruct paths
        without re-querying. Bounded by `depth` and `max_nodes`."""
        start_sym = self.get(start)
        if start_sym is None:
            return Traversal({}, {}, {})
        seen: dict[int, Sym] = {start_sym.id: start_sym}
        level: dict[int, int] = {start_sym.id: 0}
        parent: dict[int, Optional[int]] = {start_sym.id: None}
        frontier = [start_sym.id]
        for d in range(1, depth + 1):
            nxt = []
            for nid in frontier:
                for e in self._edges(
                    nid, direction, kinds, limit=max_nodes, with_sites=False
                ):
                    if e.peer.id not in seen:
                        seen[e.peer.id] = e.peer
                        level[e.peer.id] = d
                        parent[e.peer.id] = nid
                        nxt.append(e.peer.id)
                        if len(seen) >= max_nodes:
                            return Traversal(seen, level, parent)
            if not nxt:
                break
            frontier = nxt
        return Traversal(seen, level, parent)

    def reaches(
        self,
        src,
        dst,
        kinds: Sequence[str] = ("calls",),
        direction: str = "out",
        max_depth: int = 8,
    ) -> Optional[list[Sym]]:
        """Shortest path of `kinds` edges from `src` to `dst`, or None.

        Answers "can A reach B?" (e.g. does this entrypoint ever call that sink)
        and returns the actual chain for grounding."""
        s, t = self.get(src), self.get(dst)
        if s is None or t is None:
            return None
        if s.id == t.id:
            return [s]
        seen = {s.id}
        parent: dict[int, int] = {}
        frontier = [s.id]
        for _ in range(max_depth):
            nxt = []
            for nid in frontier:
                for peer in self._peers(nid, kinds, direction):
                    if peer.id in seen:
                        continue
                    seen.add(peer.id)
                    parent[peer.id] = nid
                    if peer.id == t.id:
                        chain = [t.id]
                        while chain[-1] in parent:
                            chain.append(parent[chain[-1]])
                        return [
                            x
                            for x in (self.get(i) for i in reversed(chain))
                            if x is not None
                        ]
                    nxt.append(peer.id)
            if not nxt:
                break
            frontier = nxt
        return None

    # -- class hierarchy (inherits) ----------------------------------------- #

    def bases(self, sym, direct: bool = True) -> list[Sym]:
        """Base classes of `sym` (outgoing `inherits`). direct=False walks up the
        whole hierarchy."""
        if direct:
            return self._peers(sym, ("inherits",), "out")
        return [
            s
            for s in self.walk(sym, ("inherits",), "out", depth=16).nodes
            if s.id != self._resolve_id(sym)
        ]

    def subclasses(self, sym, direct: bool = True) -> list[Sym]:
        """Derived classes of `sym` (incoming `inherits`). direct=False walks the
        whole subtree."""
        if direct:
            return self._peers(sym, ("inherits",), "in")
        return [
            s
            for s in self.walk(sym, ("inherits",), "in", depth=16).nodes
            if s.id != self._resolve_id(sym)
        ]

    def members(self, sym, access: Optional[str] = None) -> list[Sym]:
        """Members of a record/namespace.

        `contains` points scope->child (outbound: namespace members, nested
        types), but `field_of`/`method_of` point member->record (so a record's
        fields and methods are INbound). This unions both so you get the full
        member set regardless of edge direction.

        access filters by C++ access specifier: 'public' | 'protected' |
        'private'. None (default) or 'all' returns every member. A member with
        no recorded access (e.g. C struct fields) only matches None/'all'.
        """
        if access is not None and access != "all" and access not in _ACCESS:
            raise ValueError(
                f"unknown access {access!r}; valid: {', '.join(_ACCESS)}, all"
            )
        out = [
            e.peer
            for e in self._edges(sym, "out", ("contains",), 500, with_sites=False)
        ]
        inn = [
            e.peer
            for e in self._edges(
                sym, "in", ("field_of", "method_of"), 500, with_sites=False
            )
        ]
        seen, merged = set(), []
        for s in out + inn:
            if s.id not in seen:
                seen.add(s.id)
                merged.append(s)
        if access not in (None, "all"):
            merged = [s for s in merged if s.access == access]
        return merged

    # ===================================================================== #
    # 3b. TEMPLATE PARAMETERS / ARGUMENTS
    # ===================================================================== #

    def template_params(self, sym) -> list[TemplateParam]:
        """The formal template parameters declared by `sym` (a class/function
        template), in declaration order. Empty for non-templates."""
        sid = self._resolve_id(sym)
        from .queryplan import out as plan_out
        keys = self._adapter_rows(
            self.plan_for(sid) | plan_out("has_template_parameter"),
            ["owner_id", "position"], order=("owner_id", "position"),
        )
        allowed = set(keys)
        rows = self._c.execute(
            "SELECT position, param_kind, name, default_txt, type_id, "
            "default_type_id, default_ref_id FROM template_param "
            "WHERE owner_id = ? ORDER BY position",
            (sid,),
        ).fetchall()
        by_key = {(sid, r["position"]): r for r in rows if (sid, r["position"]) in allowed}
        return [
            TemplateParam(
                by_key[(sid, position)]["position"],
                by_key[(sid, position)]["param_kind"],
                by_key[(sid, position)]["name"],
                by_key[(sid, position)]["default_txt"],
                self._type_info(by_key[(sid, position)]["type_id"])
                if by_key[(sid, position)]["type_id"] is not None else None,
                self._type_info(by_key[(sid, position)]["default_type_id"])
                if by_key[(sid, position)]["default_type_id"] is not None else None,
                self.get(by_key[(sid, position)]["default_ref_id"])
                if by_key[(sid, position)]["default_ref_id"] is not None else None,
            )
            for _owner, position in keys
            if (sid, position) in by_key
        ]

    def template_args(self, sym) -> list[TemplateArg]:
        """The concrete template arguments bound by `sym` (a specialization, an
        explicit instantiation, or a function that instantiates a template), in
        position order. Empty when `sym` binds no template arguments."""
        sid = self._resolve_id(sym)
        from .queryplan import out as plan_out
        keys = self._adapter_rows(
            self.plan_for(sid) | plan_out("has_template_argument"),
            ["owner_id", "position", "pack_index"],
            order=("owner_id", "position", "pack_index"),
        )
        allowed = set(keys)
        rows = self._c.execute(
            "SELECT position, pack_index, arg_kind, ref_id, literal, type_id "
            "FROM template_arg "
            "WHERE owner_id = ? ORDER BY position, pack_index",
            (sid,),
        ).fetchall()
        return [
            TemplateArg(
                r["position"], r["arg_kind"], r["ref_id"], r["literal"],
                r["pack_index"],
                self._type_info(r["type_id"]) if r["type_id"] is not None else None,
            )
            for r in rows
            if (sid, r["position"], r["pack_index"]) in allowed
        ]

    def instantiations(self, sym, limit: int = 500) -> list[Sym]:
        """Implicit instantiation nodes for a template -- incoming `instantiates`
        edges whose source has ``is_instantiation=1``.

        Distinguishes ADR-004 implicit-instantiation nodes (``X<int>``,
        ``X<int>::print``) from the pre-existing explicit-instantiation records
        (``template class Foo<int>;``, stored by the declaration handler at
        ``ast.py:1330``) and from the ``instantiation_sites`` caller→primary
        edges (``ast.py:961``).  Those earlier forms may also have outgoing
        ``instantiates`` edges, but their source is a function or an explicit
        instantiation record -- not an implicit-instantiation node.

        Returns only sources with ``is_instantiation=1`` so the result is
        precisely the set of implicit-instantiation type and member nodes."""
        sid = self._resolve_id(sym)
        from .queryplan import eq, limit as plan_limit, select as plan_select
        from .queryplan import where as plan_where
        plan = self.plan_for(sid, relation="instantiates", direction="in")
        plan = plan | plan_where(eq("is_instantiation", 1))
        plan = plan | plan_select(["id"]) | plan_limit(limit)
        return self._adapter_symbols(plan)

    def template_of(self, sym) -> Optional[Sym]:
        """The primary template that `sym` is an instantiation of -- the outgoing
        ``instantiates`` target of an implicit-instantiation node.

        Returns ``None`` when ``sym`` is not an instantiation node or has no
        outgoing ``instantiates`` edge (e.g. is a template itself)."""
        sym_obj = self.get(sym)
        if sym_obj is None:
            return None
        sid = sym_obj.id
        from .queryplan import limit as plan_limit
        plan = self.plan_for(sid, relation="instantiates", direction="out")
        ids = self._adapter_ids(plan | plan_limit(1))
        if not sym_obj.is_instantiation:
            return None
        return self.get(ids[0]) if ids else None

    def template_of_member(self, inst_member) -> Optional[Sym]:
        """The instantiation TYPE node that owns ``inst_member`` via
        ``method_of`` (kind=9) or ``field_of`` (kind=8).

        For an implicit-instantiation member such as ``X<int>::print``, returns
        the corresponding ``X<int>`` TYPE node.  Used to retrieve the concrete
        template arguments of the instantiation.  Returns ``None`` when no such
        edge exists (e.g. free-function instantiation or member without a
        method_of edge)."""
        sid = self._resolve_id(inst_member)
        from .queryplan import limit as plan_limit
        from .queryplan import where as plan_where, eq
        for relation in ("method_of", "field_of"):
            plan = self.plan_for(sid, relation=relation, direction="out")
            plan = plan | plan_where(eq("is_instantiation", 1))
            ids = self._adapter_ids(plan | plan_limit(1))
            if ids:
                return self.get(ids[0])
        return None

    def _instantiation_template_args(self, inst_member: Sym) -> list[TemplateArg]:
        """Concrete args for an instantiation/specialization member.

        Class-template member instantiations store args on the owner type node
        (``X<int>::print`` -> ``X<int>``). Function/method-template
        specializations store args on the callable specialization itself
        (``identity<int>``, ``Context::register<MyType>``). Prefer callable-local
        args, then fall back to the owner type node for ADR-004 class instances.
        """
        direct = self.template_args(inst_member)
        if direct:
            return direct
        inst_type = self.template_of_member(inst_member)
        return self.template_args(inst_type) if inst_type is not None else []

    @overload
    def callers(
        self,
        sym,
        limit: int = ...,
        include_instantiations: Literal[False] = ...,
        include_overrides: bool = ...,
    ) -> list[Sym]: ...

    @overload
    def callers(
        self,
        sym,
        limit: int = ...,
        *,
        include_instantiations: Literal[True],
        include_overrides: bool = ...,
    ) -> list[CallerWithContext]: ...

    def callers(
        self,
        sym,
        limit: int = 500,
        include_instantiations: bool = False,
        include_overrides: bool = True,
    ) -> "list[Sym] | list[CallerWithContext]":
        """Symbols that call ``sym``.

        Returns both the direct callers (incoming ``calls``) **and** the
        virtual-dispatch callers: when ``sym`` overrides a virtual base method B,
        a static call recorded against B (e.g. ``execute() -> base::doSomething``)
        can land on ``sym`` at run time. Those are read in one hop from the
        materialised ``dispatch_calls`` edges (kind 18) that ``resolve`` builds --
        so once you index + resolve, ``callers(child::doSomething)`` returns
        ``execute`` with no extra flag. Direct callers come first, deduplicated.

        ``include_overrides=False`` — opt out to get **only** the direct callers
        of the literal node ``sym`` (the pre-dispatch behaviour). Ignored when
        ``include_instantiations=True``.

        ``include_instantiations=True`` — when ``sym`` is a template method/
        function, rolls up callers of all implicit-instantiation members
        (``is_instantiation=1`` nodes reached via ``instantiates`` edges).
        Return type: ``list[CallerWithContext]``.

        Each :class:`CallerWithContext` carries:
          * ``.sym`` — the caller symbol.
          * ``.via_instantiation`` — the instantiation member node that was
            the intermediate step (``X<int>::print``); ``None`` for direct
            callers of the primary.
          * ``.via_template_args`` — concrete template arguments from the
            callable specialization itself (``register<MyType>``) or, for
            class-template member instantiations, the owner TYPE node
            (``X<int>::print`` -> ``[int]``); empty for direct callers or when no
            args are stored.

        A caller that calls ``X<int>::print`` **and** a different caller that
        calls ``X<double>::print`` both appear, each tagged with their own
        concrete type.  A caller that reaches the *same* instantiation member
        via multiple sites is deduplicated (appears once per instance).
        """
        direct = self._peers(sym, ("calls",), "in", limit)
        if not include_instantiations:
            if include_overrides:
                virtual = self._peers(sym, ("dispatch_calls",), "in", limit)
                seen = {s.id for s in direct}
                return direct + [s for s in virtual if s.id not in seen]
            return direct
        # Opt-in path: build CallerWithContext entries.
        # Direct callers of the primary get via_instantiation=None, targs=[].
        result: list[CallerWithContext] = [
            CallerWithContext(sym=s, via_instantiation=None, via_template_args=[])
            for s in direct
        ]
        # Track (caller_id, inst_member_id) so a caller reaching the same
        # instance member via multiple sites appears exactly once per instance.
        seen_pairs: set[tuple[int, int]] = {(s.id, -1) for s in direct}
        for inst_member in self.instantiations(sym, limit=limit):
            targs = self._instantiation_template_args(inst_member)
            for caller in self._peers(inst_member, ("calls",), "in", limit):
                pair = (caller.id, inst_member.id)
                if pair not in seen_pairs:
                    seen_pairs.add(pair)
                    result.append(
                        CallerWithContext(
                            sym=caller,
                            via_instantiation=inst_member,
                            via_template_args=targs,
                        )
                    )
        return result

    @overload
    def callees(
        self,
        sym,
        limit: int = ...,
        include_instantiations: Literal[False] = ...,
        include_overrides: bool = ...,
    ) -> list[Sym]: ...

    @overload
    def callees(
        self,
        sym,
        limit: int = ...,
        *,
        include_instantiations: Literal[True],
        include_overrides: bool = ...,
    ) -> list[CallerWithContext]: ...

    def callees(
        self,
        sym,
        limit: int = 500,
        include_instantiations: bool = False,
        include_overrides: bool = True,
    ) -> "list[Sym] | list[CallerWithContext]":
        """Symbols that ``sym`` calls.

        Returns both the direct callees (outgoing ``calls``) **and** the
        virtual-dispatch targets: when ``sym`` calls a virtual method B, the
        recorded edge is to B's declared node (e.g. ``execute() ->
        base::doSomething``); the concrete override(s) B can reach at run time
        are read in one hop from the materialised ``dispatch_calls`` edges (kind
        18) that ``resolve`` builds. This is what lets ``callgraph()`` descend
        through a virtual call into the real override (and on into what *it*
        calls, e.g. ``print()``). Direct callees first, deduplicated.

        ``include_overrides=False`` — opt out to the literal outgoing ``calls``
        only (the pre-dispatch view). Ignored when
        ``include_instantiations=True``.
        """
        direct = self._peers(sym, ("calls",), "out", limit)
        if not include_instantiations:
            if include_overrides:
                virtual = self._peers(sym, ("dispatch_calls",), "out", limit)
                seen = {s.id for s in direct}
                return direct + [s for s in virtual if s.id not in seen]
            return direct
        result: list[CallerWithContext] = [
            CallerWithContext(sym=s, via_instantiation=None, via_template_args=[])
            for s in direct
        ]
        seen_pairs: set[tuple[int, int]] = {(s.id, -1) for s in direct}
        for inst_member in self.instantiations(sym, limit=limit):
            targs = self._instantiation_template_args(inst_member)
            for callee in self._peers(inst_member, ("calls",), "out", limit):
                pair = (callee.id, inst_member.id)
                if pair not in seen_pairs:
                    seen_pairs.add(pair)
                    result.append(
                        CallerWithContext(
                            sym=callee,
                            via_instantiation=inst_member,
                            via_template_args=targs,
                        )
                    )
        return result

    # ===================================================================== #
    # 3b. MULTI-DEFINITION (per-backend redefinitions, v27)
    # ===================================================================== #

    def redefined(self, limit: int = 500) -> list[Sym]:
        """Every symbol defined in more than one backend (multi_def > 1) -- the
        answer to "list all re-defined". Most-redefined first."""
        if limit < 1:
            return []
        from .queryplan import all_of, ne
        # multi_def is an integer counter; excluding 0 and 1 expresses > 1 in
        # the portable predicate vocabulary, while the derived negative field
        # preserves the legacy descending order in the plan itself.
        pred = all_of([ne("multi_def", 0), ne("multi_def", 1)])
        return self._adapter_symbols(
            self._symbol_plan(
                pred,
                limit=limit,
                order=("negative_multi_def", "name"),
            )
        )

    def _definition_rows(self, sql: str, params: Sequence[Any]) -> list[Definition]:
        files = self._files()
        out: list[Definition] = []
        for r in self._c.execute(sql, params):
            sym = self.get(r["symbol_id"])
            if sym is None:
                continue
            path, comp = files.get(r["file_id"], (None, None))
            keys = r.keys()
            out.append(
                Definition(
                    sym=sym,
                    component=comp,
                    file=self.make_file(path, component_name=comp),
                    line=r["line"],
                    col=r["col"],
                    end_line=r["end_line"],
                    end_col=r["end_col"],
                    init_text=r["init_text"] if "init_text" in keys else None,
                    def_id=r["def_id"] if "def_id" in keys else -1,
                )
            )
        return out

    def definitions(self, sym) -> list[Definition]:
        """The distinct bodies of `sym`, one per backend (component, file). A
        normal symbol has one; a redefined one has several."""
        sid = self._resolve_id(sym)
        if not self._adapter_symbol_rows(sid, ["id", "kind", "is_definition"]):
            return []
        return self._definition_rows(
            "SELECT id AS def_id, symbol_id, file_id, line, col, end_line, "
            "       end_col, init_text "
            "FROM definition WHERE symbol_id = ? ORDER BY file_id, line",
            (sid,),
        )

    def possible_callees(self, sym) -> list[Definition]:
        """The candidate target BODIES a call to/through `sym` may reach at run
        time: for each redefined callee of any of `sym`'s bodies, every backend
        definition of that callee (materialised `possible_call`). This is the
        "possible call" fan-out -- e.g. ``do()`` -> {Server1::reg, Server2::reg}."""
        sid = self._resolve_id(sym)
        if not self._adapter_symbol_rows(sid, ["id", "kind", "name"]):
            return []
        return self._definition_rows(
            "SELECT td.id AS def_id, td.symbol_id AS symbol_id, "
            "       td.file_id AS file_id, "
            "       td.line AS line, td.col AS col, "
            "       td.end_line AS end_line, td.end_col AS end_col, "
            "       td.init_text AS init_text "
            "FROM possible_call pc "
            "JOIN definition sd ON sd.id = pc.src_def_id "
            "JOIN definition td ON td.id = pc.dst_def_id "
            "WHERE sd.symbol_id = ? ORDER BY td.symbol_id, td.file_id",
            (sid,),
        )

    def _definition_edge_syms(self, definition, kind: int, limit: int) -> list[Sym]:
        """dst symbols of this ONE body's def_edge rows of `kind` (1 calls / 7 uses)."""
        if isinstance(definition, Definition):
            if not self._adapter_symbol_rows(definition.sym.id, ["id"]):
                return []
        def_id = definition.def_id if isinstance(definition, Definition) else int(definition)
        rows = self._c.execute(
            f"SELECT {_SYM_COLS} FROM def_edge de JOIN symbol s ON s.id = de.dst_id "
            "WHERE de.src_def_id = ? AND de.kind = ? "
            "ORDER BY s.qual_name, s.spelling LIMIT ?",
            (def_id, kind, limit),
        ).fetchall()
        return [self._sym(r) for r in rows]

    def callees_of_definition(self, definition, limit: int = 500) -> list[Sym]:
        """What THIS backend body calls -- its own outgoing calls (def_edge kind 1),
        separate from any other backend's body. Pass a Definition (from
        definitions()) or a raw definition-row id."""
        return self._definition_edge_syms(definition, 1, limit)

    def uses_of_definition(self, definition, limit: int = 500) -> list[Sym]:
        """What THIS backend body uses (def_edge kind 7)."""
        return self._definition_edge_syms(definition, 7, limit)

    def referencing_definitions(self, sym, limit: int = 500) -> list[Definition]:
        """The backend BODIES whose own calls/uses target `sym` -- the
        per-backend "who references me" (reverse of callees_of_definition). E.g.
        ``helper_b`` -> [Server2's ``Context::reg`` body]. Distinct from the
        symbol-level references() (which is the same for every backend)."""
        sid = self._resolve_id(sym)
        return self._definition_rows(
            "SELECT DISTINCT d.id AS def_id, d.symbol_id AS symbol_id, "
            "       d.file_id AS file_id, d.line AS line, d.col AS col, "
            "       d.end_line AS end_line, d.end_col AS end_col, "
            "       d.init_text AS init_text "
            "FROM def_edge de JOIN definition d ON d.id = de.src_def_id "
            "WHERE de.dst_id = ? ORDER BY d.symbol_id, d.file_id LIMIT ?",
            (sid, limit),
        )

    # ===================================================================== #
    # 3c. SIGNATURE/TYPE TIER (v30 -- read parity with src/graph/query.cpp)
    # ===================================================================== #

    def _type_info(self, type_id: int) -> Optional[TypeInfo]:
        """Display info for one type_node id (kind resolved to its name;
        canonical spelling attached when the node is sugared)."""
        if not self._adapter_type_ids(type_id):
            return None
        r = self._c.execute(
            "SELECT id, spelling, kind, canonical_id, decl_usr, is_const, "
            "is_volatile, is_restrict, extent FROM type_node "
            "WHERE id = ?",
            (type_id,),
        ).fetchone()
        if r is None:
            return None
        canonical = None
        if r["canonical_id"] is not None:
            c = self._c.execute(
                "SELECT spelling FROM type_node WHERE id = ?",
                (r["canonical_id"],),
            ).fetchone()
            if c is not None:
                canonical = c["spelling"]
        return TypeInfo(
            id=r["id"],
            spelling=r["spelling"],
            kind=TYPE_KIND_NAMES.get(r["kind"], str(r["kind"])),
            canonical=canonical,
            decl_usr=r["decl_usr"],
            is_const=bool(r["is_const"]),
            is_volatile=bool(r["is_volatile"]),
            is_restrict=bool(r["is_restrict"]),
            extent=r["extent"],
        )

    def signature(self, sym) -> SignatureInfo:
        """Signature/type facts of one symbol: returns/params for callables,
        of_type for variables/fields, underlying for typedef/alias symbols."""
        sid = self._resolve_id(sym)
        if not self._adapter_symbol_rows(sid, ["id"]):
            return SignatureInfo()
        slots = self.signature_slots(sid)
        returns = next(
            (slot.declared_type for slot in slots if slot.role == "return"), None
        )
        params = tuple(
            ParamInfo(slot.position, slot.name, slot.declared_type)
            for slot in slots if slot.role == "parameter" and slot.position is not None
        )
        kinds = {
            r["kind"]: r["type_id"]
            for r in self._c.execute(
                "SELECT kind, type_id FROM symbol_type WHERE symbol_id = ?",
                (sid,),
            )
        }

        def info(k: int) -> Optional[TypeInfo]:
            return self._type_info(kinds[k]) if k in kinds else None

        return SignatureInfo(
            returns=returns or info(1),
            params=params,
            of_type=info(2),
            underlying=info(3),
        )

    def _type_child(self, type_id: int, kind: int, position: int = 0) -> Optional[TypeInfo]:
        row = self._c.execute(
            "SELECT dst_id FROM type_edge WHERE src_id = ? AND kind = ? AND position = ?",
            (type_id, kind, position),
        ).fetchone()
        return self._type_info(row["dst_id"]) if row is not None else None

    def type_layers(self, type_or_id) -> list[dict[str, Any]]:
        """Return the recursive type shape as deterministic root-first rows."""
        tid = type_or_id if isinstance(type_or_id, int) else getattr(type_or_id, "id", None)
        if tid is None:
            return []
        # Preserve the legacy unknown-type record; existing types are
        # validated through the typed plan before the recursive hydrator runs.
        self._adapter_type_ids(tid)
        out: list[dict[str, Any]] = []
        pending = [(tid, "root", "root", 0, 0, (tid,))]
        relations = {1: "pointee", 2: "element_type", 3: "alias_of",
                     4: "return_type", 5: "param_type",
                     6: "template_argument_type", 7: "member_owner",
                     8: "member_component"}
        while pending and len(out) < 256:
            current, path, relation, position, depth, ancestry = pending.pop()
            t = self._type_info(current)
            if t is None:
                out.append({"path": path, "relation": relation,
                            "position": position, "depth": depth,
                            "status": "unknown"})
                continue
            element_type = None
            if t.kind == "array":
                element_row = self._c.execute(
                    "SELECT dst_id FROM type_edge "
                    "WHERE src_id = ? AND kind = 2 AND position = 0",
                    (t.id,),
                ).fetchone()
                if element_row is not None:
                    element = self._type_info(element_row["dst_id"])
                    element_type = element.spelling if element is not None else None
            out.append({
                "path": path, "relation": relation, "position": position,
                "depth": depth,
                "status": "truncated" if depth >= 64 else "complete",
                "id": t.id, "spelling": t.spelling, "kind": t.kind,
                "const": t.is_const, "volatile": t.is_volatile,
                "restrict": t.is_restrict,
                "declaration": self._name_for_usr(t.decl_usr),
                "decl_usr": t.decl_usr, "extent": t.extent,
                "element_type": element_type,
            })
            if depth >= 64:
                continue
            edges = self._c.execute(
                "SELECT kind, position, dst_id FROM type_edge WHERE src_id = ? "
                "ORDER BY kind, position", (current,)).fetchall()
            for edge_kind, edge_position, child_id in reversed(edges):
                edge_name = relations.get(edge_kind, "unknown")
                path_name = ("referent" if edge_kind == 1 and t.kind in
                             {"lvalue-reference", "rvalue-reference"}
                             else "element" if edge_kind == 2 else edge_name)
                child_path = f"{path}.{path_name}"
                if edge_kind in {5, 6}:
                    child_path += f"[{edge_position}]"
                if child_id in ancestry:
                    child = self._type_info(child_id)
                    if child is not None:
                        out.append({"path": child_path, "relation": edge_name,
                                    "position": edge_position, "depth": depth + 1,
                                    "status": "cycle", "id": child.id,
                                    "spelling": child.spelling, "kind": child.kind,
                                    "const": child.is_const, "volatile": child.is_volatile,
                                    "restrict": child.is_restrict,
                                    "declaration": self._name_for_usr(child.decl_usr),
                                    "decl_usr": child.decl_usr, "extent": child.extent})
                    continue
                pending.append((child_id, child_path, edge_name, edge_position,
                                depth + 1, ancestry + (child_id,)))
        if pending and out and out[-1].get("status") == "complete":
            out[-1]["status"] = "truncated"
        return out

    def _name_for_usr(self, usr: Optional[str]) -> Optional[str]:
        if not usr:
            return None
        row = self._c.execute(
            "SELECT COALESCE(qual_name, spelling) AS name FROM symbol WHERE usr = ?",
            (usr,),
        ).fetchone()
        return row["name"] if row is not None else None

    def _slot_type_facts(
        self, declared: Optional[TypeInfo], adjusted: Optional[TypeInfo]
    ) -> tuple[str, str, Optional[str]]:
        if declared is None:
            return "value", "other", None
        mode = "value"
        if declared.kind in {"lvalue-reference", "rvalue-reference"}:
            mode = declared.kind
            base = declared
        else:
            base = adjusted or declared
        if mode != "value":
            child = self._type_child(base.id, 1)
            while child is not None and child.kind in {"lvalue-reference", "rvalue-reference"}:
                child = self._type_child(child.id, 1)
            if child is not None:
                base = child
            else:
                # A missing structural edge is an unknown typed fact. Both
                # runtimes retain the wrapper and return the same null-safe
                # result; never infer a child by matching display spelling.
                pass
        value_kind = base.kind
        named = None
        current = base
        through_pointer = base.kind == "pointer"
        seen: set[int] = set()
        while current is not None and current.id not in seen:
            seen.add(current.id)
            if current.decl_usr:
                named = self._name_for_usr(current.decl_usr)
                break
            edge_kind = 1 if current.kind in {
                "pointer", "lvalue-reference", "rvalue-reference"
            } else 2 if current.kind == "array" else 4 if current.kind == "function" and not through_pointer else None
            if edge_kind is None:
                break
            current = self._type_child(current.id, edge_kind)
        return mode, value_kind, named

    def slot_type_facts_for_ids(
        self, declared_type_id: Optional[int], adjusted_type_id: Optional[int]
    ) -> tuple[str, str, Optional[str]]:
        """Derive public signature-slot facts from typed relation IDs."""
        if declared_type_id is not None:
            self._adapter_type_ids(declared_type_id)
        if adjusted_type_id is not None:
            self._adapter_type_ids(adjusted_type_id)
        declared = (
            self._type_info(declared_type_id)
            if declared_type_id is not None else None
        )
        adjusted = (
            self._type_info(adjusted_type_id)
            if adjusted_type_id is not None else None
        )
        return self._slot_type_facts(declared, adjusted)

    def signature_slots(self, sym) -> list[SignatureSlot]:
        """Unified return/parameter view used by the E2E and public clients."""
        sid = self._resolve_id(sym)
        rows: list[SignatureSlot] = []
        for r in self._adapter_signature_slot_rows(sid):
            (slot_kind, position, pack_index, name, type_id_value,
             declared_value, adjusted_value, default_text, default_origin,
             reference_semantics, mode, value_kind, named_decl) = r
            declared_id = declared_value or type_id_value
            adjusted_id = adjusted_value or type_id_value
            declared = self._type_info(declared_id) if declared_id is not None else None
            adjusted = self._type_info(adjusted_id) if adjusted_id is not None else None
            if slot_kind == "return":
                declared = adjusted = self._type_info(type_id_value) if type_id_value is not None else None
            rows.append(SignatureSlot(
                slot_kind, None if position is None or position < 0 else position,
                None if pack_index is None or pack_index < 0 else pack_index,
                name, declared, adjusted, mode or "value", value_kind or "other",
                named_decl, reference_semantics, default_text, default_origin,
            ))
        return rows

    def type_users(self, sym, limit: int = 500) -> list[TypeUser]:
        """Symbols whose signature/type facts reach the type named by `sym`,
        through pointer/reference/array/alias/template-argument layers.
        Ordered parameter rows first then symbol_type rows (each by symbol
        id) -- byte-identical to the C++ GraphQuery.type_users."""
        target = self.get(sym) if not isinstance(sym, Sym) else sym
        if target is None:
            return []
        from .queryplan import Executor, select as plan_select
        from .storage import Storage
        type_plan = (
            self.plan_for(target, relation="of_type", direction="out")
            | plan_select(["id"])
        ).plan
        Executor(Storage.from_connection(self._c, self.db_path)).run(type_plan)
        tids = [
            r["id"]
            for r in self._c.execute(
                "WITH RECURSIVE reach(id) AS ("
                "  SELECT id FROM type_node WHERE decl_usr = ?"
                "  UNION"
                "  SELECT te.src_id FROM type_edge te "
                "    JOIN reach r ON te.dst_id = r.id"
                "  UNION"
                "  SELECT tn.id FROM type_node tn "
                "    JOIN reach r ON tn.canonical_id = r.id"
                ") SELECT id FROM reach ORDER BY id",
                (target.usr,),
            )
        ]
        if not tids:
            return []
        marks = ", ".join("?" for _ in tids)
        out: list[TypeUser] = []
        for r in self._c.execute(
            f"SELECT owner_id, position FROM parameter WHERE "
            f"type_id IN ({marks}) OR declared_type_id IN ({marks}) "
            f"OR adjusted_type_id IN ({marks}) ORDER BY owner_id, position",
            tids * 3,
        ).fetchall():
            if len(out) >= limit:
                return out
            s = self.get(r["owner_id"])
            if s is not None:
                out.append(TypeUser(sym=s, role="param", position=r["position"]))
        roles = {1: "returns", 2: "of_type", 3: "underlying_type"}
        for r in self._c.execute(
            f"SELECT symbol_id, kind FROM symbol_type WHERE type_id IN ({marks}) "
            "ORDER BY symbol_id, kind",
            tids,
        ).fetchall():
            if len(out) >= limit:
                return out
            s = self.get(r["symbol_id"])
            if s is not None:
                out.append(TypeUser(sym=s, role=roles.get(r["kind"], "?")))
        return out

    # ===================================================================== #
    # 4. DYNAMIC DISPATCH
    # ===================================================================== #

    def overrides(self, method) -> list[Sym]:
        """Base methods that `method` overrides (outgoing `overrides`)."""
        return self._peers(method, ("overrides",), "out")

    def overridden_by(self, method) -> list[Sym]:
        """Methods that directly override `method` (incoming `overrides`)."""
        return self._peers(method, ("overrides",), "in")

    def is_virtual_method(self, method) -> bool:
        """True if `method` participates in dynamic dispatch -- it is pure, it
        overrides something, or something overrides it."""
        m = self.get(method)
        if m is None:
            return False
        if m.is_pure:
            return True
        return bool(self.overridden_by(m) or self.overrides(m))

    def dispatch_targets(self, method) -> list[Sym]:
        """All concrete methods a virtual call to `method` could land on at run
        time: `method` itself (unless pure-virtual, which has no body) plus every
        method that overrides it, transitively down the class hierarchy.

        This is the core dynamic-dispatch resolver: a single `calls` edge to a
        virtual method understates reality -- the real callee set is this."""
        root = self.get(method)
        if root is None:
            return []
        targets: dict[int, Sym] = {}
        if not root.is_pure:
            targets[root.id] = root
        # BFS down the override chain (incoming `overrides`).
        seen = {root.id}
        frontier = [root.id]
        while frontier:
            nxt = []
            for nid in frontier:
                for d in self.overridden_by(nid):
                    if d.id in seen:
                        continue
                    seen.add(d.id)
                    if not d.is_pure:
                        targets[d.id] = d
                    nxt.append(d.id)
            frontier = nxt
        return list(targets.values())

    def virtual_callees(self, fn) -> list[Sym]:
        """Callees of `fn` that are virtual -- the dispatch points inside it.
        Pair with dispatch_targets() to expand each into its real target set."""
        # The declared virtual call sites -- NOT the dispatch_calls targets that
        # callees() now folds in by default (those are the expansion of these).
        return [
            c
            for c in self.callees(fn, include_overrides=False)
            if self.is_virtual_method(c)
        ]

    # ===================================================================== #
    # 4b. DEVIRTUALIZATION — PHASE 1 (selection maps, NO pruning)
    # ===================================================================== #

    def dispatch_selection(
        self, method, *, close_subtypes: bool = False
    ) -> DispatchSite:
        """The Phase-1 over-approximation for a virtual call whose static callee
        is `method`: the full selection map (concrete-type -> target) plus a
        prunable flag for Phase 2.

        `method` is the statically-recorded callee (e.g. the `A::rank` a `calls`
        edge points at). Returns a :class:`DispatchSite`. The result is purely
        derived from existing edges (overrides/inherits + dispatch_targets) -- no
        schema change. With `close_subtypes=True`, every subtype of the receiver
        that does NOT declare its own override also gets a candidate
        (inherited=True) mapping it to the ancestor override it would inherit.

        A site is flagged UNPRUNABLE (prunable=False) -- meaning Phase 2 must keep
        it fully expanded -- when its candidate set cannot be trusted as complete:
        'not-virtual' (the callee isn't a dispatch point), 'no-receiver-type'
        (the declared receiver class isn't resolvable), 'target-stub' (a target
        is an unindexed stub, so the hierarchy is open), 'pure-no-targets' (a
        pure base with no indexed override), or 'unknown-symbol'."""
        sym = self.get(method)
        if sym is None:
            return DispatchSite(None, None, (), False, ("unknown-symbol",))

        # A non-virtual callee is a fully static call: nothing to devirtualize.
        if not self.is_virtual_method(sym):
            owner = self.get(sym.parent_usr) if sym.parent_usr else None
            return DispatchSite(owner, sym, (), False, ("not-virtual",))

        reasons: list[str] = []
        receiver = self.get(sym.parent_usr) if sym.parent_usr else None
        if receiver is None:
            reasons.append("no-receiver-type")

        targets = self.dispatch_targets(sym)
        candidates: list[Selection] = []
        owner_target: dict[int, Sym] = {}  # selecting-class id -> its target
        for t in targets:
            owner = self.get(t.parent_usr) if t.parent_usr else None
            candidates.append(Selection(selecting_type=owner, target=t))
            if owner is not None:
                owner_target[owner.id] = t

        if any(t.is_stub for t in targets):
            reasons.append("target-stub")
        if not candidates:
            reasons.append("pure-no-targets")

        if close_subtypes and receiver is not None:
            for sub in self.subclasses(receiver, direct=False):
                if sub.id in owner_target:
                    continue  # declares its own override already
                inherited = self._nearest_owned_target(sub.id, owner_target)
                if inherited is not None:
                    candidates.append(
                        Selection(selecting_type=sub, target=inherited, inherited=True)
                    )

        return DispatchSite(
            receiver, sym, tuple(candidates), not reasons, tuple(reasons)
        )

    def _nearest_owned_target(
        self, subtype_id: int, owner_target: dict[int, Sym]
    ) -> Optional[Sym]:
        """Walk up `subtype_id`'s bases (nearest first) to the first class that
        owns a dispatch target, and return that target. None if none found."""
        seen = {subtype_id}
        frontier = [subtype_id]
        while frontier:
            nxt = []
            for cid in frontier:
                for base in self.bases(cid, direct=True):
                    if base.id in owner_target:
                        return owner_target[base.id]
                    if base.id not in seen:
                        seen.add(base.id)
                        nxt.append(base.id)
            frontier = nxt
        return None

    def virtual_call_sites(self, fn) -> list[DispatchSite]:
        """One :class:`DispatchSite` per VIRTUAL callee of `fn` (its dynamic
        dispatch points), in callee order. Non-virtual (fully static) callees are
        omitted; a function with no virtual callees returns []."""
        return [self.dispatch_selection(c) for c in self.virtual_callees(fn)]

    # ===================================================================== #
    # 4c. PHASE 2 — argument / receiver provenance reads (Python-only)
    # ===================================================================== #

    def call_args(self, edge_id: int) -> list[CallArg]:
        """All :class:`CallArg` rows for ``edge_id``, ordered by position.

        Returns one row per non-literal positional argument at any call site
        of this edge, ordered by (file_id, line, col, position)."""
        if not self._adapter_edge_ids(edge_id):
            return []
        rows = self._c.execute(
            "SELECT position, src_kind, type_usr, decl_usr, callee_usr, type_is_value "
            "FROM call_arg_read WHERE edge_id = ? "
            "ORDER BY file_id, line, col, position",
            (edge_id,),
        ).fetchall()
        return [
            CallArg(
                position=r["position"],
                src_kind=r["src_kind"],
                type_usr=r["type_usr"],
                decl_usr=r["decl_usr"],
                callee_usr=r["callee_usr"],
                type_is_value=r["type_is_value"],
            )
            for r in rows
        ]

    def call_args_at(
        self, edge_id: int, file_id: int, line: int, col: int
    ) -> list[CallArg]:
        """The :class:`CallArg` rows for a specific call site PK.

        Returns args for the given (edge_id, file_id, line, col) site in
        position order."""
        if not self._adapter_edge_ids(edge_id):
            return []
        rows = self._c.execute(
            "SELECT position, src_kind, type_usr, decl_usr, callee_usr, type_is_value "
            "FROM call_arg_read WHERE edge_id = ? AND file_id = ? "
            "AND line = ? AND col = ? ORDER BY position",
            (edge_id, file_id, line, col),
        ).fetchall()
        return [
            CallArg(
                position=r["position"],
                src_kind=r["src_kind"],
                type_usr=r["type_usr"],
                decl_usr=r["decl_usr"],
                callee_usr=r["callee_usr"],
                type_is_value=r["type_is_value"],
            )
            for r in rows
        ]

    def call_sites_into(self, callee: "Sym") -> "list[CallContext]":
        """Every incoming ``calls`` edge to ``callee``, each with caller + per-arg
        provenance. Built on edges_in(callee, kinds=('calls',)) + call_args(edge).
        Cross-TU/cross-repo callers appear automatically once the index is
        ``resolve``d (stub->def edges point at the real callee)."""
        out: list[CallContext] = []
        for edge in self.edges_in(callee, kinds=("calls",), limit=10_000):
            site = edge.sites[0] if edge.sites else None
            out.append(
                CallContext(
                    caller=edge.peer,
                    edge_id=edge.edge_id,
                    site=site,
                    args=self.call_args(edge.edge_id),
                )
            )
        return out

    def receiver_provenance(
        self, edge_id: int, file_id: int, line: int, col: int
    ) -> Site | None:
        """The :class:`Site` row for the given call-site PK, including
        Phase-2 receiver provenance fields (recv_src_kind / recv_type_usr /
        recv_decl_usr / recv_param_pos).  Returns None when no such site row exists."""
        if not self._adapter_edge_ids(edge_id):
            return None
        files = self._files()
        r = self._c.execute(
            "SELECT file_id, line, col, conditional, args_sig, "
            "       recv_src_kind, recv_type_usr, recv_decl_usr, recv_param_pos,"
            "       recv_type_is_value "
            "FROM edge_site_read WHERE edge_id = ? AND file_id = ? "
            "AND line = ? AND col = ?",
            (edge_id, file_id, line, col),
        ).fetchone()
        if r is None:
            return None
        p = files.get(r["file_id"], (None, None))[0] if r["file_id"] else None
        return Site(
            file=p,
            line=r["line"],
            col=r["col"],
            conditional=bool(r["conditional"]),
            args_sig=r["args_sig"],
            recv_src_kind=r["recv_src_kind"],
            recv_type_usr=r["recv_type_usr"],
            recv_decl_usr=r["recv_decl_usr"],
            recv_param_pos=r["recv_param_pos"],
            recv_type_is_value=r["recv_type_is_value"],
        )

    def _recv_for(self, edge_ids: Sequence[int]) -> dict[int, list[Site]]:
        """Batch-fetch edge_site rows including Phase-2 receiver provenance.

        Returns {edge_id: [Site, ...]} for any site with a non-NULL
        recv_src_kind. This feeds the Gamma engine's receiver lookup."""
        if not edge_ids:
            return {}
        files = self._files()
        q = ",".join("?" * len(edge_ids))
        out: dict[int, list[Site]] = {}
        for r in self._c.execute(
            "SELECT edge_id, file_id, line, col, conditional, args_sig, "
            "       recv_src_kind, recv_type_usr, recv_decl_usr, recv_param_pos,"
            "       recv_type_is_value "
            f"FROM edge_site_read WHERE edge_id IN ({q}) "
            "ORDER BY edge_id, file_id, line, col",
            list(edge_ids),
        ):
            p = files.get(r["file_id"], (None, None))[0] if r["file_id"] else None
            out.setdefault(r["edge_id"], []).append(
                Site(
                    file=p,
                    line=r["line"],
                    col=r["col"],
                    conditional=bool(r["conditional"]),
                    args_sig=r["args_sig"],
                    recv_src_kind=r["recv_src_kind"],
                    recv_type_usr=r["recv_type_usr"],
                    recv_decl_usr=r["recv_decl_usr"],
                    recv_param_pos=r["recv_param_pos"],
                    recv_type_is_value=r["recv_type_is_value"],
                )
            )
        return out

    # ===================================================================== #
    # Introspection
    # ===================================================================== #

    def stats(self) -> dict[str, Any]:
        """Counts that tell you how complete the index is before you trust it."""
        self.edge_count()
        self._adapter_symbol_rows(0, ["id"])
        one = lambda s: self._c.execute(s).fetchone()[0]  # noqa: E731
        by_edge = {
            EDGE_NAMES[r["kind"]]: r["n"]
            for r in self._c.execute(
                "SELECT kind, COUNT(*) AS n FROM edge GROUP BY kind"
            )
        }
        return {
            "db": self.db_path,
            "components": one("SELECT COUNT(*) FROM component"),
            "files_indexed": one("SELECT COUNT(*) FROM file WHERE indexed = 1"),
            "symbols": one("SELECT COUNT(*) FROM symbol"),
            "stubs": one(
                "SELECT COUNT(*) FROM symbol WHERE resolved = 0 "
                "AND file_id IS NULL AND decl_file_id IS NULL"
            ),
            "edges": one("SELECT COUNT(*) FROM edge"),
            "edges_by_kind": by_edge,
            "resolved_at": (lambda r: r[0] if r else None)(
                self._c.execute(
                    "SELECT value FROM meta WHERE key = 'graph_resolved_at'"
                ).fetchone()
            ),
        }


@dataclass
class Traversal:
    """Result of GraphQuery.walk(): reached symbols + how they were reached."""

    nodes_by_id: dict[int, Sym]
    depth_by_id: dict[int, int]
    parent_by_id: Optional[dict[int, Optional[int]]] = None

    @property
    def nodes(self) -> list[Sym]:
        """All reached symbols, shallowest first."""
        return sorted(
            self.nodes_by_id.values(),
            key=lambda s: (self.depth_by_id.get(s.id, 0), s.name),
        )

    def path_to(self, ident) -> list[Sym]:
        """Reconstruct the discovery path from start to a reached symbol."""
        if self.parent_by_id is None:
            raise ValueError("this Traversal did not record parents")
        sid = ident.id if isinstance(ident, Sym) else int(ident)
        if sid not in self.nodes_by_id:
            return []
        chain = [sid]
        while True:
            par = self.parent_by_id.get(chain[-1])
            if par is None:
                break
            chain.append(par)
        return [self.nodes_by_id[i] for i in reversed(chain)]

    def __len__(self) -> int:
        return len(self.nodes_by_id)

    def __repr__(self) -> str:
        return (
            f"Traversal({len(self.nodes_by_id)} nodes, "
            f"max_depth={max(self.depth_by_id.values(), default=0)})"
        )
