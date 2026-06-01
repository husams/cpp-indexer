# memory_nav API Reference

Complete API surface for the `memory_nav` module. This is a total substitute for
the module source — **do not read `scripts/memory_nav.py` or the engine under
`cpp-graph-code-reasoning/scripts/`**. Build small task-specific scripts against
the API below.

## Import incantation

```python
import sys
sys.path.insert(0, ".claude/skills/cpp-memory-optimization/scripts")
from memory_nav import MemorySession   # also: DEFAULT_DB, DEFAULT_BUDGET
```

`MemorySession` subclasses the shared `NavigationSession`, so it has the full
engine surface **plus** the `triage` helper. libclang is loaded lazily — set
`LIBCLANG_LIBRARY_FILE` or `LIBCLANG_PATH`. The IndraDB graph defaults to
`indradb://localhost:27615` and needs the `indradb` Python driver.

---

## MemorySession

```python
MemorySession(db_path=".agent-nav.sqlite3", session="memory",
              compile_commands=None, default_budget=12)
```

`.store` exposes the SQLite store (`.store.observe(...)`, `.store.node(...)`).

### triage (the memory-specific helper)

```python
triage(profile, project_root=None, source=None, name=None, focus=True, budget=None)
```
Parses a bounded set of profile frames (Massif / heaptrack / stack trace),
flags project-owned frames, and optionally resolves a named hot frame — all
stored in SQLite. Returns:
```python
{ "profile_summary": {
      "parsed_frames": int,
      "project_owned_frames": int,
      "top_project_frames": [ frame dict, ... ] },
  "resolved_hot_symbol_candidates": [ NodeRef.brief(), ... ],
  "snapshot": { "db", "session", "current", "open_frontier", "recent_observations" } }
```
frame dict: `{ "line", "function", "location", "project_owned", "raw" }`.

### Inherited engine methods (use these for evidence)

| Method | Signature | Returns |
|---|---|---|
| `record_profile` | `record_profile(profile, project_root=None, budget=None)` | `list[frame dict]` |
| `resolve_symbol` | `resolve_symbol(source, name, focus=False, budget=None)` | `list[NodeRef]` |
| `snippet` | `snippet(source, name, char_budget=2400, target_key=None)` | snippet dict (below) |
| `graph_lookup` | `graph_lookup(uri, property_name, value, focus=False, budget=None)` | `list[NodeRef]` |
| `lookahead` | `lookahead(uri, node_key="current", direction="outbound", edge_kind=None, reason=None, budget=None)` | lookahead dict (below) |
| `frontier` | `frontier(state="open", budget=None)` | `list[dict]` |
| `step` | `step(frontier_id, checkpoint=None)` | `NodeRef \| None` |
| `checkpoint` | `checkpoint(label, note=None)` | `int` |
| `backtrack` | `backtrack(checkpoint_id)` | `dict` (restores focus) |
| `status` | `status(budget=None)` | snapshot dict |
| `close` | `close()` | — |

`direction` is `"outbound"` or `"inbound"`. `lookahead` from `"current"` requires a
graph-anchored focus — call `graph_lookup(..., focus=True)` first.

### Return shapes

`NodeRef.brief()` (items of `resolve_symbol` / `graph_lookup`, and frontier rows):
```python
{ "key", "graph_id", "usr", "kind", "qualified_name",
  "signature", "file_path", "line", "source" }
```

`snippet(...)`:
```python
{ "snippet_id", "target_key", "file_path", "start_line", "end_line",
  "text", "truncated", "char_budget", "allocation_hints", "symbol" }
```
`allocation_hints` = `[{"kind", "count"}, ...]`; kinds: `new-expression`,
`malloc-family`, `make-unique`, `make-shared`, `container-growth`, `map-insert`,
`string-growth`, `loop`. This is the primary per-body allocation signal.

`lookahead(...)`:
```python
{ "from", "stored_neighbors", "rows", "direction",
  "edge_kind", "fetched_count", "truncated", "budget" }
```
`stored_neighbors` = list of `NodeRef.brief()` each with an added `frontier_id`.

Graph edge kinds for blast-radius: inbound `CALLS` = fan-in (who reaches the hot
function), outbound `CALLS` = allocation-heavy callees, outbound `USES` = state
touched, `INHERITS` / `OVERRIDES` = virtual-dispatch reach, `EXTERNAL_REF` =
cross-repo callers.

---

## Worked example — triage one hot frame

```python
import sys
sys.path.insert(0, ".claude/skills/cpp-memory-optimization/scripts")
from memory_nav import MemorySession

URI = "indradb://localhost:27615"
nav = MemorySession(compile_commands="build/compile_commands.json")

t = nav.triage(profile="/path/to/massif.txt", project_root=".",
               source="src/hot.cc", name="Namespace::hotFunction")
print(t["profile_summary"]["project_owned_frames"], "project frames")

body = nav.snippet("src/hot.cc", "Namespace::hotFunction", char_budget=2400)
print("alloc hints:", body["allocation_hints"])

nav.graph_lookup(URI, "qualified_name", "Namespace::hotFunction", focus=True, budget=4)
fan_in  = nav.lookahead(URI, direction="inbound",  edge_kind="CALLS", budget=8)
callees = nav.lookahead(URI, direction="outbound", edge_kind="CALLS", budget=8)
state   = nav.lookahead(URI, direction="outbound", edge_kind="USES",  budget=8)
nav.close()
```

Rank candidates by profile support × fan-in × in-loop allocation, then propose
the smallest behavior-preserving patch first. Use explicit budgets on every call.
