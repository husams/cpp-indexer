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
| **Conformance replay** | A concrete action/observation trace -- hand-written, TLC-counterexample-derived, or captured from a real C++ run -- is a legal trace of the abstract spec (or is correctly rejected as illegal). | That the C++ implementation is correct in general; only the traces actually replayed are checked. | `tools/check-conformance.sh`, `tools/check-sidecar-conformance.sh` (TLA+ side), `ctest -L default` via `tests/conformance_recorder_test.cpp` (C++ side, `ConformanceRecorder`) |
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
| `CurrentGenerationInvariant`, `NoInvalidCurrentInvariant`, `SidecarInvariant`, `CrossFileAtomicityInvariant`, `CleanupSafetyInvariant`, `PackageInvariant`, `PublicationRecoveryInvariant` (CidxStorageLifecycle) | CidxStorageLifecycle | yes | no | **yes** for the sidecar-publication slice. `BuildSidecar`/`PrepareSidecarArtifact`/`PublishSidecar` are replayed by `conformance/CidxStorageConformance.tla`; `MarkSidecarMissing`/`MarkSidecarCorrupt` are mapped in `sidecar-operation-map.json` and checked against that same declared vocabulary by the C++ conformance test (`tests/conformance_recorder_test.cpp`, `ConformanceRecorder::analysis()`), but neither yet appears in a TLA+-replayed scenario -- `sidecar-scenarios.json` currently has one scenario (`sidecar-current`). `PublishSidecar` has 7 preconditions; the recorder observes 5 of them and deliberately does not observe `migrationPhase = "none"` or `readerStatus = "current"` (no such fields exist in `sidecar-observation-map.json`) because `src/storage/storage_migrate.cpp` runs migration synchronously in the `Storage` constructor before any `ApplicationServices` call dispatches, so both guards are structurally always satisfied on every observable trace -- not an oversight. | Sidecar publication is the third named conformance flow in the acceptance criteria. |
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

## check-proofs.sh binding-check scope (round-3 acceptance review)

`tools/check-proofs.sh`'s theorem/invariant binding check accepts a declared
invariant as proved only when some theorem's statement, in the BY-citation
closure of the manifest-declared theorem, structurally matches
`<Spec> => []<Invariant>` for a real, module-defined `Spec` operator -- not
merely contains that text as a substring (round-2's gap) and not merely sits
inside the closure regardless of shape (round-3's gap, closed by requiring
the structural match). Two accepted, non-blocking limitations of this
approach, documented here rather than left implicit:

- **Only top-level `THEOREM` declarations are collected as closure carriers;
  `LEMMA` is treated purely as a closure *boundary*, never as a member.** A
  legitimate restructuring that proves a corollary via an intermediate
  `LEMMA` (rather than directly `BY <declared-theorem>`) would fail the gate
  with `reason=proves-invariant-not-found`, even though the underlying TLAPS
  proof is genuinely sound. This is a false-negative failure mode, not a
  soundness gap: it can only make the gate too strict, never accept a vacuous
  proof. Should this repository ever add a proof shaped that way, treat the
  gate failure as a binding-logic limitation to fix, not a real regression.
- **The check requires the literal antecedent `Spec` (dynamically discovered
  as any module-defined `<Name> == Init /\ ...` operator, not hardcoded) --
  a theorem proving the same invariant from a differently-named but
  equivalent temporal formula would also fail closed.** Every module in this
  repository names its top-level specification `Spec`
  (`modules/CidxResult.tla:61` and siblings), so this is not a practical
  restriction today.

## check-proofs.sh proof-module assumption binding (HSE-89 acceptance review)

The binding check above constrains theorem *shape*; it says nothing about
whether the theorem was proved from a sound premise. TLAPS treats every
`ASSUME` in a proof module as a hypothesis available to every later proof in
that module, so a structurally-correct `Spec => []Invariant` theorem can
still be vacuous if the module also carries an unconstrained assumption such
as `ASSUME FALSE` -- anything follows from a contradiction, and TLAPS reports
"All N obligations proved." exactly as it would for a genuine proof.
`check-proofs-binding.sh`'s `check_proof_assumptions()` closes this with two
checks over the (untrusted, `proofs/`-tree) module's own `ASSUME`s, run
before the theorem-shape check:

1. **Allowlist.** Every `ASSUME` found in the module must match, verbatim
   once whitespace is collapsed, an entry in `manifest.json`'s
   `proofs[].trustedAssumptions` for that module -- itself a
   CODEOWNER-protected path, so a new assumption needs the same review as
   the proof module itself.
2. **Vacuousness.** Independent of the allowlist: the assumption set is
   rejected if any top-level conjunct (split on `/\ ` at parenthesis depth
   zero) of any assumption is the literal boolean `FALSE`, or if two
   assumptions' top-level conjuncts are syntactic negations of each other.

This is a **syntactic** check, not a general decision procedure for
first-order satisfiability -- documented here rather than left implicit:

- It recognizes negation only one level deep (`~P` or `~(P)`), and only
  between conjuncts that are otherwise textually identical once collapsed.
  `ASSUME P` alongside `ASSUME ~(~(~P))` would not be caught by this check
  (though it would still need to clear the allowlist, which a genuinely
  unreviewed assumption cannot).
- It cannot detect semantic (non-syntactic) contradictions, e.g. two
  assumptions that are individually satisfiable but jointly impossible only
  by arithmetic reasoning (`ASSUME x > 5` and `ASSUME x < 3`). The allowlist
  is the primary defense for those; a human reviewer approving
  `trustedAssumptions` is expected to notice a jointly-unsatisfiable pair
  when they are added, exactly as they are expected to notice any other
  proof-module change to a protected path.
- Both checks apply only to the `ASSUME`s of the proof module itself
  (`proofs/CidxResultProof.tla` and any future sibling), not to `ASSUME`s in
  the trusted modules it extends -- those are separately scoped by the
  Spec-provenance restriction to `modules/conformance/protected` described
  above, and are reviewed at the same protection level as everything else in
  `modules/`.

## Conformance recorder: tautological sidecar branches (round-3 acceptance
review)

`ConformanceRecorder::analysis()`'s `sidecar.missing` and `sidecar.corrupt`
branches (round-2 critic P1-1b/P1-1c fix) assert `sidecarFilePublication`,
`sidecarState`, `sidecarQuality`, and `sidecarValidated` as literal values
matching exactly what `sidecar-operation-map.json` declares each operation
expects -- they do not derive those values from anything independently
observed on those two paths. This is sound (each operation's postcondition
in `CidxStorageLifecycle.tla`'s `MarkSidecarMissing`/`MarkSidecarCorrupt`
really does set those fields unconditionally), but it means `conformant()`
retains discriminating power on the sidecar side only through the
`sidecar.publish` branch (which does compare recorded artifact provenance
against `last_published_generation_`) and through the entry condition that
selects which of the three branches runs at all (empty artifacts vs.
top-level error vs. neither). A defect that mis-selected the wrong branch
(e.g. treated a real error as a healthy empty result) would still be caught;
a defect confined entirely within an already-selected `sidecar.missing`/
`sidecar.corrupt` branch's field values would not be, because those values
are constants, not computed. This mirrors the already-accepted
`index.publish` tautology noted elsewhere in this document's spirit and is
flagged here for the same reason: narrowed discriminating power, not a
silently-hidden gap.

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
