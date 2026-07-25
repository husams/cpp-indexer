#include "storage/storage_schema.hpp"

namespace cidx::detail {

// Schema v6 — exact text from design §4 (= Python's expanded _SCHEMA, kinds
// in sorted order).
extern const char *const kSchema = R"sql(
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);

-- v35: explicit declared program/dependency universes. The numeric id is
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
    callable_kind TEXT,
    template_origin TEXT,
    template_form TEXT,
                                              -- instantiation node (own USR,
                                              -- definition via `instantiates` edge)
    is_named_instance INTEGER NOT NULL DEFAULT 0, -- v20: instance minted from a
                                              -- NAMED using/typedef alias (X<B>);
                                              -- carries its own composes/
                                              -- aggregates/associates (T->B)
                                              -- instead of collapsing onto the
                                              -- primary
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
  (2,'struct'), (3,'union'), (4,'class'), (5,'enum'), (6,'member'), (7,'enum-constant'), (8,'function'), (9,'variable'), (20,'typedef'), (21,'method'), (22,'namespace'), (24,'constructor'), (25,'destructor'), (30,'function-template'), (31,'class-template'), (36,'type-alias'), (501,'macro');

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
  -- definitional X -> alias_of -> Y. Mirrors type_edge alias_of(3) and the
  -- symbol_type 'underlying' relation on the type tier.
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

-- v35: one lossless dictionary row for every identity that cannot resolve to a
-- local symbol/type row. Resolved identities use the integer FK on hot rows;
-- unresolved values remain exactly recoverable here and are never confused
-- with NULL (absent evidence).
CREATE TABLE IF NOT EXISTS external_identity (
    id               INTEGER PRIMARY KEY,
    identity_kind    INTEGER NOT NULL CHECK (identity_kind IN (1, 2, 3)),
    identity_text    TEXT NOT NULL,
    resolution_status INTEGER NOT NULL DEFAULT 0
                     CHECK (resolution_status IN (0, 1)),
    symbol_id        INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
    type_id          INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
    UNIQUE (identity_kind, identity_text)
);
CREATE INDEX IF NOT EXISTS idx_external_identity_symbol
    ON external_identity(symbol_id);
CREATE INDEX IF NOT EXISTS idx_external_identity_type
    ON external_identity(type_id);

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
CREATE INDEX IF NOT EXISTS idx_edge_site_recv_type_identity
    ON edge_site(recv_type_identity_id);
CREATE INDEX IF NOT EXISTS idx_edge_site_recv_decl_identity
    ON edge_site(recv_decl_identity_id);
CREATE INDEX IF NOT EXISTS idx_call_arg_type_identity
    ON call_arg(type_identity_id);
CREATE INDEX IF NOT EXISTS idx_call_arg_decl_identity
    ON call_arg(decl_identity_id);
CREATE INDEX IF NOT EXISTS idx_call_arg_callee_identity
    ON call_arg(callee_identity_id);

-- Compatibility surfaces reconstruct stable USR/type-key identities for
-- readers while the physical occurrence rows carry only compact IDs.
CREATE VIEW IF NOT EXISTS edge_site_read AS
SELECT es.edge_id, es.file_id, es.line, es.col, es.conditional, es.args_sig,
       COALESCE(es.recv_src_kind,
                CASE es.recv_src_kind_id
                  WHEN 1 THEN 'literal' WHEN 2 THEN 'local'
                  WHEN 3 THEN 'construct' WHEN 4 THEN 'member'
                  WHEN 5 THEN 'global' WHEN 6 THEN 'call_result'
                  WHEN 7 THEN 'this' WHEN 8 THEN 'unknown' END)
           AS recv_src_kind,
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
       COALESCE(ca.src_kind,
                CASE ca.src_kind_id
                  WHEN 1 THEN 'literal' WHEN 2 THEN 'local'
                  WHEN 3 THEN 'construct' WHEN 4 THEN 'member'
                  WHEN 5 THEN 'global' WHEN 6 THEN 'call_result'
                  WHEN 7 THEN 'this' WHEN 8 THEN 'unknown' END)
           AS src_kind,
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
-- Entity = a symbol whose kind is in {class,struct,union,enum}; no separate table.
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
-- INSERT ... ON CONFLICT upserts in the materialise pass would silently fan out
-- into duplicate rows on every materialise.  A COALESCE expression index folds
-- NULL to a sentinel so the identity is NULL-safe; create_form is part of the
-- key so distinct creates/destroys forms (value/temp/heap/...) stay separate.
CREATE UNIQUE INDEX IF NOT EXISTS idx_entity_edge_identity ON entity_edge(
    src_id, dst_id, kind,
    COALESCE(via_member_id, -1), COALESCE(create_form, -1)
);
CREATE INDEX IF NOT EXISTS idx_entity_edge_src  ON entity_edge(src_id, kind);
CREATE INDEX IF NOT EXISTS idx_entity_edge_dst  ON entity_edge(dst_id, kind);

-- ---- v22: entity-node type (Layer-1 design-entity classification) -----------
-- The *type of an entity node* in the UML/abstraction graph, materialised at
-- `cidx resolve` alongside entity_edge. Mirrors storage.py byte-identically.
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

-- ---- v30/v32: signature/type tier (parameters + normalized types) ------------
-- Makes callable signatures traversable instead of strings/coarse uses edges:
-- which callables accept/return T, where T is used by pointer/reference/alias,
-- which aliases lead to a canonical type. Populated by the LibTooling decl
-- pass; the retired Python indexer never writes these tables (read-only parity).

-- type_node.kind metadata (display only, same pattern as symbol_kind/edge_kind).
CREATE TABLE IF NOT EXISTS type_kind (
    id   INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);
INSERT OR IGNORE INTO type_kind (id, name) VALUES
  (1,'builtin'), (2,'record'), (3,'enum'), (4,'alias'),
  (5,'pointer'), (6,'lvalue-reference'), (7,'rvalue-reference'),
  (8,'array'), (9,'function'), (10,'template-param'), (11,'other'),
  (12,'member-data-pointer'), (13,'member-function-pointer'),
  (14,'pack-expansion');

-- One row per distinct type SHAPE. Identity is type_key -- a deterministic
-- structural encoding of the Clang type (grammar in ast/type_graph.cpp), NOT
-- the printed spelling (aliases/qualifiers/layers need structure). spelling is
-- display-only. decl_usr names the record/enum/typedef declaration this layer
-- resolves to (NULL for builtins/pointers/...) -- the reverse-query anchor.
-- canonical_id links a sugared node to its canonical shape (NULL when the node
-- is itself canonical). Interned rows are never deleted on re-index (append-
-- only dictionary; orphans are harmless and bounded by distinct shapes seen).
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
    canonical_id INTEGER REFERENCES type_node(id) ON DELETE SET NULL,
    extent      TEXT
);
CREATE INDEX IF NOT EXISTS idx_type_node_decl_usr ON type_node(decl_usr);
CREATE INDEX IF NOT EXISTS idx_type_node_decl_id ON type_node(decl_id);
CREATE INDEX IF NOT EXISTS idx_type_node_canonical ON type_node(canonical_id);

-- Structural relations BETWEEN type nodes: pointee(1) pointer/reference ->
-- inner, element_type(2) array -> element, alias_of(3) alias -> one-step
-- target, return_type(4) function type -> return, param_type(5) function type
-- -> param at `position`, template_argument_type(6) specialization -> type arg
-- at `position`. position is 0 except param_type/template_argument_type.
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

-- One row per parameter of a callable symbol. Identity is (owner, position);
-- a parameter USR is NOT the key (Clang cannot mint a useful one for every
-- parameter). name/site are optional attributes. Refreshed wholesale per owner
-- on re-index (replace_parameters), so arity changes leave no stale rows.
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

-- Symbol -> type relations: returns(1) callable -> return type (ctors/dtors
-- have none), of_type(2) variable/field -> declared type, underlying_type(3)
-- typedef/alias -> target type. One row per (symbol, relation).
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
-- Preprocessing facts live in their OWN file domain: `edge` is symbol->symbol
-- and cannot hold a file->file relation. Collapsed edge + per-directive site,
-- mirroring the edge/edge_site pattern.

-- Stable identity for one normalized compilation configuration. A TU has one
-- compile command, so in practice this is one row per indexed TU; the digest
-- makes plan freshness checkable without re-reading the compile database.
-- digest = sha1 over driver \n working_dir \n lang_mode \n resource_dir \n
-- each argument, in order (see include_config_digest in include_facts.cpp).
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

-- One collapsed row per (source file, resolved target, configuration).
-- dst_file_id is NULL for system/unowned/unresolved targets; dst_path is the
-- path AS OPENED (never symlink-resolved), or the written spelling when the
-- directive did not resolve.
-- v35 adds a shared normalized descriptor and per-file applicability rows;
-- v36 adds configuration-qualified semantic fact generations.
CREATE TABLE IF NOT EXISTS translation_unit_config (
    id                    INTEGER PRIMARY KEY,
    descriptor_hash       TEXT NOT NULL UNIQUE,
    descriptor_json       TEXT NOT NULL,
    driver                TEXT,
    working_dir           TEXT,
    language              TEXT,
    standard              TEXT,
    target                TEXT,
    abi_options           TEXT NOT NULL,
    sysroot               TEXT,
    resource_dir          TEXT,
    include_paths         TEXT NOT NULL,
    macro_state           TEXT NOT NULL,
    relevant_environment  TEXT NOT NULL,
    generated_inputs      TEXT NOT NULL,
    diagnostics_policy    TEXT,
    arguments             TEXT NOT NULL,
    state                 TEXT NOT NULL DEFAULT 'registered'
                          CHECK (state IN ('registered','unregistered',
                                            'ambiguous','stale','unavailable'))
);
CREATE INDEX IF NOT EXISTS idx_translation_unit_config_hash
    ON translation_unit_config(descriptor_hash);

CREATE TABLE IF NOT EXISTS translation_unit (
    id        INTEGER PRIMARY KEY,
    file_id   INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    config_id INTEGER NOT NULL REFERENCES translation_unit_config(id)
              ON DELETE CASCADE,
    state     TEXT NOT NULL DEFAULT 'registered'
              CHECK (state IN ('registered','unregistered','ambiguous',
                               'stale','unavailable')),
    UNIQUE (file_id, config_id)
);

CREATE TABLE IF NOT EXISTS file_config (
    file_id   INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    config_id INTEGER NOT NULL REFERENCES translation_unit_config(id)
              ON DELETE CASCADE,
    role      TEXT NOT NULL CHECK (role IN ('translation_unit','header')),
    state     TEXT NOT NULL DEFAULT 'registered'
              CHECK (state IN ('registered','unregistered','ambiguous',
                               'stale','unavailable')),
    reason    TEXT,
    PRIMARY KEY (file_id, config_id, role)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_file_config_config ON file_config(config_id);

-- v36: every extracted or derived semantic fact is qualified by the file and
-- normalized configuration generation that produced it.  fact_kind keeps the
-- relation polymorphic while fact_id remains the native row id; the source
-- file and configuration FKs make replacement/cleanup precise.
CREATE TABLE IF NOT EXISTS fact_applicability (
    fact_kind  TEXT NOT NULL,
    fact_id    INTEGER NOT NULL,
    file_id    INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    config_id  INTEGER NOT NULL REFERENCES translation_unit_config(id)
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

-- One row per directive occurrence. begin_offset/end_offset bound the exact
-- removal range (hash through end-of-line) in the source file's buffer;
-- replacements are applied by ORIGINAL offsets, never by line renumbering.
-- cond_fingerprint is '' at unconditional top level, else a digest of the
-- enclosing #if/#elif/#else region stack (see ConditionalTracker).
-- guarded=1 only when the target carries #pragma once or a recognized include
-- guard -- the precondition for automatic duplicate classification.
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

-- Non-symbol dependency: `src_file_id` expanded macro `name`, whose definition
-- lives in `def_path`. A symbol-unused include that supplies a macro the source
-- expands is real usage the reference graph cannot see -- it downgrades the
-- candidate to manual_review and is never auto-removed.
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

INSERT OR IGNORE INTO meta (key, value) VALUES ('schema_version', '40');
)sql";

// v2 -> v3 qual_name backfill — verbatim from storage.py:231-244: the longest
// stored parent_usr chain per symbol is the full qualified path; empty parent
// spellings (anonymous scopes) are skipped.
extern const char *const kQualNameBackfill = R"sql(
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
)sql";

} // namespace cidx::detail
