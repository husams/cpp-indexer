# ADR-011: CIDX module architecture and dependency rules

- Status: accepted for incremental enforcement
- Date: 2026-07-23
- Owners: CIDX maintainers
- Issue: HSE-58
- Supersedes: none; it makes the layer direction in the Platform M0 architecture document enforceable.

## Decision

CIDX uses a downward dependency architecture. Product surfaces call application and
analysis services; services consume the domain model and ports; adapters own Clang,
SQLite, filesystem, and process APIs.

The versioned machine-readable source of truth is
[`architecture/cidx-module-manifest.json`](../../architecture/cidx-module-manifest.json).
The bootstrap checker is intentionally Python stdlib-only and runs before a C++
build exists:

```text
python3 scripts/check_architecture.py \
  --manifest architecture/cidx-module-manifest.json
```

The checker assigns every project-owned production source and declared production
CMake target exactly once, validates internal include edges and target links,
checks contract purity, and rejects cycles in the declared dependency graph.

## Layer and ownership rules

| Layer | Owns | May depend on | Must not own |
| --- | --- | --- | --- |
| Model | stable IDs, records, IR values, artifact payloads | standard library and model | Clang/LLVM, SQLite, CLI, filesystem, process |
| Foundation | small reusable platform utilities | model | product semantics and CLI rendering |
| Workspace/frontend | source identity, compile databases, compiler sessions | foundation, model, declared frontend adapters | persistence schema and product output |
| Extraction | typed AST visitors and fact emitters | model and focused ports | CLI parsing, terminal output, direct database lifecycle |
| Persistence | SQLite schema, migrations, stores | model, workspace input adapters, foundation | Clang AST classification and product rendering |
| Query/analysis | graph reads, QueryPlan, diff, include and AST analyses | model, ports, adapters | command parsing and direct compiler setup outside adapters |
| Product surface | CLI, executable entry points, output formatting | all lower layers through application APIs | semantic rules, migrations, direct ad-hoc SQL |

The current `cidx_core` static library is a compatibility bundle, not a desired
architectural layer. It is explicitly owned by `compat.core-bundle` and is tracked
as a target migration exception while HSE-61, HSE-62, HSE-63, and HSE-68 introduce
the focused ports and application services.

Clang/LLVM includes are allowed only in modules declared as frontend, extraction,
or lowering adapters. SQLite includes and links are allowed only in the persistence
adapter and explicitly declared analysis runners that consume a persisted artifact.
The domain and artifact contract files listed in the manifest are checked directly
for forbidden includes.

## Ports, adapters, and surfaces

- Domain model: plain values with stable serialization identity; no environmental handles.
- Port: an interface or value contract consumed by an upper layer and implemented by an adapter.
- Adapter: the only place where a port is bound to Clang, SQLite, process, filesystem, or Soufflé APIs.
- Application service: an orchestration operation that resolves context and invokes ports/transforms.
- Product surface: CLI, executable, SDK, agent, or IDE boundary; it parses requests and renders results.

New semantic logic belongs in a model, pass, transform, or service—not in a CLI
handler or JSON renderer. New public read behavior first extends QueryPlan/CXQ and
then adds thin compatibility adapters where necessary.

## Exceptions

An existing cross-boundary edge is not implicitly grandfathered. Each exception in
the manifest must name an owner, rationale, affected boundary, expiry date, and
removal issue. The checker fails on missing metadata or an expired exception.
Exceptions are temporary migration work items, not permission to add a new edge
without review.

## Review policy

Before adding a module or dependency, a change must:

1. name the owning layer/module and the port or adapter boundary;
2. update the manifest's allowed edge or add a complete issue-linked exception;
3. add a positive check and, when the rule changes, a mutation test;
4. state the Clang/LLVM, SQLite, CLI, filesystem, process, and Python impact;
5. run the architecture checker, mutation tests, and the relevant C++/Python gates.

The dependency must use an existing standard-library or project utility when one
already provides the required capability. A new third-party dependency requires a
separate design review, ownership, license/provenance note, and removal or upgrade
policy.

## Consequences

This is an enforcement-first, directory-stable migration. It does not require a
big-bang move or a breakup of `Storage`. It does make the current coupling visible,
prevents unclassified production files, and gives later Platform M0–M4 stories a
single contract to extend.
