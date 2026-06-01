---
name: cpp-functional-analysis
description: Explain C++ functions or methods from a functional specification perspective with libclang and GraphDB. Use when a user provides requirements, acceptance criteria, behavior specs, or asks whether implementation matches intended behavior.
---

# C++ Functional Analysis

## Operating Model

Start from the functional specification, then use libclang and GraphDB as evidence for how the implementation realizes that specification.

The goal is not to narrate every line of code. The goal is to explain the function's domain behavior, requirement coverage, side effects, gaps, and risks.

Hard restrictions:

- Never read C++ source files directly with `cat`, `sed`, editor open, ad hoc Python file reads, or similar raw file-reading commands.
- Never search C++ source files with `grep`, `rg`, IDE search, or ad hoc text scanning.
- Never grep/search for skills. Use only the skill that was triggered and its documented API surface.
- Never read anything under `scripts/` (`functional_nav.py` or the shared engine). It is a closed API — use it only through `reference/API_REFERENCE.md`, which is a complete substitute for the source.
- The only source text allowed in model context is a bounded snippet returned by `LibclangAPI` / `NavigationSession.snippet`.
- Use functional spec parsing, libclang APIs, IndraDB APIs, and SQLite navigation state instead of raw text inspection.

## Workflow

Import the `functional_nav` wrapper module and build a short task-specific script. Do not run fixed CLI flows and do not read the module source — every method's signature, return shape, and a worked example are in `reference/API_REFERENCE.md`, which is a complete substitute for the source.

`FunctionalSession` subclasses the shared navigation engine, so it has the full surface (`resolve_symbol`, `snippet`, `graph_lookup`, `lookahead`, `frontier`, `step`, `checkpoint`, `backtrack`, `record_spec`) plus a `spec_context` helper that records a bounded spec outline + optional implementation symbol in SQLite and returns a small plan. It never fetches broad graph context or full code bodies.

```python
import sys
sys.path.insert(0, ".claude/skills/cpp-functional-analysis/scripts")
from functional_nav import FunctionalSession

URI = "indradb://localhost:27615"
nav = FunctionalSession(compile_commands="build/compile_commands.json")

ctx = nav.spec_context(
    spec="/path/to/functional-spec.md",
    source="src/foo.cc",
    name="Namespace::Class::method",
)
body = nav.snippet("src/foo.cc", "Namespace::Class::method", char_budget=2400)
nav.graph_lookup(URI, "qualified_name", "Namespace::Class::method", focus=True, budget=4)
delegated    = nav.lookahead(URI, direction="outbound", edge_kind="CALLS", budget=8)
side_effects = nav.lookahead(URI, direction="outbound", edge_kind="USES", budget=8)
usage        = nav.lookahead(URI, direction="inbound",  edge_kind="CALLS", budget=8)
nav.close()
```

Use explicit `budget` / `char_budget` on every exploratory call; navigate by focus and frontier rather than dumping all results.

1. Extract the expected behavior.
   - Identify requirements, acceptance criteria, input conditions, outputs, state transitions, permissions, validation rules, error behavior, and boundary cases from the functional specification.
   - Preserve ambiguity. Do not invent requirements that are not in the spec.

2. Resolve the exact function or method.
   - Use libclang to identify USR, qualified name, signature, return type, parameters, source location, enclosing class/namespace, and method qualifiers.
   - If the name is ambiguous because of overloads, templates, constructors, operators, or inheritance, list candidates and select only when evidence supports it.

3. Inspect implementation semantics with libclang.
   - Read the function body, branches, early returns, loops, local variables, referenced declarations, called functions, type conversions, exceptions, output parameters, and state mutations.
   - Map parameters and return values to domain concepts from the spec.

4. Query GraphDB for functional context.
   - Find callers to understand when this behavior is invoked.
   - Find callees to identify behavior delegated to other functions.
   - Find `USES` read/write edges to identify domain state read or modified.
   - Find containing class/module to understand ownership of the behavior.
   - Check `INHERITS` and `OVERRIDES` for virtual behavior, interface contracts, and polymorphic dispatch.
   - Check `EXTERNAL_REF` when behavior crosses repository boundaries.

5. Build a requirement-to-code map.
   - For each relevant requirement, cite implementation evidence and mark status as implemented, partially implemented, not implemented, ambiguous, or implemented elsewhere.
   - If a callee implements part of the requirement, say so and name the dependency.

6. Identify mismatches and gaps.
   - Spec says one thing but code appears to do another.
   - Spec is silent but code makes a meaningful assumption.
   - Behavior depends on caller preconditions not stated in the spec.
   - Error, null, empty, overflow, permission, concurrency, or lifetime cases are not covered.

## Functional Explanation Output

Use this structure unless the user requests another format:

```text
## Functional Purpose
What user-visible or domain capability this function provides.

## Function Identity
Qualified name, signature, return type, location, kind, and relevant qualifiers.

## Specification Mapping
For each requirement:
- Requirement:
- Implementation evidence:
- Status:
- Notes:

## Functional Flow
Step-by-step domain workflow, not line-by-line code narration.

## Inputs In Functional Terms
Domain meaning, valid values, boundary values, ownership/lifetime/nullability if relevant.

## Outputs And Effects
Return value, state changes, output parameters, emitted events, persistence, logging, I/O, external calls.

## Business Or Domain Rules
Rules enforced by this function and rules delegated to callees.

## Callers And Usage
Who invokes this function and what scenario that represents.

## Gaps Or Mismatches
Spec/code mismatches, ambiguity, missing edge cases, and risks.

## Final Mental Model
One concise paragraph: this function implements X by doing Y, depends on Z, and changing it affects W.
```

## Evidence Discipline

Separate direct facts from inference:

- Spec evidence: quote or paraphrase the relevant requirement.
- libclang evidence: signature, types, branches, calls, side effects, source range.
- GraphDB evidence: callers, callees, read/write state, ownership, inheritance, cross-repo links.
- Inference: the functional interpretation built from the evidence.

If evidence is missing, say what is missing and what would be needed to confirm the behavior.

Keep context small:

- Use SQLite navigation state instead of carrying all query results in the prompt.
- Use `snippet` for the selected function body only.
- Use outbound `CALLS` to identify delegated functional behavior.
- Use outbound `USES` to identify state and side effects.
- Use inbound `CALLS` to infer usage scenarios.
- Use `checkpoint` before exploring another overload or alternate requirement interpretation.

## Safety

Never expose secrets or GraphDB credentials. Do not claim requirement compliance unless the mapping is supported by code and graph evidence.
