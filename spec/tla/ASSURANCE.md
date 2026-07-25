# Assurance-level classification

HSE-89 turns the M0-M3 TLA+ foundation into a continuously enforced review
boundary. Four independent assurance mechanisms exist, and this table states
which one (or which combination) each named invariant/property requires. A
mechanism proves what it proves; it does not silently upgrade to a stronger
claim.

| Mechanism | What it proves | What it does not prove | Gate |
| --- | --- | --- | --- |
| **TLC** (finite model checking) | The named invariant/property holds for every state reachable from the *one* concrete constant instantiation in a model's `.cfg`, up to the explored state-space bound. | Anything about a different constant choice, an unbounded domain, or states beyond the explored bound. | `tools/check.sh` |
| **TLAPS** (mechanized proof) | The named invariant is *inductive*: it holds in every `Init` state and is preserved by every `Next` step, for **every** constant instantiation satisfying the module's stated `ASSUME`s -- not just the one TLC sample. | Anything about the C++/Python implementation; a proof is still a claim about the abstract spec only. | `tools/check-proofs.sh` |
| **Conformance replay** | A concrete action/observation trace -- hand-written, TLC-counterexample-derived, or captured from a real C++ run -- is a legal trace of the abstract spec (or is correctly rejected as illegal). | That the C++ implementation is correct in general; only the traces actually replayed are checked. | `tools/check-conformance.sh`, `tools/check-cpp-conformance.sh` |
| **C++/Python test suite** | The concrete implementation satisfies its own unit/integration/E2E contracts, independent of the TLA+ models. | Consistency with the abstract spec, unless the test is also a conformance-replay producer. | `ctest -L default`/`-L clang`, `uv run pytest` |

## Per-invariant classification

Every model listed in `manifest.json` is TLC-checked by construction (that is
the smoke-model contract from M0-M3 and is not repeated per-row below).
This table adds the *additional* assurance level layered on top for HSE-89.

| Invariant / property | Module | TLC | TLAPS | Conformance | Rationale |
| --- | --- | --- | --- | --- | --- |
| `SharedResultTypeInvariant` | CidxResult | yes | **yes** (`proofs/CidxResultProof.tla`) | no | Smallest, most reusable module; a good proof-of-toolchain module with a genuinely non-trivial inductive step (`ProveUnderAssumptions`). |
| `TrustedOutcomeInvariant` | CidxResult | yes | **yes** (`proofs/CidxResultProof.tla`) | no | This is the one invariant in the foundation whose informal English reading ("a proved-under-assumptions result always carries a nonempty assumption set") is exactly the kind of claim TLC's one-instance check cannot generalize past its finite sample; TLAPS proves it for arbitrary `EvidenceIds`/`QueryIds`/`ResultIds`. |
| `SharedStatusInvariant` | CidxResult, CidxRepository | yes | no (implied by the proved `IsResult` shape combined with the `ResultStatuses` domain restriction already inside `IsResult`) | no | Follow-on of the proved type invariant; not separately proof-worthy. |
| `NoPartialPublication`, `ReadOnlyQueries`, `PreservePublishedGeneration`, `HonestPartialResults` (`CidxProtected`) | protected/CidxProtected | yes (via consuming models) | not yet (candidate for a future HSE) | no | Genuinely important, but each depends on multi-variable behavioral models (`CidxBehavior`, `CidxStorageLifecycle`) with much larger inductive-step case counts; deferred rather than rushed. See "Deferred TLAPS candidates" below. |
| `AtomicPublicationInvariant`, `InvalidationInvariant`, `MigrationInvariant`, `IdentityInvariant`, `GraphInvariant`, `QueryPlanInvariant`, `IncludeHygieneInvariant`, `TraceInvariant`, `BoundedProgressInvariant` (CidxBehavior) | CidxBehavior | yes | no | **yes** -- `PublishGeneration`/`ValidateQueryPlan`/`ReturnQuery`/etc. are replayed by `conformance/CidxConformance.tla` against real and hand-written traces. | These are the index-generation-publication and QueryPlan-read-only-execution flows named in the HSE-89 acceptance criteria; conformance replay against actual observed C++ traces is the assurance level the issue asks for, not a second inductive proof. |
| `CurrentGenerationInvariant`, `NoInvalidCurrentInvariant`, `SidecarInvariant`, `CrossFileAtomicityInvariant`, `CleanupSafetyInvariant`, `PackageInvariant`, `PublicationRecoveryInvariant` (CidxStorageLifecycle) | CidxStorageLifecycle | yes | no | **yes** for the sidecar-publication slice (`BuildSidecar`/`PrepareSidecarArtifact`/`PublishSidecar`/`MarkSidecarMissing`/`MarkSidecarCorrupt`) -- replayed by `conformance/CidxStorageConformance.tla`. | Sidecar publication is the third named conformance flow in the acceptance criteria. |
| All `*AdversarialInvariant`, cycle/diamond/fan-out invariants (CidxSemanticGraph) | CidxSemanticGraph | yes (`check-regression.sh` seeds 30+ defects) | no | no | These are already exercised as seeded TLC counterexamples (`tools/check-regression.sh`); the counterexample-export pipeline (`tools/export-counterexample.sh`) turns a representative one into a reproducible C++ regression scenario instead of duplicating the proof effort. |

## Deferred TLAPS candidates

`CidxProtected`'s four predicates are the highest-value future TLAPS targets
because they are exactly the review-boundary claims AI-generated code must
never silently weaken. Proving them inductively over `CidxBehavior`/
`CidxStorageLifecycle` requires a much larger case split (order of 20-30
named actions per module, per the `Next` disjunction) than `CidxResult`'s
seven. That is real, trackable follow-up work, not a gap papered over here:
TLC continues to check all four predicates via `ProtectedInvariant` in every
smoke model in the meantime, and `tools/check-policy.sh` continues to require
human `CODEOWNERS` review before `protected/CidxProtected.tla` can change at
all.

## Model bounds are not exhaustive proof

Every TLC run in this repository uses one finite constant instantiation, one
worker, fixed fingerprint polynomial `0`, and fixed seed `1` (`TOOLCHAIN.md`).
`tools/check.sh` now also emits `TLA_MODEL_COVERAGE` lines reporting the
exact number of distinct states TLC generated and the state-graph diameter it
explored for each model, so a reader never has to infer "how much was
checked" from an opaque pass/fail line. A passing model result means:
*no violation exists among the states TLC actually visited for this specific
constant choice*; it is not a claim that the property holds for arbitrary
domain sizes. Only the modules listed with **yes** under TLAPS above carry
that stronger, domain-independent guarantee.
