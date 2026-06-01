# cpp_agent_nav API Reference

Complete API surface for the `cpp_agent_nav` engine. This is a total substitute
for the module source — **do not read `scripts/cpp_agent_nav/*.py`**. Build small
task-specific scripts against the classes below.

## Import incantation

Every script starts with this (regenerate it each time):

```python
import sys
sys.path.insert(0, ".claude/skills/cpp-graph-code-reasoning/scripts")
from cpp_agent_nav import (
    NavigationSession, LibclangAPI, IndraDBAPI, NavStore, NodeRef, EdgeRef,
    DEFAULT_DB, DEFAULT_SESSION, DEFAULT_BUDGET, DEFAULT_SNIPPET_CHARS,
)
```

Defaults: `DEFAULT_DB=".agent-nav.sqlite3"`, `DEFAULT_SESSION="default"`,
`DEFAULT_BUDGET=12` (env `CPP_AGENT_NAV_BUDGET`), `DEFAULT_SNIPPET_CHARS=2400`
(env `CPP_AGENT_NAV_SNIPPET_CHARS`).

libclang is loaded lazily — set `LIBCLANG_LIBRARY_FILE` or `LIBCLANG_PATH`. The
IndraDB graph defaults to `indradb://localhost:27615` and needs the `indradb`
Python driver.

---

## NavigationSession

High-level orchestration. Owns the SQLite store and a `LibclangAPI`. This is the
default entry point.

```python
NavigationSession(db_path=".agent-nav.sqlite3", session="default",
                  compile_commands=None, default_budget=12)
```

`.store` exposes the `NavStore` (use `.store.observe(...)`, `.store.node(...)`);
`.libclang` exposes the `LibclangAPI`.

### Methods and return shapes

| Method | Signature | Returns |
|---|---|---|
| `init` | `init(repo_root=None)` | status dict (see `status`) |
| `status` | `status(budget=None)` | `{db, session, current, open_frontier, recent_observations}` — `current` is a `NodeRef.brief()` or `None` |
| `resolve_symbol` | `resolve_symbol(source, name, focus=False, budget=None)` | `list[NodeRef]` (libclang-sourced) |
| `snippet` | `snippet(source, name, char_budget=2400, target_key=None)` | snippet dict (below) |
| `graph_lookup` | `graph_lookup(uri, property_name, value, focus=False, budget=None)` | `list[NodeRef]` (indradb-sourced) |
| `lookahead` | `lookahead(uri, node_key="current", direction="outbound", edge_kind=None, reason=None, budget=None)` | lookahead dict (below) |
| `frontier` | `frontier(state="open", budget=None)` | `list[dict]` frontier rows |
| `step` | `step(frontier_id, checkpoint=None)` | `NodeRef \| None` (moves focus) |
| `checkpoint` | `checkpoint(label, note=None)` | `int` checkpoint id |
| `backtrack` | `backtrack(checkpoint_id)` | checkpoint row `dict` (restores focus) |
| `record_profile` | `record_profile(profile, project_root=None, budget=None)` | `list[frame dict]` (below) |
| `record_spec` | `record_spec(spec_path, budget=None)` | spec outline dict or `None` (below) |
| `close` | `close()` | — |

`direction` is `"outbound"` or `"inbound"`. `node_key="current"` resolves to the
session focus. `lookahead` requires the focused node to have a `graph_id`, so call
`graph_lookup(..., focus=True)` first.

### Return shapes

`NodeRef.brief()` (what `resolve_symbol` / `graph_lookup` items expose, and the
shape stored in the frontier/snapshot):

```python
{ "key", "graph_id", "usr", "kind", "qualified_name",
  "signature", "file_path", "line", "source" }
```
`source` is `"libclang"` or `"indradb"`. `key` is `usr:<usr>` or `graph:<id>` or a
hashed fallback. A `NodeRef` object also has these as attributes plus `.payload`.

`snippet(...)`:
```python
{ "snippet_id", "target_key", "file_path", "start_line", "end_line",
  "text", "truncated", "char_budget", "allocation_hints", "symbol" }
```
`allocation_hints` is `[{"kind", "count"}, ...]` (kinds: `new-expression`,
`malloc-family`, `make-unique`, `make-shared`, `container-growth`, `map-insert`,
`string-growth`, `loop`). `symbol` is a libclang cursor record dict.

`lookahead(...)`:
```python
{ "from", "stored_neighbors", "rows", "direction",
  "edge_kind", "fetched_count", "truncated", "budget" }
```
`stored_neighbors` is a list of `NodeRef.brief()` each with an added
`frontier_id`. `rows` carries the raw `{edge, node}` pairs.

`record_profile(...)` → list of frame dicts:
```python
{ "line", "function", "location", "project_owned", "raw" }
```

`record_spec(...)`:
```python
{ "path", "excerpt", "candidate_requirements", "truncated" }
```

`status(...)` / `init(...)`:
```python
{ "db", "session", "current", "open_frontier", "recent_observations" }
```

---

## LibclangAPI

Lower-level compiler-accurate API. `NavigationSession` wraps it; use directly only
for compile-arg discovery or raw cursor records.

```python
LibclangAPI(compile_commands=None)
```

| Method | Returns |
|---|---|
| `find_compile_commands(start)` *(staticmethod)* | `Path \| None` |
| `load_compile_commands(path)` *(classmethod)* | `list[dict]` |
| `compile_args_for_source(source, extra_args=())` | `list[str]` (driver + `-c/-o` and the source path stripped) |
| `resolve_symbols(source, name, budget=12)` | `list[dict]` cursor records |
| `find_function_cursor(source, name)` | clang cursor or `None` |
| `snippet(source, name, char_budget=2400)` | snippet dict (same shape as above, minus `snippet_id`/`target_key`) |

Cursor record dict (`resolve_symbols` items):
```python
{ "usr", "kind", "spelling", "displayname", "qualified_name",
  "signature", "type", "result_type", "location" }
```
`location`: `{file, line, column, start_line, start_column, end_line, end_column}`.
`resolve_symbols` matches kinds: FUNCTION_DECL, CXX_METHOD, CONSTRUCTOR,
DESTRUCTOR, FUNCTION_TEMPLATE, CLASS_DECL, STRUCT_DECL, FIELD_DECL, VAR_DECL,
ENUM_DECL, NAMESPACE. Name match: exact, `::`-suffix, or substring.

---

## IndraDBAPI

Read-only graph wrapper (constructing it pings the server).

```python
IndraDBAPI(uri)   # e.g. "indradb://localhost:27615"
```

| Method | Returns |
|---|---|
| `lookup_property(name, value, budget=12)` | `{rows, matched_count, truncated, budget}` |
| `neighbors(vertex_id, direction, edge_kind=None, budget=12)` | `{rows, direction, edge_kind, fetched_count, truncated, budget}` |

`lookup_property` rows: `[{id, t, properties}, ...]`. `neighbors` rows:
`[{edge:{outbound_id, inbound_id, t, properties}, node:{id, t, properties}}, ...]`.

Edge kinds in the graph: `CALLS` (inbound = callers, outbound = callees), `USES`
(data/type/state access), `HAS_PARAM`, `RETURNS`, `OF_TYPE`, `INHERITS`,
`OVERRIDES`, `INCLUDES`, `EXTERNAL_REF` (cross-repo).

---

## NavStore

SQLite bookkeeping. Reach it via `session.store`. Most scripts only need
`observe`, `node`, and `snapshot`; the rest back the session methods.

```python
NavStore(db_path=".agent-nav.sqlite3", session="default")
```

| Method | Returns |
|---|---|
| `node(key)` (`key="current"` ⇒ focus) | `NodeRef \| None` |
| `set_focus(key)` / `current_key()` | — / `str \| None` |
| `upsert_node(node)` | `NodeRef` |
| `add_edge(edge)` | — |
| `enqueue(from_key, to_key, edge_kind, direction, reason, priority=0)` | `int` frontier id |
| `frontier(state="open", budget=12)` | `list[dict]` |
| `mark_frontier(frontier_id, state)` | — |
| `checkpoint(label, note=None)` / `backtrack(checkpoint_id)` | `int` / `dict` |
| `observe(kind, summary, target_key=None, payload=None)` | `int` |
| `add_snippet(file_path, text, start_line, end_line, target_key)` | `int` |
| `snapshot(budget=12)` | status dict |
| `set_meta(key, value)` | — |

`NodeRef` fields: `key, graph_id, usr, kind, qualified_name, signature,
file_path, line, source, payload`; `.brief()` drops `payload`. Build from records
with `NodeRef.from_libclang(record)` / `NodeRef.from_indradb(row)`.
`EdgeRef(src_key, dst_key, kind, direction, payload=None)`.

---

## Module-level helpers

| Function | Returns |
|---|---|
| `allocation_hints(text)` | `[{kind, count}, ...]` |
| `profile_frames(profile, project_root=None, budget=12)` | `list[frame dict]` |
| `spec_outline(spec_path, budget=12)` | spec outline dict or `None` |

---

## Worked example — callers and one callee body

```python
import sys
sys.path.insert(0, ".claude/skills/cpp-graph-code-reasoning/scripts")
from cpp_agent_nav import NavigationSession

URI = "indradb://localhost:27615"
nav = NavigationSession(db_path=".agent-nav.sqlite3", session="query",
                        compile_commands="build/compile_commands.json")
nav.init(repo_root=".")

# 1. exact identity + body
nav.resolve_symbol("src/foo.cc", "Namespace::Class::method", focus=True, budget=8)
body = nav.snippet("src/foo.cc", "Namespace::Class::method", char_budget=2200)

# 2. anchor in the graph, then expand only what we need
nav.graph_lookup(URI, "qualified_name", "Namespace::Class::method", focus=True, budget=4)
callers = nav.lookahead(URI, direction="inbound", edge_kind="CALLS", budget=8)
callees = nav.lookahead(URI, direction="outbound", edge_kind="CALLS", budget=8)

for n in callers["stored_neighbors"]:
    print(n["qualified_name"], n["file_path"], n["line"])
nav.close()
```

Use explicit `budget` / `char_budget` on every exploratory call. Navigate by
focus + frontier; never request "all callers" unless the next step needs it.
