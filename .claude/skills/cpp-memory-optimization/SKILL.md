---
name: cpp-memory-optimization
description: Analyze C++ memory profiles and allocation hot spots with libclang plus GraphDB. Use for Valgrind Massif, heaptrack, leak or peak-memory reports, stack traces, container growth, allocation-heavy paths, and memory optimization planning.
---

# C++ Memory Optimization

## Operating Model

Use the runtime profile as the symptom, libclang as the compiler-accurate view of each hot C++ frame, and GraphDB as the repository-scale impact map.

Do not optimize from stack-frame names alone. Resolve symbols, inspect the real function bodies, then use the graph to rank impact and blast radius.

Hard restrictions:

- Never read C++ source files directly with `cat`, `sed`, editor open, ad hoc Python file reads, or similar raw file-reading commands.
- Never search C++ source files with `grep`, `rg`, IDE search, or ad hoc text scanning.
- Never grep/search for skills. Use only the skill that was triggered and its documented API surface.
- Never read anything under `scripts/` (`memory_nav.py` or the shared engine). It is a closed API — use it only through `reference/API_REFERENCE.md`, which is a complete substitute for the source.
- The only source text allowed in model context is a bounded snippet returned by `LibclangAPI` / `NavigationSession.snippet`.
- Use profile parsing, libclang APIs, IndraDB APIs, and SQLite navigation state instead of raw text inspection.

## Workflow

Import the `memory_nav` wrapper module and build a short task-specific script. Do not run fixed CLI flows and do not read the module source — every method's signature, return shape, and a worked example are in `reference/API_REFERENCE.md`, which is a complete substitute for the source.

`MemorySession` subclasses the shared navigation engine, so it has the full surface (`resolve_symbol`, `snippet`, `graph_lookup`, `lookahead`, `frontier`, `step`, `checkpoint`, `backtrack`, `record_profile`) plus a `triage` helper that records bounded profile frames + an optional hot symbol in SQLite and returns a small summary. It never fetches broad graph neighborhoods or full function bodies.

```python
import sys
sys.path.insert(0, ".claude/skills/cpp-memory-optimization/scripts")
from memory_nav import MemorySession

URI = "indradb://localhost:27615"
nav = MemorySession(compile_commands="build/compile_commands.json")

t = nav.triage(
    profile="/path/to/massif-or-stack.txt",
    project_root=".",
    source="src/hot.cc",
    name="Namespace::hotFunction",
)
body = nav.snippet("src/hot.cc", "Namespace::hotFunction", char_budget=2400)  # body["allocation_hints"]
nav.graph_lookup(URI, "qualified_name", "Namespace::hotFunction", focus=True, budget=4)
fan_in  = nav.lookahead(URI, direction="inbound",  edge_kind="CALLS", budget=8)
callees = nav.lookahead(URI, direction="outbound", edge_kind="CALLS", budget=8)
state   = nav.lookahead(URI, direction="outbound", edge_kind="USES",  budget=8)
nav.close()
```

Use explicit `budget` / `char_budget` on every exploratory call; navigate by focus and frontier rather than dumping all results.

1. Normalize the profile.
   - Extract project-owned stack frames.
   - Preserve the raw hot path in the answer.
   - Ignore allocator and STL frames except when they identify a specific container operation such as `reserve`, `resize`, `push_back`, `emplace_back`, string growth, map insertion, or shared-pointer control-block allocation.

2. Resolve hot frames with libclang.
   - Resolve exact USR, qualified name, signature, source file, line range, enclosing class/namespace, return type, parameters, and template arguments.
   - If a frame is ambiguous because of overloads, templates, inlining, or stripped symbols, list candidates and state which evidence disambiguates them.

3. Inspect hot function bodies with libclang.
   - Look for `new`, `delete`, `malloc`, `free`, `make_unique`, `make_shared`, container construction, container growth, string concatenation, map/set insertion, large local temporaries, large return-by-value values, large pass-by-value parameters, recursive calls, and allocations inside loops.
   - Record whether the allocation happens per call, per loop iteration, per translation unit, per object, per request, or globally.

4. Query GraphDB around each resolved symbol.
   - Exact node lookup by USR or qualified name.
   - Incoming `CALLS` to find who reaches the hot function.
   - Outgoing `CALLS` to find allocation-heavy callees.
   - `USES` edges and access kind to identify state read or written.
   - Containing class, namespace, module, and repo.
   - `INHERITS`, `OVERRIDES`, and `EXTERNAL_REF` when virtual dispatch or cross-repo usage affects blast radius.

5. Classify optimization patterns.
   - Repeated allocation in a loop.
   - Missing preallocation or poor capacity growth.
   - Temporary container built only to be iterated once.
   - Copy where move, reference, view, span, or iterator range would preserve behavior.
   - Shared ownership used where unique or borrowed ownership is enough.
   - Data retained longer than needed.
   - Cacheable computation or repeated parsing.
   - Hot call chain fan-in where one local change affects many callers.

6. Rank candidates.
   - Highest priority: direct Massif/heaptrack frame, high caller fan-in, allocation inside loop, low semantic risk.
   - Medium priority: architectural changes, ownership changes, cross-module behavior, cache introduction.
   - Low priority: cosmetic micro-optimizations without profile support.

7. Recommend the first patch.
   - Prefer the smallest behavior-preserving change with measurable memory impact.
   - Name the tests or profiling rerun needed to validate the change.

## Navigation Discipline

Keep the model context small:

- Store profile frames, symbol candidates, snippets, frontier, and checkpoints in `.agent-nav.sqlite3`.
- Use `snippet` for one bounded function body at a time.
- Use `indradb-lookup` once per selected symbol, then use `lookahead` from `current`.
- Use inbound `CALLS` for fan-in, outbound `CALLS` for allocation-heavy callees, and outbound `USES` for state touched.
- Use `checkpoint` before trying a risky optimization branch and `backtrack` when that branch is unhelpful.
- Treat `--budget` and `--char-budget` as explicit investigation budgets, not model context expansion permission.

## Output Contract

Structure the final answer as:

- Hot allocation path: raw profile stack and resolved project symbols.
- What allocates: libclang evidence from function bodies and AST context.
- Why it matters: GraphDB evidence about callers, callees, fan-in, data access, and subsystem ownership.
- Optimization opportunities: ranked candidates with impact, risk, and suggested change.
- Recommended first patch: the smallest change to try first.
- Validation: profiling rerun, unit/integration tests, and regression risks.

Separate evidence from inference. Label profile evidence, libclang evidence, GraphDB evidence, and engineering judgment.

## Safety

Never print credentials or connection tokens for GraphDB. Use existing environment variables or file-based secret references. Do not propose semantic changes unless the profile and code evidence support them.
