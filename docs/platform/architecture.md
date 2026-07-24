# CIDX platform architecture and extension model

- Status: accepted incremental architecture
- Owner: HSE-57
- Machine-readable contract: [`spec/platform/architecture.json`](../../spec/platform/architecture.json)
- Enforced module rules: [`architecture/cidx-module-manifest.json`](../../architecture/cidx-module-manifest.json)
- Dependency ADR: [`ADR-011`](../adr/ADR-011-module-architecture-and-dependency-rules.md)

## Purpose

CIDX is a modular code-intelligence and reasoning platform for one or many
C/C++ repositories. The persistent semantic index remains the core product
artifact, but workspace identity, reproducible frontend sessions, extraction,
analysis, query, explanation, and proof all use the same contracts:

```text
workspace/build identity
  -> frontend session
  -> typed facts and evidence
  -> persisted or content-addressed artifacts
  -> derived analyses
  -> QueryPlan/CXQ navigation and explanation
  -> effect reasoning and proof
```

The architecture is incremental. Existing directories and compatibility
surfaces remain in place while the ports and ownership boundaries below become
enforceable. A physical target or directory split is not a prerequisite for a
new boundary.

## Layer ownership

Dependencies point downward through ports and adapters. The machine-readable
layer graph is authoritative for the allowed direction.

| Layer | Owns | Boundary rule |
| --- | --- | --- |
| Workspace | repositories, revisions, components, compile inputs, freshness | resolves immutable workspace and TU identity |
| Frontend | frontend-neutral session ports and Clang/LLVM adapters | the only owner of compiler-session construction |
| Extraction | registered core passes and declarative extension plans | emits facts/evidence; does not own CLI or DB lifecycle |
| Model | stable IDs, catalogs, facts, evidence, status, trust, and artifact values | contains no Clang, LLVM, SQLite, CLI, filesystem, or process types |
| Persistence | focused stores, SQLite schema, migrations, and transactions | adapts ports to SQLite; does not classify AST semantics |
| Derivation | named transforms and analysis-package runners | consumes declared fact sets and publishes versioned artifacts |
| Query | QueryPlan/CXQ, bounded reads, explanations, and read adapters | new public reads lower to QueryPlan operations |
| Proof | effect summaries, proof IR, replay, and solver evidence | consumes shared workspace, fact, identity, and evidence contracts |
| Product | application services, CLI, SDK, agents, IDE, and compatibility adapters | parses requests and renders results; contains no semantic rules |

Clang/LLVM, SQLite, filesystem, process, and CLI dependencies are classified in
the platform contract and enforced by the bootstrap architecture checker.
`cidx_core` remains a temporary compatibility bundle tracked by HSE-62; it is
not a product layer or a reason for new consumers to depend on the monolithic
facade.

## Stable ports

The first stable port set is:

- `WorkspaceSnapshot` and `TranslationUnitDescriptor` — immutable source and
  build semantics shared by indexing, AST graph generation, diff, include
  validation, analysis, and proof preparation;
- `FrontendSessionFactory` — opens a reproducible frontend session for a
  descriptor and requested capabilities;
- `ExtractionPassRegistry`, `FactEmitter`, and `EvidenceEmitter` — describe,
  order, and test extraction independently of persistence;
- `WorkspaceStore`, `SourceStore`, `SymbolStore`, `TypeStore`, `FactStore`,
  `EvidenceStore`, `AnalysisArtifactStore`, and `ProofArtifactStore` — focused
  persistence ports implemented first by SQLite;
- `TransformRegistry` and `AnalysisEngine` — versioned derivation and analysis
  execution with dependency, budget, invalidation, and unknown propagation;
- `QueryPlan` — the canonical read algebra for C++, Python, CLI, agent, and
  future IDE clients;
- `ApplicationContext` — one selected workspace/index, policy set, artifact
  store, and read/write mode for product services.

Clang and SQLite types may occur in adapters and compiled implementations, but
never in the semantic model, public artifact contracts, QueryPlan values, or
proof contracts.

## Artifact contract

Every built-in or extension fact/result identifies its source and configuration,
producer and version, schema/catalog versions, completeness, evidence, and
freshness. It also records deterministic identity, diagnostics, budgets, and
truncation or unknown reasons where applicable. The common status vocabulary is
`complete`, `partial`, `unknown`, and `error`; evidence classes distinguish
source, derived, inferred, runtime, assumption, and proof claims.

Core facts use reviewed catalog and schema contracts. Custom facts default to a
content-addressed, immutable extension artifact and use package-qualified
identifiers. Promotion into the core persisted catalog requires a compatibility
entry, schema/migration review, deterministic golden coverage, and compiled
regression coverage.

## Three extension surfaces

CIDX deliberately keeps extraction, analysis, and query responsibilities
separate:

1. **CXQ / QueryPlan** reads declared fact sets and produces bounded results or
   explanations. It is read-only and cannot execute arbitrary SQL or callbacks.
2. **ExtractionPlan** matches typed AST constructs against one pinned
   translation unit and emits namespaced preview facts, relations, attributes,
   and evidence. It requires declared bindings, identity recipes, traversal and
   completeness policy, applicability, duplicate identity, and cardinality,
   time, and memory budgets. It cannot write files, load native plugins, or
   overwrite core facts.
3. **Analysis packages** consume semantic-index, raw-AST, or extension fact
   providers through one versioned runner and emit a result or derived-fact
   artifact with provenance, budgets, and unknown propagation. Soufflé is the
   first engine, not the package contract.

Compiled C++ extraction remains authoritative for core facts requiring precise
C++ semantics, performance, or proof-grade soundness. Declarative extensions
are a safe experimentation and preview boundary, not a way to redefine core
semantics silently.

## Product and compatibility policy

The normal user workflow is represented by application services and the unified
command model:

```text
cidx workspace | index | query | analyze | inspect | diff | refactor | proof
```

Legacy commands, `cidx-astgraph`, `cidx-diff`, noun-specific query APIs, and the
Python extraction path remain compatibility adapters until their migration
gates are met. Adapters call canonical services or QueryPlan; they do not keep
independent SQL, Clang setup, or result semantics.

The C++ core owns Clang, core extraction, persistence, built-in transforms,
product services, and the CLI. The Python SDK owns QueryPlan/CXQ builders,
artifact/result models, read clients, and notebook conveniences. Generated
versions, catalogs, schemas, status/error codes, and golden vectors are shared
contracts. Cross-language behavioral parity is required only where the
compatibility manifest explicitly promises it.

## Delivery and review gates

The parent feature is delivered through the Platform M0–M4 stories recorded in
the machine-readable contract. Each change must identify its owning layer,
port/adapter boundary, external dependency impact, and compatibility impact.
It must update the architecture manifest or add a complete issue-linked
exception, add focused positive/negative coverage, and run the architecture,
contract, and relevant language gates.

The qualification endpoint is CIDX self-hosting plus the multi-repository
banking corpus. Both must report source/configuration identity, catalog and
package versions, completeness, evidence, budgets, performance, and unresolved
unknown boundaries. Static replay remains deterministic and credential-free.
