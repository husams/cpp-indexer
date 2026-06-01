# functional_nav API Reference

Complete API surface for the `functional_nav` module. This is a total substitute
for the module source — **do not read `scripts/functional_nav.py` or the engine
under `cpp-graph-code-reasoning/scripts/`**. Build small task-specific scripts
against the API below.

## Import incantation

```python
import sys
sys.path.insert(0, ".claude/skills/cpp-functional-analysis/scripts")
from functional_nav import FunctionalSession   # also: DEFAULT_DB, DEFAULT_BUDGET
```

`FunctionalSession` subclasses the shared `NavigationSession`, so it has the full
engine surface **plus** the `spec_context` helper. libclang is loaded lazily —
set `LIBCLANG_LIBRARY_FILE` or `LIBCLANG_PATH`. The IndraDB graph defaults to
`indradb://localhost:27615` and needs the `indradb` Python driver.

---

## FunctionalSession

```python
FunctionalSession(db_path=".agent-nav.sqlite3", session="functional",
                  compile_commands=None, default_budget=12)
```

`.store` exposes the SQLite store (`.store.observe(...)`, `.store.node(...)`).

### spec_context (the functional-specific helper)

```python
spec_context(spec=None, source=None, name=None, focus=True, budget=None)
```
Records a bounded spec outline and (optionally) resolves the implementation
symbol, storing both in SQLite. Returns:
```python
{ "spec_outline": { "path", "excerpt", "candidate_requirements", "truncated" } | None,
  "resolved_symbol_candidates": [ NodeRef.brief(), ... ],
  "snapshot": { "db", "session", "current", "open_frontier", "recent_observations" } }
```

### Inherited engine methods (use these for evidence)

| Method | Signature | Returns |
|---|---|---|
| `record_spec` | `record_spec(spec_path, budget=None)` | `{path, excerpt, candidate_requirements, truncated}` or `None` |
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

`lookahead(...)`:
```python
{ "from", "stored_neighbors", "rows", "direction",
  "edge_kind", "fetched_count", "truncated", "budget" }
```
`stored_neighbors` = list of `NodeRef.brief()` each with an added `frontier_id`.

Graph edge kinds for functional evidence: outbound `CALLS` = delegated behavior,
outbound `USES` = state read/written, inbound `CALLS` = usage scenarios,
`INHERITS` / `OVERRIDES` = contract / polymorphic dispatch, `EXTERNAL_REF` =
cross-repo behavior.

---

## Worked example — spec coverage of one method

```python
import sys
sys.path.insert(0, ".claude/skills/cpp-functional-analysis/scripts")
from functional_nav import FunctionalSession

URI = "indradb://localhost:27615"
nav = FunctionalSession(compile_commands="build/compile_commands.json")

ctx = nav.spec_context(
    spec="/path/to/spec.md",
    source="src/foo.cc",
    name="Namespace::Class::method",
)
for req in (ctx["spec_outline"] or {}).get("candidate_requirements", []):
    print("REQ:", req)

body = nav.snippet("src/foo.cc", "Namespace::Class::method", char_budget=2400)
nav.graph_lookup(URI, "qualified_name", "Namespace::Class::method", focus=True, budget=4)
delegated   = nav.lookahead(URI, direction="outbound", edge_kind="CALLS", budget=8)
side_effects = nav.lookahead(URI, direction="outbound", edge_kind="USES", budget=8)
usage        = nav.lookahead(URI, direction="inbound",  edge_kind="CALLS", budget=8)
nav.close()
```

Map each requirement to code/graph evidence and mark status (implemented /
partial / not / ambiguous / elsewhere). Use explicit budgets on every call.
