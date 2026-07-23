# CIDX normative TLA+ specification

TLA+ is the normative behavioral specification of CIDX itself. The modules in
this directory define the vocabulary, state transitions, invariants, fairness
obligations, and result statuses that future CIDX specifications must reuse.
The C++ indexer, Python query surface, SQLite schema, and generated artifacts
are implementations or evidence; none is a semantic axiom here.

## Layout

| Path | Role | Ownership |
| --- | --- | --- |
| `modules/CidxTypes.tla` | shared abstract domains, relation kinds, evidence/trust levels, and result statuses | human-authored normative contract |
| `modules/CidxRepository.tla` | repository/workspace structure smoke specification | human-authored normative contract |
| `modules/CidxResult.tla` | result-status lifecycle smoke specification | human-authored normative contract |
| `models/*.tla` and `models/*.cfg` | finite TLC smoke models and their constants/invariants | human-authored model boundary |
| `protected/CidxProtected.tla` | protected invariant predicates consumed by models | explicit human review required |
| `trusted/assumptions.md` | trusted external assumptions and review rules | explicit human review required |
| `tools/check.sh` | pinned syntax and TLC command-line gate | human-authored tool contract |
| `generated/` | disposable reports and translated output only | generator-owned, ignored |

The dependency direction is intentionally one-way:

```text
CidxTypes <- CidxRepository <- CidxRepositorySmoke
          <- CidxResult     <- CidxResultSmoke
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

The first models deliberately cover repository identity/containment and result
status transitions. They do not specify all subsystem behavior, compiler
semantics, storage recovery, query planning, or implementation performance.

## Reproducible check

From the repository root, run:

```bash
spec/tla/tools/check.sh
```

The checker downloads and SHA-256 verifies the pinned `tla2tools.jar` when it
is not already cached, requires Java 17, runs SANY syntax checks first, then
runs TLC with one worker and fingerprint polynomial 0 for each checked-in
model. It emits stable `TLA_SYNTAX_STATUS`, `TLA_MODEL_STATUS`,
`TLA_TOOLCHAIN_STATUS`, and `TLA_CHECK_STATUS` lines. Syntax/toolchain failures
and model failures have distinct exit classes, and the CI workflow exposes the
TLA+ gate separately from C++ tests.

See [TOOLCHAIN.md](TOOLCHAIN.md), [POLICY.md](POLICY.md), and
[trusted/assumptions.md](trusted/assumptions.md) before changing the contract.
