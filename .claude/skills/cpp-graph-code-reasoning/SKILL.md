---
name: cpp-graph-code-reasoning
description: Answer repository-scale C++ code questions using libclang and GraphDB. Use for callers, callees, data access, inheritance, overrides, includes, templates, bug localization, impact analysis, API migration, and architecture questions.
---

# C++ Graph Code Reasoning

## Operating Model

Use libclang for exact C++ semantics and GraphDB for repository-scale structure.

- libclang answers: What is this symbol exactly? What does this function body contain? What are the types, declarations, references, and source ranges?
- GraphDB answers: How is this symbol connected to the rest of the codebase? Who calls it, what does it call, what data does it touch, what owns it, and what depends on it?

Hard restrictions:

- Never read C++ source files directly with `cat`, `sed`, editor open, ad hoc Python file reads, or similar raw file-reading commands.
- Never search C++ source files with `grep`, `rg`, IDE search, or ad hoc text scanning.
- Never grep/search for skills. Use only the skill that was triggered and its documented API surface.
- Never read anything under `scripts/` (the `cpp_agent_nav` module or any other `.py` there). It is a closed API — use it only through `reference/API_REFERENCE.md`, which is a complete substitute for the source.
- The only source text allowed in model context is a bounded snippet returned by `LibclangAPI` / `NavigationSession.snippet`.
- Prefer structural evidence from libclang, IndraDB, and SQLite navigation state over text search.

## Workflow

Import the `cpp_agent_nav` wrapper module and build small task-specific scripts around it. Do not run fixed CLI flows and do not read the module source — the full API (signatures + return shapes + worked example) is in `reference/API_REFERENCE.md`, which is a complete substitute for the source.

```python
import sys
from pathlib import Path

sys.path.insert(0, str(Path(".claude/skills/cpp-graph-code-reasoning/scripts").resolve()))

from cpp_agent_nav import NavigationSession, LibclangAPI, IndraDBAPI, NavStore

nav = NavigationSession(
    db_path=".agent-nav.sqlite3",
    session="query",
    compile_commands="build/compile_commands.json",
)
nav.init(repo_root=".")

candidates = nav.resolve_symbol("src/foo.cc", "Namespace::Class::method", focus=True, budget=8)
snippet = nav.snippet("src/foo.cc", "Namespace::Class::method", char_budget=2200)
graph_nodes = nav.graph_lookup(
    "indradb://localhost:27615",
    "qualified_name",
    "Namespace::Class::method",
    focus=True,
    budget=4,
)
callers = nav.lookahead(
    "indradb://localhost:27615",
    node_key="current",
    direction="inbound",
    edge_kind="CALLS",
    budget=8,
)
```

Core APIs:

- `NavigationSession`: high-level orchestration API for agent-built scripts. Owns SQLite state and exposes `resolve_symbol`, `snippet`, `graph_lookup`, `lookahead`, `frontier`, `step`, `checkpoint`, `backtrack`, `record_profile`, and `record_spec`.
- `LibclangAPI`: lower-level libclang API for compile-command discovery, exact symbol resolution, function cursor lookup, bounded snippets, and allocation hints.
- `IndraDBAPI`: lower-level read-only graph API for property lookup and one-hop neighbor traversal.
- `NavStore`: SQLite state API for focus, observations, snippets, frontier, checkpoints, backtracking, nodes, and edges.

The API must be used as a navigator, not as a context dumper: every call returns a small result and stores larger bookkeeping in SQLite.

See `reference/API_REFERENCE.md` for every method's signature, return shape, and a worked end-to-end example. Build a short Python script per investigation rather than reusing a fixed flow.

Use explicit budgets (`budget`, `char_budget`) for every exploratory call. Do not ask for "all callers" or "all vertices" unless the next step genuinely requires it; navigate by focus and frontier.

A CLI, `scripts/cpp_structure_api.py`, exists only as a smoke test to exercise the engine after edits. It is not part of the agent workflow; reason through the imported module instead.

IndraDB support uses the Python `indradb` driver and the same JSON-query model used by the sibling `cpp-mcp` project: property lookup once, then `pipe`-style one-hop traversal by vertex UUID. If the Python driver is missing, install the graphdb-indradb extra in the active environment before using graph traversal.

1. Convert the user's question into retrieval intents.
   - Symbol identity: exact class/function/method/field/type.
   - Call relationships: callers, callees, call paths.
   - Data relationships: fields/globals read or written.
   - Type relationships: inheritance, overrides, virtual dispatch, templates, specializations.
   - Ownership relationships: class, namespace, module, repo, includes.
   - Change impact: affected callers, subclasses, users, and cross-repo references.

2. Resolve important symbols.
   - Use libclang when the question names a C++ symbol, source location, stack frame, overload, template, method, constructor, operator, or field.
   - Use GraphDB exact lookup by USR when available. Fall back to qualified name only when USR is unavailable.
   - If ambiguous, list candidates and continue with the best-supported candidate.

3. Query GraphDB iteratively.
   - Prefer several small targeted graph queries over one broad query.
   - Inspect schema first if labels, edge names, or properties are uncertain.
   - Use SQLite state to preserve focus, observations, snippets, frontier items, and checkpoints.
   - Expand only from relevant nodes using `lookahead`.
   - Use incoming and outgoing edges to test both "who depends on this" and "what this depends on".

4. Inspect source selectively.
   - Read only source files and function bodies needed to confirm behavior.
   - Use libclang source ranges for exact bodies when possible.
   - Use graph results to choose what to inspect next.

5. Synthesize an answer.
   - Explain what is known, what was queried, and what remains uncertain.
   - Cite concrete symbols, files, and relationships.
   - Separate graph facts, libclang facts, and inference.

## Common Graph Query Shapes

Prefer the wrapper commands above to direct graph queries. For IndraDB, use `indradb-lookup` by `qualified_name`, then `lookahead` by edge type:

- inbound `CALLS`: callers.
- outbound `CALLS`: callees.
- outbound `USES`: data/type usage and state access.
- outbound `HAS_PARAM`, `RETURNS`, `OF_TYPE`: signature/type reasoning.
- inbound/outbound `OVERRIDES` and `INHERITS`: virtual dispatch and object model.
- `EXTERNAL_REF`: cross-repo impact.

Use `frontier`, `step`, `checkpoint`, and `backtrack` to manage the exploration path.

## Task Patterns

### Explain A Function Or Method

Resolve the exact symbol, inspect the body with libclang, then query callers, callees, data access, class/module ownership, and overrides. Explain both the local behavior and repository role.

### Answer "Where Is This Used?"

Query incoming `CALLS`, `USES`, `INCLUDES`, `INHERITS`, `OVERRIDES`, and `EXTERNAL_REF` edges as appropriate. Group results by subsystem or repo instead of returning a flat dump.

### Do Impact Analysis

Start from the changed symbol. Expand to callers, subclasses, overrides, read/write users, includes, tests, and cross-repo references. Rank impact by directness and confidence.

### Localize A Bug

Start from the symptom symbol, stack frame, failing test, or error message. Resolve candidates, walk callers/callees and state writes, then inspect the smallest set of source bodies needed to explain the failure path.

### Plan An API Migration

Find all call sites and type users, group by signature/overload/template specialization, identify mechanical edits versus semantic edits, and call out cross-repo references.

## Output Contract

Use a compact answer by default:

- Direct answer.
- Evidence: key libclang facts, focused snippets, and GraphDB relationships found through navigation.
- Reasoning: how the evidence supports the answer.
- Uncertainty: missing graph edges, ambiguous overloads, stale index, or code not indexed.
- Next action: source files or patches to inspect/change if the user is asking for implementation work.

## Safety

Never expose GraphDB credentials or tokens. If project instructions require wiki-first lookup before factual project answers, follow those instructions before querying code or the graph.
