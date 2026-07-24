"""indexer.storage -- SQLite persistence layer for the cidx symbol index.

Schema (all stdlib sqlite3, no dependencies):

    repository  a logical code base (git repo / external lib) grouping >=1
                components; may have several `clone` checkout dirs, one active
    clone       one checkout/worktree directory of a repository (switchable)
    component   one indexed code base (a git repo) or an external library
    directory   a directory, path relative to its component root
    file        a source/header file inside a directory; tracks indexing state
    symbol      one declaration/definition, keyed by its clang USR (unique)

A symbol's location is (file_id, line, col); the absolute path is recovered by
joining component.path / directory.path / file.name, so moving a repo only
requires updating one component row.

Usage:
    with Storage(".cidx/index.db") as db:
        comp_id = db.add_component("librdkafka", "/path/to/librdkafka")
        dir_id  = db.add_directory(comp_id, "src")
        file_id = db.add_file(dir_id, "rdkafka.c", mtime=1718000000.0)
        db.add_symbol(Symbol(usr="c:@F@rd_kafka_new", spelling="rd_kafka_new",
                             kind="function", file_id=file_id, line=42, col=1))
        sym = db.lookup_symbol("c:@F@rd_kafka_new")
"""

from __future__ import annotations

import hashlib
import json
import os
import sqlite3
from dataclasses import dataclass, field, fields, replace
from collections.abc import Iterator, Sequence
from typing import Any, Optional

from indexer import pathx as _pathx
from indexer.generated_catalog import CATALOG_HASH, CATALOG_SEED_SQL, CATALOG_VERSION
from indexer.generated_catalog import (
    IDENTITY_KIND_IDS as _GENERATED_IDENTITY_KIND_IDS,
    SOURCE_KIND_IDS as _GENERATED_SOURCE_KIND_IDS,
    SYMBOL_KIND_IDS as _GENERATED_SYMBOL_KIND_IDS,
)

SCHEMA_VERSION = 38
PREVIOUS_SCHEMA_VERSION = SCHEMA_VERSION - 1

# HSE-77 is the only supported predecessor for the HSE-79 storage migration.
# Keep this explicit so an unrelated semantic catalog is never silently
# accepted merely because the database is writable.
PREVIOUS_CATALOG_HASH = "be3a97cf69140080586a079a27a97da7816455f477ce56435ee91c600cc993fc"


def _md5_of(path: str) -> Optional[str]:
    """Hash a current source file without importing the utils package."""
    try:
        with open(path, "rb") as fh:
            return hashlib.md5(fh.read()).hexdigest()
    except OSError:
        return None


@dataclass(frozen=True)
class IndexIdentity:
    """Persisted source/configuration identity plus current checkout status."""

    schema_version: int
    source_revision: Optional[str]
    source_fingerprint: Optional[str]
    index_config: Optional[str]
    index_config_fingerprint: Optional[str]
    freshness: str  # current | stale | unverifiable

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "source_revision": self.source_revision,
            "source_fingerprint": self.source_fingerprint,
            "index_config": self.index_config,
            "index_config_fingerprint": self.index_config_fingerprint,
            "freshness": self.freshness,
        }


def _catalog_hash(conn: sqlite3.Connection) -> Optional[str]:
    """Return the stored catalog hash, or None when the database has no meta row."""
    try:
        row = conn.execute(
            "SELECT value FROM meta WHERE key = 'catalog_hash'"
        ).fetchone()
    except sqlite3.OperationalError as exc:
        if "no such table" in str(exc):
            return None
        raise
    return None if row is None else row[0]


def _schema_version(conn: sqlite3.Connection) -> Optional[int]:
    """Return the stored schema version, or None for a fresh database."""
    try:
        row = conn.execute(
            "SELECT value FROM meta WHERE key = 'schema_version'"
        ).fetchone()
    except sqlite3.OperationalError as exc:
        if "no such table" in str(exc):
            return None
        raise
    return None if row is None else int(row[0])


def _validate_catalog_hash(
    conn: sqlite3.Connection,
    database: str,
    *,
    require_present: bool,
    allow_predecessor: bool = False,
) -> None:
    actual = _catalog_hash(conn)
    if actual is None:
        if require_present:
            raise RuntimeError(
                f"incompatible cidx semantic catalogs: catalog_hash missing; "
                f"expected {CATALOG_HASH!r} for {database}. Regenerate the index."
            )
        return
    if actual == CATALOG_HASH:
        return
    if allow_predecessor and actual == PREVIOUS_CATALOG_HASH:
        return
    if actual != CATALOG_HASH:
        raise RuntimeError(
            f"incompatible cidx semantic catalogs: catalog_hash {actual!r}; "
            f"expected {CATALOG_HASH!r} for {database}. Regenerate the index."
        )

#: symbol.kind name -> the integer it is stored as on disk (v16+). The integer
#: IS libclang's `CXCursorKind` enum value, so a stored kind matches the C API
#: 1:1 (e.g. CXCursor_CXXMethod == 21). Storing the small int instead of the
#: name keeps the symbol table compact; the `symbol_kind` table (and the inverse
#: map below) recover the string for display. Mirrors clang/ast.py:_KIND_MAP.
SYMBOL_KIND_IDS = dict(_GENERATED_SYMBOL_KIND_IDS)
#: stored integer -> symbol.kind name (display / read-side recovery).
SYMBOL_KIND_NAMES = {v: k for k, v in SYMBOL_KIND_IDS.items()}

SOURCE_KIND_IDS = dict(_GENERATED_SOURCE_KIND_IDS)
IDENTITY_KIND_IDS = dict(_GENERATED_IDENTITY_KIND_IDS)

#: Allowed values for symbol.kind. Superset of the cidx brief: the core C/C++
#: declaration kinds plus the ones any real walk over a TU produces.
SYMBOL_KINDS = frozenset(SYMBOL_KIND_IDS)

_SCHEMA = f"""
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);

-- v35: explicit declared program/dependency universes. Numeric ids are
-- database-local; key is the portable scope component of semantic identity.
CREATE TABLE IF NOT EXISTS semantic_universe (
    id      INTEGER PRIMARY KEY,
    key     TEXT NOT NULL UNIQUE,
    name    TEXT NOT NULL,
    policy  TEXT NOT NULL DEFAULT 'explicit'
);
INSERT OR IGNORE INTO semantic_universe (id, key, name, policy)
    VALUES (1, 'legacy', 'Legacy single-workspace universe', 'legacy');

-- v23: a repository groups one or more components under one logical code base.
-- A repo can be checked out in several directories (git worktrees / separate
-- clones); each is a `clone` row and `active_clone_id` names the live one.
-- v24: a grouped component stores its `path` RELATIVE to the repository's active
-- clone root (resolved at read time via component_abs_base); `repo switch` then
-- only repoints `active_clone_id` -- no per-component path rewrite. An ungrouped
-- component (repository_id NULL) keeps an absolute path. `remote_url` (git origin)
-- lets two checkouts of the same repo map to one repository.
CREATE TABLE IF NOT EXISTS repository (
    id              INTEGER PRIMARY KEY,
    name            TEXT NOT NULL UNIQUE,
    kind            TEXT NOT NULL DEFAULT 'repo'
                    CHECK (kind IN ('repo', 'external')),
    remote_url      TEXT,                 -- git origin URL when known
    active_clone_id INTEGER,              -- -> clone.id (no FK: circular w/ clone)
    semantic_universe_id INTEGER NOT NULL DEFAULT 1
            REFERENCES semantic_universe(id) ON DELETE SET DEFAULT
);

CREATE TABLE IF NOT EXISTS clone (
    id            INTEGER PRIMARY KEY,
    repository_id INTEGER NOT NULL REFERENCES repository(id) ON DELETE CASCADE,
    path          TEXT NOT NULL UNIQUE,   -- absolute checkout/worktree root
    label         TEXT                    -- optional human label (branch/worktree)
);

CREATE TABLE IF NOT EXISTS component (
    id      INTEGER PRIMARY KEY,
    name    TEXT NOT NULL,
    path    TEXT NOT NULL,                -- base path (no version segment);
                                          -- v24: RELATIVE to the active clone
                                          -- root when grouped, else absolute
    kind    TEXT NOT NULL DEFAULT 'repo'
            CHECK (kind IN ('repo', 'external')),
    version TEXT,                         -- v14: nullable; NULL = unversioned
    repository_id INTEGER                 -- v23: -> repository.id; NULL = ungrouped
            REFERENCES repository(id) ON DELETE SET NULL,
    -- v24: path is UNIQUE per repository -- a grouped component stores a
    -- clone-relative path, so several repos can each carry a '.' root;
    -- ungrouped rows (repository_id NULL) are de-duplicated by add_component.
    semantic_universe_id INTEGER
            REFERENCES semantic_universe(id) ON DELETE SET NULL,
    UNIQUE (repository_id, path)
);

CREATE TABLE IF NOT EXISTS directory (
    id           INTEGER PRIMARY KEY,
    component_id INTEGER NOT NULL REFERENCES component(id) ON DELETE CASCADE,
    path         TEXT NOT NULL,         -- relative to component.path ('' = root)
    UNIQUE (component_id, path)
);

CREATE TABLE IF NOT EXISTS file (
    id              INTEGER PRIMARY KEY,
    directory_id    INTEGER NOT NULL REFERENCES directory(id) ON DELETE CASCADE,
    name            TEXT NOT NULL,
    mtime           REAL,               -- source mtime at index time
    md5             TEXT,               -- content hash at import time
    compile_options TEXT,               -- JSON list of stripped parse args
    driver          TEXT,               -- argv[0] of the compile command; its
                                        -- system include paths are replicated
                                        -- at parse time (custom toolchains)
    indexed         INTEGER NOT NULL DEFAULT 0,
    indexed_at      TEXT,               -- ISO timestamp of last successful index
    args_overridden INTEGER NOT NULL DEFAULT 0,  -- compile_options/driver were
                                        -- edited by `cidx file`; a re-import
                                        -- (without --force) must NOT clobber them
    UNIQUE (directory_id, name)
);

CREATE TABLE IF NOT EXISTS symbol (
    id           INTEGER PRIMARY KEY,
    usr          TEXT NOT NULL,         -- clang Unified Symbol Resolution
    spelling     TEXT NOT NULL,
    qual_name    TEXT,                  -- fully qualified, e.g. 'RdKafka::ConfImpl::set'
    display_name TEXT,                  -- spelling + signature, e.g. 'multiply(int, int)'
    kind         INTEGER NOT NULL,     -- CXCursorKind value; see symbol_kind table
                                       -- (v16: was TEXT name; now stored as int)
    type_info    TEXT,                  -- cursor.type.spelling
    file_id      INTEGER REFERENCES file(id) ON DELETE SET NULL,
    line         INTEGER,                     -- definition site once seen,
    col          INTEGER,                     -- else the declaration site
    end_line     INTEGER,                     -- v25: END of the symbol's own
    end_col      INTEGER,                     -- extent (cursor.extent.end) at the
                                              -- (line, col) site above -- the
                                              -- closing brace of a function or
                                              -- method definition, or the full
                                              -- extent of a class/struct/union/
                                              -- typedef declaration.
                                              -- Paired with (line, col) and moved
                                              -- in lockstep when a definition
                                              -- supersedes a declaration, so
                                              -- (line..end_line) always slices the
                                              -- whole entity. NULL until reindexed.
    decl_file_id INTEGER REFERENCES file(id) ON DELETE SET NULL,
    decl_line    INTEGER,                     -- declaration site (e.g. the .h
    decl_col     INTEGER,                     -- prototype); NULL if none seen
    decl_path    TEXT,                         -- raw decl path for a target in an
                                              -- UNREGISTERED file (system/stdlib
                                              -- header no component owns): the AST
                                              -- has the location but there is no
                                              -- file row to point decl_file_id at,
                                              -- so the stub keeps the path here
    is_definition INTEGER NOT NULL DEFAULT 0,
    is_pure      INTEGER NOT NULL DEFAULT 0,  -- C++: pure virtual ('= 0'), so
                                              -- no definition can ever exist
    is_static    INTEGER NOT NULL DEFAULT 0,  -- v12: C++ static member function
                                              -- (clang_CXXMethod_isStatic). Free
                                              -- functions/non-methods are 0; a
                                              -- file-scope `static` free function
                                              -- is captured by linkage='internal'
    is_instantiation INTEGER NOT NULL DEFAULT 0,  -- v13: implicit template
                                              -- instantiation node (own USR,
                                              -- definition via `instantiates` edge)
    is_named_instance INTEGER NOT NULL DEFAULT 0, -- v20: a template instance
                                              -- minted from a NAMED `using`/
                                              -- typedef alias (X<B> from
                                              -- `using Y = X<B>`). Such instances
                                              -- carry their OWN composes/aggregates
                                              -- /associates (T->B substituted into
                                              -- the primary's members) instead of
                                              -- collapsing onto the primary. Plain
                                              -- call-site instantiation nodes
                                              -- (is_instantiation=1, this=0) stay
                                              -- collapsed (std::vector<Foo> etc.)
    linkage      TEXT,                  -- 'external' | 'internal' | 'no-linkage' | ...
    access       TEXT,                  -- C++: 'public' | 'protected' | 'private'
    parent_usr   TEXT,                  -- semantic parent (class/namespace) USR
    parent_id    INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
    resolved     INTEGER NOT NULL DEFAULT 0,
    multi_def    INTEGER NOT NULL DEFAULT 0, -- v27: COUNT of definitions of this
                                             -- symbol (rows in `definition`), set
                                             -- at resolve. >1 means the symbol is
                                             -- redefined per backend (library
                                             -- method left undefined, each server
                                             -- re-implements it). O(1) "list
                                             -- redefined" without a join.
    const_value  TEXT,                       -- v33: the evaluated constant value
                                             -- of a variable's initializer or an
                                             -- enumerator, as printed by Clang's
                                             -- constant evaluator. NULL when the
                                             -- initializer needs runtime
                                             -- evaluation (or there is none).
    semantic_universe_id INTEGER NOT NULL DEFAULT 1
            REFERENCES semantic_universe(id),
    identity_key TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_symbol_spelling ON symbol(spelling);
CREATE INDEX IF NOT EXISTS idx_symbol_qual     ON symbol(qual_name);
-- NOCASE companions: let a case-insensitive prefix LIKE ('Foo%') on these
-- columns become a range SEARCH instead of a full scan (query.py find tier 2,
-- search_symbols). A BINARY index cannot serve a case-insensitive LIKE; a
-- NOCASE index can. Additive -- created on every open via this script, so an
-- existing index gains them with no reindex.
CREATE INDEX IF NOT EXISTS idx_symbol_spelling_nc ON symbol(spelling COLLATE NOCASE);
CREATE INDEX IF NOT EXISTS idx_symbol_qual_nc     ON symbol(qual_name COLLATE NOCASE);
CREATE INDEX IF NOT EXISTS idx_symbol_file     ON symbol(file_id);
CREATE INDEX IF NOT EXISTS idx_symbol_parent   ON symbol(parent_usr);
CREATE INDEX IF NOT EXISTS idx_symbol_parent_id ON symbol(parent_id);
CREATE INDEX IF NOT EXISTS idx_symbol_kind     ON symbol(kind);
CREATE INDEX IF NOT EXISTS idx_symbol_usr      ON symbol(usr);
CREATE INDEX IF NOT EXISTS idx_symbol_scope    ON symbol(semantic_universe_id);
CREATE UNIQUE INDEX IF NOT EXISTS idx_symbol_identity
    ON symbol(semantic_universe_id, identity_key) WHERE identity_key <> '';

-- ---- v26: every declaration/reopen SITE of a symbol -------------------------
-- symbol.(line,col)/decl_* keep only the winning definition + one declaration
-- site (add_symbol collapses on usr). But an OPEN symbol -- above all a
-- namespace, reopened `namespace ABC { ... }` across many files/components/
-- repos -- has arbitrarily many declaration sites, and references() must list
-- them all. decl_site records ONE row per (symbol, physical location) for every
-- symbol (not just namespaces: a function/class declared in several headers
-- gains multi-site references for free). Populated in the add_symbol path on
-- every (re)index; INSERT OR IGNORE keeps it idempotent within a run and the
-- ON DELETE CASCADE clears a symbol's sites when it is removed.
CREATE TABLE IF NOT EXISTS decl_site (
    symbol_id     INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
    file_id       INTEGER REFERENCES file(id) ON DELETE CASCADE,
    line          INTEGER,
    col           INTEGER,
    end_line      INTEGER,
    end_col       INTEGER,
    is_definition INTEGER NOT NULL DEFAULT 0,
    UNIQUE (symbol_id, file_id, line, col)
);
CREATE INDEX IF NOT EXISTS idx_decl_site_symbol ON decl_site(symbol_id);

-- ---- v16: symbol-kind metadata (display only) ----------------------------
-- Maps the integer stored in symbol.kind (== libclang CXCursorKind) to its
-- string name. Purely for display/debugging -- no FK from symbol references it;
-- readers use the in-code SYMBOL_KIND_NAMES map.
CREATE TABLE IF NOT EXISTS symbol_kind (
    id   INTEGER PRIMARY KEY,    -- CXCursorKind value
    name TEXT NOT NULL UNIQUE
);
INSERT OR IGNORE INTO symbol_kind (id, name) VALUES
  {", ".join(f"({i},{k!r})" for k, i in sorted(SYMBOL_KIND_IDS.items(), key=lambda kv: kv[1]))};

-- ---- v7 graph layer (PLAN §2/§6) -----------------------------------------

-- edge.kind metadata (display only, like symbol_kind): no symbol/edge FK
-- references it -- readers use the in-code EDGE_NAMES map.
CREATE TABLE IF NOT EXISTS edge_kind (
    id   INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);
INSERT OR IGNORE INTO edge_kind (id, name) VALUES
  (1,'calls'), (2,'inherits'), (3,'contains'), (4,'specializes'),
  (5,'instantiates'), (6,'overrides'), (7,'uses'),
  (8,'field_of'), (9,'method_of'),
  (10,'construct-value'), (11,'construct-temp'), (12,'construct-heap'),
  (13,'construct-copy'), (14,'construct-move'),
  (15,'factory-construct'), (16,'destroy'), (17,'friend'),
  (18,'dispatch_calls'),
  -- v34: a typedef / using alias -> the type it names. Previously written as
  -- uses(7); a dedicated kind because "uses" is overloaded (body references,
  -- signature types, namespace qualifiers) while the alias relation is a
  -- definitional X -> alias_of -> Y.
  (19,'alias_of'),
  -- v34: a variable / class field -> its declared type. Previously written
  -- as uses(7); "of_type" matches the signature tier's of_type relation.
  (20,'of_type');

CREATE TABLE IF NOT EXISTS edge (
    id          INTEGER PRIMARY KEY,
    src_id      INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
    dst_id      INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
    kind        INTEGER NOT NULL,   -- edge_kind.id (no FK: faster inserts)
    count       INTEGER NOT NULL DEFAULT 1,
    base_access INTEGER,   -- inherits: public/protected/private of the base
    is_virtual  INTEGER,   -- inherits: virtual base
    vtable_slot INTEGER,   -- overrides: reserved (NULL today)
    UNIQUE (src_id, dst_id, kind)
);
CREATE INDEX IF NOT EXISTS idx_edge_src ON edge(src_id, kind);
CREATE INDEX IF NOT EXISTS idx_edge_dst ON edge(dst_id, kind);

-- v35: deduplicated, lossless identities for values without a local row.
CREATE TABLE IF NOT EXISTS external_identity (
    id               INTEGER PRIMARY KEY,
    identity_kind    INTEGER NOT NULL CHECK (identity_kind IN (1, 2, 3)),
    identity_text    TEXT NOT NULL,
    resolution_status INTEGER NOT NULL DEFAULT 0 CHECK (resolution_status IN (0, 1)),
    symbol_id        INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
    type_id          INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
    UNIQUE (identity_kind, identity_text)
);
CREATE INDEX IF NOT EXISTS idx_external_identity_symbol ON external_identity(symbol_id);
CREATE INDEX IF NOT EXISTS idx_external_identity_type ON external_identity(type_id);

CREATE TABLE IF NOT EXISTS edge_site (
    edge_id      INTEGER NOT NULL REFERENCES edge(id) ON DELETE CASCADE,
    file_id      INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    line         INTEGER,
    col          INTEGER,
    conditional  INTEGER NOT NULL DEFAULT 0,
    args_sig     TEXT,
    recv_src_kind TEXT,
    recv_type_usr TEXT,
    recv_decl_usr TEXT,
    recv_src_kind_id INTEGER CHECK (recv_src_kind_id IS NULL OR recv_src_kind_id IN (1,2,3,4,5,6,7,8)),
    recv_type_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
    recv_decl_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
    recv_type_identity_id INTEGER REFERENCES external_identity(id) ON DELETE SET NULL,
    recv_decl_identity_id INTEGER REFERENCES external_identity(id) ON DELETE SET NULL,
    recv_param_pos INTEGER,
    recv_type_is_value INTEGER,          -- v11: receiver held by value (1) else 0/NULL
    PRIMARY KEY (edge_id, file_id, line, col)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS template_param (
    owner_id    INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
    position    INTEGER NOT NULL,
    param_kind  INTEGER NOT NULL,  -- 1=type 2=non-type 3=template-template 4=pack
    name        TEXT,
    default_txt TEXT,
    type_id     INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
    default_type_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
    default_ref_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
    PRIMARY KEY (owner_id, position)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS template_arg (
    owner_id  INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
    position  INTEGER NOT NULL,
    pack_index INTEGER NOT NULL DEFAULT -1,
    arg_kind  INTEGER NOT NULL,  -- 1=type 2=non-type value 3=template-template 4=pack
    ref_id    INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
    literal   TEXT,
    type_id   INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
    PRIMARY KEY (owner_id, position, pack_index)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS call_arg (
    edge_id    INTEGER NOT NULL REFERENCES edge(id) ON DELETE CASCADE,
    file_id    INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    line       INTEGER NOT NULL,
    col        INTEGER NOT NULL,
    position   INTEGER NOT NULL,
    src_kind   TEXT,
    type_usr   TEXT,
    decl_usr   TEXT,
    callee_usr TEXT,
    src_kind_id INTEGER CHECK (src_kind_id IS NULL OR src_kind_id IN (1,2,3,4,5,6,7,8)),
    type_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
    decl_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
    callee_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
    type_identity_id INTEGER REFERENCES external_identity(id) ON DELETE SET NULL,
    decl_identity_id INTEGER REFERENCES external_identity(id) ON DELETE SET NULL,
    callee_identity_id INTEGER REFERENCES external_identity(id) ON DELETE SET NULL,
    type_is_value INTEGER,               -- v11: arg held by value (1) else 0/NULL
    PRIMARY KEY (edge_id, file_id, line, col, position)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_call_arg_edge ON call_arg(edge_id);
CREATE INDEX IF NOT EXISTS idx_edge_site_recv_type_identity ON edge_site(recv_type_identity_id);
CREATE INDEX IF NOT EXISTS idx_edge_site_recv_decl_identity ON edge_site(recv_decl_identity_id);
CREATE INDEX IF NOT EXISTS idx_call_arg_type_identity ON call_arg(type_identity_id);
CREATE INDEX IF NOT EXISTS idx_call_arg_decl_identity ON call_arg(decl_identity_id);
CREATE INDEX IF NOT EXISTS idx_call_arg_callee_identity ON call_arg(callee_identity_id);

CREATE VIEW IF NOT EXISTS edge_site_read AS
SELECT es.edge_id, es.file_id, es.line, es.col, es.conditional, es.args_sig,
       COALESCE(es.recv_src_kind, CASE es.recv_src_kind_id
           WHEN 1 THEN 'literal' WHEN 2 THEN 'local'
           WHEN 3 THEN 'construct' WHEN 4 THEN 'member'
           WHEN 5 THEN 'global' WHEN 6 THEN 'call_result'
           WHEN 7 THEN 'this' WHEN 8 THEN 'unknown' END) AS recv_src_kind,
       COALESCE(es.recv_type_usr, tn.decl_usr, eti.identity_text) AS recv_type_usr,
       COALESCE(es.recv_decl_usr, ds.usr, edi.identity_text) AS recv_decl_usr,
       es.recv_param_pos, es.recv_type_is_value
FROM edge_site es
LEFT JOIN type_node tn ON tn.id = es.recv_type_id
LEFT JOIN symbol ds ON ds.id = es.recv_decl_id
LEFT JOIN external_identity eti ON eti.id = es.recv_type_identity_id
LEFT JOIN external_identity edi ON edi.id = es.recv_decl_identity_id;

CREATE VIEW IF NOT EXISTS call_arg_read AS
SELECT ca.edge_id, ca.file_id, ca.line, ca.col, ca.position,
       COALESCE(ca.src_kind, CASE ca.src_kind_id
           WHEN 1 THEN 'literal' WHEN 2 THEN 'local'
           WHEN 3 THEN 'construct' WHEN 4 THEN 'member'
           WHEN 5 THEN 'global' WHEN 6 THEN 'call_result'
           WHEN 7 THEN 'this' WHEN 8 THEN 'unknown' END) AS src_kind,
       COALESCE(ca.type_usr, tn.decl_usr, eti.identity_text) AS type_usr,
       COALESCE(ca.decl_usr, ds.usr, edi.identity_text) AS decl_usr,
       COALESCE(ca.callee_usr, cs.usr, eci.identity_text) AS callee_usr,
       ca.type_is_value
FROM call_arg ca
LEFT JOIN type_node tn ON tn.id = ca.type_id
LEFT JOIN symbol ds ON ds.id = ca.decl_id
LEFT JOIN symbol cs ON cs.id = ca.callee_id
LEFT JOIN external_identity eti ON eti.id = ca.type_identity_id
LEFT JOIN external_identity edi ON edi.id = ca.decl_identity_id
LEFT JOIN external_identity eci ON eci.id = ca.callee_identity_id;

CREATE TABLE IF NOT EXISTS label (
    id   INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,   -- label key, e.g. 'libfoo-include'
    path TEXT NOT NULL           -- stored verbatim; may contain $VAR
);

-- ---- v15: per-file parse diagnostics (errors/warnings) ---------------------
-- Clang diagnostics (severity >= warning) emitted while parsing a TU, keyed by
-- the TU's file row. Refreshed wholesale on every (re)index of that file. A
-- diagnostic located in an #included header keeps its own file_path/line/col,
-- but is owned by the TU that surfaced it.
CREATE TABLE IF NOT EXISTS diagnostic (
    id        INTEGER PRIMARY KEY,
    file_id   INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    severity  INTEGER NOT NULL,   -- clang severity: 2=warning 3=error 4=fatal
    spelling  TEXT NOT NULL,      -- the diagnostic message
    file_path TEXT,               -- diagnostic location file (NULL if locationless)
    line      INTEGER,            -- NULL when locationless
    col       INTEGER             -- NULL when locationless
);
CREATE INDEX IF NOT EXISTS idx_diagnostic_file ON diagnostic(file_id);

-- ---- v17: Layer-1 entity-edge graph (UML/ER relations over record/enum symbols) --
-- Entity = a symbol whose kind is in {{class,struct,union,enum}}; no separate table.
-- All columns are INTEGER (zero text in the table itself). The 11 relation names
-- live only in entity_edge_kind (seed-only; no FK from entity_edge -- same pattern
-- as edge_kind). A NULL-safe unique identity index (see below) keeps one row
-- per logical edge so re-materialise = DELETE + re-run stays idempotent.
-- (Lexical nesting is a declaration-scope property of the symbol, not a relation,
--  so it is NOT an entity_edge kind.)

CREATE TABLE IF NOT EXISTS entity_edge_kind (
    id   INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);
INSERT OR IGNORE INTO entity_edge_kind (id, name) VALUES
  (1,'generalizes'), (2,'implements'), (3,'specializes'),
  (4,'composes'), (5,'aggregates'), (6,'associates'),
  (7,'creates'), (8,'uses'), (9,'destroys'),
  (10,'befriends'), (11,'instantiates'),
  -- v26: a namespace DIRECTLY declares a member entity (record/enum/nested
  -- namespace). Direct only -- ABC does NOT `declares` ABC::XXX's members;
  -- ABC and ABC::XXX are distinct entities (content is not recursive).
  (12,'declares');

CREATE TABLE IF NOT EXISTS entity_edge (
    src_id        INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
    dst_id        INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
    kind          INTEGER NOT NULL,   -- entity_edge_kind.id (no FK: seed-only)
    count         INTEGER NOT NULL DEFAULT 1,
    via_member_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
    multiplicity  INTEGER NOT NULL DEFAULT 1,
                                      -- 1=one 2=0..1 3=0..* 4=N
    access        INTEGER NOT NULL DEFAULT 0,
                                      -- 0=public 1=protected 2=private
    is_virtual    INTEGER NOT NULL DEFAULT 0,  -- 1 = virtual base (generalizes)
    create_form   INTEGER,            -- creates/destroys only:
                                      -- 1=ctor_call 2=return 3=value 4=temp
                                      -- 5=heap 6=factory 7=copy 8=move
    partial       INTEGER NOT NULL DEFAULT 0   -- 1 = top-soundness flag
);
-- One row per logical entity edge.  A plain UNIQUE(...via_member_id) cannot
-- enforce this: SQLite treats NULL != NULL, so the very common NULL-via edges
-- (generalizes/specializes/uses/creates/...) would never collide and the
-- INSERT ... ON CONFLICT upserts in entity_rollup would silently fan out into
-- duplicate rows on every materialise.  A COALESCE expression index folds NULL
-- to a sentinel so the identity is NULL-safe; create_form is part of the key so
-- distinct creates/destroys forms (value/temp/heap/...) stay separate rows.
CREATE UNIQUE INDEX IF NOT EXISTS idx_entity_edge_identity ON entity_edge(
    src_id, dst_id, kind,
    COALESCE(via_member_id, -1), COALESCE(create_form, -1)
);
CREATE INDEX IF NOT EXISTS idx_entity_edge_src  ON entity_edge(src_id, kind);
CREATE INDEX IF NOT EXISTS idx_entity_edge_dst  ON entity_edge(dst_id, kind);

-- ---- v22: entity-node type (Layer-1 design-entity classification) -----------
-- The *type of an entity node* in the UML/abstraction graph, materialised at
-- `cidx resolve` alongside entity_edge. Orthogonal to the C++ keyword (which
-- stays at the low-level symbol layer): a record is classified by ABSTRACTNESS
-- into class / abstract_class / interface (and the same split for class
-- templates); union / enum keep their own type. Same zero-TEXT/lookup-table
-- pattern as entity_edge_kind. Populated by entity_rollup.materialize_entity_nodes.
CREATE TABLE IF NOT EXISTS entity_kind (
    id   INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);
INSERT OR IGNORE INTO entity_kind (id, name) VALUES
  (0,'other'),
  (1,'class'), (2,'abstract_class'), (3,'interface'),
  (4,'union'), (5,'enum'),
  (6,'class_template'), (7,'abstract_class_template'), (8,'interface_template'),
  (9,'namespace');  -- v26: namespace as a first-class entity node

CREATE TABLE IF NOT EXISTS entity_node (
    id   INTEGER PRIMARY KEY REFERENCES symbol(id) ON DELETE CASCADE,
    kind INTEGER NOT NULL   -- entity_kind.id (no FK: seed-only)
);

-- ---- v27: multi-definition symbols (per-backend redefinitions) --------------
-- A library declares a method (or static member var) and leaves it undefined;
-- each backend re-implements the SAME symbol in its own file/component. The
-- `symbol` row is keyed UNIQUE(usr), so all bodies collapse to one node and their
-- call edges merge -- Server1's body cannot be told from Server2's. `definition`
-- records ONE row per body (keyed by component+file), `def_edge` hangs each body's
-- outgoing calls/uses off that definition (so bodies stay separate), and
-- `possible_call` fans a call out to every possible body at resolve time.
CREATE TABLE IF NOT EXISTS definition (
    id           INTEGER PRIMARY KEY,
    symbol_id    INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
    component_id INTEGER REFERENCES component(id) ON DELETE SET NULL,
    file_id      INTEGER REFERENCES file(id) ON DELETE CASCADE,
    line         INTEGER,
    col          INTEGER,
    end_line     INTEGER,
    end_col      INTEGER,
    init_text    TEXT,                     -- v28: for a (static member) VARIABLE
                                           -- definition, its initializer source
                                           -- text per backend (`= 5` -> '5',
                                           -- `= seed_a()` -> 'seed_a()'); NULL for
                                           -- functions and uninitialized vars
    -- one body per (component, file, symbol). component_id is derived from the
    -- file's owning component; both are in the key so the same relative file path
    -- in two clones/components stays distinct (matches the variant key the user
    -- asked for: component + file + USR).
    UNIQUE (component_id, file_id, symbol_id)
);
CREATE INDEX IF NOT EXISTS idx_definition_symbol ON definition(symbol_id);

-- Per-body outgoing calls/uses. src is a DEFINITION (a backend body), not a
-- symbol, so each variant keeps its own edges. dst stays a symbol (the callee's
-- canonical/declared node). kind reuses edge_kind (1 calls / 7 uses). resolve
-- rolls these back up into the collapsed symbol->symbol `edge` view.
CREATE TABLE IF NOT EXISTS def_edge (
    id         INTEGER PRIMARY KEY,
    src_def_id INTEGER NOT NULL REFERENCES definition(id) ON DELETE CASCADE,
    dst_id     INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
    kind       INTEGER NOT NULL,   -- edge_kind.id (1 calls / 7 uses)
    count      INTEGER NOT NULL DEFAULT 1,
    UNIQUE (src_def_id, dst_id, kind)
);
CREATE INDEX IF NOT EXISTS idx_def_edge_src ON def_edge(src_def_id, kind);
CREATE INDEX IF NOT EXISTS idx_def_edge_dst ON def_edge(dst_id, kind);

-- Materialised body->body "possible call" fan-out (mirrors dispatch_calls k18,
-- but the target is a DEFINITION not a symbol so it cannot live in `edge`). For
-- each def_edge(caller -> S) where S has >1 definition, one row per body of S.
-- Deleted and rebuilt each resolve pass.
CREATE TABLE IF NOT EXISTS possible_call (
    src_def_id INTEGER NOT NULL REFERENCES definition(id) ON DELETE CASCADE,
    dst_def_id INTEGER NOT NULL REFERENCES definition(id) ON DELETE CASCADE,
    count      INTEGER NOT NULL DEFAULT 1,
    UNIQUE (src_def_id, dst_def_id)
);
CREATE INDEX IF NOT EXISTS idx_possible_call_src ON possible_call(src_def_id);
CREATE INDEX IF NOT EXISTS idx_possible_call_dst ON possible_call(dst_def_id);

-- ---- v30: signature/type tier (parameters + normalized types) ---------------
-- Written ONLY by the C++ LibTooling indexer; Python is read/storage parity.
-- Mirrors src/storage/storage.cpp byte-for-byte.

CREATE TABLE IF NOT EXISTS type_kind (
    id   INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);
INSERT OR IGNORE INTO type_kind (id, name) VALUES
  (1,'builtin'), (2,'record'), (3,'enum'), (4,'alias'),
  (5,'pointer'), (6,'lvalue-reference'), (7,'rvalue-reference'),
  (8,'array'), (9,'function'), (10,'template-param'), (11,'other'),
  (12,'member-data-pointer'), (13,'member-function-pointer');

CREATE TABLE IF NOT EXISTS type_node (
    id           INTEGER PRIMARY KEY,
    type_key     TEXT NOT NULL UNIQUE,
    spelling     TEXT NOT NULL,
    kind         INTEGER NOT NULL,   -- type_kind.id (no FK: seed-only)
    is_const     INTEGER NOT NULL DEFAULT 0,
    is_volatile  INTEGER NOT NULL DEFAULT 0,
    is_restrict  INTEGER NOT NULL DEFAULT 0,
    decl_usr     TEXT,
    decl_id      INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
    canonical_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL
);
CREATE INDEX IF NOT EXISTS idx_type_node_decl_usr ON type_node(decl_usr);
CREATE INDEX IF NOT EXISTS idx_type_node_decl_id ON type_node(decl_id);
CREATE INDEX IF NOT EXISTS idx_type_node_canonical ON type_node(canonical_id);

CREATE TABLE IF NOT EXISTS type_edge_kind (
    id   INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);
INSERT OR IGNORE INTO type_edge_kind (id, name) VALUES
  (1,'pointee'), (2,'element_type'), (3,'alias_of'),
  (4,'return_type'), (5,'param_type'), (6,'template_argument_type'),
  (7,'member_owner'), (8,'member_component');

CREATE TABLE IF NOT EXISTS type_edge (
    src_id   INTEGER NOT NULL REFERENCES type_node(id) ON DELETE CASCADE,
    kind     INTEGER NOT NULL,   -- type_edge_kind.id (no FK: seed-only)
    position INTEGER NOT NULL DEFAULT 0,
    dst_id   INTEGER NOT NULL REFERENCES type_node(id) ON DELETE CASCADE,
    PRIMARY KEY (src_id, kind, position)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_type_edge_dst ON type_edge(dst_id);

CREATE TABLE IF NOT EXISTS parameter (
    owner_id INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    pack_index INTEGER NOT NULL DEFAULT -1,
    name     TEXT,
    type_id  INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
    declared_type_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
    adjusted_type_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
    default_text TEXT,
    default_origin TEXT,
    reference_semantics TEXT,
    file_id  INTEGER REFERENCES file(id) ON DELETE SET NULL,
    line     INTEGER,
    col      INTEGER,
    PRIMARY KEY (owner_id, position, pack_index)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_parameter_type ON parameter(type_id);
CREATE INDEX IF NOT EXISTS idx_parameter_declared_type ON parameter(declared_type_id);
CREATE INDEX IF NOT EXISTS idx_parameter_adjusted_type ON parameter(adjusted_type_id);

CREATE TABLE IF NOT EXISTS symbol_type_kind (
    id   INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);
INSERT OR IGNORE INTO symbol_type_kind (id, name) VALUES
  (1,'returns'), (2,'of_type'), (3,'underlying_type');

CREATE TABLE IF NOT EXISTS symbol_type (
    symbol_id INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
    kind      INTEGER NOT NULL,   -- symbol_type_kind.id (no FK: seed-only)
    type_id   INTEGER NOT NULL REFERENCES type_node(id) ON DELETE CASCADE,
    PRIMARY KEY (symbol_id, kind)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_symbol_type_type ON symbol_type(type_id);

-- ===================== v31: include tier =====================
-- Preprocessing facts live in their own file domain: `edge` is symbol->symbol
-- and cannot hold a file->file relation. Extraction is C++-only (LibTooling
-- PPCallbacks); Python owns storage + read queries only.

CREATE TABLE IF NOT EXISTS include_config (
    id           INTEGER PRIMARY KEY,
    tu_file_id   INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    digest       TEXT NOT NULL,
    driver       TEXT,
    working_dir  TEXT,
    arguments    TEXT,   -- JSON list of normalized parse args
    lang_mode    TEXT,   -- 'c' | 'c++'
    resource_dir TEXT,
    translation_unit_config_id INTEGER REFERENCES translation_unit_config(id)
                                ON DELETE SET NULL,
    UNIQUE (tu_file_id, digest)
);
CREATE INDEX IF NOT EXISTS idx_include_config_digest ON include_config(digest);

CREATE TABLE IF NOT EXISTS translation_unit_config (
    id INTEGER PRIMARY KEY,
    descriptor_hash TEXT NOT NULL UNIQUE,
    descriptor_json TEXT NOT NULL,
    driver TEXT,
    working_dir TEXT,
    language TEXT,
    standard TEXT,
    target TEXT,
    abi_options TEXT NOT NULL,
    sysroot TEXT,
    resource_dir TEXT,
    include_paths TEXT NOT NULL,
    macro_state TEXT NOT NULL,
    relevant_environment TEXT NOT NULL,
    generated_inputs TEXT NOT NULL,
    diagnostics_policy TEXT,
    arguments TEXT NOT NULL,
    state TEXT NOT NULL DEFAULT 'registered'
          CHECK (state IN ('registered','unregistered','ambiguous','stale','unavailable'))
);
CREATE INDEX IF NOT EXISTS idx_translation_unit_config_hash
    ON translation_unit_config(descriptor_hash);

CREATE TABLE IF NOT EXISTS translation_unit (
    id INTEGER PRIMARY KEY,
    file_id INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    config_id INTEGER NOT NULL REFERENCES translation_unit_config(id) ON DELETE CASCADE,
    state TEXT NOT NULL DEFAULT 'registered'
          CHECK (state IN ('registered','unregistered','ambiguous','stale','unavailable')),
    UNIQUE (file_id, config_id)
);

CREATE TABLE IF NOT EXISTS file_config (
    file_id INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    config_id INTEGER NOT NULL REFERENCES translation_unit_config(id) ON DELETE CASCADE,
    role TEXT NOT NULL CHECK (role IN ('translation_unit','header')),
    state TEXT NOT NULL DEFAULT 'registered'
          CHECK (state IN ('registered','unregistered','ambiguous','stale','unavailable')),
    reason TEXT,
    PRIMARY KEY (file_id, config_id, role)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_file_config_config ON file_config(config_id);

CREATE TABLE IF NOT EXISTS fact_applicability (
    fact_kind TEXT NOT NULL,
    fact_id INTEGER NOT NULL,
    file_id INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    config_id INTEGER NOT NULL REFERENCES translation_unit_config(id)
             ON DELETE CASCADE,
    generation INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (fact_kind, fact_id, file_id, config_id)
);
CREATE INDEX IF NOT EXISTS idx_fact_applicability_config
    ON fact_applicability(file_id, config_id, fact_kind, fact_id);

CREATE TABLE IF NOT EXISTS include_edge (
    id           INTEGER PRIMARY KEY,
    src_file_id  INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    dst_file_id  INTEGER REFERENCES file(id) ON DELETE SET NULL,
    dst_path     TEXT NOT NULL,
    config_id    INTEGER NOT NULL REFERENCES include_config(id) ON DELETE CASCADE,
    is_system    INTEGER NOT NULL DEFAULT 0,
    is_generated INTEGER NOT NULL DEFAULT 0,
    count        INTEGER NOT NULL DEFAULT 1,
    UNIQUE (src_file_id, dst_path, config_id)
);
CREATE INDEX IF NOT EXISTS idx_include_edge_dst ON include_edge(dst_file_id);
CREATE INDEX IF NOT EXISTS idx_include_edge_config ON include_edge(config_id);

CREATE TABLE IF NOT EXISTS include_directive_kind (
    id   INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);
INSERT OR IGNORE INTO include_directive_kind (id, name) VALUES
  (1,'include'), (2,'include_next'), (3,'import'), (4,'include_macros'),
  (5,'unknown');

CREATE TABLE IF NOT EXISTS include_site (
    id               INTEGER PRIMARY KEY,
    edge_id          INTEGER NOT NULL REFERENCES include_edge(id) ON DELETE CASCADE,
    line             INTEGER NOT NULL,
    col              INTEGER NOT NULL,
    begin_offset     INTEGER NOT NULL,
    end_offset       INTEGER NOT NULL,
    spelling         TEXT NOT NULL,   -- as written, without <> or ""
    is_angled        INTEGER NOT NULL DEFAULT 0,
    directive        INTEGER NOT NULL DEFAULT 1, -- include_directive_kind.id
    cond_fingerprint TEXT NOT NULL DEFAULT '',
    resolved         INTEGER NOT NULL DEFAULT 1,
    guarded          INTEGER NOT NULL DEFAULT 0,
    UNIQUE (edge_id, begin_offset)
);
CREATE INDEX IF NOT EXISTS idx_include_site_edge ON include_site(edge_id);

CREATE TABLE IF NOT EXISTS include_macro_use (
    src_file_id INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    def_path    TEXT NOT NULL,
    name        TEXT NOT NULL,
    config_id   INTEGER NOT NULL REFERENCES include_config(id) ON DELETE CASCADE,
    count       INTEGER NOT NULL DEFAULT 1,
    PRIMARY KEY (src_file_id, def_path, name, config_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_include_macro_use_path ON include_macro_use(def_path);

-- Manifest-governed immutable/rebuildable artifacts. These rows are core
-- metadata only: no foreign key crosses into a sidecar database.
CREATE TABLE IF NOT EXISTS artifact (
    id INTEGER PRIMARY KEY,
    logical_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    artifact_schema TEXT NOT NULL,
    catalog_version INTEGER NOT NULL,
    catalog_hash TEXT NOT NULL,
    producer_version TEXT NOT NULL,
    engine_version TEXT NOT NULL,
    workspace_identity TEXT NOT NULL,
    tu_identity TEXT NOT NULL DEFAULT '',
    configuration_identity TEXT NOT NULL DEFAULT '',
    input_fact_set_identity TEXT NOT NULL DEFAULT '',
    completeness TEXT NOT NULL CHECK (completeness IN ('complete','partial','unknown')),
    truncation TEXT NOT NULL CHECK (truncation IN ('none','truncated','unknown')),
    trust TEXT NOT NULL CHECK (trust IN ('unverified','producer-verified','reader-verified')),
    evidence TEXT NOT NULL CHECK (evidence IN ('source','derived','inferred','runtime','assumption','proof')),
    attachment_name TEXT NOT NULL,
    retention_policy TEXT NOT NULL DEFAULT 'retain',
    relative_path TEXT NOT NULL,
    content_hash TEXT NOT NULL,
    byte_size INTEGER NOT NULL CHECK (byte_size >= 0),
    state TEXT NOT NULL CHECK (state IN ('current','stale','retired')),
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    published_at TEXT,
    UNIQUE (logical_id, content_hash)
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_artifact_current_logical
    ON artifact(logical_id) WHERE state = 'current';
CREATE INDEX IF NOT EXISTS idx_artifact_state ON artifact(state);

CREATE TABLE IF NOT EXISTS artifact_relation (
    artifact_id INTEGER NOT NULL REFERENCES artifact(id) ON DELETE CASCADE,
    relation_name TEXT NOT NULL,
    PRIMARY KEY (artifact_id, relation_name)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS artifact_identity_map (
    artifact_id INTEGER NOT NULL REFERENCES artifact(id) ON DELETE CASCADE,
    local_identity TEXT NOT NULL,
    identity_kind TEXT NOT NULL,
    stable_identity TEXT NOT NULL,
    resolution_state TEXT NOT NULL CHECK (resolution_state IN ('resolved','unresolved','unknown')),
    core_symbol_id INTEGER,
    diagnostic TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (artifact_id, local_identity, identity_kind)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_artifact_identity_stable
    ON artifact_identity_map(stable_identity);

CREATE TABLE IF NOT EXISTS artifact_lease (
    artifact_id INTEGER NOT NULL REFERENCES artifact(id) ON DELETE CASCADE,
    lease_id TEXT NOT NULL,
    purpose TEXT NOT NULL,
    PRIMARY KEY (artifact_id, lease_id)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS artifact_pin (
    artifact_id INTEGER NOT NULL REFERENCES artifact(id) ON DELETE CASCADE,
    pin_id TEXT NOT NULL,
    reason TEXT NOT NULL,
    PRIMARY KEY (artifact_id, pin_id)
) WITHOUT ROWID;

INSERT OR IGNORE INTO meta (key, value) VALUES ('schema_version', '{SCHEMA_VERSION}');
INSERT INTO meta (key, value) VALUES ('catalog_version', '{CATALOG_VERSION}')
    ON CONFLICT(key) DO UPDATE SET value=excluded.value;
INSERT INTO meta (key, value) VALUES ('catalog_hash', '{CATALOG_HASH}')
    ON CONFLICT(key) DO UPDATE SET value=excluded.value;
INSERT INTO meta (key, value) VALUES ('artifact_kind', 'semantic-index')
    ON CONFLICT(key) DO UPDATE SET value=excluded.value;
INSERT INTO meta (key, value) VALUES ('status', 'complete')
    ON CONFLICT(key) DO UPDATE SET value=excluded.value;
INSERT INTO meta (key, value) VALUES ('trust', 'producer-verified')
    ON CONFLICT(key) DO UPDATE SET value=excluded.value;
INSERT INTO meta (key, value) VALUES ('evidence', 'source')
    ON CONFLICT(key) DO UPDATE SET value=excluded.value;
{CATALOG_SEED_SQL}
"""


@dataclass
class SemanticUniverse:
    """Explicit declared program/dependency scope for semantic identities."""

    key: str
    name: str
    policy: str = "explicit"
    id: Optional[int] = None


@dataclass
class Component:
    """A row in the ``component`` table -- and, once a ``Storage`` accessor hands
    it back, a *smart path* over that row.

    Like the storage-layer :class:`File`, a Component returned by
    :meth:`Storage.get_component` / :meth:`Storage.get_component_by_id` /
    :meth:`Storage.get_component_by_name` / :meth:`Storage.list_components` /
    :meth:`Storage.components_for_repository` / :meth:`Storage.component_for_path`
    is *bound* to its Storage (``_storage``), which lets it reconstruct its
    :attr:`abspath`, reach its owning :attr:`repo`, and stream the
    :meth:`directories` and :meth:`files` beneath it -- all as lazy generators."""

    name: str
    path: str
    kind: str = "repo"
    id: Optional[int] = None
    version: Optional[str] = None  # v14: nullable; NULL = unversioned
    repository_id: Optional[int] = None  # v23: owning repository; NULL = ungrouped
    semantic_universe_id: Optional[int] = None  # v35: explicit ungrouped scope

    _storage: "Optional[Storage]" = field(
        default=None, init=False, repr=False, compare=False
    )

    def _bound(self) -> "Storage":
        if self._storage is None:
            raise RuntimeError(
                "Component is not bound to a Storage; obtain it from "
                "Storage.get_component()/get_component_by_id()/list_components()"
            )
        return self._storage

    @property
    def abspath(self) -> str:
        """Absolute base directory of this component's tree (effective root,
        clone-resolved). The directory the ``path``/``version`` columns name."""
        return self._bound().component_abs_base(self)

    @property
    def repo(self) -> "Optional[Repository]":
        """The owning :class:`Repository` (v23 grouping), or ``None`` when this
        component is ungrouped (``repository_id`` is NULL)."""
        if self.repository_id is None:
            return None
        return self._bound().get_repository_by_id(self.repository_id)

    def directories(self, name: Optional[str] = None) -> "Iterator[Directory]":
        """Lazily yield every :class:`Directory` under this component, each bound
        to the same Storage, ordered by relative path. ``name`` fuzzy-filters on
        the relative directory path (e.g. ``"src"``)."""
        store = self._bound()
        if self.id is None:
            return
        for d, _comp_name in store.list_directories(
            component_id=self.id, name=name
        ):
            d._storage = store
            yield d

    def files(self, name: Optional[str] = None) -> "Iterator[File]":
        """Lazily yield every :class:`File` in this component (across all its
        directories), each bound to the same Storage, ordered by path. ``name``
        fuzzy-filters on the file name -- e.g. ``comp.files("sample.hpp")``
        yields every file whose name matches ``sample.hpp``."""
        store = self._bound()
        if self.id is None:
            return
        for f, _abspath in store.list_files(component_id=self.id, name=name):
            yield f

    # -- writable: add / remove / save --------------------------------------- #

    def add_file(
        self,
        relpath: str,
        *,
        mtime: Optional[float] = None,
        md5: Optional[str] = None,
        compile_options: "Optional[list[str]]" = None,
        driver: Optional[str] = None,
    ) -> "File":
        """Register a file under this component and return it bound.

        ``relpath`` is resolved against this component's :attr:`abspath` (so
        ``"src/util.c"`` lands in the ``src`` directory, creating the directory
        row if needed); an absolute path already under the component works too.
        Idempotent on (directory, name) -- re-adding refreshes metadata."""
        store = self._bound()
        abspath = (
            relpath if os.path.isabs(relpath)
            else os.path.join(self.abspath, relpath)
        )
        fid = store.add_file_path(
            abspath, mtime=mtime, md5=md5,
            compile_options=compile_options, driver=driver,
        )
        f = store.get_file_by_id(fid)
        assert f is not None
        return f

    def remove_file(self, name: str) -> int:
        """Delete every file in this component whose name matches the fuzzy
        ``name`` filter (and the symbols indexed from them). Returns the count
        removed. ``name`` is required -- pass a specific match to avoid wiping
        the whole component (use :meth:`Storage.delete_component` for that)."""
        if not name:
            raise ValueError("remove_file requires a name filter")
        store = self._bound()
        if self.id is None:
            return 0
        removed = 0
        for f, _abspath in store.list_files(component_id=self.id, name=name):
            if f.id is not None:
                store.delete_file(f.id)
                removed += 1
        return removed

    def save(self) -> "Component":
        """Persist this Component's current field values (name/path/kind/
        version/repository_id) back to its row. Returns self for chaining."""
        store = self._bound()
        if self.id is None:
            raise RuntimeError("component has no id; add it to a Storage first")
        store.update_component(
            self.id, self.name, self.path, self.kind,
            self.version, self.repository_id, self.semantic_universe_id,
        )
        return self


@dataclass
class Repository:
    """v23: a logical code base grouping >=1 components, with switchable clones.

    Once a ``Storage`` accessor hands it back (``get_repository_by_*`` /
    ``list_repositories``) it is bound to that Storage and acts as a *smart
    path*: :attr:`path` is its active clone root, and :meth:`components` /
    :meth:`files` stream what lives under it as lazy generators."""

    name: str
    kind: str = "repo"
    remote_url: Optional[str] = None
    active_clone_id: Optional[int] = None
    semantic_universe_id: Optional[int] = None  # v35: declared program universe
    id: Optional[int] = None

    _storage: "Optional[Storage]" = field(
        default=None, init=False, repr=False, compare=False
    )

    def _bound(self) -> "Storage":
        if self._storage is None:
            raise RuntimeError(
                "Repository is not bound to a Storage; obtain it from "
                "Storage.get_repository_by_id()/get_repository_by_name()/"
                "list_repositories()"
            )
        return self._storage

    @property
    def path(self) -> Optional[str]:
        """Absolute path of the repository's active clone root, or ``None`` when
        no clone is live (``active_clone_id`` is NULL)."""
        return self._bound()._active_clone_root(self.id)

    def components(self, name: Optional[str] = None) -> "Iterator[Component]":
        """Lazily yield every :class:`Component` grouped under this repository,
        each bound to the same Storage, ordered by name, path. ``name``
        fuzzy-filters on the component name."""
        store = self._bound()
        if self.id is None:
            return
        for comp in store.components_for_repository(self.id, name=name):
            yield comp

    def files(self, name: Optional[str] = None) -> "Iterator[File]":
        """Lazily yield every :class:`File` across all of this repository's
        components, each bound to the same Storage. ``name`` fuzzy-filters on the
        file name -- e.g. ``repo.files("sample.hpp")``."""
        for comp in self.components():
            yield from comp.files(name=name)

    # -- writable: components ------------------------------------------------- #

    def add_component(
        self,
        name: str,
        path: str,
        *,
        kind: Optional[str] = None,
        version: Optional[str] = None,
    ) -> "Component":
        """Register a component, group it under this repository, and return it
        bound. Mirrors the ``import``/``add-source`` grouping: the component's
        stored path is relativized to the active clone root (so a later
        :meth:`switch` repoints it for free). ``kind`` defaults to the repo's."""
        store = self._bound()
        if self.id is None:
            raise RuntimeError("repository has no id; add it to a Storage first")
        cid = store.add_component(name, path, kind=kind or self.kind, version=version)
        store.set_component_repository(cid, self.id)
        root = self._bound()._active_clone_root(self.id)
        if root is not None:
            store.relativize_component(cid, root)
        comp = store.get_component_by_id(cid)
        assert comp is not None
        return comp

    def remove_component(self, name: str) -> int:
        """Delete every component grouped under this repository whose name
        matches the fuzzy ``name`` filter (and its directories/files/symbols).
        Returns the count removed. ``name`` is required (avoid wiping the whole
        repository by accident)."""
        if not name:
            raise ValueError("remove_component requires a name filter")
        store = self._bound()
        if self.id is None:
            return 0
        removed = 0
        for comp in store.components_for_repository(self.id, name=name):
            if comp.id is not None:
                store.delete_component(comp.id)
                removed += 1
        return removed

    # -- writable: clones ----------------------------------------------------- #

    def _clones_matching(self, filter: str) -> "list[Clone]":
        """Clones of this repository whose label or path contains ``filter``
        (case-insensitive substring). Empty ``filter`` matches nothing."""
        if not filter or self.id is None:
            return []
        needle = filter.lower()
        out = []
        for c in self._bound().list_clones(self.id):
            hay = f"{c.label or ''}\n{c.path}".lower()
            if needle in hay:
                out.append(c)
        return out

    def add_clone(self, path: str, label: Optional[str] = None) -> "Clone":
        """Register a checkout/worktree directory for this repository and return
        the :class:`Clone`. Idempotent on path. Does NOT switch to it -- call
        :meth:`switch` (or pass its label there) to make it active."""
        store = self._bound()
        if self.id is None:
            raise RuntimeError("repository has no id; add it to a Storage first")
        cid = store.add_clone(self.id, path, label=label)
        clone = store.get_clone_by_id(cid)
        assert clone is not None
        return clone

    def switch(self, filter: str) -> "Clone":
        """Point this repository at the clone matching ``filter`` (case-
        insensitive substring on label or path) and make it active. Raises if
        the filter matches zero or more than one clone. Updates this object's
        :attr:`active_clone_id` in place and returns the now-active Clone."""
        matches = self._clones_matching(filter)
        if not matches:
            raise LookupError(f"no clone matches {filter!r}")
        if len(matches) > 1:
            labels = ", ".join(c.label or c.path for c in matches)
            raise LookupError(f"{filter!r} is ambiguous: matches {labels}")
        clone = matches[0]
        store = self._bound()
        assert self.id is not None
        store.set_active_clone(self.id, clone.id)
        self.active_clone_id = clone.id
        return clone

    def remove_clone(self, filter: str) -> int:
        """Delete every clone of this repository matching ``filter`` (case-
        insensitive substring on label or path). Returns the count removed. A
        removed active clone clears the active pointer (delete_clone handles it);
        this object's :attr:`active_clone_id` is refreshed to match. ``filter``
        is required."""
        if not filter:
            raise ValueError("remove_clone requires a filter")
        store = self._bound()
        removed = 0
        for c in self._clones_matching(filter):
            if c.id is not None:
                if c.id == self.active_clone_id:
                    self.active_clone_id = None
                store.delete_clone(c.id)
                removed += 1
        return removed

    def save(self) -> "Repository":
        """Persist this Repository's current field values (name/kind/remote_url/
        active_clone_id) back to its row. Returns self for chaining."""
        store = self._bound()
        if self.id is None:
            raise RuntimeError("repository has no id; add it to a Storage first")
        store.update_repository(
            self.id, self.name, self.kind, self.remote_url, self.active_clone_id,
            self.semantic_universe_id,
        )
        return self


@dataclass
class Clone:
    """v23: one checkout/worktree directory of a repository."""

    repository_id: int
    path: str
    label: Optional[str] = None
    id: Optional[int] = None


@dataclass
class Directory:
    """A row in the ``directory`` table -- and, once a ``Storage`` accessor hands
    it back, a *smart path* over that row.

    Bound by :meth:`Storage.get_directory` / :meth:`Storage.get_directory_by_id`
    / :meth:`Storage.list_directories`; :attr:`abspath` reconstructs its absolute
    path and :meth:`files` streams the files it directly holds."""

    component_id: int
    path: str
    id: Optional[int] = None

    _storage: "Optional[Storage]" = field(
        default=None, init=False, repr=False, compare=False
    )

    def _bound(self) -> "Storage":
        if self._storage is None:
            raise RuntimeError(
                "Directory is not bound to a Storage; obtain it from "
                "Storage.get_directory()/get_directory_by_id()/list_directories()"
            )
        return self._storage

    @property
    def name(self) -> str:
        """The directory's own name (the last path segment); ``""`` for the
        component root (whose relative ``path`` is empty)."""
        return os.path.basename(self.path) if self.path else ""

    @property
    def abspath(self) -> str:
        """Reconstructed absolute path of this directory (effective root +
        relative path, clone-resolved)."""
        store = self._bound()
        p = store.directory_abs_path(self.id) if self.id is not None else None
        if p is None:
            raise RuntimeError(
                f"cannot reconstruct abspath for directory {self.path!r}"
            )
        return p

    def files(self, name: Optional[str] = None) -> "Iterator[File]":
        """Lazily yield the :class:`File` rows held *directly* in this directory
        (not its subtree), each bound to the same Storage, ordered by name.
        ``name`` fuzzy-filters on the file name -- e.g. ``d.files("sample.hpp")``."""
        store = self._bound()
        if self.id is None:
            return
        yield from store.files_in_directory(self.id, name=name)


@dataclass
class File:
    """A row in the ``file`` table -- and, once a ``Storage`` accessor hands it
    back, a *smart path* over that row.

    The plain fields below are the persisted columns. In addition, every ``File``
    returned by :meth:`Storage.get_file` / :meth:`Storage.get_file_by_id` /
    :meth:`Storage.files` / :meth:`Storage.list_files` is *bound* to the Storage
    that produced it (``_storage``), which turns it into the storage-layer twin of
    :class:`indexer.query.File`: it can reconstruct its own :attr:`abspath`, reach
    its owning :attr:`component` / :attr:`repo`, read a :meth:`source` slice, list
    its :meth:`symbols`, and parse/walk its AST via :meth:`tu` / :meth:`walk`.

    The key difference from the read-only :class:`indexer.query.File` (whose
    ``index()`` only raises): storage is the **writable** layer, so :meth:`index`
    really parses + persists this file and :meth:`resolve` runs the resolve pass.

    Heavy collaborators (``index_source`` / ``astcache`` / ``compiledb``) are
    imported lazily inside the methods that need them to avoid an import cycle
    (``indexer.clang`` and ``indexer.utils.files`` both import this module).
    """

    directory_id: int
    name: str
    mtime: Optional[float] = None
    md5: Optional[str] = None
    compile_options: Optional[list[str]] = None
    driver: Optional[str] = None
    indexed: bool = False
    indexed_at: Optional[str] = None
    args_overridden: bool = False
    id: Optional[int] = None

    # -- live back-references (init=False => kept out of __init__ and, via the
    # `f.init` filter in _row_to, out of column hydration; Storage sets them per
    # instance). compare/repr False so two equal rows still compare equal. --- #
    _storage: "Optional[Storage]" = field(
        default=None, init=False, repr=False, compare=False
    )
    _abspath_cache: Optional[str] = field(
        default=None, init=False, repr=False, compare=False
    )
    _tu: Any = field(default=None, init=False, repr=False, compare=False)
    _tu_loaded: bool = field(default=False, init=False, repr=False, compare=False)

    # -- binding ------------------------------------------------------------- #

    def _bound(self) -> "Storage":
        """The Storage this File was handed back from, or a clear error."""
        if self._storage is None:
            raise RuntimeError(
                "File is not bound to a Storage; obtain it from "
                "Storage.get_file()/get_file_by_id()/files()/list_files()"
            )
        return self._storage

    @property
    def abspath(self) -> str:
        """Reconstructed absolute path (component effective-root + dir + name)."""
        if self._abspath_cache is None:
            store = self._bound()
            p = (
                store.file_abs_path(self.id)
                if self.id is not None
                else (
                    os.path.join(d, self.name)
                    if (d := store.directory_abs_path(self.directory_id))
                    else None
                )
            )
            if p is None:
                raise RuntimeError(
                    f"cannot reconstruct abspath for file {self.name!r}"
                )
            self._abspath_cache = p
        return self._abspath_cache

    # -- owning component / repository --------------------------------------- #

    @property
    def component(self) -> "Optional[Component]":
        """The owning :class:`Component`, or ``None`` if no component owns it."""
        return self._bound().component_for_path(self.abspath)

    @property
    def repo(self) -> "Optional[Repository]":
        """The owning :class:`Repository` (v23 grouping), or ``None`` when the
        file's component is ungrouped/unregistered."""
        comp = self.component
        if comp is None or comp.repository_id is None:
            return None
        return self._bound().get_repository_by_id(comp.repository_id)

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
        with open(self.abspath, "r", encoding=encoding) as fh:
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

    # -- symbols ------------------------------------------------------------- #

    def symbols(self, limit: Optional[int] = None) -> "list[Symbol]":
        """The indexed symbols declared in this file (``Symbol`` rows, by
        line/col). ``limit`` caps the result; ``None`` (default) returns all."""
        if self.id is None:
            return []
        syms = self._bound().symbols_in_file(self.id)
        return syms[:limit] if limit is not None else syms

    # -- AST ----------------------------------------------------------------- #

    def _target(self):
        """An :class:`indexer.astcmd.Target` for this file -- resolved flags +
        driver, ready for :func:`indexer.astcache.load_or_parse`."""
        from indexer import compiledb
        from indexer.astcmd import Target

        store = self._bound()
        opts = compiledb.resolve_options(
            compiledb.sanitize(self.compile_options or []), store.get_alias
        )
        return Target(abspath=self.abspath, flags=opts, driver=self.driver)

    def tu(self, cache: bool = True):
        """The file's ``clang.cindex.TranslationUnit``, **memoized on this File**.

        The first call parses (or, when ``cache`` is True, loads the on-disk
        ``.ast``, writing it on a miss) and stores the TU; later calls return the
        SAME object. Pass ``cache=False`` to force a fresh parse from source; that
        result replaces the memo. Returns ``None`` only if the parse fails."""
        if cache and self._tu_loaded:
            return self._tu
        from indexer import astcache

        tu = astcache.load_or_parse(self._target(), use_cache=cache)
        if tu is not None:
            self._tu = tu
            self._tu_loaded = True
        return tu

    def walk(self, cache: bool = True):
        """Generator over EVERY cursor in this file's AST (pre-order). Reuses the
        memoized :meth:`tu` (``cache`` forwarded). Yields nothing on parse fail."""
        tu = self.tu(cache=cache)
        if tu is None:
            return

        def _walk(cursor):
            yield cursor
            for child in cursor.get_children():
                yield from _walk(child)

        for child in tu.cursor.get_children():
            yield from _walk(child)

    # -- writable: index / resolve ------------------------------------------- #

    def index(self) -> "dict[str, Any]":
        """Parse + index THIS file (its TU + headers): persist its symbols, its
        parse diagnostics, and the indexed mark, then free the TU.

        This is the *real* index -- storage is the writable layer, so (unlike the
        read-only :meth:`indexer.query.File.index`, which can only defer) a caller
        just does ``f.index()`` and the File handles every detail. Returns the
        ``index_source`` result; re-raises ``ClangParseError`` on a fatal parse,
        after recording its diagnostics on this file's row."""
        from indexer import compiledb
        from indexer.clang import ClangParseError, index_source

        store = self._bound()
        if self.id is None:
            raise RuntimeError("file has no id; add_file it to a Storage first")
        path = self.abspath
        opts = compiledb.resolve_options(
            compiledb.sanitize(self.compile_options or []), store.get_alias
        )
        try:
            result = index_source(
                store,
                path,
                opts,
                self.id,
                driver=self.driver,
                header_options=self.compile_options,
            )
        except ClangParseError as e:
            store.replace_diagnostics(
                self.id,
                e.diagnostics
                or [
                    {
                        "severity": 4,  # clang.cindex.Diagnostic.Fatal
                        "spelling": str(e),
                        "file_path": path,
                        "line": None,
                        "col": None,
                    }
                ],
            )
            raise
        mtime = os.path.getmtime(path) if os.path.exists(path) else None
        store.replace_diagnostics(self.id, result["diagnostics"])
        store.mark_file_indexed(self.id, mtime=mtime)
        self.indexed = True
        self.mtime = mtime
        self._tu = None  # just (re)parsed -- drop the stale memoized TU
        self._tu_loaded = False
        return result

    def resolve(self) -> "tuple[int, int]":
        """Resolve the index (roll up edge counts, materialise ``entity_edge``,
        stamp ``graph_resolved_at``); returns ``(still_stub_count,
        cross_repo_edge_count)``. Resolution in cidx is index-wide, so this does
        the real resolve pass -- a script just calls ``f.resolve()``."""
        return self._bound().resolve_pass()

    # -- writable: save / remove --------------------------------------------- #

    def save(self) -> "File":
        """Persist this File's current field values back to its row (mtime, md5,
        compile_options, driver, indexed, args_overridden, ...). Returns self so
        edits chain: ``f.compile_options = [...]; f.save()``."""
        self._bound().update_file(self)
        return self

    def remove(self) -> None:
        """Delete THIS file and the symbols indexed from it (no filter -- a File
        is a single row). The object keeps its fields but is no longer backed by
        a row; re-add it via Storage.add_file to restore."""
        store = self._bound()
        if self.id is None:
            raise RuntimeError("file has no id; nothing to remove")
        store.delete_file(self.id)


@dataclass
class Diagnostic:
    file_id: int
    severity: int  # clang: 2=warning, 3=error, 4=fatal
    spelling: str
    file_path: Optional[str] = None
    line: Optional[int] = None
    col: Optional[int] = None
    id: Optional[int] = None


# -- v31 include tier ---------------------------------------------------------
# Read-only mirrors of the C++ records (src/storage/records.hpp). Extraction is
# C++-only -- these are never written from Python.


@dataclass
class IncludeConfig:
    tu_file_id: int
    digest: str
    driver: Optional[str] = None
    working_dir: Optional[str] = None
    arguments: Optional[str] = None  # JSON list, as stored
    lang_mode: Optional[str] = None
    resource_dir: Optional[str] = None
    translation_unit_config_id: Optional[int] = None
    id: Optional[int] = None


CONFIG_STATES = frozenset(
    {"registered", "unregistered", "ambiguous", "stale", "unavailable"}
)


@dataclass
class TranslationUnitConfig:
    descriptor_hash: str = ""
    descriptor_json: str = ""
    driver: Optional[str] = None
    working_dir: Optional[str] = None
    language: Optional[str] = None
    standard: Optional[str] = None
    target: Optional[str] = None
    abi_options: list[str] = field(default_factory=list)
    sysroot: Optional[str] = None
    resource_dir: Optional[str] = None
    include_paths: list[str] = field(default_factory=list)
    macro_state: list[str] = field(default_factory=list)
    relevant_environment: list[str] = field(default_factory=list)
    generated_inputs: list[str] = field(default_factory=list)
    diagnostics_policy: Optional[str] = None
    arguments: list[str] = field(default_factory=list)
    state: str = "registered"
    association_state: str = "registered"
    id: Optional[int] = None


@dataclass
class FileConfigApplicability:
    file_id: int
    config_id: int
    role: str = "header"
    state: str = "registered"
    reason: Optional[str] = None


def canonical_translation_unit_config_json(config: TranslationUnitConfig) -> str:
    """Return the cross-language, fixed-order descriptor representation."""
    values = [
        config.driver or "", config.working_dir or "", config.language or "",
        config.standard or "", config.target or "", config.abi_options,
        config.sysroot or "", config.resource_dir or "", config.include_paths,
        config.macro_state, config.relevant_environment, config.generated_inputs,
        config.diagnostics_policy or "", config.arguments,
    ]
    return json.dumps(values, ensure_ascii=False, separators=(",", ":"))


def translation_unit_config_hash(config: TranslationUnitConfig) -> str:
    return hashlib.sha1(
        canonical_translation_unit_config_json(config).encode("utf-8")
    ).hexdigest()


def resolve_translation_unit_config(
    arguments: Sequence[str],
    *,
    driver: Optional[str] = None,
    working_dir: Optional[str] = None,
    language: Optional[str] = None,
    resource_dir: Optional[str] = None,
    diagnostics_policy: Optional[str] = "error-limit=0",
) -> TranslationUnitConfig:
    """Build the shared descriptor fields from one replayable argument list."""
    args = list(arguments)
    config = TranslationUnitConfig(
        driver=driver, working_dir=working_dir, language=language,
        resource_dir=resource_dir, diagnostics_policy=diagnostics_policy,
        arguments=args,
    )
    def last(names: tuple[str, ...]) -> Optional[str]:
        value = None
        i = 0
        while i < len(args):
            arg = args[i]
            for name in names:
                if arg.startswith(name + "="):
                    value = arg[len(name) + 1:]
                elif arg == name and i + 1 < len(args):
                    i += 1
                    value = args[i]
            i += 1
        return value
    config.standard = last(("-std", "--std"))
    config.target = last(("-target", "--target"))
    config.sysroot = last(("-isysroot", "--sysroot"))
    def values(flags: tuple[str, ...]) -> list[str]:
        out: list[str] = []
        i = 0
        while i < len(args):
            arg = args[i]
            for flag in flags:
                if arg == flag and i + 1 < len(args):
                    i += 1
                    out.append(args[i])
                elif arg.startswith(flag) and len(arg) > len(flag):
                    out.append(arg[len(flag):])
            i += 1
        return out
    config.include_paths = values(("-I", "-isystem", "-iquote", "-F"))
    config.generated_inputs = values(("-include", "-imacros", "-include-pch"))
    for i, arg in enumerate(args):
        if arg in {"-D", "-U"} and i + 1 < len(args):
            config.macro_state.append(arg + args[i + 1])
        elif arg.startswith(("-D", "-U")):
            config.macro_state.append(arg)
        if arg.startswith(("-m", "-fabi")) or arg in {
            "-fshort-wchar", "-fshort-enums", "-fno-exceptions", "-fno-rtti",
        }:
            config.abi_options.append(arg)
    for name in (
        "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
        "SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "CIDX_RESOURCE_DIR",
    ):
        if name in os.environ:
            config.relevant_environment.append(f"{name}={os.environ[name]}")
    if not config.language:
        config.language = "c++" if (
            "--driver-mode=g++" in args or "-xc++" in args or any(
                args[i + 1].startswith("c++")
                for i, arg in enumerate(args[:-1])
                if arg == "-x"
            )
        ) else "c"
    return config


@dataclass
class IncludeEdge:
    src_file_id: int
    dst_path: str
    config_id: int
    dst_file_id: Optional[int] = None  # None: system, unowned, or unresolved
    is_system: int = 0
    is_generated: int = 0
    count: int = 1
    id: Optional[int] = None


@dataclass
class ConfiguredIncludeEdges:
    edges: list[IncludeEdge] = field(default_factory=list)
    coverage_complete: bool = False


class FactCoverage:
    ONE = "one"
    ALL = "all"
    INVARIANT = "invariant"


@dataclass
class ConfiguredSymbols:
    symbols: list["Symbol"] = field(default_factory=list)
    coverage_complete: bool = False


@dataclass
class ConfiguredFactIds:
    ids: list[int] = field(default_factory=list)
    coverage_complete: bool = False


@dataclass
class IncludeSite:
    edge_id: int
    line: int
    col: int
    begin_offset: int
    end_offset: int
    spelling: str
    is_angled: int = 0
    directive: int = 1  # include_directive_kind.id
    cond_fingerprint: str = ""  # "" = unconditional top level
    resolved: int = 1
    guarded: int = 0
    id: Optional[int] = None


@dataclass
class Symbol:
    usr: str
    spelling: str
    kind: str
    qual_name: Optional[str] = None
    display_name: Optional[str] = None
    type_info: Optional[str] = None
    file_id: Optional[int] = None
    line: Optional[int] = None
    col: Optional[int] = None
    end_line: Optional[int] = None  # v25: end of the symbol's own extent at
    end_col: Optional[int] = None   # (line, col); (line..end_line) slices it whole
    decl_file_id: Optional[int] = None
    decl_line: Optional[int] = None
    decl_col: Optional[int] = None
    decl_path: Optional[str] = None  # raw decl path for an unregistered
    # (system/stdlib) target -- see schema
    is_definition: bool = False
    is_pure: bool = False
    is_static: bool = False
    is_instantiation: bool = False  # v13: implicit template-instantiation node
    linkage: Optional[str] = None
    access: Optional[str] = None
    parent_usr: Optional[str] = None
    resolved: bool = False
    multi_def: int = 0  # v27: number of definitions (bodies) of this symbol;
    # >1 == redefined per backend. Set at resolve, not by add_symbol.
    const_value: Optional[str] = None  # v33: the evaluated constant value of a
    # variable's initializer or an enumerator (Clang's constant evaluator
    # output); None when the initializer needs runtime evaluation.
    semantic_universe_id: int = -1  # v35: database-local scope row
    identity_key: str = ""  # v35: portable scope-keyed semantic identity
    identity_source: Optional[str] = None  # transient producer hint
    identity_translation_unit: Optional[str] = None  # transient TU/build hint
    id: Optional[int] = None


def _row_to(cls, row: Optional[sqlite3.Row]) -> Any:
    if row is None:
        return None
    # Only init fields map to columns; init=False fields (e.g. File's live
    # back-references) carry their defaults and are set by the accessor.
    columns = set(row.keys())
    kwargs = {
        f.name: row[f.name]
        for f in fields(cls)
        if f.init
        and f.name in columns
        and f.name not in {"identity_source", "identity_translation_unit"}
    }
    if cls is Symbol:
        # kind is stored as a CXCursorKind int (v16); present it as the name.
        kwargs["kind"] = SYMBOL_KIND_NAMES.get(kwargs["kind"], kwargs["kind"])
        kwargs["is_definition"] = bool(kwargs["is_definition"])
        kwargs["is_pure"] = bool(kwargs["is_pure"])
        kwargs["is_static"] = bool(kwargs["is_static"])
        kwargs["is_instantiation"] = bool(kwargs["is_instantiation"])
        kwargs["resolved"] = bool(kwargs["resolved"])
    if cls is File:
        kwargs["indexed"] = bool(kwargs["indexed"])
        kwargs["args_overridden"] = bool(kwargs["args_overridden"])
        if kwargs["compile_options"] is not None:
            kwargs["compile_options"] = json.loads(kwargs["compile_options"])
    return cls(**kwargs)


class Storage:
    """All access to the index database goes through this class.

    Every public mutator commits; wrap bulk work in `with db.transaction():`
    to batch commits (row-at-a-time autocommit is the classic 100x slowdown).
    """

    @classmethod
    def from_connection(
        cls, conn: sqlite3.Connection, path: str = "<connection>"
    ) -> "Storage":
        """Wrap an already-open sqlite3 connection WITHOUT migrating or writing.

        Mirrors :meth:`indexer.query.GraphQuery.from_connection`: bypasses
        ``__init__`` (no ``_migrate`` / schema / backfill / commit) so a strictly
        read-only handle -- e.g. the one a :class:`indexer.query.File` borrows from
        a ``?mode=ro`` ``GraphQuery`` -- can reuse the read accessors
        (``component_for_path`` / ``get_file`` / ``get_repository_by_id`` /
        ``component_abs_base`` / ``get_alias``) with no side effects. Does not take
        ownership of the connection (the caller closes it)."""
        self = cls.__new__(cls)
        conn.row_factory = sqlite3.Row
        self._conn = conn
        self._in_txn = False
        self._needs_entity_node_backfill = False
        _validate_catalog_hash(conn, path, require_present=True)
        return self

    def __init__(self, path: str = ":memory:"):
        if path != ":memory:":
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        self._conn = sqlite3.connect(path)
        self._conn.row_factory = sqlite3.Row
        self._conn.execute("PRAGMA foreign_keys = ON")
        self._needs_entity_node_backfill = False
        # Reject an incompatible existing catalog before migrations or schema
        # seeding can mutate the database. Fresh and legacy databases without a
        # catalog hash are upgraded and receive the current seed below.
        previous_catalog = _catalog_hash(self._conn)
        stored_schema_version = _schema_version(self._conn)
        predecessor_catalog = (
            previous_catalog == PREVIOUS_CATALOG_HASH
            and stored_schema_version == PREVIOUS_SCHEMA_VERSION
        )
        _validate_catalog_hash(
            self._conn, path, require_present=False, allow_predecessor=True
        )
        if previous_catalog == PREVIOUS_CATALOG_HASH and not predecessor_catalog:
            raise RuntimeError(
                "incompatible cidx semantic catalogs: predecessor catalog hash "
                f"requires schema_version {PREVIOUS_SCHEMA_VERSION} -> "
                f"{SCHEMA_VERSION}, found {stored_schema_version!r} for {path}"
            )
        self._migrate()  # before _SCHEMA: its indexes need new columns
        self._conn.executescript(_SCHEMA)
        self._conn.commit()
        if predecessor_catalog:
            self._conn.execute(
                "UPDATE meta SET value=? WHERE key='catalog_hash'", (CATALOG_HASH,)
            )
            self._conn.commit()
        self._in_txn = False
        self._backfill_translation_unit_configs()
        # v21 -> v22 one-time backfill: entity_node is a pure-DB classification
        # of existing symbols (no re-parse), so an upgraded index gets its design
        # types filled in immediately on open -- no `cidx index`/`resolve` needed.
        # (entity_node did not exist during _migrate; it does now, after _SCHEMA.)
        if self._needs_entity_node_backfill:
            from indexer.entity_rollup import _materialise_entity_nodes

            with self.transaction():
                _materialise_entity_nodes(self)
        self._reconcile_external_identities()

    def _reconcile_external_identities(self) -> None:
        """Promote unresolved occurrence identities when local rows arrive."""
        for source_kind, source_kind_id in SOURCE_KIND_IDS.items():
            self._conn.execute(
                "UPDATE edge_site SET recv_src_kind_id=? WHERE recv_src_kind=?",
                (source_kind_id, source_kind),
            )
            self._conn.execute(
                "UPDATE call_arg SET src_kind_id=? WHERE src_kind=?",
                (source_kind_id, source_kind),
            )
        unknown_edge = self._conn.execute(
            "SELECT recv_src_kind FROM edge_site WHERE recv_src_kind IS NOT NULL "
            "AND recv_src_kind_id IS NULL LIMIT 1"
        ).fetchone()
        if unknown_edge is not None:
            raise ValueError(f"unknown source kind {unknown_edge[0]!r}")
        unknown_arg = self._conn.execute(
            "SELECT src_kind FROM call_arg WHERE src_kind IS NOT NULL "
            "AND src_kind_id IS NULL LIMIT 1"
        ).fetchone()
        if unknown_arg is not None:
            raise ValueError(f"unknown source kind {unknown_arg[0]!r}")
        self._conn.execute("UPDATE edge_site SET recv_src_kind=NULL WHERE recv_src_kind_id IS NOT NULL")
        self._conn.execute("UPDATE call_arg SET src_kind=NULL WHERE src_kind_id IS NOT NULL")
        self._conn.execute(
            """UPDATE external_identity SET
                symbol_id = CASE WHEN identity_kind = ? THEN
                    (SELECT id FROM symbol WHERE usr = identity_text LIMIT 1)
                    ELSE NULL END,
                type_id = CASE WHEN identity_kind = ? THEN
                    (SELECT id FROM type_node WHERE decl_usr = identity_text ORDER BY id LIMIT 1)
                    ELSE NULL END,
                resolution_status = CASE WHEN
                    (identity_kind = ? AND EXISTS (SELECT 1 FROM symbol WHERE usr = identity_text))
                    OR (identity_kind = ? AND EXISTS (SELECT 1 FROM type_node WHERE decl_usr = identity_text))
                    THEN 1 ELSE 0 END""",
            (
                IDENTITY_KIND_IDS["symbol_usr"],
                IDENTITY_KIND_IDS["type_usr"],
                IDENTITY_KIND_IDS["symbol_usr"],
                IDENTITY_KIND_IDS["type_usr"],
            ),
        )
        self._conn.execute(
            "UPDATE type_node SET decl_id=(SELECT id FROM symbol s "
            "WHERE s.usr=type_node.decl_usr LIMIT 1) WHERE decl_usr IS NOT NULL"
        )
        self._conn.execute(
            "UPDATE symbol SET parent_id=(SELECT id FROM symbol p "
            "WHERE p.usr=symbol.parent_usr) WHERE parent_usr IS NOT NULL"
        )
        self._conn.execute(
            """UPDATE edge_site SET
                recv_decl_id=COALESCE(recv_decl_id,
                    (SELECT id FROM symbol s WHERE s.usr=edge_site.recv_decl_usr LIMIT 1),
                    (SELECT symbol_id FROM external_identity i WHERE i.id=edge_site.recv_decl_identity_id)),
                recv_type_id=COALESCE(recv_type_id,
                    (SELECT id FROM type_node t WHERE t.decl_usr=edge_site.recv_type_usr ORDER BY id LIMIT 1),
                    (SELECT type_id FROM external_identity i WHERE i.id=edge_site.recv_type_identity_id))"""
        )
        self._conn.execute(
            """UPDATE call_arg SET
                decl_id=COALESCE(decl_id,
                    (SELECT id FROM symbol s WHERE s.usr=call_arg.decl_usr LIMIT 1),
                    (SELECT symbol_id FROM external_identity i WHERE i.id=call_arg.decl_identity_id)),
                callee_id=COALESCE(callee_id,
                    (SELECT id FROM symbol s WHERE s.usr=call_arg.callee_usr LIMIT 1),
                    (SELECT symbol_id FROM external_identity i WHERE i.id=call_arg.callee_identity_id)),
                type_id=COALESCE(type_id,
                    (SELECT id FROM type_node t WHERE t.decl_usr=call_arg.type_usr ORDER BY id LIMIT 1),
                    (SELECT type_id FROM external_identity i WHERE i.id=call_arg.type_identity_id))"""
        )
        self._conn.execute("UPDATE edge_site SET recv_decl_identity_id=NULL WHERE recv_decl_id IS NOT NULL")
        self._conn.execute("UPDATE edge_site SET recv_type_identity_id=NULL WHERE recv_type_id IS NOT NULL")
        self._conn.execute("UPDATE call_arg SET decl_identity_id=NULL WHERE decl_id IS NOT NULL")
        self._conn.execute("UPDATE call_arg SET callee_identity_id=NULL WHERE callee_id IS NOT NULL")
        self._conn.execute("UPDATE call_arg SET type_identity_id=NULL WHERE type_id IS NOT NULL")

    def _migrate(self) -> None:
        """In-place upgrade of a database created by an older schema version.

        v2 -> v3: adds symbol.qual_name and backfills it by walking the stored
        parent_usr chains (the longest chain per symbol is the full path).
        v3 -> v4: adds symbol.decl_file_id/decl_line/decl_col. For rows that
        are still declaration-only the stored location IS the declaration, so
        it is copied over; definition rows get their decl site on reindex.
        v5 -> v6: adds file.driver (compile-command argv[0]); backfilled on
        the next `import`.
        v7 -> v8: adds file.args_overridden (0/1); marks files whose compile
        flags were hand-edited via `cidx file` so re-import does not clobber
        them. Defaults to 0; no backfill needed.
        """
        tables = {
            r[0]
            for r in self._conn.execute(
                "SELECT name FROM sqlite_master WHERE type = 'table'"
            )
        }
        if "symbol" not in tables:
            return  # fresh database: _SCHEMA creates everything
        if "storage_enum_catalog" in tables:
            self._conn.execute("DROP VIEW IF EXISTS edge_site_read")
            self._conn.execute("DROP VIEW IF EXISTS call_arg_read")
            self._conn.execute("DROP TABLE storage_enum_catalog")
        # Older databases may retain compatibility views from a newer schema
        # while their referenced type tables were removed by a downgrade-style
        # fixture.  Drop those invalid views before any hot-table rebuild; the
        # schema script recreates them after the missing tables are restored.
        if "type_node" not in tables:
            self._conn.execute("DROP VIEW IF EXISTS edge_site_read")
            self._conn.execute("DROP VIEW IF EXISTS call_arg_read")
        cols = {r[1] for r in self._conn.execute("PRAGMA table_info(symbol)")}
        changed = False
        if "qual_name" not in cols:
            self._conn.execute("ALTER TABLE symbol ADD COLUMN qual_name TEXT")
            self._conn.execute("""
                WITH RECURSIVE chain(id, parent_usr, qual) AS (
                    SELECT id, parent_usr, spelling FROM symbol
                    UNION ALL
                    SELECT c.id, p.parent_usr,
                           CASE WHEN p.spelling = '' THEN c.qual
                                ELSE p.spelling || '::' || c.qual END
                    FROM chain c JOIN symbol p ON p.usr = c.parent_usr
                )
                UPDATE symbol SET qual_name = (
                    SELECT qual FROM chain WHERE chain.id = symbol.id
                    ORDER BY LENGTH(qual) DESC LIMIT 1
                )
            """)
            changed = True
        if "decl_file_id" not in cols:
            self._conn.execute(
                "ALTER TABLE symbol ADD COLUMN decl_file_id INTEGER "
                "REFERENCES file(id) ON DELETE SET NULL"
            )
            self._conn.execute("ALTER TABLE symbol ADD COLUMN decl_line INTEGER")
            self._conn.execute("ALTER TABLE symbol ADD COLUMN decl_col INTEGER")
            self._conn.execute(
                "UPDATE symbol SET decl_file_id = file_id, decl_line = line, "
                "decl_col = col WHERE is_definition = 0"
            )
            changed = True
        if "is_pure" not in cols:
            # No backfill possible from stored data -- reindex to populate.
            self._conn.execute(
                "ALTER TABLE symbol ADD COLUMN is_pure INTEGER NOT NULL DEFAULT 0"
            )
            changed = True
        if "is_static" not in cols:
            # v11 -> v12: C++ static member function flag. No backfill possible
            # from stored data -- reindex to populate; old rows read as 0.
            self._conn.execute(
                "ALTER TABLE symbol ADD COLUMN is_static INTEGER NOT NULL DEFAULT 0"
            )
            changed = True
        if "is_instantiation" not in cols:
            # v12 -> v13: implicit template-instantiation node marker. No backfill
            # possible from stored data -- reindex to populate; old rows read as 0.
            self._conn.execute(
                "ALTER TABLE symbol ADD COLUMN is_instantiation INTEGER NOT NULL DEFAULT 0"
            )
            changed = True
        if "decl_path" not in cols:
            # v8 -> v9: raw decl path for stubs whose target lives in an
            # unregistered (system/stdlib) file. No backfill -- those rows had no
            # location to recover; a reindex repopulates it from the AST.
            self._conn.execute("ALTER TABLE symbol ADD COLUMN decl_path TEXT")
            changed = True
        # v15 -> v16: symbol.kind moves from a TEXT name to its CXCursorKind
        # integer (compact storage; symbol_kind table recovers the string). The
        # column type changes and the old CHECK constraint must go, so the table
        # is rebuilt in place with the kind values converted. Runs after the
        # column-add migrations above so the new table mirrors all columns.
        kind_type = next(
            (r[2] for r in self._conn.execute("PRAGMA table_info(symbol)")
             if r[1] == "kind"),
            "",
        )
        if (kind_type or "").upper() != "INTEGER":
            nrows = next(
                (r[0] for r in self._conn.execute("SELECT COUNT(*) FROM symbol")),
                0,
            )
            self._migrate_symbol_kind_to_int()
            changed = True
        # v19 -> v20: named-instance marker. A template instance minted from a
        # NAMED `using`/typedef alias (X<B>) carries its own composes/aggregates
        # /associates instead of collapsing onto the primary. No backfill -- a
        # reindex repopulates it; old rows read as 0. Re-read the column set here
        # because the v15->v16 rebuild above recreates `symbol` (without this
        # column), so the snapshot taken at the top of _migrate may be stale.
        cols2 = {r[1] for r in self._conn.execute("PRAGMA table_info(symbol)")}
        if "is_named_instance" not in cols2:
            self._conn.execute(
                "ALTER TABLE symbol ADD COLUMN is_named_instance INTEGER NOT NULL DEFAULT 0"
            )
            changed = True
        # v24 -> v25: end of the symbol's own extent (end_line/end_col), paired
        # with (line, col). Only the START was stored before, so there is nothing
        # to backfill -- old rows read NULL until a reindex populates them from
        # the AST (cursor.extent.end).
        if "end_line" not in cols2:
            self._conn.execute("ALTER TABLE symbol ADD COLUMN end_line INTEGER")
            self._conn.execute("ALTER TABLE symbol ADD COLUMN end_col INTEGER")
            changed = True
        # v26 -> v27: count of definitions of this symbol (>1 == redefined per
        # backend). No backfill from stored data -- a reindex + resolve
        # populates it (definition rows are written at index, counted at
        # resolve); old rows read 0 until then. Uses cols2 because the v15->v16
        # rebuild recreates `symbol` without this column.
        if "multi_def" not in cols2:
            self._conn.execute(
                "ALTER TABLE symbol ADD COLUMN multi_def INTEGER NOT NULL DEFAULT 0"
            )
            changed = True
        # v32 -> v33: the evaluated constant value of a variable initializer /
        # enumerator. No backfill is possible from stored rows -- a reindex
        # populates it; old rows read NULL until then.
        if "const_value" not in cols2:
            self._conn.execute("ALTER TABLE symbol ADD COLUMN const_value TEXT")
            changed = True
        # v33 -> v34: dedicated alias_of(19) edge kind. The typedef/using-alias
        # -> underlying-type edge was previously stored as the overloaded
        # uses(7); rewrite exactly those rows: source is an alias symbol
        # (typedef 20 / type-alias 36) and the target is not a namespace (a
        # qualified alias like `using X = ns::Foo` also carries an alias -> ns
        # namespace-qualifier uses(7) edge, which stays a use). Mirrors
        # storage_migrate.cpp.
        if "edge_kind" in tables:
            have_alias_of = self._conn.execute(
                "SELECT 1 FROM edge_kind WHERE id = 19"
            ).fetchone()
            if have_alias_of is None:
                self._conn.execute(
                    "INSERT OR IGNORE INTO edge_kind (id, name) "
                    "VALUES (19, 'alias_of')"
                )
                self._conn.execute(
                    "UPDATE edge SET kind = 19 WHERE kind = 7 AND src_id IN "
                    "(SELECT id FROM symbol WHERE kind IN (20, 36)) "
                    "AND dst_id NOT IN (SELECT id FROM symbol WHERE kind = 22)"
                )
                changed = True
            # Same step, second rewrite: a variable(9) / member(6) -> its
            # declared type becomes of_type(20). Namespace-qualifier edges
            # are excluded exactly as above.
            have_of_type = self._conn.execute(
                "SELECT 1 FROM edge_kind WHERE id = 20"
            ).fetchone()
            if have_of_type is None:
                self._conn.execute(
                    "INSERT OR IGNORE INTO edge_kind (id, name) "
                    "VALUES (20, 'of_type')"
                )
                self._conn.execute(
                    "UPDATE edge SET kind = 20 WHERE kind = 7 AND src_id IN "
                    "(SELECT id FROM symbol WHERE kind IN (6, 9)) "
                    "AND dst_id NOT IN (SELECT id FROM symbol WHERE kind = 22)"
                )
                changed = True
        # v34 -> v35: make the symbol USR an explicitly scoped identity. All
        # existing rows belong to the legacy single-workspace universe; the
        # table rebuild preserves their ids and graph foreign keys.
        symbol_cols = {r[1] for r in self._conn.execute("PRAGMA table_info(symbol)")}
        repository_cols = (
            {r[1] for r in self._conn.execute("PRAGMA table_info(repository)")}
            if "repository" in tables
            else set()
        )
        component_cols = (
            {r[1] for r in self._conn.execute("PRAGMA table_info(component)")}
            if "component" in tables
            else set()
        )
        if (
            "semantic_universe" not in tables
            or "semantic_universe_id" not in symbol_cols
            or ("repository" in tables and "semantic_universe_id" not in repository_cols)
            or ("component" in tables and "semantic_universe_id" not in component_cols)
        ):
            self._migrate_symbol_identity_scope()
            changed = True
        # v27 -> v28: per-backend initializer text on a (static member) variable
        # definition. `definition` is created by the schema script; ALTER the
        # existing table so a v27 DB gains the column. No backfill -- a reindex
        # repopulates it; old rows read NULL until then.
        if "definition" in tables:
            dcols = {r[1] for r in self._conn.execute("PRAGMA table_info(definition)")}
            if "init_text" not in dcols:
                self._conn.execute("ALTER TABLE definition ADD COLUMN init_text TEXT")
                changed = True
        stored_version_row = self._conn.execute(
            "SELECT value FROM meta WHERE key = 'schema_version'"
        ).fetchone()
        stored_version = int(stored_version_row[0]) if stored_version_row and stored_version_row[0] else 0
        if stored_version < SCHEMA_VERSION:
            self._conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS external_identity (
                    id INTEGER PRIMARY KEY,
                    identity_kind INTEGER NOT NULL CHECK (identity_kind IN (1, 2, 3)),
                    identity_text TEXT NOT NULL,
                    resolution_status INTEGER NOT NULL DEFAULT 0
                        CHECK (resolution_status IN (0, 1)),
                    symbol_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
                    type_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
                    UNIQUE (identity_kind, identity_text)
                );
                CREATE INDEX IF NOT EXISTS idx_external_identity_symbol
                    ON external_identity(symbol_id);
                CREATE INDEX IF NOT EXISTS idx_external_identity_type
                    ON external_identity(type_id);
                """
            )

            def add_column(table: str, column: str, definition: str) -> None:
                cols = {r[1] for r in self._conn.execute(f"PRAGMA table_info({table})")}
                if column not in cols:
                    self._conn.execute(
                        f"ALTER TABLE {table} ADD COLUMN {column} {definition}"
                    )

        if stored_version < SCHEMA_VERSION and "symbol" in tables:
            add_column("symbol", "parent_id", "INTEGER REFERENCES symbol(id) ON DELETE SET NULL")
            self._conn.execute(
                "CREATE INDEX IF NOT EXISTS idx_symbol_parent_id ON symbol(parent_id)"
            )
            self._conn.execute(
                "UPDATE symbol SET parent_id = (SELECT id FROM symbol p "
                "WHERE p.usr = symbol.parent_usr) WHERE parent_usr IS NOT NULL"
            )
            if "type_node" in tables:
                add_column("type_node", "decl_id", "INTEGER REFERENCES symbol(id) ON DELETE SET NULL")
                self._conn.execute(
                    "CREATE INDEX IF NOT EXISTS idx_type_node_decl_id ON type_node(decl_id)"
                )
                self._conn.execute(
                    "UPDATE type_node SET decl_id = (SELECT id FROM symbol s "
                    "WHERE s.usr = type_node.decl_usr) WHERE decl_usr IS NOT NULL"
                )
            has_type_node = "type_node" in tables
            edge_cols = (
                {r[1] for r in self._conn.execute("PRAGMA table_info(edge_site)")}
                if "edge_site" in tables
                else set()
            )
            call_cols = (
                {r[1] for r in self._conn.execute("PRAGMA table_info(call_arg)")}
                if "call_arg" in tables
                else set()
            )
            has_edge_identity_text = {"recv_type_usr", "recv_decl_usr"}.issubset(edge_cols)
            has_call_identity_text = {"type_usr", "decl_usr", "callee_usr"}.issubset(call_cols)
            if "edge_site" in tables:
                for column, definition in (
                    ("recv_src_kind_id", "INTEGER"),
                    ("recv_type_id", "INTEGER REFERENCES type_node(id) ON DELETE SET NULL"),
                    ("recv_decl_id", "INTEGER REFERENCES symbol(id) ON DELETE SET NULL"),
                    ("recv_type_identity_id", "INTEGER REFERENCES external_identity(id) ON DELETE SET NULL"),
                    ("recv_decl_identity_id", "INTEGER REFERENCES external_identity(id) ON DELETE SET NULL"),
                ):
                    add_column("edge_site", column, definition)
                if "recv_src_kind" in edge_cols:
                    # v34 benchmark fixtures used "value" as a placeholder
                    # for absent provenance. It is missing evidence, not a
                    # source-kind domain member, so normalize it to NULL.
                    self._conn.execute(
                        "UPDATE edge_site SET recv_src_kind=NULL WHERE recv_src_kind='value'"
                    )
                    for source_kind, source_kind_id in SOURCE_KIND_IDS.items():
                        self._conn.execute(
                            "UPDATE edge_site SET recv_src_kind_id=? WHERE recv_src_kind=?",
                            (source_kind_id, source_kind),
                        )
                    unknown = self._conn.execute(
                        "SELECT recv_src_kind FROM edge_site WHERE recv_src_kind IS NOT NULL "
                        "AND recv_src_kind_id IS NULL LIMIT 1"
                    ).fetchone()
                    if unknown is not None:
                        raise ValueError(f"unknown source kind {unknown[0]!r} in edge_site migration")
                    self._conn.execute("UPDATE edge_site SET recv_src_kind=NULL")
                if has_type_node:
                    self._conn.execute(
                        "INSERT OR IGNORE INTO external_identity(identity_kind,identity_text,resolution_status,type_id) "
                        "SELECT 1, es.recv_type_usr, CASE WHEN tn.id IS NULL THEN 0 ELSE 1 END, tn.id "
                        "FROM edge_site es LEFT JOIN type_node tn ON tn.decl_usr = es.recv_type_usr "
                        "WHERE es.recv_type_usr IS NOT NULL"
                    )
                if has_edge_identity_text and has_type_node:
                    self._conn.execute(
                        "INSERT OR IGNORE INTO external_identity(identity_kind,identity_text,resolution_status,symbol_id) "
                        "SELECT 2, es.recv_decl_usr, CASE WHEN s.id IS NULL THEN 0 ELSE 1 END, s.id "
                        "FROM edge_site es LEFT JOIN symbol s ON s.usr = es.recv_decl_usr "
                        "WHERE es.recv_decl_usr IS NOT NULL"
                    )
                    self._conn.execute(
                        "UPDATE edge_site SET recv_decl_id = (SELECT id FROM symbol s WHERE s.usr = edge_site.recv_decl_usr), "
                        "recv_decl_identity_id = (SELECT id FROM external_identity i WHERE i.identity_kind=2 AND i.identity_text=edge_site.recv_decl_usr)"
                    )
                if has_edge_identity_text and has_type_node:
                    self._conn.execute(
                        "UPDATE edge_site SET recv_type_id = (SELECT id FROM type_node t WHERE t.decl_usr = edge_site.recv_type_usr), "
                        "recv_type_identity_id = (SELECT id FROM external_identity i WHERE i.identity_kind=1 AND i.identity_text=edge_site.recv_type_usr)"
                    )
                if has_edge_identity_text and has_type_node:
                    self._conn.execute(
                        "UPDATE edge_site SET recv_type_usr = NULL, recv_decl_usr = NULL"
                    )
            if "call_arg" in tables:
                for column, definition in (
                    ("src_kind_id", "INTEGER"),
                    ("type_id", "INTEGER REFERENCES type_node(id) ON DELETE SET NULL"),
                    ("decl_id", "INTEGER REFERENCES symbol(id) ON DELETE SET NULL"),
                    ("callee_id", "INTEGER REFERENCES symbol(id) ON DELETE SET NULL"),
                    ("type_identity_id", "INTEGER REFERENCES external_identity(id) ON DELETE SET NULL"),
                    ("decl_identity_id", "INTEGER REFERENCES external_identity(id) ON DELETE SET NULL"),
                    ("callee_identity_id", "INTEGER REFERENCES external_identity(id) ON DELETE SET NULL"),
                ):
                    add_column("call_arg", column, definition)
                for column in ("type_usr", "decl_usr", "callee_usr"):
                    add_column("call_arg", column, "TEXT")
                add_column("call_arg", "type_is_value", "INTEGER")
                source_kind_not_null = any(
                    row[1] == "src_kind" and row[3] for row in self._conn.execute("PRAGMA table_info(call_arg)")
                )
                if source_kind_not_null:
                    self._conn.execute("DROP VIEW IF EXISTS call_arg_read")
                    self._conn.execute("PRAGMA foreign_keys = OFF")
                    type_ref = " REFERENCES type_node(id) ON DELETE SET NULL" if has_type_node else ""
                    self._conn.executescript(
                        f"""
                        CREATE TABLE call_arg_v35 (
                            edge_id INTEGER NOT NULL REFERENCES edge(id) ON DELETE CASCADE,
                            file_id INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
                            line INTEGER NOT NULL,
                            col INTEGER NOT NULL,
                            position INTEGER NOT NULL,
                            src_kind TEXT,
                            type_usr TEXT,
                            decl_usr TEXT,
                            callee_usr TEXT,
                            src_kind_id INTEGER,
                            type_id INTEGER{type_ref},
                            decl_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
                            callee_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
                            type_identity_id INTEGER REFERENCES external_identity(id) ON DELETE SET NULL,
                            decl_identity_id INTEGER REFERENCES external_identity(id) ON DELETE SET NULL,
                            callee_identity_id INTEGER REFERENCES external_identity(id) ON DELETE SET NULL,
                            type_is_value INTEGER,
                            PRIMARY KEY (edge_id, file_id, line, col, position)
                        ) WITHOUT ROWID;
                        INSERT INTO call_arg_v35 SELECT edge_id,file_id,line,col,position,
                            src_kind,type_usr,decl_usr,callee_usr,src_kind_id,type_id,
                            decl_id,callee_id,type_identity_id,decl_identity_id,
                            callee_identity_id,type_is_value FROM call_arg;
                        DROP TABLE call_arg;
                        ALTER TABLE call_arg_v35 RENAME TO call_arg;
                        CREATE INDEX IF NOT EXISTS idx_call_arg_edge ON call_arg(edge_id);
                        """
                    )
                    self._conn.execute("PRAGMA foreign_keys = ON")
                if has_type_node:
                    self._conn.execute(
                        "INSERT OR IGNORE INTO external_identity(identity_kind,identity_text,resolution_status,type_id) "
                        "SELECT 1, ca.type_usr, CASE WHEN tn.id IS NULL THEN 0 ELSE 1 END, tn.id "
                        "FROM call_arg ca LEFT JOIN type_node tn ON tn.decl_usr = ca.type_usr "
                        "WHERE ca.type_usr IS NOT NULL"
                    )
                if has_call_identity_text and has_type_node:
                    self._conn.execute(
                        "INSERT OR IGNORE INTO external_identity(identity_kind,identity_text,resolution_status,symbol_id) "
                        "SELECT 2, u.value, CASE WHEN s.id IS NULL THEN 0 ELSE 1 END, s.id "
                        "FROM (SELECT decl_usr AS value FROM call_arg UNION SELECT callee_usr FROM call_arg) u "
                        "LEFT JOIN symbol s ON s.usr = u.value WHERE u.value IS NOT NULL"
                    )
                    self._conn.execute(
                        "UPDATE call_arg SET "
                        "decl_id=(SELECT id FROM symbol s WHERE s.usr=call_arg.decl_usr), "
                        "callee_id=(SELECT id FROM symbol s WHERE s.usr=call_arg.callee_usr), "
                        "decl_identity_id=(SELECT id FROM external_identity i WHERE i.identity_kind=2 AND i.identity_text=call_arg.decl_usr), "
                        "callee_identity_id=(SELECT id FROM external_identity i WHERE i.identity_kind=2 AND i.identity_text=call_arg.callee_usr)"
                    )
                for source_kind, source_kind_id in SOURCE_KIND_IDS.items():
                    self._conn.execute(
                        "UPDATE call_arg SET src_kind=NULL WHERE src_kind='value'"
                    )
                    self._conn.execute(
                        "UPDATE call_arg SET src_kind_id=? WHERE src_kind=?",
                        (source_kind_id, source_kind),
                    )
                unknown = self._conn.execute(
                    "SELECT src_kind FROM call_arg WHERE src_kind IS NOT NULL "
                    "AND src_kind_id IS NULL LIMIT 1"
                ).fetchone()
                if unknown is not None:
                    raise ValueError(f"unknown source kind {unknown[0]!r} in call_arg migration")
                self._conn.execute("UPDATE call_arg SET src_kind=NULL")
                if has_call_identity_text and has_type_node:
                    self._conn.execute(
                        "UPDATE call_arg SET type_id=(SELECT id FROM type_node t WHERE t.decl_usr=call_arg.type_usr), "
                        "type_identity_id=(SELECT id FROM external_identity i WHERE i.identity_kind=1 AND i.identity_text=call_arg.type_usr)"
                    )
                if has_call_identity_text and has_type_node:
                    self._conn.execute(
                        "UPDATE call_arg SET type_usr=NULL, decl_usr=NULL, callee_usr=NULL"
                    )
            # ALTER TABLE cannot add the CHECK clauses required by the
            # normalized domains. Rebuild migrated hot tables to match the
            # fresh v37 schema exactly.
            if "edge_site" in tables:
                add_column("edge_site", "args_sig", "TEXT")
                add_column("edge_site", "recv_param_pos", "INTEGER")
                add_column("edge_site", "recv_type_is_value", "INTEGER")
                self._conn.executescript(
                    """
                    DROP VIEW IF EXISTS edge_site_read;
                    PRAGMA foreign_keys = OFF;
                    CREATE TABLE edge_site_v37 (
                        edge_id INTEGER NOT NULL REFERENCES edge(id) ON DELETE CASCADE,
                        file_id INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
                        line INTEGER, col INTEGER,
                        conditional INTEGER NOT NULL DEFAULT 0,
                        args_sig TEXT, recv_src_kind TEXT, recv_type_usr TEXT,
                        recv_decl_usr TEXT,
                        recv_src_kind_id INTEGER CHECK (recv_src_kind_id IS NULL OR recv_src_kind_id IN (1,2,3,4,5,6,7,8)),
                        recv_type_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
                        recv_decl_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
                        recv_type_identity_id INTEGER REFERENCES external_identity(id) ON DELETE SET NULL,
                        recv_decl_identity_id INTEGER REFERENCES external_identity(id) ON DELETE SET NULL,
                        recv_param_pos INTEGER, recv_type_is_value INTEGER,
                        PRIMARY KEY (edge_id, file_id, line, col)
                    ) WITHOUT ROWID;
                    INSERT INTO edge_site_v37
                    SELECT edge_id,file_id,line,col,conditional,args_sig,
                           NULL,NULL,NULL,recv_src_kind_id,recv_type_id,
                           recv_decl_id,recv_type_identity_id,
                           recv_decl_identity_id,recv_param_pos,recv_type_is_value
                    FROM edge_site;
                    DROP TABLE edge_site;
                    ALTER TABLE edge_site_v37 RENAME TO edge_site;
                    PRAGMA foreign_keys = ON;
                    """
                )
            if "call_arg" in tables:
                add_column("call_arg", "type_is_value", "INTEGER")
                self._conn.executescript(
                    """
                    DROP VIEW IF EXISTS call_arg_read;
                    PRAGMA foreign_keys = OFF;
                    CREATE TABLE call_arg_v37 (
                        edge_id INTEGER NOT NULL REFERENCES edge(id) ON DELETE CASCADE,
                        file_id INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
                        line INTEGER NOT NULL, col INTEGER NOT NULL,
                        position INTEGER NOT NULL, src_kind TEXT,
                        type_usr TEXT, decl_usr TEXT, callee_usr TEXT,
                        src_kind_id INTEGER CHECK (src_kind_id IS NULL OR src_kind_id IN (1,2,3,4,5,6,7,8)),
                        type_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
                        decl_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
                        callee_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
                        type_identity_id INTEGER REFERENCES external_identity(id) ON DELETE SET NULL,
                        decl_identity_id INTEGER REFERENCES external_identity(id) ON DELETE SET NULL,
                        callee_identity_id INTEGER REFERENCES external_identity(id) ON DELETE SET NULL,
                        type_is_value INTEGER,
                        PRIMARY KEY (edge_id, file_id, line, col, position)
                    ) WITHOUT ROWID;
                    INSERT INTO call_arg_v37
                    SELECT edge_id,file_id,line,col,position,NULL,NULL,NULL,NULL,
                           src_kind_id,type_id,decl_id,callee_id,type_identity_id,
                           decl_identity_id,callee_identity_id,type_is_value
                    FROM call_arg;
                    DROP TABLE call_arg;
                    ALTER TABLE call_arg_v37 RENAME TO call_arg;
                    CREATE INDEX IF NOT EXISTS idx_call_arg_edge ON call_arg(edge_id);
                    PRAGMA foreign_keys = ON;
                    """
                )
            changed = True
        fcols = {r[1] for r in self._conn.execute("PRAGMA table_info(file)")}
        if "file" in tables and "driver" not in fcols:
            # No backfill possible from stored data -- re-import to populate.
            self._conn.execute("ALTER TABLE file ADD COLUMN driver TEXT")
            changed = True
        if "file" in tables and "args_overridden" not in fcols:
            # v7 -> v8: per-file flag override marker (`cidx file`). Existing
            # rows default to 0 (not overridden), so re-import behaves as before.
            self._conn.execute(
                "ALTER TABLE file ADD COLUMN args_overridden INTEGER NOT NULL DEFAULT 0"
            )
            changed = True
        # v9 -> v10: receiver provenance + per-argument provenance for virtual
        # dispatch.  No backfill -- reindex repopulates from the AST.
        escols = (
            {r[1] for r in self._conn.execute("PRAGMA table_info(edge_site)")}
            if "edge_site" in tables
            else set()
        )
        if "edge_site" in tables and "recv_src_kind" not in escols:
            self._conn.execute("ALTER TABLE edge_site ADD COLUMN recv_src_kind TEXT")
            self._conn.execute("ALTER TABLE edge_site ADD COLUMN recv_type_usr TEXT")
            self._conn.execute("ALTER TABLE edge_site ADD COLUMN recv_decl_usr TEXT")
            changed = True
        if "edge_site" in tables and "recv_param_pos" not in escols:
            self._conn.execute(
                "ALTER TABLE edge_site ADD COLUMN recv_param_pos INTEGER"
            )
            changed = True
        if "edge_site" in tables and "call_arg" not in tables:
            # The call_arg table itself is created by _SCHEMA (CREATE TABLE IF
            # NOT EXISTS), run after _migrate(), so the migration only needs to
            # flip changed to bump the version -- identical to the v6->v7 graph
            # tables pattern.
            changed = True
        # v10 -> v11: value-ness booleans for exact-singleton Gamma narrowing.
        # No backfill -- reindex repopulates; old rows read as NULL == not-value == TOP.
        if "edge_site" in tables and "recv_type_is_value" not in escols:
            self._conn.execute(
                "ALTER TABLE edge_site ADD COLUMN recv_type_is_value INTEGER"
            )
            changed = True
        cacols = (
            {r[1] for r in self._conn.execute("PRAGMA table_info(call_arg)")}
            if "call_arg" in tables
            else set()
        )
        if "call_arg" in tables and "type_is_value" not in cacols:
            self._conn.execute("ALTER TABLE call_arg ADD COLUMN type_is_value INTEGER")
            changed = True
        # v13 -> v14: component.version column + label table.
        compcols = {r[1] for r in self._conn.execute("PRAGMA table_info(component)")}
        if "component" in tables and "version" not in compcols:
            self._conn.execute("ALTER TABLE component ADD COLUMN version TEXT")
            # No backfill -- existing components get version = NULL.
            changed = True
        # v22 -> v23: repository + clone tables and component.repository_id.
        # The two tables are created by the schema script (CREATE TABLE IF NOT
        # EXISTS, run after _migrate); only the new component column needs an
        # ALTER here. No backfill -- existing components stay ungrouped
        # (repository_id NULL) until a re-import or a `cidx repo` command
        # attaches them.
        if "component" in tables and "repository_id" not in compcols:
            self._conn.execute(
                "ALTER TABLE component ADD COLUMN repository_id INTEGER"
            )
            changed = True
        if "repository" not in tables:
            changed = True
        # v23 -> v24: `component.path` is now UNIQUE per repository (was global).
        # Rebuild the table when the old global UNIQUE(path) is still in place so
        # the clone-relative paths below (multiple '.' roots) do not collide.
        if "component" in tables:
            comp_sql_row = self._conn.execute(
                "SELECT sql FROM sqlite_master "
                "WHERE type = 'table' AND name = 'component'"
            ).fetchone()
            if (
                comp_sql_row is not None
                and "UNIQUE (repository_id, path)" not in (comp_sql_row[0] or "")
            ):
                self._migrate_component_repo_unique()
                changed = True
        # v23 -> v24: a grouped component's path becomes RELATIVE to its
        # repository's active clone root, so `repo switch` only repoints the
        # active pointer (no per-component rewrite). Convert any component that
        # is grouped (repository_id set), has a live active clone, stores an
        # ABSOLUTE path, and sits under that clone -- strip the clone prefix
        # (`.` when it IS the clone root). Ungrouped/portable/outside-clone
        # components stay absolute. Idempotent: an already-relative path is
        # skipped, so this never fires twice. Runs here (the repository/clone
        # tables already exist on a v23 DB; a fresh/<v23 DB has no grouped
        # components, so this is a no-op for them).
        if (
            "repository" in tables
            and "clone" in tables
            and "repository_id" in compcols
        ):
            for crow in self._conn.execute(
                "SELECT id, path, repository_id FROM component "
                "WHERE repository_id IS NOT NULL"
            ).fetchall():
                cpath = crow["path"]
                if "<" in cpath or "$" in cpath or not os.path.isabs(cpath):
                    continue  # portable or already relative
                if _pathx.split_base_version(cpath)[1] is not None:
                    continue  # version-in-path: keep absolute (see relativize)
                rrow = self._conn.execute(
                    "SELECT active_clone_id FROM repository WHERE id = ?",
                    (crow["repository_id"],),
                ).fetchone()
                if rrow is None or rrow["active_clone_id"] is None:
                    continue
                clrow = self._conn.execute(
                    "SELECT path FROM clone WHERE id = ?",
                    (rrow["active_clone_id"],),
                ).fetchone()
                if clrow is None:
                    continue
                root = os.path.abspath(
                    _pathx.resolve_fs_path(clrow["path"])
                ).rstrip(os.sep)
                base = os.path.abspath(cpath).rstrip(os.sep)
                if base == root:
                    rel = "."
                elif base.startswith(root + os.sep):
                    rel = os.path.relpath(base, root)
                else:
                    continue  # component outside the active clone -> keep abs
                self._conn.execute(
                    "UPDATE component SET path = ? WHERE id = ?",
                    (rel, crow["id"]),
                )
                changed = True
        if "label" not in tables:
            # The schema script (run AFTER migrate) creates the table via
            # CREATE TABLE IF NOT EXISTS; the migration only needs to flip
            # changed so the schema_version meta is bumped.
            changed = True
        if "diagnostic" not in tables:
            # v14 -> v15: per-file parse diagnostics. Created by the schema
            # script (CREATE TABLE IF NOT EXISTS); no backfill possible -- a
            # reindex repopulates it from each TU's diagnostics.
            changed = True
        if "entity_edge" not in tables:
            # v16 -> v17: Layer-1 entity_edge + entity_edge_kind tables.
            # Created by the schema script (CREATE TABLE IF NOT EXISTS).
            # entity_edge is a derived, materialized table -- populate via
            # `cidx resolve`. No backfill on migration.
            changed = True
        else:
            # The `nests` entity_edge kind was removed (lexical nesting is a
            # symbol declaration-scope property, not a relation). Clean the DB in
            # place: drop the defunct nests rows (kind 10) and renumber befriends
            # 11 -> 10 to match the new contiguous seed. Order matters -- delete
            # the old kind-10 rows BEFORE renumbering 11 -> 10 so the two never
            # collide on UNIQUE(src,dst,kind,via). Also drop the stale
            # entity_edge_kind rows so _SCHEMA's INSERT OR IGNORE reseeds
            # (10,'befriends').
            #
            # Gate on the STALE DATA (a leftover 'nests' seed row), NOT the schema
            # version: an earlier build bumped schema_version to 18 WITHOUT
            # cleaning, so a version gate would skip those already-stamped DBs.
            # Idempotent -- after cleanup there is no 'nests' row, so it never
            # runs again.
            stale = self._conn.execute(
                "SELECT 1 FROM entity_edge_kind WHERE name = 'nests' LIMIT 1"
            ).fetchone()
            if stale is not None:
                self._conn.execute("DELETE FROM entity_edge WHERE kind = 10")
                self._conn.execute("UPDATE entity_edge SET kind = 10 WHERE kind = 11")
                self._conn.execute("DELETE FROM entity_edge_kind WHERE id IN (10, 11)")
                changed = True
            # Rename kind 2 'realizes' -> 'implements' (display name only; the
            # stored entity_edge.kind int is unchanged). Data-gated on the old
            # name so it fires regardless of schema_version; _SCHEMA's INSERT OR
            # IGNORE would otherwise leave the stale (2,'realizes') row in place.
            renamed = self._conn.execute(
                "SELECT 1 FROM entity_edge_kind WHERE id = 2 AND name = 'realizes'"
            ).fetchone()
            if renamed is not None:
                self._conn.execute(
                    "UPDATE entity_edge_kind SET name = 'implements' WHERE id = 2"
                )
                changed = True
            # v20 -> v21: NULL-safe entity_edge identity. The old table-level
            # UNIQUE(src,dst,kind,via_member_id) never collided on NULL-via rows
            # (SQLite NULL != NULL), so every materialise fanned NULL-via edges
            # out into duplicate copies. _SCHEMA now builds a COALESCE unique
            # index idx_entity_edge_identity; it would fail to create over a DB
            # that already carries those duplicates, so dedup in place first
            # (keep the lowest rowid per logical key). Gate on the index's
            # absence so it runs exactly once; entity_edge is derived, so `cidx
            # resolve` repopulates cleanly regardless.
            has_identity_idx = self._conn.execute(
                "SELECT 1 FROM sqlite_master WHERE type = 'index' "
                "AND name = 'idx_entity_edge_identity'"
            ).fetchone()
            if has_identity_idx is None:
                self._conn.execute(
                    "DELETE FROM entity_edge WHERE rowid NOT IN ("
                    "  SELECT MIN(rowid) FROM entity_edge GROUP BY "
                    "    src_id, dst_id, kind, "
                    "    COALESCE(via_member_id, -1), COALESCE(create_form, -1))"
                )
                changed = True
        if "entity_node" not in tables:
            # v21 -> v22: entity_node + entity_kind tables (the entity's design
            # type). The table is created by the schema script (run after this);
            # because the type is a pure-DB classification of existing symbols,
            # __init__ backfills it right after _SCHEMA -- no re-index/resolve.
            self._needs_entity_node_backfill = True
            changed = True
        if "decl_site" not in tables:
            # v25 -> v26: per-symbol declaration/reopen sites. Created by the
            # schema script (CREATE TABLE IF NOT EXISTS). No backfill from stored
            # rows is possible -- only the winning site survives on the symbol
            # row -- so a reindex repopulates every site. Bump the version so the
            # DB is stamped v26; references() on a namespace stays empty until
            # reindex (documented).
            changed = True
        if "template_arg" in tables:
            # v28 -> v29: canonical template_arg.arg_kind (the C++ indexer's
            # class-spec path used to store raw CXTemplateArgumentKind values;
            # contract codes are 1=type 2=non-type 3=template-template 4=pack).
            # VERSION-gated, not data-gated: after the remap a legitimate
            # template-template row (3) on a record-like owner is
            # indistinguishable from a legacy NullPtr row, so this must run
            # exactly once. Legacy 3 (NullPtr) only ever occurred on
            # record-like owners, and the ambiguous 3 must remap BEFORE
            # 5/6 -> 3 mints new, valid 3s. Mirrors storage.cpp.
            row = self._conn.execute(
                "SELECT value FROM meta WHERE key = 'schema_version'"
            ).fetchone()
            stored = int(row[0]) if row is not None and row[0] else 0
            if 0 < stored < 29:
                self._conn.execute(
                    "UPDATE template_arg SET arg_kind = 2 WHERE arg_kind = 3 "
                    "AND owner_id IN (SELECT id FROM symbol "
                    "                 WHERE kind IN (2, 3, 4, 31))"
                )
                self._conn.execute(
                    "UPDATE template_arg SET arg_kind = 3 WHERE arg_kind IN (5, 6)"
                )
                self._conn.execute(
                    "UPDATE template_arg SET arg_kind = 2 WHERE arg_kind = 7"
                )
                self._conn.execute(
                    "UPDATE template_arg SET arg_kind = 4 WHERE arg_kind = 8"
                )
                self._conn.execute("DELETE FROM template_arg WHERE arg_kind = 0")
                changed = True
        if "type_node" not in tables:
            # v29 -> v30: signature/type tier (type_node/type_edge/parameter/
            # symbol_type + seed tables). All created by the schema script
            # (CREATE TABLE IF NOT EXISTS); no backfill is possible from stored
            # rows -- a C++ reindex populates them. Mirrors storage.cpp.
            changed = True
        if "parameter" in tables:
            cols = {r[1] for r in self._conn.execute("PRAGMA table_info(parameter)")}
            for col, definition in (
                ("pack_index", "INTEGER NOT NULL DEFAULT -1"),
                ("declared_type_id", "INTEGER REFERENCES type_node(id) ON DELETE SET NULL"),
                ("adjusted_type_id", "INTEGER REFERENCES type_node(id) ON DELETE SET NULL"),
                ("default_text", "TEXT"),
                ("default_origin", "TEXT"),
                ("reference_semantics", "TEXT"),
            ):
                if col not in cols:
                    self._conn.execute(
                        f"ALTER TABLE parameter ADD COLUMN {col} {definition}"
                    )
                    changed = True
            self._conn.execute(
                "CREATE INDEX IF NOT EXISTS idx_parameter_declared_type "
                "ON parameter(declared_type_id)"
            )
            self._conn.execute(
                "CREATE INDEX IF NOT EXISTS idx_parameter_adjusted_type "
                "ON parameter(adjusted_type_id)"
            )
            sql = self._conn.execute(
                "SELECT sql FROM sqlite_master WHERE type='table' AND name='parameter'"
            ).fetchone()[0] or ""
            if "PRIMARY KEY (owner_id, position, pack_index)" not in sql:
                self._conn.commit()
                self._conn.execute("PRAGMA foreign_keys = OFF")
                self._conn.execute("""
                    CREATE TABLE parameter_v32 (
                        owner_id INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
                        position INTEGER NOT NULL,
                        pack_index INTEGER NOT NULL DEFAULT -1,
                        name TEXT,
                        type_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
                        declared_type_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
                        adjusted_type_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
                        default_text TEXT,
                        default_origin TEXT, reference_semantics TEXT,
                        file_id INTEGER REFERENCES file(id) ON DELETE SET NULL,
                        line INTEGER, col INTEGER,
                        PRIMARY KEY (owner_id, position, pack_index)
                    ) WITHOUT ROWID
                """)
                self._conn.execute("INSERT INTO parameter_v32 SELECT owner_id, position, pack_index, name, type_id, declared_type_id, adjusted_type_id, default_text, default_origin, reference_semantics, file_id, line, col FROM parameter")
                self._conn.execute("DROP TABLE parameter")
                self._conn.execute("ALTER TABLE parameter_v32 RENAME TO parameter")
                self._conn.commit()
                self._conn.execute("PRAGMA foreign_keys = ON")
                changed = True
        if "template_param" in tables:
            cols = {r[1] for r in self._conn.execute("PRAGMA table_info(template_param)")}
            for col in ("type_id", "default_type_id", "default_ref_id"):
                if col not in cols:
                    self._conn.execute(f"ALTER TABLE template_param ADD COLUMN {col} INTEGER")
                    changed = True
        if "template_arg" in tables:
            cols = {r[1] for r in self._conn.execute("PRAGMA table_info(template_arg)")}
            if "pack_index" not in cols:
                self._conn.execute(
                    "ALTER TABLE template_arg ADD COLUMN pack_index INTEGER NOT NULL DEFAULT -1"
                )
                changed = True
            if "type_id" not in cols:
                self._conn.execute("ALTER TABLE template_arg ADD COLUMN type_id INTEGER")
                changed = True
            sql = self._conn.execute(
                "SELECT sql FROM sqlite_master WHERE type='table' AND name='template_arg'"
            ).fetchone()[0] or ""
            if "PRIMARY KEY (owner_id, position, pack_index)" not in sql:
                type_fk = (
                    " REFERENCES type_node(id) ON DELETE SET NULL"
                    if "type_node" in tables
                    else ""
                )
                self._conn.commit()
                self._conn.execute("PRAGMA foreign_keys = OFF")
                self._conn.execute("""
                    CREATE TABLE template_arg_v32 (
                        owner_id INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
                        position INTEGER NOT NULL,
                        pack_index INTEGER NOT NULL DEFAULT -1,
                        arg_kind INTEGER NOT NULL,
                        ref_id INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
                        literal TEXT,
                        type_id INTEGER%s,
                        PRIMARY KEY (owner_id, position, pack_index)
                    ) WITHOUT ROWID
                """ % type_fk)
                self._conn.execute("INSERT INTO template_arg_v32 SELECT owner_id, position, pack_index, arg_kind, ref_id, literal, type_id FROM template_arg")
                self._conn.execute("DROP TABLE template_arg")
                self._conn.execute("ALTER TABLE template_arg_v32 RENAME TO template_arg")
                self._conn.commit()
                self._conn.execute("PRAGMA foreign_keys = ON")
                changed = True
        if "include_edge" not in tables:
            # v30 -> v31: include tier (include_config/include_edge/
            # include_site/include_macro_use + the directive seed table). All
            # created by the schema script (CREATE TABLE IF NOT EXISTS).
            # Preprocessing facts cannot be recovered from stored rows -- only a
            # C++ reindex populates them, so an upgraded DB has an EMPTY include
            # graph until `cidx index` reruns. Mirrors storage.cpp.
            changed = True
        # v34 -> v35: normalized translation-unit configuration identity. The
        # legacy include_config columns remain for read/API compatibility.
        if "include_config" in tables:
            include_cols = {
                r[1] for r in self._conn.execute("PRAGMA table_info(include_config)")
            }
            if "translation_unit_config_id" not in include_cols:
                self._conn.execute(
                    "ALTER TABLE include_config ADD COLUMN "
                    "translation_unit_config_id INTEGER REFERENCES "
                    "translation_unit_config(id) ON DELETE SET NULL"
                )
                changed = True
        if "artifact" in tables:
            artifact_columns = {
                row[1]
                for row in self._conn.execute("PRAGMA table_info(artifact)")
            }
            if "catalog_hash" not in artifact_columns:
                self._conn.execute("PRAGMA foreign_keys = OFF")
                for table in (
                    "artifact_relation",
                    "artifact_identity_map",
                    "artifact_lease",
                    "artifact_pin",
                ):
                    if table in tables:
                        self._conn.execute(
                            f"ALTER TABLE {table} RENAME TO {table}_v35"
                        )
                for index in (
                    "idx_artifact_current_logical",
                    "idx_artifact_state",
                    "idx_artifact_identity_stable",
                ):
                    self._conn.execute(f"DROP INDEX IF EXISTS {index}")
                self._conn.execute("ALTER TABLE artifact RENAME TO artifact_v35")
                self._conn.executescript(
                    """
                    CREATE TABLE artifact (
                        id INTEGER PRIMARY KEY,
                        logical_id TEXT NOT NULL,
                        kind TEXT NOT NULL,
                        artifact_schema TEXT NOT NULL,
                        catalog_version INTEGER NOT NULL,
                        catalog_hash TEXT NOT NULL,
                        producer_version TEXT NOT NULL,
                        engine_version TEXT NOT NULL,
                        workspace_identity TEXT NOT NULL,
                        tu_identity TEXT NOT NULL DEFAULT '',
                        configuration_identity TEXT NOT NULL DEFAULT '',
                        input_fact_set_identity TEXT NOT NULL DEFAULT '',
                        completeness TEXT NOT NULL CHECK (completeness IN ('complete','partial','unknown')),
                        truncation TEXT NOT NULL CHECK (truncation IN ('none','truncated','unknown')),
                        trust TEXT NOT NULL CHECK (trust IN ('unverified','producer-verified','reader-verified')),
                        evidence TEXT NOT NULL CHECK (evidence IN ('source','derived','inferred','runtime','assumption','proof')),
                        attachment_name TEXT NOT NULL,
                        retention_policy TEXT NOT NULL DEFAULT 'retain',
                        relative_path TEXT NOT NULL,
                        content_hash TEXT NOT NULL,
                        byte_size INTEGER NOT NULL CHECK (byte_size >= 0),
                        state TEXT NOT NULL CHECK (state IN ('current','stale','retired')),
                        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                        published_at TEXT,
                        UNIQUE (logical_id, content_hash)
                    );
                    """
                )
                self._conn.execute(
                    """INSERT INTO artifact(
                        id, logical_id, kind, artifact_schema, catalog_version,
                        catalog_hash, producer_version, engine_version,
                        workspace_identity, tu_identity, configuration_identity,
                        input_fact_set_identity, completeness, truncation, trust,
                        evidence, attachment_name, retention_policy, relative_path,
                        content_hash, byte_size, state, created_at, published_at)
                    SELECT id, logical_id, kind, artifact_schema,
                           CASE WHEN catalog_version GLOB '[0-9]*'
                                     AND catalog_version NOT GLOB '*[^0-9]*'
                                THEN CAST(catalog_version AS INTEGER)
                                ELSE 0 END,
                           '', producer_version, engine_version, workspace_identity,
                           tu_identity, configuration_identity,
                           input_fact_set_identity, completeness, truncation,
                           'unverified', 'assumption', attachment_name,
                           retention_policy, relative_path, content_hash,
                           byte_size,
                           CASE WHEN state = 'current' THEN 'stale' ELSE state END,
                           created_at, published_at
                    FROM artifact_v35""",
                )
                self._conn.executescript(
                    """
                    CREATE UNIQUE INDEX idx_artifact_current_logical
                        ON artifact(logical_id) WHERE state = 'current';
                    CREATE INDEX idx_artifact_state ON artifact(state);
                    CREATE TABLE artifact_relation (
                        artifact_id INTEGER NOT NULL REFERENCES artifact(id) ON DELETE CASCADE,
                        relation_name TEXT NOT NULL,
                        PRIMARY KEY (artifact_id, relation_name)
                    ) WITHOUT ROWID;
                    INSERT INTO artifact_relation SELECT * FROM artifact_relation_v35;
                    CREATE TABLE artifact_identity_map (
                        artifact_id INTEGER NOT NULL REFERENCES artifact(id) ON DELETE CASCADE,
                        local_identity TEXT NOT NULL,
                        identity_kind TEXT NOT NULL,
                        stable_identity TEXT NOT NULL,
                        resolution_state TEXT NOT NULL CHECK (resolution_state IN ('resolved','unresolved','unknown')),
                        core_symbol_id INTEGER,
                        diagnostic TEXT NOT NULL DEFAULT '',
                        PRIMARY KEY (artifact_id, local_identity, identity_kind)
                    ) WITHOUT ROWID;
                    INSERT INTO artifact_identity_map SELECT * FROM artifact_identity_map_v35;
                    CREATE INDEX idx_artifact_identity_stable
                        ON artifact_identity_map(stable_identity);
                    CREATE TABLE artifact_lease (
                        artifact_id INTEGER NOT NULL REFERENCES artifact(id) ON DELETE CASCADE,
                        lease_id TEXT NOT NULL,
                        purpose TEXT NOT NULL,
                        PRIMARY KEY (artifact_id, lease_id)
                    ) WITHOUT ROWID;
                    INSERT INTO artifact_lease SELECT * FROM artifact_lease_v35;
                    CREATE TABLE artifact_pin (
                        artifact_id INTEGER NOT NULL REFERENCES artifact(id) ON DELETE CASCADE,
                        pin_id TEXT NOT NULL,
                        reason TEXT NOT NULL,
                        PRIMARY KEY (artifact_id, pin_id)
                    ) WITHOUT ROWID;
                    INSERT INTO artifact_pin SELECT * FROM artifact_pin_v35;
                    """
                )
                for table in (
                    "artifact_relation",
                    "artifact_identity_map",
                    "artifact_lease",
                    "artifact_pin",
                ):
                    if table in tables:
                        self._conn.execute(f"DROP TABLE {table}_v35")
                self._conn.execute("DROP TABLE artifact_v35")
                self._conn.execute("PRAGMA foreign_keys = ON")
                changed = True
            legacy_hashes = self._conn.execute(
                """
                UPDATE artifact
                   SET content_hash = 'legacy-sha1:' || content_hash,
                       state = CASE WHEN state = 'current' THEN 'stale' ELSE state END
                 WHERE content_hash NOT LIKE 'sha256:%'
                   AND content_hash NOT LIKE 'legacy-sha1:%'
                """
            )
            if legacy_hashes.rowcount > 0:
                changed = True
        if "artifact" not in tables:
            # v37 -> v38: add the manifest-governed artifact tables. The schema
            # script creates the tables after migrate; this probe only advances
            # the compatibility version. Older artifact-less databases follow
            # the same ordered upgrade path.
            changed = True
        if "edge" not in tables:
            # v6 -> v7: graph layer. The schema script (run AFTER migrate) creates
            # the tables + indexes + seeds edge_kind; nothing to backfill from
            # stored data (edges are derived — re-run `cidx index`/`resolve`).
            changed = True
        elif not changed:
            # edge table exists: bump version only when stored version is OLDER
            # (future-schema DBs — version > SCHEMA_VERSION — are left untouched).
            row = self._conn.execute(
                "SELECT value FROM meta WHERE key = 'schema_version'"
            ).fetchone()
            if row is not None:
                v = row[0]
                if v and int(v) < SCHEMA_VERSION:
                    changed = True
        if changed:
            self._conn.execute(
                "UPDATE meta SET value = ? WHERE key = 'schema_version'",
                (str(SCHEMA_VERSION),),
            )
            self._conn.commit()

    def _backfill_translation_unit_configs(self) -> None:
        """Adapt v34 include rows into the v35 normalized identity tier."""
        rows = self._conn.execute(
            "SELECT * FROM include_config WHERE translation_unit_config_id IS NULL "
            "ORDER BY id"
        ).fetchall()
        for row in rows:
            include = _row_to(IncludeConfig, row)
            args = json.loads(include.arguments) if include.arguments else []
            config = resolve_translation_unit_config(
                args,
                driver=include.driver,
                working_dir=include.working_dir,
                language=include.lang_mode,
                resource_dir=include.resource_dir,
            )
            config_id = self.add_translation_unit_config(config)
            self._conn.execute(
                "UPDATE include_config SET translation_unit_config_id = ? WHERE id = ?",
                (config_id, include.id),
            )
            self._conn.execute(
                "INSERT OR IGNORE INTO translation_unit(file_id, config_id) VALUES (?, ?)",
                (include.tu_file_id, config_id),
            )
            self._conn.execute(
                "INSERT OR IGNORE INTO file_config(file_id, config_id, role) VALUES (?, ?, 'translation_unit')",
                (include.tu_file_id, config_id),
            )
        self._conn.commit()

    def _migrate_symbol_identity_scope(self) -> None:
        """v34 -> v35: scope portable USRs by a declared semantic universe."""
        self._conn.executescript("""
            CREATE TABLE IF NOT EXISTS semantic_universe (
                id INTEGER PRIMARY KEY,
                key TEXT NOT NULL UNIQUE,
                name TEXT NOT NULL,
                policy TEXT NOT NULL DEFAULT 'explicit'
            );
            INSERT OR IGNORE INTO semantic_universe (id, key, name, policy)
                VALUES (1, 'legacy', 'Legacy single-workspace universe', 'legacy');
        """)
        tables = {
            r[0] for r in self._conn.execute(
                "SELECT name FROM sqlite_master WHERE type = 'table'"
            )
        }
        if "repository" in tables:
            cols = {r[1] for r in self._conn.execute("PRAGMA table_info(repository)")}
            if "semantic_universe_id" not in cols:
                self._conn.execute(
                    "ALTER TABLE repository ADD COLUMN semantic_universe_id "
                    "INTEGER NOT NULL DEFAULT 1"
                )
        if "component" in tables:
            cols = {r[1] for r in self._conn.execute("PRAGMA table_info(component)")}
            if "semantic_universe_id" not in cols:
                self._conn.execute(
                    "ALTER TABLE component ADD COLUMN semantic_universe_id "
                    "INTEGER REFERENCES semantic_universe(id) ON DELETE SET NULL"
                )
        symbol_cols = {r[1] for r in self._conn.execute("PRAGMA table_info(symbol)")}
        if "semantic_universe_id" in symbol_cols:
            return

        self._conn.commit()
        self._conn.execute("PRAGMA foreign_keys = OFF")
        has_file_paths = {
            "file", "directory", "component", "repository"
        }.issubset(tables)
        if has_file_paths:
            legacy_source = (
                "COALESCE((SELECT 'source:' || "
                "COALESCE(r.remote_url, 'repo:' || r.name, 'component:' || c.path) "
                "|| char(31) || c.path || char(31) || d.path || char(31) || f.name "
                "FROM file f JOIN directory d ON d.id = f.directory_id "
                "JOIN component c ON c.id = d.component_id "
                "LEFT JOIN repository r ON r.id = c.repository_id "
                "WHERE f.id = symbol.file_id), 'source:unknown')"
            )
        else:
            legacy_source = "'source:unknown'"
        self._conn.executescript(f"""
            CREATE TABLE symbol_v35 (
                id INTEGER PRIMARY KEY,
                usr TEXT NOT NULL,
                spelling TEXT NOT NULL,
                qual_name TEXT,
                display_name TEXT,
                kind INTEGER NOT NULL,
                type_info TEXT,
                file_id INTEGER REFERENCES file(id) ON DELETE SET NULL,
                line INTEGER,
                col INTEGER,
                end_line INTEGER,
                end_col INTEGER,
                decl_file_id INTEGER REFERENCES file(id) ON DELETE SET NULL,
                decl_line INTEGER,
                decl_col INTEGER,
                decl_path TEXT,
                is_definition INTEGER NOT NULL DEFAULT 0,
                is_pure INTEGER NOT NULL DEFAULT 0,
                is_static INTEGER NOT NULL DEFAULT 0,
                is_instantiation INTEGER NOT NULL DEFAULT 0,
                is_named_instance INTEGER NOT NULL DEFAULT 0,
                linkage TEXT,
                access TEXT,
                parent_usr TEXT,
                resolved INTEGER NOT NULL DEFAULT 0,
                multi_def INTEGER NOT NULL DEFAULT 0,
                const_value TEXT,
                semantic_universe_id INTEGER NOT NULL DEFAULT 1
                    REFERENCES semantic_universe(id),
                identity_key TEXT NOT NULL DEFAULT ''
            );
            INSERT INTO symbol_v35 (
                id, usr, spelling, qual_name, display_name, kind, type_info,
                file_id, line, col, end_line, end_col, decl_file_id, decl_line,
                decl_col,
                decl_path, is_definition, is_pure, is_static, is_instantiation,
                is_named_instance, linkage, access, parent_usr, resolved,
                multi_def, const_value, semantic_universe_id, identity_key
            )
            SELECT id, usr, spelling, qual_name, display_name, kind, type_info,
                   file_id, line, col, end_line, end_col, decl_file_id, decl_line,
                   decl_col,
                   decl_path, is_definition, is_pure, is_static, is_instantiation,
                   is_named_instance, linkage, access, parent_usr, resolved,
                   multi_def, const_value, 1,
                   'legacy' || char(31) ||
                   CASE WHEN linkage IN ('internal', 'no-linkage')
                        THEN 'local:legacy' || char(31) || {legacy_source} || char(31)
                        ELSE '' END || usr
              FROM symbol;
            DROP TABLE symbol;
            ALTER TABLE symbol_v35 RENAME TO symbol;
        """)
        self._conn.commit()
        self._conn.execute("PRAGMA foreign_keys = ON")

    def _migrate_symbol_kind_to_int(self) -> None:
        """v15 -> v16: rebuild `symbol` with kind stored as its CXCursorKind int.

        SQLite cannot ALTER a column's type or drop the old `kind IN (...)` CHECK,
        so the table is recreated and the rows copied with the kind names mapped
        to integers. Foreign keys are disabled for the swap so dropping the old
        table does not cascade-delete edges (edge.src_id/dst_id keep the same ids
        the new rows carry). The schema script (run right after) recreates the
        symbol indexes via CREATE INDEX IF NOT EXISTS.
        """
        case = "CASE kind " + " ".join(
            f"WHEN {name!r} THEN {i}" for name, i in SYMBOL_KIND_IDS.items()
        ) + " ELSE kind END"
        self._conn.commit()  # close any open txn so the pragma below takes effect
        self._conn.execute("PRAGMA foreign_keys = OFF")
        self._conn.executescript(f"""
            CREATE TABLE symbol_new (
                id           INTEGER PRIMARY KEY,
                usr          TEXT NOT NULL UNIQUE,
                spelling     TEXT NOT NULL,
                qual_name    TEXT,
                display_name TEXT,
                kind         INTEGER NOT NULL,
                type_info    TEXT,
                file_id      INTEGER REFERENCES file(id) ON DELETE SET NULL,
                line         INTEGER,
                col          INTEGER,
                decl_file_id INTEGER REFERENCES file(id) ON DELETE SET NULL,
                decl_line    INTEGER,
                decl_col     INTEGER,
                decl_path    TEXT,
                is_definition INTEGER NOT NULL DEFAULT 0,
                is_pure      INTEGER NOT NULL DEFAULT 0,
                is_static    INTEGER NOT NULL DEFAULT 0,
                is_instantiation INTEGER NOT NULL DEFAULT 0,
                linkage      TEXT,
                access       TEXT,
                parent_usr   TEXT,
                resolved     INTEGER NOT NULL DEFAULT 0
            );
            INSERT INTO symbol_new
                SELECT id, usr, spelling, qual_name, display_name, {case},
                       type_info, file_id, line, col, decl_file_id, decl_line,
                       decl_col, decl_path, is_definition, is_pure, is_static,
                       is_instantiation, linkage, access, parent_usr, resolved
                FROM symbol;
            DROP TABLE symbol;
            ALTER TABLE symbol_new RENAME TO symbol;
        """)
        self._conn.commit()
        self._conn.execute("PRAGMA foreign_keys = ON")

    def _migrate_component_repo_unique(self) -> None:
        """v23 -> v24: rebuild `component` so `path` is UNIQUE per repository
        (was globally UNIQUE). A grouped component stores a clone-RELATIVE path,
        so several repositories can each carry a '.' root -- the old global
        UNIQUE(path) would reject that. Foreign keys are disabled for the swap so
        dropping the table does not cascade-delete directories (they keep the ids
        the copied rows carry). The schema script (run right after) is a no-op
        (CREATE TABLE IF NOT EXISTS). Mirrors _migrate_symbol_kind_to_int.
        """
        self._conn.commit()  # close any open txn so the pragma takes effect
        self._conn.execute("PRAGMA foreign_keys = OFF")
        self._conn.executescript("""
            CREATE TABLE component_new (
                id      INTEGER PRIMARY KEY,
                name    TEXT NOT NULL,
                path    TEXT NOT NULL,
                kind    TEXT NOT NULL DEFAULT 'repo'
                        CHECK (kind IN ('repo', 'external')),
                version TEXT,
                repository_id INTEGER
                        REFERENCES repository(id) ON DELETE SET NULL,
                UNIQUE (repository_id, path)
            );
            INSERT INTO component_new
                SELECT id, name, path, kind, version, repository_id FROM component;
            DROP TABLE component;
            ALTER TABLE component_new RENAME TO component;
        """)
        self._conn.commit()
        self._conn.execute("PRAGMA foreign_keys = ON")

    # -- lifecycle -----------------------------------------------------------

    def close(self) -> None:
        self._conn.close()

    def __enter__(self) -> "Storage":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def transaction(self):
        """Context manager batching many mutations into one commit."""
        return _Transaction(self)

    def current_artifact(self, logical_id: str) -> Optional[dict[str, Any]]:
        """Return the current manifest row without opening the sidecar.

        The returned identity is the portable manifest identity; callers must
        not treat any sidecar-local integer as a core symbol identity.
        """
        row = self._conn.execute(
            "SELECT * FROM artifact WHERE logical_id = ? AND state = 'current' "
            "ORDER BY id DESC LIMIT 1",
            (logical_id,),
        ).fetchone()
        if row is None:
            return None
        result = dict(row)
        result["exposed_relations"] = [
            relation[0]
            for relation in self._conn.execute(
                "SELECT relation_name FROM artifact_relation WHERE artifact_id = ? "
                "ORDER BY relation_name",
                (row["id"],),
            )
        ]
        return result

    def artifact_identity_mappings(self, logical_id: str) -> list[dict[str, Any]]:
        """Read stable cross-file mappings, including unresolved mappings."""
        return [
            dict(row)
            for row in self._conn.execute(
                "SELECT m.* FROM artifact_identity_map AS m "
                "JOIN artifact AS a ON a.id = m.artifact_id "
                "WHERE a.logical_id = ? AND a.state = 'current' "
                "ORDER BY m.stable_identity, m.local_identity",
                (logical_id,),
            )
        ]

    def _commit(self) -> None:
        if not self._in_txn:
            self._conn.commit()

    # -- semantic universes / symbol identity (v35) -------------------------

    def add_semantic_universe(
        self, key: str, name: Optional[str] = None, policy: str = "explicit"
    ) -> int:
        """Create or refresh an explicit program/dependency universe."""
        if not key:
            raise ValueError("semantic universe key must not be empty")
        cur = self._conn.execute(
            "INSERT INTO semantic_universe (key, name, policy) VALUES (?, ?, ?) "
            "ON CONFLICT(key) DO UPDATE SET name = excluded.name, "
            "policy = excluded.policy RETURNING id",
            (key, name or key, policy),
        )
        uid = cur.fetchone()["id"]
        self._commit()
        return uid

    def get_semantic_universe_by_id(self, universe_id: int) -> Optional[SemanticUniverse]:
        row = self._conn.execute(
            "SELECT * FROM semantic_universe WHERE id = ?", (universe_id,)
        ).fetchone()
        return _row_to(SemanticUniverse, row)

    def get_semantic_universe_by_key(self, key: str) -> Optional[SemanticUniverse]:
        row = self._conn.execute(
            "SELECT * FROM semantic_universe WHERE key = ?", (key,)
        ).fetchone()
        return _row_to(SemanticUniverse, row)

    def list_semantic_universes(self) -> list[SemanticUniverse]:
        return [
            _row_to(SemanticUniverse, row)
            for row in self._conn.execute(
                "SELECT * FROM semantic_universe ORDER BY key"
            )
        ]

    def set_repository_semantic_universe(
        self, repository_id: int, universe_id: Optional[int]
    ) -> None:
        self._conn.execute(
            "UPDATE repository SET semantic_universe_id = COALESCE(?, 1) "
            "WHERE id = ?",
            (universe_id, repository_id),
        )
        self._commit()

    def set_component_semantic_universe(
        self, component_id: int, universe_id: Optional[int]
    ) -> None:
        self._conn.execute(
            "UPDATE component SET semantic_universe_id = ? WHERE id = ?",
            (universe_id, component_id),
        )
        self._commit()

    def _default_semantic_universe_id(self) -> int:
        row = self._conn.execute(
            "SELECT id FROM semantic_universe WHERE key = 'legacy'"
        ).fetchone()
        return row["id"] if row else self.add_semantic_universe(
            "legacy", "Legacy single-workspace universe", "legacy"
        )

    def _semantic_universe_for_file(self, file_id: Optional[int]) -> int:
        if file_id is None:
            return self._default_semantic_universe_id()
        row = self._conn.execute(
            "SELECT COALESCE(c.semantic_universe_id, r.semantic_universe_id, ?) "
            "AS universe_id FROM file f "
            "JOIN directory d ON d.id = f.directory_id "
            "JOIN component c ON c.id = d.component_id "
            "LEFT JOIN repository r ON r.id = c.repository_id "
            "WHERE f.id = ?",
            (self._default_semantic_universe_id(), file_id),
        ).fetchone()
        return row["universe_id"] if row else self._default_semantic_universe_id()

    def semantic_universe_for_file_id(self, file_id: int) -> int:
        return self._semantic_universe_for_file(file_id)

    def portable_source_identity_for_path(self, path: str) -> str:
        abs_path = os.path.abspath(path)
        comp = self.component_for_path(abs_path)
        if comp is None:
            return f"path:{abs_path}"
        owner = ""
        if comp.repository_id is not None:
            repo = self.get_repository_by_id(comp.repository_id)
            if repo is not None and repo.remote_url:
                owner = f"remote:{repo.remote_url}"
            elif repo is not None:
                owner = f"repo:{repo.name}"
        if not owner:
            owner = f"component:{self.effective_root(comp)}"
        rel = os.path.relpath(abs_path, self.component_abs_base(comp))
        return f"{owner}\x1f{self.effective_root(comp)}\x1f{rel}"

    def portable_source_identity_for_file(self, file_id: int) -> str:
        path = self.file_abs_path(file_id)
        return self.portable_source_identity_for_path(path) if path else (
            f"file-id-missing:{file_id}"
        )

    def portable_translation_unit_identity_for_config(
        self, config_id: int, translation_unit_file_id: Optional[int] = None
    ) -> str:
        row = self._conn.execute(
            "SELECT descriptor_hash FROM translation_unit_config WHERE id = ?",
            (config_id,),
        ).fetchone()
        identity = (
            f"config:{row['descriptor_hash']}"
            if row is not None
            else f"config-id-missing:{config_id}"
        )
        if translation_unit_file_id is not None:
            identity += "\x1fsource:" + self.portable_source_identity_for_file(
                translation_unit_file_id
            )
        return identity

    def portable_translation_unit_identity_for_file(self, file_id: int) -> str:
        configs = self.translation_unit_configs_for_file(file_id)
        if len(configs) == 1:
            return self.portable_translation_unit_identity_for_config(
                configs[0].id or -1, file_id
            )
        if configs:
            return (
                "source:" + self.portable_source_identity_for_file(file_id)
                + "\x1fconfigs:"
                + ",".join(c.descriptor_hash for c in configs)
            )
        file = self.get_file_by_id(file_id)
        if file is None:
            return f"config-id-missing:{file_id}"
        config = resolve_translation_unit_config(
            file.compile_options or [],
            driver=file.driver,
            working_dir=".",
        )
        return (
            "config:"
            + translation_unit_config_hash(config)
            + "\x1fsource:"
            + self.portable_source_identity_for_file(file_id)
        )

    def _symbol_identity_key(
        self,
        sym: Symbol,
        universe_id: int,
        file_id: Optional[int],
        translation_unit: Optional[str] = None,
    ) -> str:
        universe = self.get_semantic_universe_by_id(universe_id)
        key = universe.key if universe else "legacy"
        local = sym.linkage in {"internal", "no-linkage"}
        prefix = f"{key}\x1f"
        if local:
            source = sym.identity_source
            if source:
                source_key = self.portable_source_identity_for_path(source)
            elif file_id is not None:
                source_key = self.portable_source_identity_for_file(file_id)
            else:
                source_key = "unknown"
            tu_key = translation_unit or sym.identity_translation_unit
            if not tu_key and file_id is not None:
                tu_key = self.portable_translation_unit_identity_for_file(file_id)
            prefix += f"local:{tu_key or 'unknown'}\x1f{source_key}\x1f"
        return prefix + sym.usr

    # -- components ----------------------------------------------------------

    def add_component(
        self, name: str, path: str, kind: str = "repo", version: Optional[str] = None
    ) -> int:
        """Insert a component; idempotent on path. Returns the component id.

        Idempotent on the exact stored path string: an existing row with that
        path has its name and kind updated (version only when a non-None value
        is supplied -- COALESCE). v24: `path` is no longer globally UNIQUE (it
        is UNIQUE per repository, so grouped components can share a '.' root), so
        the dedup is done here in code rather than via ON CONFLICT(path). This is
        a low-level primitive -- callers pass the ABSOLUTE base before grouping;
        re-resolving an already-relativized component is the caller's job
        (get_component / component_for_path are clone-aware).
        """
        # Preserve indirected (portable) paths verbatim; absolutize plain paths.
        if "$" not in path and "<" not in path:
            path = os.path.abspath(path)
        row = self._conn.execute(
            "SELECT id FROM component WHERE path = ?", (path,)
        ).fetchone()
        if row is not None:
            cid = row["id"]
            self._conn.execute(
                "UPDATE component SET name = ?, kind = ?, "
                "version = COALESCE(?, version) WHERE id = ?",
                (name, kind, version, cid),
            )
        else:
            cur = self._conn.execute(
                "INSERT INTO component (name, path, kind, version) "
                "VALUES (?, ?, ?, ?) RETURNING id",
                (name, path, kind, version),
            )
            cid = cur.fetchone()["id"]
        self._commit()
        return cid

    def update_component_meta(
        self, component_id: int, name: str, kind: str,
        version: Optional[str] = None,
    ) -> None:
        """Update a component's name/kind in place (version only when a non-None
        value is supplied -- COALESCE). Used by add-source/import to refresh an
        EXISTING (already-grouped, possibly clone-relative) component without
        touching its stored path. Mirrors the add_component upsert metadata."""
        self._conn.execute(
            "UPDATE component SET name = ?, kind = ?, "
            "version = COALESCE(?, version) WHERE id = ?",
            (name, kind, version, component_id),
        )
        self._commit()

    def update_component(
        self,
        component_id: int,
        name: str,
        path: str,
        kind: str,
        version: Optional[str],
        repository_id: Optional[int],
        semantic_universe_id: Optional[int] = None,
    ) -> None:
        """Persist every mutable column of a component row in place. Backs
        Component.save() -- writes the object's current field values wholesale
        (this DOES clear a field set to None, unlike the COALESCE upserts)."""
        self._conn.execute(
            "UPDATE component SET name = ?, path = ?, kind = ?, version = ?, "
            "repository_id = ?, semantic_universe_id = ? WHERE id = ?",
            (name, path, kind, version, repository_id, semantic_universe_id,
             component_id),
        )
        self._commit()

    def set_component_version(self, name: str, version: Optional[str]) -> bool:
        """Set (or clear when version=None) a component's version by name.

        Returns False when no component with that name exists.
        """
        cur = self._conn.execute(
            "UPDATE component SET version = ? WHERE name = ?", (version, name)
        )
        self._commit()
        return cur.rowcount > 0

    def set_component_effective_version(self, name: str, version: str) -> bool:
        """Set a component's EFFECTIVE version regardless of how the existing
        version is represented, non-destructively.

        Two representations exist (see component_alias_index):
          - version-as-property: the `version` column carries it, `path` has no
            trailing version segment  -> just UPDATE the column.
          - version-in-path: the version is the trailing segment of `path`
            (no `version` column)     -> rewrite that trailing segment in place
            and leave `version` NULL.

        Only applied when the name resolves to exactly ONE component row;
        multi-row names (duplicate/ambiguous) are left untouched and False is
        returned. The stored path is split (not the resolved one) so portable
        `<label>` / `$VAR` prefixes survive the rewrite.
        """
        rows = [c for c in self.list_components() if c.name == name and c.id is not None]
        if len(rows) != 1:
            return False
        comp = rows[0]
        base, seg = _pathx.split_base_version(comp.path)
        if seg is not None:
            # version embedded in the path: swap the trailing segment.
            new_path = os.path.normpath(os.path.join(base, version))
            if "$" not in new_path and "<" not in new_path:
                new_path = os.path.abspath(new_path)
            self._conn.execute(
                "UPDATE component SET path = ?, version = NULL WHERE id = ?",
                (new_path, comp.id),
            )
        else:
            self._conn.execute(
                "UPDATE component SET version = ? WHERE id = ?", (version, comp.id)
            )
        self._commit()
        return True

    @staticmethod
    def effective_root(comp: "Component") -> str:
        """Stored effective root (NOT resolved): version joined onto path.

        Returns normpath(join(path, version)) when versioned, else path.
        """
        if comp.version:
            return os.path.normpath(os.path.join(comp.path, comp.version))
        return comp.path

    def _active_clone_root(self, repository_id: Optional[int]) -> Optional[str]:
        """Resolved absolute path of a repository's active clone, or None when
        the component is ungrouped / the repository has no live clone."""
        if repository_id is None:
            return None
        repo = self.get_repository_by_id(repository_id)
        if repo is None or repo.active_clone_id is None:
            return None
        clone = self.get_clone_by_id(repo.active_clone_id)
        if clone is None:
            return None
        return os.path.abspath(_pathx.resolve_fs_path(clone.path))

    def component_abs_base(self, comp: "Component") -> str:
        """Absolute base directory of a component's tree (the effective root).

        v24: a component grouped under a repository stores its `path` RELATIVE to
        that repository's active clone root; this anchors the (relative)
        effective root under the resolved clone path. An ungrouped component
        (repository_id NULL), an absolute path, or a portable ``<label>``/``$VAR``
        path resolves exactly as before -- clone-agnostic. This is the single
        choke point every path-reconstruction site routes through."""
        eff = Storage.effective_root(comp)
        if (
            comp.repository_id is not None
            and not os.path.isabs(comp.path)
            and "<" not in comp.path
            and "$" not in comp.path
        ):
            root = self._active_clone_root(comp.repository_id)
            if root is not None:
                return os.path.abspath(os.path.join(root, eff))
        return os.path.abspath(_pathx.resolve_fs_path(eff))

    def relativize_component(self, component_id: int, clone_root: str) -> None:
        """Rewrite a grouped component's stored `path` to be RELATIVE to its
        repository's active clone root (so `repo switch` repoints one pointer
        instead of rewriting N rows). The component's CURRENT absolute base is
        rebased onto clone_root: ``.`` when it IS the clone root, else the
        relative remainder. A portable ``<label>``/``$VAR`` path, an
        already-relative path, or a base that does not sit under clone_root is
        left untouched (stays absolute / portable)."""
        comp = self.get_component_by_id(component_id)
        if comp is None:
            return
        if "<" in comp.path or "$" in comp.path or not os.path.isabs(comp.path):
            return
        if _pathx.split_base_version(comp.path)[1] is not None:
            # version-in-path representation: the version segment is part of the
            # absolute path identity (set_component_effective_version rewrites
            # it in place), so keep it absolute -- relativizing would drop it.
            return
        root = os.path.abspath(_pathx.resolve_fs_path(clone_root)).rstrip(os.sep)
        base = os.path.abspath(comp.path).rstrip(os.sep)
        if base == root:
            rel = "."
        elif base.startswith(root + os.sep):
            rel = os.path.relpath(base, root)
        else:
            return  # component lives outside this clone -> keep it absolute
        self._conn.execute(
            "UPDATE component SET path = ? WHERE id = ?", (rel, component_id)
        )
        self._commit()

    # -- labels (v14) --------------------------------------------------------

    def add_label(self, name: str, path: str) -> int:
        """Upsert a label by name. Returns the label id."""
        cur = self._conn.execute(
            "INSERT INTO label (name, path) VALUES (?, ?) "
            "ON CONFLICT(name) DO UPDATE SET path = excluded.path "
            "RETURNING id",
            (name, path),
        )
        lid = cur.fetchone()["id"]
        self._commit()
        return lid

    def remove_label(self, name: str) -> bool:
        """Delete a label by name. Returns False if it did not exist."""
        cur = self._conn.execute("DELETE FROM label WHERE name = ?", (name,))
        self._commit()
        return cur.rowcount > 0

    def get_label(self, name: str) -> Optional[str]:
        """Return the stored path for a label, or None if absent."""
        row = self._conn.execute(
            "SELECT path FROM label WHERE name = ?", (name,)
        ).fetchone()
        return row["path"] if row else None

    def list_labels(self) -> list[tuple[str, str]]:
        """All labels sorted by name; returns (name, stored_path) pairs."""
        return [
            (r["name"], r["path"])
            for r in self._conn.execute("SELECT name, path FROM label ORDER BY name")
        ]

    def component_alias_index(
        self,
    ) -> dict[str, tuple[str, Optional[str], bool]]:
        """Version-agnostic component alias map: name -> (base, max_version,
        bumpable).

        For each component the resolved effective root is split into
        (base, version) by trailing-segment detection, so matching is done on
        the VERSION-STRIPPED base. Rows are grouped by name; a name is included
        only when all its rows share ONE base (conflicting bases = ambiguous,
        skipped). `max_version` is the numeric-max trailing version across the
        rows (None if none). `bumpable` is True only when the name has exactly
        one row whose stored `path` carries no embedded version segment (pure
        version-as-property) — so `set_component_version` is safe and
        non-destructive; otherwise version-bump-on-import is skipped for it.
        """
        by_name: dict[str, list[tuple[str, Optional[str], bool]]] = {}
        for c in self.list_components():
            eff = self.component_abs_base(c)
            base, ver = _pathx.split_base_version(eff)
            _, path_ver = _pathx.split_base_version(
                self.component_abs_base(replace(c, version=None))
            )
            by_name.setdefault(c.name, []).append((base, ver, path_ver is None))
        out: dict[str, tuple[str, Optional[str], bool]] = {}
        for name, rows in by_name.items():
            bases = {b for b, _v, _p in rows}
            if len(bases) != 1:
                continue  # ambiguous: same name, different base dirs
            base = next(iter(bases))
            vers = [v for _b, v, _p in rows if v]
            maxver = max(vers, key=_pathx.version_key) if vers else None
            bumpable = len(rows) == 1 and rows[0][2]
            out[name] = (base, maxver, bumpable)
        return out

    def list_alias_pairs(self) -> list[tuple[str, str, bool]]:
        """Encode registry for include-path aliasing as (name, match_path,
        versioned) triples:

          - explicit labels -> (name, stored_path, False): exact match, no
            version handling.
          - components -> (name, version-stripped base, True): version-agnostic
            match; the version segment after the base is dropped at encode and
            re-injected at decode (`get_alias` -> base + highest version).

        Labels win on a name collision; component names with conflicting bases
        are skipped (ambiguous). Sorted by name; `build_label_map` re-sorts by
        resolved length for longest-match. Decode mirror = `get_alias`.
        """
        triples: list[tuple[str, str, bool]] = []
        label_names: set[str] = set()
        for name, stored in self.list_labels():
            triples.append((name, stored, False))
            label_names.add(name)
        for name, (base, _ver, _bump) in self.component_alias_index().items():
            if name not in label_names:
                triples.append((name, base, True))
        triples.sort(key=lambda t: t[0])
        return triples

    def get_alias(self, name: str) -> Optional[str]:
        """Decode an alias name: explicit label -> stored path; else a
        uniquely-based component -> its base joined with the highest known
        version (= effective root at the max version). None if neither applies
        (or an ambiguous duplicate-based component name). Mirror of the
        `list_alias_pairs` encode registry."""
        path = self.get_label(name)
        if path is not None:
            return path
        entry = self.component_alias_index().get(name)
        if entry is None:
            return None
        base, maxver, _bump = entry
        return os.path.join(base, maxver) if maxver else base

    def get_component_by_name(self, name: str) -> Optional[Component]:
        row = self._conn.execute(
            "SELECT * FROM component WHERE name = ?", (name,)
        ).fetchone()
        return self._bind_component(_row_to(Component, row))

    def get_component(self, path: str) -> Optional[Component]:
        """Look up a component by root path.

        Two-step lookup for version-split safety (§4.4):
          1. Exact match on the stored BASE path.
          2. If that misses, match where effective_root(comp) == abspath(path).
        """
        abs_path = os.path.abspath(path)
        row = self._conn.execute(
            "SELECT * FROM component WHERE path = ?", (abs_path,)
        ).fetchone()
        if row is not None:
            return self._bind_component(_row_to(Component, row))
        # Fallback: match effective root (handles the version-split case where the
        # user registered /src/v8 but the stored base is /src with version=v8).
        for row in self._conn.execute("SELECT * FROM component"):
            comp = _row_to(Component, row)
            eff = self.component_abs_base(comp)
            if eff == abs_path:
                return self._bind_component(comp)
        return None

    def get_component_by_id(self, component_id: int) -> Optional[Component]:
        row = self._conn.execute(
            "SELECT * FROM component WHERE id = ?", (component_id,)
        ).fetchone()
        return self._bind_component(_row_to(Component, row))

    def component_for_path(self, abs_path: str) -> Optional[Component]:
        """Longest-prefix match: which component owns this absolute path?

        Uses the effective root (base+version, resolved) for prefix matching,
        so a versioned component stored as (path=/opt/libfoo, version=1.2.3)
        correctly claims /opt/libfoo/1.2.3/include/... .
        """
        abs_path = os.path.abspath(abs_path)
        best: Optional[Component] = None
        best_root_len = -1
        for row in self._conn.execute("SELECT * FROM component"):
            comp = _row_to(Component, row)
            root = self.component_abs_base(comp).rstrip(os.sep)
            if abs_path == root or abs_path.startswith(root + os.sep):
                if len(root) > best_root_len:
                    best = comp
                    best_root_len = len(root)
        return self._bind_component(best)

    def delete_component(self, component_id: int) -> None:
        """Remove a component and everything derived from it.

        Directories and files vanish via ON DELETE CASCADE; symbols reference
        files with ON DELETE SET NULL, so symbols indexed from this component's
        files are deleted explicitly first -- otherwise they would linger as
        file-less orphans. Used by `import --force` to rebuild from scratch."""
        sub = (
            "SELECT f.id FROM file f "
            "JOIN directory d ON f.directory_id = d.id "
            "WHERE d.component_id = ?"
        )
        self._conn.execute(
            f"DELETE FROM symbol WHERE file_id IN ({sub}) OR decl_file_id IN ({sub})",
            (component_id, component_id),
        )
        self._conn.execute("DELETE FROM component WHERE id = ?", (component_id,))
        self._commit()

    def delete_directory(self, directory_id: int) -> None:
        """Remove a directory, its files (ON DELETE CASCADE), and the symbols
        indexed from those files (file_id/decl_file_id are ON DELETE SET NULL,
        so they are deleted explicitly to avoid file-less orphans)."""
        sub = "SELECT id FROM file WHERE directory_id = ?"
        self._conn.execute(
            f"DELETE FROM symbol WHERE file_id IN ({sub}) OR decl_file_id IN ({sub})",
            (directory_id, directory_id),
        )
        self._conn.execute("DELETE FROM directory WHERE id = ?", (directory_id,))
        self._commit()

    def delete_file(self, file_id: int) -> None:
        """Remove a file and the symbols indexed from it (referenced by
        file_id/decl_file_id with ON DELETE SET NULL, so deleted explicitly to
        avoid file-less orphans)."""
        self._conn.execute(
            "DELETE FROM symbol WHERE file_id = ? OR decl_file_id = ?",
            (file_id, file_id),
        )
        self._conn.execute("DELETE FROM file WHERE id = ?", (file_id,))
        self._commit()

    def delete_symbol(self, symbol_id: int) -> None:
        """Remove a single symbol row."""
        self._conn.execute("DELETE FROM symbol WHERE id = ?", (symbol_id,))
        self._commit()

    @staticmethod
    def _fuzzy_like(text: str) -> str:
        """LIKE pattern for fzf-style fuzzy matching (use with ESCAPE '\\').

        Every non-space character of `text` must appear in the column, in
        order: 'shp' matches 'shapes.c'. LIKE is case-insensitive for ASCII.
        """
        chars = [
            c.replace("\\", "\\\\").replace("%", r"\%").replace("_", r"\_")
            for c in text
            if not c.isspace()
        ]
        return "%" + "%".join(chars) + "%"

    def list_components(
        self, name: Optional[str] = None, kind: Optional[str] = None
    ) -> list[Component]:
        """All components, optionally fuzzy-filtered by name and/or kind."""
        sql = "SELECT * FROM component"
        where, args = [], []
        if name:
            where.append(r"name LIKE ? ESCAPE '\'")
            args.append(self._fuzzy_like(name))
        if kind is not None:
            where.append("kind = ?")
            args.append(kind)
        if where:
            sql += " WHERE " + " AND ".join(where)
        sql += " ORDER BY name, path"
        return [self._bound_component(r) for r in self._conn.execute(sql, args)]

    def _bound_component(self, row: sqlite3.Row) -> Component:
        """Hydrate a non-NULL component row and bind it (list-accessor helper)."""
        c = _row_to(Component, row)
        c._storage = self
        return c

    def _bound_repository(self, row: sqlite3.Row) -> "Repository":
        r = _row_to(Repository, row)
        r._storage = self
        return r

    def _bound_directory(self, row: sqlite3.Row) -> Directory:
        d = _row_to(Directory, row)
        d._storage = self
        return d

    def set_component_repository(
        self, component_id: int, repository_id: Optional[int]
    ) -> None:
        """Attach (or, with None, detach) a component to a repository."""
        self._conn.execute(
            "UPDATE component SET repository_id = ? WHERE id = ?",
            (repository_id, component_id),
        )
        self._commit()

    def components_for_repository(
        self, repository_id: int, name: Optional[str] = None
    ) -> list[Component]:
        """All components grouped under a repository, ordered by name, path,
        optionally fuzzy-filtered by component name. Bound to this Storage."""
        sql = "SELECT * FROM component WHERE repository_id = ?"
        args: list = [repository_id]
        if name:
            sql += r" AND name LIKE ? ESCAPE '\'"
            args.append(self._fuzzy_like(name))
        sql += " ORDER BY name, path"
        return [self._bound_component(r) for r in self._conn.execute(sql, args)]

    # -- repositories / clones (v23) -----------------------------------------

    def add_repository(
        self, name: str, kind: str = "repo", remote_url: Optional[str] = None,
        semantic_universe_id: Optional[int] = None,
    ) -> int:
        """Insert a repository; idempotent on name. Returns the repository id.

        On conflict (same name) updates kind, and updates remote_url only when a
        non-None value is supplied (COALESCE: a re-import that cannot determine a
        remote does NOT wipe a stored one)."""
        if semantic_universe_id is None:
            cur = self._conn.execute(
                "INSERT INTO repository (name, kind, remote_url) "
                "VALUES (?, ?, ?) ON CONFLICT(name) DO UPDATE SET "
                "kind = excluded.kind, "
                "remote_url = COALESCE(excluded.remote_url, repository.remote_url) "
                "RETURNING id",
                (name, kind, remote_url),
            )
        else:
            cur = self._conn.execute(
                "INSERT INTO repository (name, kind, remote_url, "
                "semantic_universe_id) VALUES (?, ?, ?, ?) "
                "ON CONFLICT(name) DO UPDATE SET "
                "kind = excluded.kind, "
                "remote_url = COALESCE(excluded.remote_url, repository.remote_url), "
                "semantic_universe_id = excluded.semantic_universe_id "
                "RETURNING id",
                (name, kind, remote_url, semantic_universe_id),
            )
        rid = cur.fetchone()["id"]
        self._commit()
        return rid

    def get_repository_by_name(self, name: str) -> Optional["Repository"]:
        row = self._conn.execute(
            "SELECT * FROM repository WHERE name = ?", (name,)
        ).fetchone()
        return self._bind_repository(_row_to(Repository, row))

    def get_repository_by_id(self, repository_id: int) -> Optional["Repository"]:
        row = self._conn.execute(
            "SELECT * FROM repository WHERE id = ?", (repository_id,)
        ).fetchone()
        return self._bind_repository(_row_to(Repository, row))

    def get_repository_by_remote(self, remote_url: str) -> Optional["Repository"]:
        """First repository whose remote_url matches (clone-identity lookup)."""
        row = self._conn.execute(
            "SELECT * FROM repository WHERE remote_url = ? ORDER BY id LIMIT 1",
            (remote_url,),
        ).fetchone()
        return self._bind_repository(_row_to(Repository, row))

    def list_repositories(
        self, name: Optional[str] = None, kind: Optional[str] = None
    ) -> list["Repository"]:
        """All repositories, optionally fuzzy-filtered by name and/or kind."""
        sql = "SELECT * FROM repository"
        where, args = [], []
        if name:
            where.append(r"name LIKE ? ESCAPE '\'")
            args.append(self._fuzzy_like(name))
        if kind is not None:
            where.append("kind = ?")
            args.append(kind)
        if where:
            sql += " WHERE " + " AND ".join(where)
        sql += " ORDER BY name"
        return [self._bound_repository(r) for r in self._conn.execute(sql, args)]

    def set_active_clone(
        self, repository_id: int, clone_id: Optional[int]
    ) -> None:
        """Point a repository at its live clone (or None to clear)."""
        self._conn.execute(
            "UPDATE repository SET active_clone_id = ? WHERE id = ?",
            (clone_id, repository_id),
        )
        self._commit()

    def update_repository(
        self,
        repository_id: int,
        name: str,
        kind: str,
        remote_url: Optional[str],
        active_clone_id: Optional[int],
        semantic_universe_id: Optional[int] = None,
    ) -> None:
        """Persist every mutable column of a repository row in place. Backs
        Repository.save() -- writes the object's current field values wholesale
        (unlike add_repository's COALESCE upsert, this DOES clear a field set to
        None, so it is a true 'save what I have')."""
        self._conn.execute(
            "UPDATE repository SET name = ?, kind = ?, remote_url = ?, "
            "active_clone_id = ?, semantic_universe_id = COALESCE(?, 1) "
            "WHERE id = ?",
            (name, kind, remote_url, active_clone_id, semantic_universe_id,
             repository_id),
        )
        self._commit()

    def delete_repository(self, repository_id: int) -> None:
        """Remove a repository. Clones cascade (ON DELETE CASCADE); components
        are detached (repository_id ON DELETE SET NULL) but otherwise untouched
        -- their symbols survive. Use delete_component to discard those."""
        self._conn.execute(
            "DELETE FROM repository WHERE id = ?", (repository_id,)
        )
        self._commit()

    def add_clone(
        self, repository_id: int, path: str, label: Optional[str] = None
    ) -> int:
        """Register a checkout/worktree directory for a repository; idempotent
        on path. Plain (non-portable) paths are absolutized. Returns clone id."""
        if "$" not in path and "<" not in path:
            path = os.path.abspath(path)
        cur = self._conn.execute(
            "INSERT INTO clone (repository_id, path, label) VALUES (?, ?, ?) "
            "ON CONFLICT(path) DO UPDATE SET "
            "  repository_id = excluded.repository_id, "
            "  label         = COALESCE(excluded.label, clone.label) "
            "RETURNING id",
            (repository_id, path, label),
        )
        cid = cur.fetchone()["id"]
        self._commit()
        return cid

    def get_clone_by_id(self, clone_id: int) -> Optional["Clone"]:
        row = self._conn.execute(
            "SELECT * FROM clone WHERE id = ?", (clone_id,)
        ).fetchone()
        return _row_to(Clone, row)

    def get_clone_by_path(self, path: str) -> Optional["Clone"]:
        if "$" not in path and "<" not in path:
            path = os.path.abspath(path)
        row = self._conn.execute(
            "SELECT * FROM clone WHERE path = ?", (path,)
        ).fetchone()
        return _row_to(Clone, row)

    def list_clones(
        self, repository_id: Optional[int] = None
    ) -> list["Clone"]:
        """Clones, optionally scoped to one repository, ordered by id."""
        if repository_id is None:
            rows = self._conn.execute("SELECT * FROM clone ORDER BY id")
        else:
            rows = self._conn.execute(
                "SELECT * FROM clone WHERE repository_id = ? ORDER BY id",
                (repository_id,),
            )
        return [_row_to(Clone, r) for r in rows]

    def delete_clone(self, clone_id: int) -> None:
        """Remove a clone. If it was the repository's active clone, the active
        pointer is cleared first (it is a plain int, no FK to drive it)."""
        self._conn.execute(
            "UPDATE repository SET active_clone_id = NULL "
            "WHERE active_clone_id = ?",
            (clone_id,),
        )
        self._conn.execute("DELETE FROM clone WHERE id = ?", (clone_id,))
        self._commit()

    # -- directories ---------------------------------------------------------

    def add_directory(self, component_id: int, path: str) -> int:
        """Insert a directory (path relative to its component); idempotent."""
        path = os.path.normpath(path) if path else "."
        if path == ".":
            path = ""
        cur = self._conn.execute(
            "INSERT INTO directory (component_id, path) VALUES (?, ?) "
            "ON CONFLICT(component_id, path) DO UPDATE SET path = excluded.path "
            "RETURNING id",
            (component_id, path),
        )
        did = cur.fetchone()["id"]
        self._commit()
        return did

    def get_directory(self, component_id: int, path: str) -> Optional[Directory]:
        row = self._conn.execute(
            "SELECT * FROM directory WHERE component_id = ? AND path = ?",
            (component_id, os.path.normpath(path) if path not in ("", ".") else ""),
        ).fetchone()
        return self._bind_directory(_row_to(Directory, row))

    def list_directories(
        self, component_id: Optional[int] = None, name: Optional[str] = None
    ) -> list[tuple[Directory, str]]:
        """(Directory, component name) pairs, optionally scoped to one
        component and/or fuzzy-filtered on the relative directory path."""
        sql = (
            "SELECT d.*, c.name AS comp_name "
            "FROM directory d JOIN component c ON c.id = d.component_id"
        )
        where, args = [], []
        if component_id is not None:
            where.append("d.component_id = ?")
            args.append(component_id)
        if name:
            where.append(r"d.path LIKE ? ESCAPE '\'")
            args.append(self._fuzzy_like(name))
        if where:
            sql += " WHERE " + " AND ".join(where)
        sql += " ORDER BY c.name, d.path"
        return [
            (self._bound_directory(r), r["comp_name"])
            for r in self._conn.execute(sql, args)
        ]

    def get_directory_by_id(self, directory_id: int) -> Optional[Directory]:
        row = self._conn.execute(
            "SELECT * FROM directory WHERE id = ?", (directory_id,)
        ).fetchone()
        return self._bind_directory(_row_to(Directory, row))

    @staticmethod
    def _dir_scope_sql(dir_path: str, args: list) -> str:
        """WHERE fragment matching a directory and its whole subtree."""
        rel = os.path.normpath(dir_path)
        if rel in (".", ""):
            rel = ""
        esc = rel.replace("\\", "\\\\").replace("%", r"\%").replace("_", r"\_")
        # '' is the component root: its subtree is every directory.
        args.extend([rel, esc + os.sep + "%" if rel else "%"])
        return r"(d.path = ? OR d.path LIKE ? ESCAPE '\')"

    # -- files ----------------------------------------------------------------

    def add_file(
        self,
        directory_id: int,
        name: str,
        mtime: Optional[float] = None,
        md5: Optional[str] = None,
        compile_options: Optional[list[str]] = None,
        driver: Optional[str] = None,
    ) -> int:
        """Insert a file row; idempotent on (directory, name). Returns file id.

        Re-adding with a *different* md5 resets the indexed flag (the content
        changed, so the stored symbols are stale).
        """
        opts = json.dumps(compile_options) if compile_options is not None else None
        previous = self._conn.execute(
            "SELECT compile_options, driver, args_overridden FROM file "
            "WHERE directory_id = ? AND name = ?",
            (directory_id, name),
        ).fetchone()
        config_changed = bool(
            previous is not None
            and not previous["args_overridden"]
            and (
                opts is not None
                and (previous["compile_options"] is None or previous["compile_options"] != opts)
                or driver is not None
                and (previous["driver"] is None or previous["driver"] != driver)
            )
        )
        cur = self._conn.execute(
            "INSERT INTO file (directory_id, name, mtime, md5, compile_options, driver) "
            "VALUES (?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(directory_id, name) DO UPDATE SET "
            "  mtime           = COALESCE(excluded.mtime, file.mtime), "
            "  compile_options = CASE WHEN file.args_overridden = 1 "
            "                         THEN file.compile_options "
            "                         ELSE COALESCE(excluded.compile_options, "
            "                                       file.compile_options) END, "
            "  driver          = CASE WHEN file.args_overridden = 1 "
            "                         THEN file.driver "
            "                         ELSE COALESCE(excluded.driver, "
            "                                       file.driver) END, "
            "  indexed         = CASE WHEN (excluded.md5 IS NOT NULL "
            "                          AND excluded.md5 IS NOT file.md5) "
            "                         OR (file.args_overridden = 0 AND "
            "                             ((excluded.compile_options IS NOT NULL "
            "                               AND excluded.compile_options IS NOT file.compile_options) "
            "                              OR (excluded.driver IS NOT NULL "
            "                               AND excluded.driver IS NOT file.driver))) "
            "                         THEN 0 ELSE file.indexed END, "
            "  md5             = COALESCE(excluded.md5, file.md5) "
            "RETURNING id",
            (directory_id, name, mtime, md5, opts, driver),
        )
        fid = cur.fetchone()["id"]
        if config_changed:
            self._conn.execute(
                "UPDATE file_config SET state = 'stale', "
                "reason = 'translation-unit configuration changed' "
                "WHERE file_id = ? AND role = 'translation_unit'",
                (fid,),
            )
            self._conn.execute(
                "UPDATE translation_unit SET state = 'stale' WHERE file_id = ?",
                (fid,),
            )
        self._commit()
        return fid

    def add_file_path(
        self,
        abs_path: str,
        mtime: Optional[float] = None,
        md5: Optional[str] = None,
        compile_options: Optional[list[str]] = None,
        driver: Optional[str] = None,
    ) -> int:
        """Convenience: register an absolute path, creating the directory row.

        The owning component must already exist (add_component first).
        """
        comp_id, rel_dir, name = self._split_path(abs_path)
        dir_id = self.add_directory(comp_id, rel_dir)
        return self.add_file(
            dir_id,
            name,
            mtime=mtime,
            md5=md5,
            compile_options=compile_options,
            driver=driver,
        )

    def _bind_component(self, c: Optional[Component]) -> Optional[Component]:
        """Bind a freshly-read Component to this Storage so its smart-path
        methods (abspath/repo/directories/files) work."""
        if c is not None:
            c._storage = self
        return c

    def _bind_repository(
        self, r: "Optional[Repository]"
    ) -> "Optional[Repository]":
        """Bind a freshly-read Repository to this Storage so its smart-path
        methods (path/components/files) work."""
        if r is not None:
            r._storage = self
        return r

    def _bind_directory(self, d: Optional[Directory]) -> Optional[Directory]:
        """Bind a freshly-read Directory to this Storage so its smart-path
        methods (name/abspath/files) work."""
        if d is not None:
            d._storage = self
        return d

    def files_in_directory(
        self, directory_id: int, name: Optional[str] = None
    ) -> list[File]:
        """Bound File rows held DIRECTLY in one directory (not its subtree),
        ordered by name and optionally fuzzy-filtered by file name. The
        Directory.files() smart-path backs onto this."""
        sql = (
            "SELECT f.*, c.path AS root, c.version AS comp_version, "
            "c.repository_id AS comp_repo_id, d.path AS rel "
            "FROM file f JOIN directory d ON d.id = f.directory_id "
            "JOIN component c ON c.id = d.component_id "
            "WHERE f.directory_id = ?"
        )
        args: list = [directory_id]
        if name:
            sql += r" AND f.name LIKE ? ESCAPE '\'"
            args.append(self._fuzzy_like(name))
        sql += " ORDER BY f.name"
        out = []
        for row in self._conn.execute(sql, args):
            comp_stub = Component(
                name="", path=row["root"], version=row["comp_version"],
                repository_id=row["comp_repo_id"],
            )
            eff = self.component_abs_base(comp_stub)
            abs_path = (
                os.path.join(eff, row["rel"], row["name"])
                if row["rel"]
                else os.path.join(eff, row["name"])
            )
            out.append(self._bind_file(_row_to(File, row), abs_path))
        return out

    def _bind_file(
        self, f: Optional[File], abspath: Optional[str] = None
    ) -> Optional[File]:
        """Bind a freshly-read File to this Storage so its smart-path methods
        (source/symbols/tu/walk/index/resolve/component/repo) work; optionally
        seed its memoized abspath when the caller already reconstructed it."""
        if f is not None:
            f._storage = self
            if abspath is not None:
                f._abspath_cache = abspath
        return f

    def get_file(self, abs_path: str) -> Optional[File]:
        """File row for an absolute path, or None."""
        try:
            comp_id, rel_dir, name = self._split_path(abs_path)
        except KeyError:
            return None
        row = self._conn.execute(
            "SELECT f.* FROM file f JOIN directory d ON d.id = f.directory_id "
            "WHERE d.component_id = ? AND d.path = ? AND f.name = ?",
            (comp_id, rel_dir, name),
        ).fetchone()
        return self._bind_file(_row_to(File, row), os.path.abspath(abs_path))

    def get_file_by_id(self, file_id: int) -> Optional[File]:
        row = self._conn.execute(
            "SELECT * FROM file WHERE id = ?", (file_id,)
        ).fetchone()
        return self._bind_file(_row_to(File, row))

    def files(self) -> list[tuple[File, str]]:
        """Every file row with its reconstructed absolute path, sorted by path."""
        rows = self._conn.execute(
            "SELECT f.*, c.path AS root, c.version AS comp_version, "
            "c.repository_id AS comp_repo_id, d.path AS rel "
            "FROM file f JOIN directory d ON d.id = f.directory_id "
            "JOIN component c ON c.id = d.component_id "
            "ORDER BY c.path, d.path, f.name"
        ).fetchall()
        out = []
        for row in rows:
            comp_stub = Component(
                name="", path=row["root"], version=row["comp_version"],
                repository_id=row["comp_repo_id"],
            )
            eff = self.component_abs_base(comp_stub)
            abs_path = (
                os.path.join(eff, row["rel"], row["name"])
                if row["rel"]
                else os.path.join(eff, row["name"])
            )
            out.append((self._bind_file(_row_to(File, row), abs_path), abs_path))
        return out

    def list_files(
        self,
        component_id: Optional[int] = None,
        dir_path: Optional[str] = None,
        name: Optional[str] = None,
        indexed: Optional[bool] = None,
    ) -> list[tuple[File, str]]:
        """Like files(), with optional filters: component, directory subtree,
        fuzzy file name, and indexed state."""
        sql = (
            "SELECT f.*, c.path AS root, c.version AS comp_version, "
            "c.repository_id AS comp_repo_id, d.path AS rel "
            "FROM file f JOIN directory d ON d.id = f.directory_id "
            "JOIN component c ON c.id = d.component_id"
        )
        where, args = [], []
        if component_id is not None:
            where.append("d.component_id = ?")
            args.append(component_id)
        if dir_path is not None:
            where.append(self._dir_scope_sql(dir_path, args))
        if name:
            where.append(r"f.name LIKE ? ESCAPE '\'")
            args.append(self._fuzzy_like(name))
        if indexed is not None:
            where.append("f.indexed = ?")
            args.append(int(indexed))
        if where:
            sql += " WHERE " + " AND ".join(where)
        sql += " ORDER BY c.path, d.path, f.name"
        out = []
        for row in self._conn.execute(sql, args):
            comp_stub = Component(
                name="", path=row["root"], version=row["comp_version"],
                repository_id=row["comp_repo_id"],
            )
            eff = self.component_abs_base(comp_stub)
            abs_path = (
                os.path.join(eff, row["rel"], row["name"])
                if row["rel"]
                else os.path.join(eff, row["name"])
            )
            out.append((self._bind_file(_row_to(File, row), abs_path), abs_path))
        return out

    def mark_file_indexed(self, file_id: int, mtime: Optional[float] = None) -> None:
        self._conn.execute(
            "UPDATE file SET indexed = 1, indexed_at = datetime('now'), "
            "  mtime = COALESCE(?, mtime) WHERE id = ?",
            (mtime, file_id),
        )
        self._commit()

    def update_file(self, f: "File") -> None:
        """Persist every mutable column of a file row from the object in place.
        Backs File.save() -- writes the File's current field values wholesale
        (directory_id/name/mtime/md5/compile_options/driver/indexed/indexed_at/
        args_overridden). compile_options is re-serialised to JSON."""
        if f.id is None:
            raise RuntimeError("file has no id; add it to a Storage first")
        opts = (
            json.dumps(f.compile_options)
            if f.compile_options is not None
            else None
        )
        self._conn.execute(
            "UPDATE file SET directory_id = ?, name = ?, mtime = ?, md5 = ?, "
            "compile_options = ?, driver = ?, indexed = ?, indexed_at = ?, "
            "args_overridden = ? WHERE id = ?",
            (
                f.directory_id, f.name, f.mtime, f.md5, opts, f.driver,
                int(f.indexed), f.indexed_at, int(f.args_overridden), f.id,
            ),
        )
        self._commit()

    def set_file_indexed(self, file_id: int, indexed: bool) -> None:
        """Flip a file's indexed/pending flag in place; symbols are untouched.

        Setting indexed=0 marks the file pending so the next `index` re-parses
        it (regenerating graph edges) without losing its existing symbols."""
        self._conn.execute(
            "UPDATE file SET indexed = ? WHERE id = ?",
            (int(bool(indexed)), file_id),
        )
        self._commit()

    # -- diagnostics (v15) ---------------------------------------------------

    def replace_diagnostics(
        self, file_id: int, diags: Sequence[dict[str, Any]]
    ) -> None:
        """Replace the stored parse diagnostics for a file (TU) wholesale.

        Called on every (re)index so a now-clean file drops its stale rows.
        Each diag is a dict with severity/spelling/file_path/line/col; rows are
        inserted in the given order so their ids follow TU diagnostic order."""
        self._conn.execute("DELETE FROM diagnostic WHERE file_id = ?", (file_id,))
        for d in diags:
            self._conn.execute(
                "INSERT INTO diagnostic "
                "(file_id, severity, spelling, file_path, line, col) "
                "VALUES (?, ?, ?, ?, ?, ?)",
                (
                    file_id,
                    d["severity"],
                    d["spelling"],
                    d.get("file_path"),
                    d.get("line"),
                    d.get("col"),
                ),
            )
        self._commit()

    # -- v31 include tier (read-only) -------------------------------------
    # Extraction lives in the C++ LibTooling engine; Python owns the schema,
    # the migration, and these compatibility queries.

    def add_translation_unit_config(self, config: TranslationUnitConfig) -> int:
        """Store or refresh one canonical configuration descriptor."""
        if config.state not in CONFIG_STATES:
            raise ValueError(f"unknown translation-unit config state: {config.state}")
        resolved = resolve_translation_unit_config(
            config.arguments,
            driver=config.driver,
            working_dir=config.working_dir,
            language=config.language,
            resource_dir=config.resource_dir,
            diagnostics_policy=config.diagnostics_policy,
        )
        for name in ("standard", "target", "sysroot"):
            if getattr(config, name):
                setattr(resolved, name, getattr(config, name))
        for name in (
            "abi_options", "include_paths", "macro_state",
            "relevant_environment", "generated_inputs",
        ):
            if getattr(config, name):
                setattr(resolved, name, list(getattr(config, name)))
        resolved.state = config.state
        config = resolved
        config.descriptor_json = canonical_translation_unit_config_json(config)
        config.descriptor_hash = translation_unit_config_hash(config)
        cur = self._conn.execute(
            "INSERT INTO translation_unit_config "
            "(descriptor_hash, descriptor_json, driver, working_dir, language, standard, target, "
            "abi_options, sysroot, resource_dir, include_paths, macro_state, relevant_environment, "
            "generated_inputs, diagnostics_policy, arguments, state) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(descriptor_hash) DO UPDATE SET descriptor_json=excluded.descriptor_json, "
            "driver=excluded.driver, working_dir=excluded.working_dir, language=excluded.language, "
            "standard=excluded.standard, target=excluded.target, abi_options=excluded.abi_options, "
            "sysroot=excluded.sysroot, resource_dir=excluded.resource_dir, include_paths=excluded.include_paths, "
            "macro_state=excluded.macro_state, relevant_environment=excluded.relevant_environment, "
            "generated_inputs=excluded.generated_inputs, diagnostics_policy=excluded.diagnostics_policy, "
            "arguments=excluded.arguments, state=excluded.state RETURNING id",
            (
                config.descriptor_hash, config.descriptor_json, config.driver, config.working_dir,
                config.language, config.standard, config.target,
                json.dumps(config.abi_options, ensure_ascii=False, separators=(",", ":")),
                config.sysroot, config.resource_dir,
                json.dumps(config.include_paths, ensure_ascii=False, separators=(",", ":")),
                json.dumps(config.macro_state, ensure_ascii=False, separators=(",", ":")),
                json.dumps(config.relevant_environment, ensure_ascii=False, separators=(",", ":")),
                json.dumps(config.generated_inputs, ensure_ascii=False, separators=(",", ":")),
                config.diagnostics_policy,
                json.dumps(config.arguments, ensure_ascii=False, separators=(",", ":")),
                config.state,
            ),
        )
        config.id = cur.fetchone()["id"]
        self._commit()
        return config.id

    def add_file_config(self, applicability: FileConfigApplicability) -> None:
        if applicability.role not in {"translation_unit", "header"}:
            raise ValueError(f"unknown file configuration role: {applicability.role}")
        if applicability.state not in CONFIG_STATES:
            raise ValueError(f"unknown file configuration state: {applicability.state}")
        self._conn.execute(
            "INSERT INTO file_config(file_id, config_id, role, state, reason) VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(file_id, config_id, role) DO UPDATE SET state=excluded.state, reason=excluded.reason",
            (applicability.file_id, applicability.config_id, applicability.role,
             applicability.state, applicability.reason),
        )
        self._commit()

    def translation_unit_configs_for_file(self, file_id: int) -> list[TranslationUnitConfig]:
        rows = self._conn.execute(
            "SELECT c.*, f.state AS association_state "
            "FROM translation_unit_config c JOIN translation_unit t "
            "ON t.config_id = c.id JOIN file_config f "
            "ON f.file_id = t.file_id AND f.config_id = c.id "
            "AND f.role = 'translation_unit' WHERE t.file_id = ? "
            "ORDER BY c.descriptor_hash",
            (file_id,),
        ).fetchall()
        return [_row_to(TranslationUnitConfig, row) for row in rows]

    def file_configs_for(self, file_id: int) -> list[FileConfigApplicability]:
        rows = self._conn.execute(
            "SELECT file_id, config_id, role, state, reason FROM file_config "
            "WHERE file_id = ? ORDER BY config_id, role", (file_id,)
        ).fetchall()
        return [_row_to(FileConfigApplicability, row) for row in rows]

    def include_graph_populated(self) -> bool:
        """True once any include fact exists.

        Distinguishes "this DB predates v31 / has not been reindexed" from
        "this file includes nothing". Callers must refuse rather than report
        zero findings on an empty tier -- a vacuous "no unused includes" reads
        exactly like a clean bill of health.
        """
        return (
            self._conn.execute("SELECT 1 FROM include_edge LIMIT 1").fetchone()
            is not None
        )

    def include_edges_from(
        self, src_file_id: int, include_system: bool = False
    ) -> list[IncludeEdge]:
        """Direct include edges out of a file, ordered by (target, config)."""
        sql = (
            "SELECT e.* FROM include_edge e "
            "JOIN include_config c ON c.id = e.config_id "
            "WHERE e.src_file_id = ?"
        )
        if not include_system:
            sql += " AND e.is_system = 0"
        sql += " ORDER BY e.dst_path, c.digest"
        return [
            _row_to(IncludeEdge, r)
            for r in self._conn.execute(sql, (src_file_id,)).fetchall()
        ]

    def include_edges_from_config(
        self, src_file_id: int, translation_unit_config_id: int,
        include_system: bool = False,
    ) -> list[IncludeEdge]:
        sql = (
            "SELECT e.* FROM include_edge e JOIN include_config c ON c.id = e.config_id "
            "WHERE e.src_file_id = ? AND c.translation_unit_config_id = ?"
        )
        args: tuple[int, int] = (src_file_id, translation_unit_config_id)
        if not include_system:
            sql += " AND e.is_system = 0"
        sql += " ORDER BY e.dst_path, c.digest"
        return [
            _row_to(IncludeEdge, row)
            for row in self._conn.execute(sql, args).fetchall()
        ]

    def invariant_include_edges(
        self, src_file_id: int, declared_config_ids: Sequence[int],
        include_system: bool = False,
    ) -> ConfiguredIncludeEdges:
        if not declared_config_ids:
            return ConfiguredIncludeEdges()
        common: Optional[dict[str, IncludeEdge]] = None
        for config_id in declared_config_ids:
            covered = self._conn.execute(
                "SELECT 1 FROM translation_unit_config c JOIN file_config f "
                "ON f.config_id = c.id WHERE f.file_id = ? AND f.config_id = ? "
                "AND f.role = 'header' AND f.state = 'registered' "
                "AND c.state = 'registered'",
                (src_file_id, config_id),
            ).fetchone()
            if covered is None:
                return ConfiguredIncludeEdges()
            current = {
                edge.dst_path: edge
                for edge in self.include_edges_from_config(
                    src_file_id, config_id, include_system
                )
            }
            common = current if common is None else {
                path: edge for path, edge in common.items() if path in current
            }
        return ConfiguredIncludeEdges(list(common.values()), True)

    def include_edges_to(self, dst_file_id: int) -> list[IncludeEdge]:
        """Direct include edges INTO a file -- who includes it."""
        return [
            _row_to(IncludeEdge, r)
            for r in self._conn.execute(
                "SELECT e.* FROM include_edge e "
                "JOIN include_config c ON c.id = e.config_id "
                "WHERE e.dst_file_id = ? ORDER BY e.src_file_id, c.digest",
                (dst_file_id,),
            ).fetchall()
        ]

    def include_sites_for(self, edge_id: int) -> list[IncludeSite]:
        """Directive occurrences of one collapsed edge, by byte offset."""
        return [
            _row_to(IncludeSite, r)
            for r in self._conn.execute(
                "SELECT * FROM include_site WHERE edge_id = ? "
                "ORDER BY begin_offset",
                (edge_id,),
            ).fetchall()
        ]

    def include_configs_for_tu(self, tu_file_id: int) -> list[IncludeConfig]:
        """Compile configurations recorded for a translation unit."""
        return [
            _row_to(IncludeConfig, r)
            for r in self._conn.execute(
                "SELECT * FROM include_config WHERE tu_file_id = ? "
                "ORDER BY digest",
                (tu_file_id,),
            ).fetchall()
        ]

    def include_macro_uses(self, src_file_id: int, def_path: str) -> list[str]:
        """Macros `src_file_id` expands that are defined in `def_path`.

        The dependency the symbol-reference graph structurally cannot see: a
        header supplying only a macro has zero symbol references and is still
        required.
        """
        return [
            r[0]
            for r in self._conn.execute(
                "SELECT name FROM include_macro_use "
                "WHERE src_file_id = ? AND def_path = ? ORDER BY name",
                (src_file_id, def_path),
            ).fetchall()
        ]

    def get_diagnostics(self, file_id: int) -> list[Diagnostic]:
        """Stored parse diagnostics for a file, in insertion (TU) order."""
        rows = self._conn.execute(
            "SELECT * FROM diagnostic WHERE file_id = ? ORDER BY id", (file_id,)
        ).fetchall()
        return [_row_to(Diagnostic, r) for r in rows]

    def diagnostic_counts(self) -> dict[int, dict[int, int]]:
        """Per-file diagnostic counts grouped by severity: {file_id: {sev: n}}."""
        out: dict[int, dict[int, int]] = {}
        for fid, sev, n in self._conn.execute(
            "SELECT file_id, severity, COUNT(*) FROM diagnostic "
            "GROUP BY file_id, severity"
        ):
            out.setdefault(fid, {})[sev] = n
        return out

    def set_file_compile_options(
        self,
        file_id: int,
        options: list[str],
        driver: Optional[str] = None,
        update_driver: bool = False,
    ) -> None:
        """Replace a file's stored compile flags (and optionally its driver) and
        mark it args_overridden=1 so a later `import` (without --force) keeps the
        edit. Used by `cidx file flags -set-flag/-unset-flag/-import-args`."""
        opts = json.dumps(options)
        if update_driver:
            self._conn.execute(
                "UPDATE file SET compile_options = ?, driver = ?, "
                "args_overridden = 1 WHERE id = ?",
                (opts, driver, file_id),
            )
        else:
            self._conn.execute(
                "UPDATE file SET compile_options = ?, args_overridden = 1 WHERE id = ?",
                (opts, file_id),
            )
        self._commit()

    def update_file_compile_options(self, file_id: int, options: list[str]) -> None:
        """Replace a file's stored compile flags WITHOUT marking args_overridden.

        Used by `cidx repo realias`, which rewrites include paths to <label> tokens as
        a portability transform (not a manual edit) -- a later `import` should be
        free to re-strip + re-alias these files."""
        self._conn.execute(
            "UPDATE file SET compile_options = ? WHERE id = ?",
            (json.dumps(options), file_id),
        )
        self._commit()

    def is_file_indexed(
        self, abs_path: str, mtime: Optional[float] = None, md5: Optional[str] = None
    ) -> bool:
        """True if the file has been indexed (and is not stale, if mtime/md5 given).

        `mtime`/`md5` describe the file's *current* state: pass either to also
        treat a changed file as NOT indexed (incremental reindex).
        """
        f = self.get_file(abs_path)
        if f is None or not f.indexed:
            return False
        if mtime is not None and (f.mtime is None or f.mtime < mtime):
            return False
        if md5 is not None and f.md5 != md5:
            return False
        return True

    def file_abs_path(self, file_id: int) -> Optional[str]:
        """Reconstructed absolute path for a file id (uses effective root)."""
        row = self._conn.execute(
            "SELECT c.path AS root, c.version AS version, "
            "c.repository_id AS comp_repo_id, d.path AS rel, f.name AS name "
            "FROM file f JOIN directory d ON d.id = f.directory_id "
            "JOIN component c ON c.id = d.component_id WHERE f.id = ?",
            (file_id,),
        ).fetchone()
        if row is None:
            return None
        # Reconstruct via the effective root (base + version, clone-resolved).
        comp_stub = Component(
            name="", path=row["root"], version=row["version"],
            repository_id=row["comp_repo_id"],
        )
        eff = self.component_abs_base(comp_stub)
        return (
            os.path.join(eff, row["rel"], row["name"])
            if row["rel"]
            else os.path.join(eff, row["name"])
        )

    def directory_abs_path(self, directory_id: int) -> Optional[str]:
        """Reconstructed absolute path for a directory id (uses effective root)."""
        row = self._conn.execute(
            "SELECT c.path AS root, c.version AS version, "
            "c.repository_id AS comp_repo_id, d.path AS rel FROM directory d "
            "JOIN component c ON c.id = d.component_id WHERE d.id = ?",
            (directory_id,),
        ).fetchone()
        if row is None:
            return None
        comp_stub = Component(
            name="", path=row["root"], version=row["version"],
            repository_id=row["comp_repo_id"],
        )
        eff = self.component_abs_base(comp_stub)
        return os.path.join(eff, row["rel"]) if row["rel"] else eff

    def _split_path(self, abs_path: str) -> tuple[int, str, str]:
        """Absolute path -> (component_id, relative dir, file name)."""
        abs_path = os.path.abspath(abs_path)
        comp = self.component_for_path(abs_path)
        if comp is None or comp.id is None:
            raise KeyError(f"no component owns {abs_path} (add_component first)")
        resolved_root = self.component_abs_base(comp)
        rel = os.path.relpath(abs_path, resolved_root)
        rel_dir, name = os.path.split(rel)
        if rel_dir == ".":
            rel_dir = ""
        return comp.id, rel_dir, name

    # -- symbols ---------------------------------------------------------------

    _SYMBOL_COLS = (
        "usr",
        "spelling",
        "qual_name",
        "display_name",
        "kind",
        "type_info",
        "file_id",
        "line",
        "col",
        "end_line",
        "end_col",
        "decl_file_id",
        "decl_line",
        "decl_col",
        "decl_path",
        "is_definition",
        "is_pure",
        "is_static",
        "is_instantiation",
        "linkage",
        "access",
        "parent_usr",
        "resolved",
        "const_value",
        "semantic_universe_id",
        "identity_key",
    )

    def add_symbol(self, sym: Symbol) -> int:
        """Insert or upsert a symbol keyed by declared scope plus identity.

        A definition always wins over a previously stored declaration; a
        declaration never downgrades a stored definition's location.
        """
        if sym.kind not in SYMBOL_KINDS:
            raise ValueError(f"unknown symbol kind {sym.kind!r}")
        scope_file_id = sym.file_id if sym.file_id is not None else sym.decl_file_id
        universe_id = (
            sym.semantic_universe_id
            if sym.semantic_universe_id > 0
            else self._semantic_universe_for_file(scope_file_id)
        )
        identity_key = self._symbol_identity_key(sym, universe_id, scope_file_id)
        # kind is stored as its CXCursorKind integer (v16); convert on the way in.
        vals = tuple(
            SYMBOL_KIND_IDS[sym.kind] if c == "kind"
            else universe_id if c == "semantic_universe_id"
            else identity_key if c == "identity_key"
            else getattr(sym, c)
            for c in self._SYMBOL_COLS
        )
        cur = self._conn.execute(
            f"INSERT INTO symbol ({', '.join(self._SYMBOL_COLS)}) "
            f"VALUES ({', '.join('?' * len(self._SYMBOL_COLS))}) "
            "ON CONFLICT(semantic_universe_id, identity_key) WHERE identity_key <> '' "
            "DO UPDATE SET "
            "  spelling      = excluded.spelling, "
            "  qual_name     = COALESCE(excluded.qual_name, symbol.qual_name), "
            "  display_name  = COALESCE(excluded.display_name, symbol.display_name), "
            "  kind          = excluded.kind, "
            "  type_info     = COALESCE(excluded.type_info, symbol.type_info), "
            "  file_id       = CASE WHEN excluded.is_definition >= symbol.is_definition "
            "                       THEN excluded.file_id ELSE symbol.file_id END, "
            "  line          = CASE WHEN excluded.is_definition >= symbol.is_definition "
            "                       THEN excluded.line ELSE symbol.line END, "
            "  col           = CASE WHEN excluded.is_definition >= symbol.is_definition "
            "                       THEN excluded.col ELSE symbol.col END, "
            "  end_line      = CASE WHEN excluded.is_definition >= symbol.is_definition "
            "                       THEN excluded.end_line ELSE symbol.end_line END, "
            "  end_col       = CASE WHEN excluded.is_definition >= symbol.is_definition "
            "                       THEN excluded.end_col ELSE symbol.end_col END, "
            "  decl_file_id  = COALESCE(excluded.decl_file_id, symbol.decl_file_id), "
            "  decl_line     = COALESCE(excluded.decl_line, symbol.decl_line), "
            "  decl_col      = COALESCE(excluded.decl_col, symbol.decl_col), "
            "  is_definition    = MAX(excluded.is_definition, symbol.is_definition), "
            "  is_pure          = MAX(excluded.is_pure, symbol.is_pure), "
            "  is_static        = MAX(excluded.is_static, symbol.is_static), "
            # A real decl row states its TemplateSpecializationKind
            # authoritatively, so reindexing an instantiation-turned-explicit-
            # specialization (same USR) must DOWNGRADE the flag. Stub promotion
            # stays monotonic in mint_symbol_id, which keeps its MAX.
            "  is_instantiation = excluded.is_instantiation, "
            "  linkage       = COALESCE(excluded.linkage, symbol.linkage), "
            "  access        = COALESCE(excluded.access, symbol.access), "
            "  parent_usr    = COALESCE(excluded.parent_usr, symbol.parent_usr), "
            "  resolved      = MAX(excluded.resolved, symbol.resolved), "
            # v33: only the initializer-bearing decl evaluates to a value, so a
            # plain declaration must not erase the definition's stored constant.
            "  const_value   = COALESCE(excluded.const_value, symbol.const_value) "
            "RETURNING id",
            vals,
        )
        sid = cur.fetchone()["id"]
        self._conn.execute(
            "UPDATE symbol SET parent_id=? WHERE parent_usr=? "
            "AND (parent_id IS NULL OR parent_id<>?)",
            (sid, sym.usr, sid),
        )
        # v26: record THIS cursor's own site. The symbol row keeps only the
        # winning definition + one declaration; decl_site keeps every physical
        # site so references() can list all reopenings of an open symbol (a
        # namespace above all). Guard on a real (file, line): locationless stubs
        # would otherwise fan out on the NULL-file UNIQUE (NULL != NULL). INSERT
        # OR IGNORE is idempotent -- reindexing the same TU re-adds nothing.
        if sym.file_id is not None and sym.line is not None:
            self._conn.execute(
                "INSERT OR IGNORE INTO decl_site "
                "(symbol_id, file_id, line, col, end_line, end_col, is_definition) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)",
                (
                    sid,
                    sym.file_id,
                    sym.line,
                    sym.col,
                    sym.end_line,
                    sym.end_col,
                    1 if sym.is_definition else 0,
                ),
            )
        self._reconcile_external_identities()
        self._commit()
        return sid

    def update_symbol(
        self,
        usr: str,
        semantic_universe_id: Optional[int] = None,
        identity_source: Optional[str] = None,
        identity_translation_unit: Optional[str] = None,
        **values: Any,
    ) -> bool:
        """Update one explicitly resolved scoped symbol, never every USR row."""
        target = self.lookup_symbol(
            usr, semantic_universe_id, identity_source, identity_translation_unit
        )
        if target is None:
            return False
        return self.update_symbol_by_id(target.id, **values)

    def update_symbol_by_id(self, symbol_id: int, **values: Any) -> bool:
        """Update a single symbol row by its database-local handle."""
        bad = set(values) - set(self._SYMBOL_COLS)
        if bad:
            raise ValueError(f"unknown symbol column(s): {sorted(bad)}")
        if "kind" in values:
            if values["kind"] not in SYMBOL_KINDS:
                raise ValueError(f"unknown symbol kind {values['kind']!r}")
            values = {**values, "kind": SYMBOL_KIND_IDS[values["kind"]]}
        if not values:
            return self.lookup_symbol_by_id(symbol_id) is not None
        sets = ", ".join(f"{c} = ?" for c in values)
        cur = self._conn.execute(
            f"UPDATE symbol SET {sets} WHERE id = ?", (*values.values(), symbol_id)
        )
        self._commit()
        return cur.rowcount > 0

    def lookup_symbol(
        self,
        usr: str,
        semantic_universe_id: Optional[int] = None,
        identity_source: Optional[str] = None,
        identity_translation_unit: Optional[str] = None,
    ) -> Optional[Symbol]:
        """Return one scoped symbol; reject ambiguous bare-USR lookups."""
        rows = self.lookup_symbols_by_usr(usr, semantic_universe_id)
        if not rows:
            return None
        if identity_source:
            probe = Symbol(
                usr=usr,
                spelling="",
                kind="function",
                linkage="internal",
                identity_source=identity_source,
                identity_translation_unit=identity_translation_unit,
            )
            for row in rows:
                if row.identity_key == self._symbol_identity_key(
                    probe,
                    row.semantic_universe_id,
                    row.file_id,
                    identity_translation_unit,
                ):
                    return row
            universe = self.get_semantic_universe_by_id(rows[0].semantic_universe_id)
            universe_key = universe.key if universe is not None else "legacy"
            portable_matches = [
                row for row in rows
                if row.identity_key == f"{universe_key}\x1f{usr}"
            ]
            if len(portable_matches) == 1:
                return portable_matches[0]
            return None
        if identity_translation_unit:
            universe = self.get_semantic_universe_by_id(
                rows[0].semantic_universe_id
            )
            prefix = (
                f"{universe.key if universe is not None else 'legacy'}\x1flocal:"
                f"{identity_translation_unit}\x1f"
            )
            tu_matches = [row for row in rows if row.identity_key.startswith(prefix)]
            if len(tu_matches) == 1:
                return tu_matches[0]
            if len(tu_matches) > 1:
                raise ValueError(
                    f"ambiguous symbol USR within translation unit: {usr}"
                )
        if len(rows) > 1:
            raise ValueError(
                f"ambiguous symbol USR; pass semantic universe scope: {usr}"
            )
        return rows[0]

    def lookup_symbols_by_usr(
        self, usr: str, semantic_universe_id: Optional[int] = None
    ) -> list[Symbol]:
        """Return all scoped matches, deterministically ordered.

        Passing ``semantic_universe_id`` makes the scope explicit; the integer
        is local to this database and must not be used as a portable identity.
        """
        sql = "SELECT * FROM symbol WHERE usr = ?"
        args: list[Any] = [usr]
        if semantic_universe_id is not None:
            sql += " AND semantic_universe_id = ?"
            args.append(semantic_universe_id)
        sql += " ORDER BY semantic_universe_id, identity_key"
        return [_row_to(Symbol, row) for row in self._conn.execute(sql, args)]

    def lookup_symbol_by_id(self, symbol_id: int) -> Optional[Symbol]:
        row = self._conn.execute(
            "SELECT * FROM symbol WHERE id = ?", (symbol_id,)
        ).fetchone()
        return _row_to(Symbol, row)

    def lookup_symbols_by_name(
        self, spelling: str, kind: Optional[str] = None
    ) -> list[Symbol]:
        """All symbols with this spelling (overloads / statics give several rows)."""
        sql = "SELECT * FROM symbol WHERE spelling = ?"
        args: list[Any] = [spelling]
        if kind is not None:
            sql += " AND kind = ?"
            args.append(SYMBOL_KIND_IDS.get(kind, -1))
        sql += " ORDER BY usr"
        return [_row_to(Symbol, r) for r in self._conn.execute(sql, args)]

    def lookup_symbols_by_qual_name(
        self, qual_name: str, kind: Optional[str] = None
    ) -> list[Symbol]:
        """All symbols with this fully-qualified name (overloads give several).

        Used to resolve a callee whose USR cannot be matched directly -- e.g. a
        member function template referenced in a dependent template body, where
        libclang emits an inconsistent USR (parameter types collapse). The
        qualified name + kind are stable, so an unambiguous (single) match
        recovers the target."""
        sql = "SELECT * FROM symbol WHERE qual_name = ?"
        args: list[Any] = [qual_name]
        if kind is not None:
            sql += " AND kind = ?"
            args.append(SYMBOL_KIND_IDS.get(kind, -1))
        sql += " ORDER BY usr"
        return [_row_to(Symbol, r) for r in self._conn.execute(sql, args)]

    def search_symbols(
        self, pattern: str, kind: Optional[str] = None,
        config_id: Optional[int] = None,
    ) -> list[Symbol]:
        """Fuzzy match against the qualified name (case-insensitive).

        Each '::'-separated segment of `pattern` must appear, in order, as a
        substring of qual_name: 'conf::set' matches 'RdKafka::ConfImpl::set'.
        """
        like = (
            "%"
            + "%".join(
                seg.replace("%", r"\%").replace("_", r"\_")
                for seg in pattern.split("::")
                if seg
            )
            + "%"
        )
        sql = r"SELECT * FROM symbol WHERE qual_name LIKE ? ESCAPE '\'"
        args: list[Any] = [like]
        if kind is not None:
            sql += " AND kind = ?"
            args.append(SYMBOL_KIND_IDS.get(kind, -1))
        if config_id is not None:
            sql += (
                " AND EXISTS (SELECT 1 FROM fact_applicability fa WHERE "
                "fa.fact_kind = 'symbol' AND fa.fact_id = symbol.id "
                "AND fa.config_id = ?)"
            )
            args.append(config_id)
        sql += " ORDER BY LENGTH(qual_name), qual_name"
        return [_row_to(Symbol, r) for r in self._conn.execute(sql, args)]

    def symbols_for_config(
        self, file_id: int, config_ids: Sequence[int],
        coverage: str = FactCoverage.ONE,
    ) -> ConfiguredSymbols:
        """Read configuration-qualified symbols with explicit unknown coverage."""
        if not config_ids:
            return ConfiguredSymbols()
        for config_id in config_ids:
            covered = self._conn.execute(
                "SELECT 1 FROM translation_unit_config c JOIN file_config f "
                "ON f.config_id = c.id WHERE f.file_id = ? AND f.config_id = ? "
                "AND f.state = 'registered' AND c.state = 'registered' LIMIT 1",
                (file_id, config_id),
            ).fetchone()
            if covered is None:
                return ConfiguredSymbols()

        def read(config_id: int) -> dict[int, Symbol]:
            rows = self._conn.execute(
                "SELECT s.* FROM symbol s JOIN fact_applicability fa ON "
                "fa.fact_kind = 'symbol' AND fa.fact_id = s.id "
                "AND fa.file_id = ? AND fa.config_id = ? ORDER BY s.usr",
                (file_id, config_id),
            ).fetchall()
            return {row["id"]: _row_to(Symbol, row) for row in rows}

        selected: dict[int, Symbol] = {}
        for index, config_id in enumerate(config_ids):
            current = read(config_id)
            if coverage == FactCoverage.ONE:
                selected = current
                break
            if index == 0:
                selected = current
            elif coverage == FactCoverage.ALL:
                selected.update(current)
            elif coverage == FactCoverage.INVARIANT:
                selected = {
                    symbol_id: symbol
                    for symbol_id, symbol in selected.items()
                    if symbol_id in current
                }
            else:
                raise ValueError(f"unknown fact coverage {coverage!r}")
        return ConfiguredSymbols(
            symbols=[selected[key] for key in sorted(selected)],
            coverage_complete=True,
        )

    def fact_ids_for_config(
        self, file_id: int, fact_kind: str, config_ids: Sequence[int],
        coverage: str = FactCoverage.ONE,
    ) -> ConfiguredFactIds:
        if not config_ids:
            return ConfiguredFactIds()
        selected: set[int] = set()
        for index, config_id in enumerate(config_ids):
            covered = self._conn.execute(
                "SELECT 1 FROM translation_unit_config c JOIN file_config f "
                "ON f.config_id = c.id WHERE f.file_id = ? AND f.config_id = ? "
                "AND f.state = 'registered' AND c.state = 'registered' LIMIT 1",
                (file_id, config_id),
            ).fetchone()
            if covered is None:
                return ConfiguredFactIds()
            current = {
                row[0]
                for row in self._conn.execute(
                    "SELECT fact_id FROM fact_applicability WHERE fact_kind = ? "
                    "AND file_id = ? AND config_id = ? ORDER BY fact_id",
                    (fact_kind, file_id, config_id),
                )
            }
            if coverage == FactCoverage.ONE:
                selected = current
                break
            if index == 0:
                selected = current
            elif coverage == FactCoverage.ALL:
                selected |= current
            elif coverage == FactCoverage.INVARIANT:
                selected &= current
            else:
                raise ValueError(f"unknown fact coverage {coverage!r}")
        return ConfiguredFactIds(sorted(selected), True)

    def list_symbols(
        self,
        component_id: Optional[int] = None,
        dir_path: Optional[str] = None,
        file_id: Optional[int] = None,
        name: Optional[str] = None,
        kind: Optional[str] = None,
    ) -> list[Symbol]:
        """Symbols filtered by location and/or name.

        Location scoping (component / directory subtree / file) matches a
        symbol if EITHER its definition site or its declaration site falls
        inside the scope -- so listing a header shows the prototypes whose
        definitions live in a .c file. `name` is a free-text fuzzy match
        against the qualified name (spelling when no qual_name is stored).
        """
        sql = "SELECT s.* FROM symbol s"
        where, args = [], []
        if component_id is not None or dir_path is not None:
            scope, scope_args = ["d.component_id = ?"], [component_id]
            if component_id is None:  # dir filter across all components
                scope, scope_args = [], []
            if dir_path is not None:
                scope.append(self._dir_scope_sql(dir_path, scope_args))
            where.append(
                "EXISTS (SELECT 1 FROM file f "
                "JOIN directory d ON d.id = f.directory_id "
                "WHERE f.id IN (s.file_id, s.decl_file_id) AND "
                + " AND ".join(scope)
                + ")"
            )
            args.extend(scope_args)
        if file_id is not None:
            where.append("(s.file_id = ? OR s.decl_file_id = ?)")
            args.extend([file_id, file_id])
        if name:
            where.append(r"COALESCE(s.qual_name, s.spelling) LIKE ? ESCAPE '\'")
            args.append(self._fuzzy_like(name))
        if kind is not None:
            where.append("s.kind = ?")
            args.append(SYMBOL_KIND_IDS.get(kind, -1))
        if where:
            sql += " WHERE " + " AND ".join(where)
        sql += (
            " ORDER BY LENGTH(COALESCE(s.qual_name, s.spelling)),"
            " COALESCE(s.qual_name, s.spelling)"
        )
        return [_row_to(Symbol, r) for r in self._conn.execute(sql, args)]

    def symbols_in_file(self, file_id: int) -> list[Symbol]:
        return [
            _row_to(Symbol, r)
            for r in self._conn.execute(
                "SELECT * FROM symbol WHERE file_id = ? ORDER BY line, col", (file_id,)
            )
        ]

    def unresolved_symbols(self) -> list[Symbol]:
        return [
            _row_to(Symbol, r)
            for r in self._conn.execute(
                "SELECT * FROM symbol WHERE resolved = 0 ORDER BY usr"
            )
        ]

    # -- graph (v7) ------------------------------------------------------------

    def mint_symbol_id(
        self,
        usr: str,
        spelling: str = "",
        qual_name: Optional[str] = None,
        display_name: Optional[str] = None,
        kind: str = "function",
        decl_file_id: Optional[int] = None,
        decl_line: Optional[int] = None,
        decl_col: Optional[int] = None,
        decl_path: Optional[str] = None,
        is_instantiation: bool = False,
        is_named_instance: bool = False,
        semantic_universe_id: Optional[int] = None,
        identity_source: Optional[str] = None,
        linkage: Optional[str] = None,
        identity_translation_unit: Optional[str] = None,
    ) -> int:
        """Insert a stub row for `usr` (if absent), then SELECT its id.

        The callee/base/override/primary reference cursor is always in hand at
        the call site, so its name, kind AND declaration location travel with
        the USR: a stub is born NAMED, correctly typed, and -- when the
        reference cursor carries a source location in an indexed file -- LOCATED
        (e.g. a defaulted ctor anchored to its `struct` line). This matters for
        targets whose definition is never separately indexed (implicit/defaulted
        special members, implicit template instantiations) -- without a
        backfilling `add_symbol`, this mint is all the graph will ever have, so
        dropping libclang's location here is what made `chain::D::D` print
        `@<no-location>`. `decl_file_id` is None for targets in unregistered
        (e.g. system/stdlib) headers; for those the AST still carries a real
        source location, so the caller passes the raw path as `decl_path` (with
        decl_line/decl_col) and the stub stays located -- e.g. a libstdc++
        `__normal_iterator::operator*` resolves to `stl_iterator.h:1234` instead
        of `@<no-location>`. Only a target with no source location at all
        (implicit/builtin) is truly location-less.

        'function' is the fallback kind when the cursor kind is unknown; the
        real def's add_symbol upsert overwrites kind/spelling/location/resolved
        later. On a repeat mint we only UPGRADE an unnamed stub (empty spelling)
        -- name and kind together -- never clobber a real symbol's; the decl
        location (registered or raw path) is filled in only when still absent
        (COALESCE).

        `is_instantiation=True` marks implicit template-instantiation nodes
        (v13: both the X<int> type node and each X<int>::member node). The flag
        is set via MAX() so a later stub->instantiation promotion always upgrades
        but never downgrades.
        """
        universe_id = (
            semantic_universe_id
            if semantic_universe_id is not None
            else self._semantic_universe_for_file(decl_file_id)
        )
        identity_key = self._symbol_identity_key(
            Symbol(
                usr=usr,
                spelling=spelling,
                kind=kind,
                linkage=linkage,
                identity_source=identity_source,
                identity_translation_unit=identity_translation_unit,
            ),
            universe_id,
            decl_file_id,
        )
        self._conn.execute(
            "INSERT INTO symbol (usr, spelling, qual_name, display_name, kind, "
            "                    decl_file_id, decl_line, decl_col, decl_path, "
            "                    is_instantiation, is_named_instance, resolved, "
            "                    semantic_universe_id, identity_key) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?) "
            "ON CONFLICT(semantic_universe_id, identity_key) WHERE identity_key <> '' "
            "DO UPDATE SET "
            "  kind             = CASE WHEN symbol.spelling = '' "
            "                          THEN excluded.kind ELSE symbol.kind END, "
            "  spelling         = CASE WHEN symbol.spelling = '' "
            "                          THEN excluded.spelling ELSE symbol.spelling END, "
            "  qual_name        = COALESCE(symbol.qual_name, excluded.qual_name), "
            "  display_name     = COALESCE(symbol.display_name, excluded.display_name), "
            "  decl_file_id     = COALESCE(symbol.decl_file_id, excluded.decl_file_id), "
            "  decl_line        = COALESCE(symbol.decl_line, excluded.decl_line), "
            "  decl_col         = COALESCE(symbol.decl_col, excluded.decl_col), "
            "  decl_path        = COALESCE(symbol.decl_path, excluded.decl_path), "
            "  is_instantiation = MAX(symbol.is_instantiation, excluded.is_instantiation), "
            "  is_named_instance = MAX(symbol.is_named_instance, excluded.is_named_instance)",
            (
                usr,
                spelling,
                qual_name or None,
                display_name or None,
                SYMBOL_KIND_IDS.get(kind, -1),  # kind stored as CXCursorKind int (v16)
                decl_file_id,
                decl_line,
                decl_col,
                decl_path or None,
                1 if is_instantiation else 0,
                1 if is_named_instance else 0,
                universe_id,
                identity_key,
            ),
        )
        row = self._conn.execute(
            "SELECT id FROM symbol WHERE semantic_universe_id = ? "
            "AND identity_key = ?", (universe_id, identity_key)
        ).fetchone()
        if row is None:
            raise RuntimeError(
                f"mint_symbol_id: SELECT returned no row for usr={usr!r}"
            )
        return row["id"]

    def add_edge(
        self,
        src_id: int,
        dst_id: int,
        kind: int,
        count: int = 1,
        base_access: Optional[int] = None,
        is_virtual: Optional[int] = None,
        vtable_slot: Optional[int] = None,
    ) -> int:
        """Upsert an edge; returns the edge id."""
        cur = self._conn.execute(
            "INSERT INTO edge (src_id, dst_id, kind, count, base_access, is_virtual, "
            "                  vtable_slot) "
            "VALUES (?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(src_id, dst_id, kind) DO UPDATE SET "
            "  count       = edge.count + excluded.count, "
            "  base_access = COALESCE(excluded.base_access, edge.base_access), "
            "  is_virtual  = COALESCE(excluded.is_virtual,  edge.is_virtual), "
            "  vtable_slot = COALESCE(excluded.vtable_slot, edge.vtable_slot) "
            "RETURNING id",
            (src_id, dst_id, kind, count, base_access, is_virtual, vtable_slot),
        )
        row = cur.fetchone()
        if row is None:
            raise RuntimeError("add_edge: upsert returned no id")
        return row["id"]

    def add_edge_site(
        self,
        edge_id: int,
        file_id: Optional[int],
        line: Optional[int],
        col: Optional[int],
        conditional: int = 0,
        args_sig: Optional[str] = None,
        recv_src_kind: Optional[str] = None,
        recv_type_usr: Optional[str] = None,
        recv_decl_usr: Optional[str] = None,
        recv_param_pos: Optional[int] = None,
        recv_type_is_value: Optional[int] = None,
    ) -> None:
        """INSERT OR IGNORE an edge_site (PK collision = same site, harmless)."""
        source_kind_id = None if recv_src_kind is None else SOURCE_KIND_IDS.get(recv_src_kind)
        if recv_src_kind is not None and source_kind_id is None:
            raise ValueError(f"unknown source kind {recv_src_kind!r}")

        def symbol_id(usr: Optional[str]) -> Optional[int]:
            if not usr:
                return None
            row = self._conn.execute("SELECT id FROM symbol WHERE usr=? LIMIT 1", (usr,)).fetchone()
            return None if row is None else row["id"]

        def type_id(usr: Optional[str]) -> Optional[int]:
            if not usr:
                return None
            row = self._conn.execute(
                "SELECT id FROM type_node WHERE decl_usr=? ORDER BY id LIMIT 1", (usr,)
            ).fetchone()
            return None if row is None else row["id"]

        def identity_id(kind: int, text: Optional[str], local_id: Optional[int]) -> Optional[int]:
            if not text or local_id is not None:
                return None
            row = self._conn.execute(
                "INSERT INTO external_identity(identity_kind,identity_text,resolution_status) "
                "VALUES (?, ?, 0) ON CONFLICT(identity_kind,identity_text) DO UPDATE SET "
                "resolution_status=0 RETURNING id",
                (kind, text),
            ).fetchone()
            return None if row is None else row["id"]

        recv_type_id = type_id(recv_type_usr)
        recv_decl_id = symbol_id(recv_decl_usr)
        self._conn.execute(
            "INSERT OR IGNORE INTO edge_site "
            "(edge_id, file_id, line, col, conditional, args_sig, "
            " recv_src_kind, recv_type_usr, recv_decl_usr, recv_src_kind_id, "
            " recv_type_id, recv_decl_id, recv_type_identity_id, recv_decl_identity_id, "
            " recv_param_pos, recv_type_is_value) "
            "VALUES (?, ?, ?, ?, ?, ?, NULL, NULL, NULL, ?, ?, ?, ?, ?, ?, ?)",
            (
                edge_id,
                file_id,
                line,
                col,
                conditional,
                args_sig,
                source_kind_id,
                recv_type_id,
                recv_decl_id,
                identity_id(IDENTITY_KIND_IDS["type_usr"], recv_type_usr, recv_type_id),
                identity_id(IDENTITY_KIND_IDS["symbol_usr"], recv_decl_usr, recv_decl_id),
                recv_param_pos,
                recv_type_is_value,
            ),
        )
        self._reconcile_external_identities()

    def add_call_arg(
        self,
        edge_id: int,
        file_id: int,
        line: int,
        col: int,
        position: int,
        src_kind: str,
        type_usr: Optional[str] = None,
        decl_usr: Optional[str] = None,
        callee_usr: Optional[str] = None,
        type_is_value: Optional[int] = None,
    ) -> None:
        """INSERT OR IGNORE a call_arg row (PK collision = same arg, harmless)."""
        source_kind_id = SOURCE_KIND_IDS.get(src_kind)
        if source_kind_id is None:
            raise ValueError(f"unknown source kind {src_kind!r}")

        def symbol_id(usr: Optional[str]) -> Optional[int]:
            if not usr:
                return None
            row = self._conn.execute("SELECT id FROM symbol WHERE usr=? LIMIT 1", (usr,)).fetchone()
            return None if row is None else row["id"]

        def type_id(usr: Optional[str]) -> Optional[int]:
            if not usr:
                return None
            row = self._conn.execute(
                "SELECT id FROM type_node WHERE decl_usr=? ORDER BY id LIMIT 1", (usr,)
            ).fetchone()
            return None if row is None else row["id"]

        def identity_id(kind: int, text: Optional[str], local_id: Optional[int]) -> Optional[int]:
            if not text or local_id is not None:
                return None
            row = self._conn.execute(
                "INSERT INTO external_identity(identity_kind,identity_text,resolution_status) "
                "VALUES (?, ?, 0) ON CONFLICT(identity_kind,identity_text) DO UPDATE SET "
                "resolution_status=0 RETURNING id",
                (kind, text),
            ).fetchone()
            return None if row is None else row["id"]

        arg_type_id = type_id(type_usr)
        arg_decl_id = symbol_id(decl_usr)
        arg_callee_id = symbol_id(callee_usr)
        self._conn.execute(
            "INSERT OR IGNORE INTO call_arg "
            "(edge_id, file_id, line, col, position, src_kind, "
            " type_usr, decl_usr, callee_usr, src_kind_id, type_id, decl_id, "
            " callee_id, type_identity_id, decl_identity_id, callee_identity_id, type_is_value) "
            "VALUES (?, ?, ?, ?, ?, NULL, NULL, NULL, NULL, ?, ?, ?, ?, ?, ?, ?, ?)",
            (
                edge_id,
                file_id,
                line,
                col,
                position,
                source_kind_id,
                arg_type_id,
                arg_decl_id,
                arg_callee_id,
                identity_id(IDENTITY_KIND_IDS["type_usr"], type_usr, arg_type_id),
                identity_id(IDENTITY_KIND_IDS["symbol_usr"], decl_usr, arg_decl_id),
                identity_id(IDENTITY_KIND_IDS["symbol_usr"], callee_usr, arg_callee_id),
                type_is_value,
            ),
        )
        self._reconcile_external_identities()

    def add_template_param(
        self,
        owner_id: int,
        position: int,
        param_kind: int,
        name: Optional[str] = None,
        default_txt: Optional[str] = None,
        type_id: Optional[int] = None,
        default_type_id: Optional[int] = None,
        default_ref_id: Optional[int] = None,
    ) -> None:
        self._conn.execute(
            "INSERT OR REPLACE INTO template_param "
            "(owner_id, position, param_kind, name, default_txt, type_id, "
            "default_type_id, default_ref_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (owner_id, position, param_kind, name, default_txt, type_id,
             default_type_id, default_ref_id),
        )

    def add_template_arg(
        self,
        owner_id: int,
        position: int,
        arg_kind: int,
        ref_id: Optional[int] = None,
        literal: Optional[str] = None,
        pack_index: int = -1,
        type_id: Optional[int] = None,
    ) -> None:
        self._conn.execute(
            "INSERT OR REPLACE INTO template_arg "
            "(owner_id, position, pack_index, arg_kind, ref_id, literal, type_id) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (owner_id, position, pack_index, arg_kind, ref_id, literal, type_id),
        )

    def delete_edges_for_file(self, file_id: int) -> None:
        """Remove edges whose src symbol was indexed from this file.

        Contains edges (kind=3) are excluded: they are declaration-level
        structural edges emitted once during header indexing. Namespace and
        record membership spans multiple TUs (the same namespace reopens in
        every .cpp that uses it), so deleting contains on each re-index would
        permanently erase edges emitted during the header-indexing pass.
        Contains edges are idempotent (UPSERT) and survive stale re-indexes.
        """
        self._conn.execute(
            "DELETE FROM edge WHERE kind != 3 AND src_id IN "
            "(SELECT id FROM symbol WHERE file_id = ?)",
            (file_id,),
        )
        self._commit()

    # -- entity_edge (v17) ------------------------------------------------------

    def add_entity_edge(
        self,
        src_id: int,
        dst_id: int,
        kind: int,
        count: int = 1,
        via_member_id: Optional[int] = None,
        multiplicity: int = 1,
        access: int = 0,
        is_virtual: int = 0,
        create_form: Optional[int] = None,
        partial: int = 0,
    ) -> None:
        """Upsert an entity_edge row (re-materialise safe via ON CONFLICT DO UPDATE)."""
        self._conn.execute(
            "INSERT INTO entity_edge "
            "(src_id, dst_id, kind, count, via_member_id, multiplicity, "
            " access, is_virtual, create_form, partial) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(src_id, dst_id, kind, "
            "            COALESCE(via_member_id, -1), COALESCE(create_form, -1)) "
            "DO UPDATE SET "
            "  count       = excluded.count, "
            "  multiplicity = excluded.multiplicity, "
            "  access      = excluded.access, "
            "  is_virtual  = excluded.is_virtual, "
            "  create_form = COALESCE(excluded.create_form, entity_edge.create_form), "
            "  partial     = excluded.partial",
            (src_id, dst_id, kind, count, via_member_id, multiplicity,
             access, is_virtual, create_form, partial),
        )
        self._commit()

    def clear_entity_edges(self) -> None:
        """Delete all entity_edge rows (pre-step for idempotent re-materialise)."""
        self._conn.execute("DELETE FROM entity_edge")
        self._commit()

    def entity_edges(
        self,
        src_id: Optional[int] = None,
        dst_id: Optional[int] = None,
        kind: Optional[int] = None,
    ) -> list[dict]:
        """Return entity_edge rows as dicts, optionally filtered.

        All columns returned. Rows sorted by (src_id, kind, dst_id).
        """
        wheres: list[str] = []
        params: list[Any] = []
        if src_id is not None:
            wheres.append("src_id = ?")
            params.append(src_id)
        if dst_id is not None:
            wheres.append("dst_id = ?")
            params.append(dst_id)
        if kind is not None:
            wheres.append("kind = ?")
            params.append(kind)
        where_sql = ("WHERE " + " AND ".join(wheres)) if wheres else ""
        rows = self._conn.execute(
            f"SELECT src_id, dst_id, kind, count, via_member_id, multiplicity, "
            f"access, is_virtual, create_form, partial "
            f"FROM entity_edge {where_sql} "
            f"ORDER BY src_id, kind, dst_id",
            params,
        ).fetchall()
        return [dict(r) for r in rows]

    def rollup_edge_counts(self) -> None:
        """For calls (1) and uses (7): set count = COUNT(edge_site)."""
        self._conn.execute(
            "UPDATE edge SET count = ("
            "  SELECT COUNT(*) FROM edge_site WHERE edge_site.edge_id = edge.id"
            ") "
            "WHERE kind IN (1, 7)"
            "  AND EXISTS (SELECT 1 FROM edge_site WHERE edge_site.edge_id = edge.id)"
        )
        self._commit()

    def materialize_dispatch_calls(self) -> None:
        """Materialise virtual-dispatch caller edges (kind 18, ``dispatch_calls``).

        A static ``calls`` edge (1) into a virtual method B understates reality:
        at run time the call can land on any method that overrides B, transitively
        down the class hierarchy. libclang records the site against the *declared*
        target (e.g. ``execute() -> base::doSomething`` for a pure-virtual base),
        so ``callers(child::doSomething)`` -- the concrete override -- comes back
        empty even though ``execute`` reaches it via dynamic dispatch.

        For each ``caller -> B`` calls edge and each transitive override M of B,
        store a ``dispatch_calls`` edge ``caller -> M`` so ``callers(M,
        include_overrides=True)`` recovers the virtual caller in a single hop.
        The bridge is the existing ``overrides`` edge (6); no new extraction.

        Idempotent: kind-18 edges are deleted and rebuilt each pass (a re-run of
        ``resolve`` reflects any override/call changes). Sound over-approximation
        -- the mirror of ``dispatch_targets`` -- so every caller that could reach
        M at run time appears; type-based pruning is out of scope here.
        """
        self._conn.execute("DELETE FROM edge WHERE kind = 18")
        self._conn.execute(
            """
            WITH RECURSIVE dispatch(base_id, target_id) AS (
                -- direct override: target overrides base (overrides edge = 6,
                -- src = overriding method, dst = overridden base method)
                SELECT dst_id AS base_id, src_id AS target_id
                FROM edge WHERE kind = 6
              UNION
                -- transitive: target overrides a mid method already reachable
                -- from base, so a call to base can dispatch to target too
                SELECT d.base_id, o.src_id
                FROM dispatch d
                JOIN edge o ON o.dst_id = d.target_id AND o.kind = 6
            )
            INSERT OR IGNORE INTO edge (src_id, dst_id, kind, count)
            SELECT c.src_id, d.target_id, 18, c.count
            FROM edge c
            JOIN dispatch d ON d.base_id = c.dst_id
            WHERE c.kind = 1
              AND c.src_id != d.target_id
            """
        )
        self._commit()

    # -- v27: multi-definition (per-backend redefinition) --------------------
    # `definition` and `def_edge` are written at INDEX time (see clang/ast.py):
    # deriving them in resolve is impossible because delete_edges_for_file wipes
    # a losing backend's call edges from `edge` (the shared symbol's file_id
    # flips to the last-indexed TU), so the raw per-body edges must be captured
    # while each TU is live. Keyed by the backend body's file, they are immune
    # to that flip. resolve only counts them (set_multi_def) and fans them out
    # (materialize_possible_calls) -- both set-based and idempotent.
    #
    # Scope (the kinds a "redefinition" is tracked for): function, method,
    # constructor, destructor, and static member variables. Header-defined
    # classes/typedefs are excluded (an #include in many TUs is not redefinition).

    def get_or_create_definition(
        self,
        symbol_id: int,
        file_id: Optional[int],
        line: Optional[int] = None,
        col: Optional[int] = None,
        end_line: Optional[int] = None,
        end_col: Optional[int] = None,
        component_id: Optional[int] = None,
        init_text: Optional[str] = None,
    ) -> int:
        """Return the `definition` row id for this symbol's body in this
        (component, file), creating it if new. One row per backend body -- keyed
        by (component_id, file_id, symbol_id) so Server1::reg and Server2::reg
        (same USR, different files) stay distinct. component derived from file.
        `init_text` is a (static member) variable's initializer source per
        backend (NULL for functions)."""
        if component_id is None:
            component_id = self.component_id_for_file(file_id)
        cur = self._conn.execute(
            "INSERT INTO definition "
            "(symbol_id, component_id, file_id, line, col, end_line, end_col, "
            " init_text) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(component_id, file_id, symbol_id) DO UPDATE SET "
            "  line = excluded.line, col = excluded.col, "
            "  end_line = excluded.end_line, end_col = excluded.end_col, "
            "  init_text = excluded.init_text "
            "RETURNING id",
            (symbol_id, component_id, file_id, line, col, end_line, end_col,
             init_text),
        )
        row = cur.fetchone()
        if row is None:
            raise RuntimeError("get_or_create_definition: upsert returned no id")
        return row["id"]

    def component_id_for_file(self, file_id: Optional[int]) -> Optional[int]:
        """The component that owns this file (file -> directory -> component)."""
        if file_id is None:
            return None
        row = self._conn.execute(
            "SELECT d.component_id AS cid FROM file f "
            "JOIN directory d ON d.id = f.directory_id WHERE f.id = ?",
            (file_id,),
        ).fetchone()
        return row["cid"] if row is not None else None

    def add_def_edge(
        self, src_def_id: int, dst_id: int, kind: int, count: int = 1
    ) -> int:
        """Upsert a per-body outgoing edge (src is a definition). kind reuses
        edge_kind (1 calls / 7 uses). Returns the def_edge id."""
        cur = self._conn.execute(
            "INSERT INTO def_edge (src_def_id, dst_id, kind, count) "
            "VALUES (?, ?, ?, ?) "
            "ON CONFLICT(src_def_id, dst_id, kind) DO UPDATE SET "
            "  count = def_edge.count + excluded.count "
            "RETURNING id",
            (src_def_id, dst_id, kind, count),
        )
        row = cur.fetchone()
        if row is None:
            raise RuntimeError("add_def_edge: upsert returned no id")
        return row["id"]

    def copy_body_edges_to_def_edge(self, def_id: int, symbol_id: int) -> None:
        """Snapshot a function body's just-emitted calls/uses into def_edge.

        Called right after _body_descent for one function in one TU: at that
        instant `edge` holds exactly THIS TU's kind-1/7 edges for the symbol
        (delete_edges_for_file cleared the prior TU's). Copying them keyed by
        this backend's `def_id` preserves them even after a later TU re-index
        wipes the symbol's `edge` rows."""
        self._conn.execute(
            "INSERT INTO def_edge (src_def_id, dst_id, kind, count) "
            "SELECT ?, dst_id, kind, count FROM edge "
            "WHERE src_id = ? AND kind IN (1, 7) "
            "ON CONFLICT(src_def_id, dst_id, kind) DO UPDATE SET "
            "  count = excluded.count",
            (def_id, symbol_id),
        )
        self._commit()

    def delete_definitions_for_file(self, file_id: int) -> None:
        """Drop this file's definition rows (cascades def_edge) before re-index.
        Keyed on definition.file_id (the actual body file), so re-indexing one
        backend never disturbs another backend's rows."""
        self._conn.execute(
            "DELETE FROM definition WHERE file_id = ?", (file_id,)
        )
        self._commit()

    def set_multi_def(self) -> None:
        """Set symbol.multi_def = COUNT(definition rows). >1 == redefined."""
        self._conn.execute("UPDATE symbol SET multi_def = 0")
        self._conn.execute(
            "UPDATE symbol SET multi_def = "
            "  (SELECT COUNT(*) FROM definition d WHERE d.symbol_id = symbol.id) "
            "WHERE id IN (SELECT DISTINCT symbol_id FROM definition)"
        )
        self._commit()

    def materialize_possible_calls(self) -> None:
        """Rebuild `possible_call`: body->body fan-out. For each per-body call
        (def_edge kind 1) into a symbol with >1 definition, one row per candidate
        body of that callee. Requires set_multi_def() to have run."""
        self._conn.execute("DELETE FROM possible_call")
        self._conn.execute(
            """
            INSERT OR IGNORE INTO possible_call (src_def_id, dst_def_id, count)
            SELECT de.src_def_id, td.id, SUM(de.count)
            FROM def_edge de
            JOIN symbol s     ON s.id = de.dst_id
            JOIN definition td ON td.symbol_id = de.dst_id
            WHERE de.kind = 1 AND s.multi_def > 1
            GROUP BY de.src_def_id, td.id
            """
        )
        self._commit()

    def cross_repo_edges(self) -> list[tuple[int, int, int]]:
        """Return (src_id, dst_id, kind) for edges crossing component boundaries."""
        rows = self._conn.execute(
            "SELECT e.src_id, e.dst_id, e.kind FROM edge e "
            "JOIN symbol src ON src.id = e.src_id "
            "JOIN symbol dst ON dst.id = e.dst_id "
            "JOIN file sf ON sf.id = src.file_id "
            "JOIN directory sd ON sd.id = sf.directory_id "
            "JOIN file df ON df.id = dst.file_id "
            "JOIN directory dd ON dd.id = df.directory_id "
            "WHERE sd.component_id != dd.component_id"
        ).fetchall()
        return [(r[0], r[1], r[2]) for r in rows]

    def set_meta(self, key: str, value: str) -> None:
        """Upsert a meta row."""
        self._conn.execute(
            "INSERT INTO meta (key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
            (key, value),
        )
        self._commit()

    def _identity_files(self) -> list[dict[str, Any]]:
        rows = self._conn.execute(
            "SELECT f.id, c.name AS component_name, c.path AS component_path, "
            "c.kind AS component_kind, c.version, r.name AS repository_name, "
            "r.remote_url, d.path AS directory_path, f.name, "
            "f.compile_options, f.driver, f.indexed "
            "FROM file f JOIN directory d ON d.id = f.directory_id "
            "JOIN component c ON c.id = d.component_id "
            "LEFT JOIN repository r ON r.id = c.repository_id "
            "ORDER BY c.name, c.path, c.kind, COALESCE(c.version, '<null>'), "
            "COALESCE(r.name, '<null>'), COALESCE(r.remote_url, '<null>'), "
            "d.path, f.name"
        ).fetchall()

        def identity_field(value: Optional[str]) -> str:
            return value if value is not None else "<null>"

        return [
            {
                "id": row["id"],
                "key": (
                    "\0".join(
                        identity_field(row[column])
                        for column in (
                            "component_name", "component_path", "component_kind",
                            "version", "repository_name", "remote_url",
                        )
                    )
                    + f"\0{row['directory_path']}\0{row['name']}"
                ),
                "compile_options": row["compile_options"],
                "driver": row["driver"],
                "indexed": bool(row["indexed"]),
            }
            for row in rows
        ]

    def _source_manifest(
        self, files: list[dict[str, Any]],
    ) -> tuple[str, bool]:
        parts: list[str] = []
        complete = True
        for file in files:
            path = self.file_abs_path(file["id"]) or ""
            digest = _md5_of(path)
            if digest is None:
                complete = False
            parts.append(
                f"{file['key']}\0{digest or '<unreadable>'}\0"
                f"{'1' if file['indexed'] else '0'}\n"
            )
        return "".join(parts), complete

    @staticmethod
    def _config_manifest(files: list[dict[str, Any]]) -> str:
        return "".join(
            f"{file['key']}\0"
            f"{file['compile_options'] if file['compile_options'] is not None else '<null>'}\0"
            f"{file['driver'] if file['driver'] is not None else '<null>'}\n"
            for file in files
        )

    def index_identity(self) -> IndexIdentity:
        """Return stored identity and compare it with the current checkout."""
        files = self._identity_files()
        source, complete = self._source_manifest(files)
        source_fingerprint = hashlib.sha1(source.encode()).hexdigest()
        config_fingerprint = hashlib.sha1(
            self._config_manifest(files).encode()
        ).hexdigest()
        values = {}
        for key in (
            "source_revision", "source_fingerprint", "index_config",
            "index_config_fingerprint", "index_identity_version",
        ):
            row = self._conn.execute(
                "SELECT value FROM meta WHERE key = ?", (key,)
            ).fetchone()
            values[key] = row[0] if row is not None and row[0] else None
        freshness = "unverifiable"
        if (
            values["index_identity_version"] == "1"
            and values["source_revision"]
            and values["source_fingerprint"]
            and values["index_config"]
            and values["index_config_fingerprint"]
        ):
            if complete:
                freshness = (
                    "current"
                    if (
                        values["source_fingerprint"] == source_fingerprint
                        and values["source_revision"]
                        == f"content-sha1:{source_fingerprint}"
                        and values["index_config_fingerprint"]
                        == config_fingerprint
                        and all(file["indexed"] for file in files)
                    )
                    else "stale"
                )
        return IndexIdentity(
            schema_version=SCHEMA_VERSION,
            source_revision=values["source_revision"],
            source_fingerprint=values["source_fingerprint"],
            index_config=values["index_config"],
            index_config_fingerprint=values["index_config_fingerprint"],
            freshness=freshness,
        )

    def stamp_index_identity(self) -> None:
        """Persist the v1 identity after a successful indexing pass."""
        files = self._identity_files()
        source, complete = self._source_manifest(files)
        source_fingerprint = hashlib.sha1(source.encode()).hexdigest()
        config_fingerprint = hashlib.sha1(
            self._config_manifest(files).encode()
        ).hexdigest()
        self.set_meta("index_identity_version", "1")
        self.set_meta("index_config", "manifest-sha1-v1")
        self.set_meta("index_config_fingerprint", config_fingerprint)
        self.set_meta("source_fingerprint", source_fingerprint if complete else "")
        self.set_meta(
            "source_revision",
            f"content-sha1:{source_fingerprint}" if complete else "",
        )

    def resolve_pass(self) -> tuple[int, int]:
        """Roll up edge counts, materialise entity_edge, write graph_resolved_at meta.

        Returns (still_stub_count, cross_repo_edge_count).
        """
        from datetime import datetime, timezone
        from indexer.entity_rollup import materialize_entity_edges

        self.rollup_edge_counts()
        # v27: multi-definition. `definition`/`def_edge` are already written at
        # index time; here we just count them into symbol.multi_def and fan out
        # per-body calls into `possible_call`. Order: possible_call needs multi_def.
        self.set_multi_def()
        self.materialize_possible_calls()
        self.materialize_dispatch_calls()
        materialize_entity_edges(self)
        # A still-stub is a minted placeholder never backfilled by a real
        # symbol: resolved=0 with NO location (neither a definition nor a decl
        # site). NOT keyed on spelling -- stubs are now minted NAMED, so the
        # absence of any location is the robust signal (matches Sym.is_stub).
        row = self._conn.execute(
            "SELECT COUNT(*) FROM symbol "
            "WHERE resolved = 0 AND file_id IS NULL AND decl_file_id IS NULL"
        ).fetchone()
        stubs = row[0] if row else 0
        cross = self.cross_repo_edges()
        ts = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        self.set_meta("graph_resolved_at", ts)
        return stubs, len(cross)

    # -- stats -----------------------------------------------------------------

    def stats(self) -> dict[str, Any]:
        one = lambda sql: self._conn.execute(sql).fetchone()[0]  # noqa: E731
        by_kind = {
            SYMBOL_KIND_NAMES.get(r["kind"], r["kind"]): r["n"]
            for r in self._conn.execute(
                "SELECT kind, COUNT(*) AS n FROM symbol GROUP BY kind ORDER BY kind"
            )
        }
        return {
            "components": one("SELECT COUNT(*) FROM component"),
            "directories": one("SELECT COUNT(*) FROM directory"),
            "files": one("SELECT COUNT(*) FROM file"),
            "files_indexed": one("SELECT COUNT(*) FROM file WHERE indexed = 1"),
            "symbols": one("SELECT COUNT(*) FROM symbol"),
            "symbols_unresolved": one("SELECT COUNT(*) FROM symbol WHERE resolved = 0"),
            "symbols_by_kind": by_kind,
        }


class _Transaction:
    def __init__(self, db: Storage):
        self._db = db

    def __enter__(self):
        self._db._in_txn = True
        return self._db

    def __exit__(self, exc_type, exc, tb):
        self._db._in_txn = False
        if exc_type is None:
            self._db._conn.commit()
        else:
            self._db._conn.rollback()
        return False
