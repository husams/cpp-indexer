# CIDX normative TLA+ specification

TLA+ is the normative behavioral specification of CIDX itself. The modules in
this directory define the vocabulary, state transitions, invariants, fairness
obligations, and result statuses that future CIDX specifications must reuse.
The C++ indexer, Python query surface, SQLite schema, and generated artifacts
are implementations or evidence; none is a semantic axiom here. The current
specification version is recorded in `manifest.json` and is advanced only by a
reviewed contract change.

## Layout

| Path | Role | Ownership |
| --- | --- | --- |
| `modules/CidxTypes.tla` | shared abstract domains, relation kinds, evidence/trust levels, and result statuses | human-authored normative contract |
| `modules/CidxRepository.tla` | repository/workspace structure smoke specification | human-authored normative contract |
| `modules/CidxResult.tla` | result-status lifecycle smoke specification | human-authored normative contract |
| `modules/CidxWorkspaceLifecycle.tla` | workspace identity, configuration applicability, and generation lifecycle | human-authored normative contract |
| `modules/CidxBehavior.tla` | bounded end-to-end lifecycle, identity, query, transform, storage, failure, and recovery behavior | human-authored normative contract |
| `modules/CidxSemanticGraph.tla` | typed graph domains, evidence ownership, QueryPlan/CXQ safety, witnesses, completeness, and transform freshness | human-authored normative contract |
| `conformance/CidxConformance.tla` | deterministic action-sequence replay against the behavioral model | conformance checker contract |
| `models/*.tla` and `models/*.cfg` | finite TLC smoke models and their constants/invariants | human-authored model boundary |
| `manifest.json` | versioned module/model/invariant inventory | human-authored contract index |
| `protected/CidxProtected.tla` | protected invariant predicates consumed by models | explicit human review required |
| `trusted/assumptions.md` | trusted external assumptions and review rules | explicit human review required |
| `conformance/` | operation/observation mappings and deterministic scenario traces | implementation adapter contract |
| `tools/check.sh` | pinned syntax and TLC command-line gate | human-authored tool contract |
| `tools/check-conformance.sh` | conformance inventory and scenario determinism gate | human-authored tool contract |
| `generated/` | disposable reports and translated output only | generator-owned, ignored |

The dependency direction is intentionally one-way:

```text
CidxTypes <- CidxRepository <- CidxRepositorySmoke
          <- CidxResult     <- CidxResultSmoke
          <- CidxWorkspaceLifecycle <- CidxWorkspaceLifecycleSmoke
          <- CidxBehavior <- CidxBehaviorSmoke
```

`CidxProtected` is a policy module imported by the repository model. Later
modules may depend on `CidxTypes` and protected predicates, but shared types
must not depend on C++ headers, SQLite tables, generated code, or implementation
queries.

## Shared vocabulary

`CidxTypes.tla` defines the abstract domains for workspace, repository,
component, source, translation unit, configuration, symbol, type, relation,
evidence, generation, transform, artifact, query, and result. It also defines
the one shared `ResultStatuses` set:

`suggested`, `observed`, `inferred`, `bounded-verified`,
`proved-under-assumptions`, `refuted`, and `unknown`.

`CidxTypes.tla` also defines the shared generation lifecycle states
`imported`, `capturing`, `extracting`, `validating`, `published`, `current`,
`stale`, `failed`, and `retired`, plus reader states `current`, `stale`,
`partial`, and `unavailable`. Later models must reuse these sets instead of
introducing subsystem-specific spellings.

`CidxWorkspaceLifecycle.tla` keeps environment weak fairness as an assumption
inside `Spec`, but checks the separate `RebuildEventuallySettles` property.
That property requires an enabled import/rebuild path to reach a settled
reader state; the regression checker removes publication and verifies TLC
rejects the resulting non-progressing behavior.

Future modules must import these operators instead of defining look-alike
status strings or implementation-specific record shapes. Concrete identifiers
and finite model sizes belong in `.cfg` files, not in the normative type
module.

## Normative boundary

Normative content is the hand-authored TLA+ module and model contract: named
operators, actions, invariants, fairness formulas, constants, and the explicit
trusted-assumption policy. An implementation detail is any C++/Python/SQLite
layout, traversal order, cache format, CLI spelling, or generated representation
that is not imported as a TLA+ axiom. A successful smoke model proves only the
modeled behavior and tool invocation; it is not implementation conformance.

The first models deliberately cover repository identity/containment, result
status transitions, and the workspace/generation lifecycle. They do not
specify compiler semantics, storage recovery internals, query planning, or
implementation performance.

## Contract handoffs

The lifecycle model is the normative vocabulary handoff for the related
implementation contracts:

| Specification concern | Downstream contract |
| --- | --- |
| workspace, repository, clone/component boundary, and configuration capture | HSE-61 `WorkspaceContext` and translation-unit descriptor |
| configuration-qualified facts and invariant facts | HSE-76 translation-unit configuration applicability |
| scoped symbol identity and intentional cross-repository merge | HSE-82 semantic-universe identity |
| generation inputs, dependency invalidation, atomic publication, and failed/stale reads | HSE-67 named transform lifecycle |

The model deliberately treats those implementation issues as observable
contracts: it does not import their C++ types, SQLite tables, or filesystem
operations.

`CidxBehavior` covers the M0 observable pipeline: workspace import,
configuration capture, indexing, atomic publication, invalidation, graph edge
evidence/target/configuration applicability, one ordered argument sequence,
validated query stream shape/bounds/view safety, derived transforms, read-only
query outcomes, migration interruption/recovery, include-hygiene plan/validate/apply
authorization, and semantic-universe identity separation/merging. The manifest
explicitly marks concrete graph identity/completeness records, separate ordered
slot families, and deterministic query result sets/paths/witnesses as outside
this model's current coverage. It deliberately abstracts Clang, SQLite,
filesystem durability, query algorithms, and implementation performance.
The conformance package maps those abstract actions to observed C++ operations
without importing implementation details into the model.

`CidxSemanticGraph` is the M2 contract for the previously abstracted concerns.
It defines typed node/relation domains, endpoint compatibility, owned evidence,
ordered slots, retained unknown targets, legal QueryPlan stream/view
transitions, duplicate-free canonical set results, bounded witness paths,
read-only execution, completeness/truncation/unknown honesty, and named
transform publication and consumption freshness. Its smoke model also covers
cycles, diamonds, bounded fan-out, incomplete targets, and partial evidence.
The `Defect` constant seeds reproducible TLC counterexamples for each safety
boundary; `spec/tla/tools/check-regression.sh` runs those seeds explicitly.

## Reproducible check

From the repository root, run:

```bash
spec/tla/tools/check.sh
```

The protected-invariant, liveness-mutation, and trace-boundary regressions are
checked separately with:

```bash
spec/tla/tools/check-regression.sh
```

The machine-readable conformance and deterministic scenario gate is checked
separately with:

```bash
spec/tla/tools/check-conformance.sh
```

The checker downloads and SHA-256 verifies the pinned `tla2tools.jar` when it
is not already cached, requires Java 17, runs SANY syntax checks first, then
runs TLC with one worker and fingerprint polynomial 0 for each checked-in
model. It emits stable `TLA_SYNTAX_STATUS`, `TLA_MODEL_STATUS`,
`TLA_INVARIANT_STATUS`, `TLA_LIVENESS_STATUS`, `TLA_TOOLCHAIN_STATUS`, and
`TLA_CHECK_STATUS` lines.
Syntax/toolchain failures and model failures have distinct exit classes, and
the CI workflow exposes syntax, model, invariant, conformance, and C++ gates
separately. The checker cross-checks every model's
required invariant set against the `INVARIANT` entries in its `.cfg`; the
regression command proves that removing `ProtectedInvariant` fails closed.

See [TOOLCHAIN.md](TOOLCHAIN.md), [POLICY.md](POLICY.md), and
[trusted/assumptions.md](trusted/assumptions.md) before changing the contract.
