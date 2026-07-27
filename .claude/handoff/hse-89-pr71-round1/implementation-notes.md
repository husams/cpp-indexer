# Implementation notes — PR #71 (HSE-89) acceptance-gap round 1

## Scope

Closed AC4 only, per dispatch. AC3 (CODEOWNERS/self-approval) and the CI-wording
item are organizational and out of scope; not touched; reported in
`notAddressed`.

## Gap and general invariant

Reviewer's example: adding `ASSUME FALSE` to `proofs/CidxResultProof.tla` lets
TLAPS "prove" `ResultInvarianceTheorem == Spec => []Invariant` vacuously (TLAPS
treats every `ASSUME` in a module as a premise available to every later proof
in that module; anything follows from a contradiction). The existing
`check-proofs-binding.sh` binding check constrained theorem *shape* and the
*Spec* provenance, but never looked at the proof module's own `ASSUME`s.

General invariant fixed (not the specific string "ASSUME FALSE"): **an
untrusted proof module's assumption set must itself be constrained, not only
the shape of what it claims to prove.** Two independent checks added to
`check_proof_assumptions()` in `check-proofs-binding.sh`, run before the
existing theorem-shape checks:

1. **Allowlist.** Every `ASSUME` in the proof module must match, verbatim once
   whitespace is collapsed, an entry in `manifest.json`'s new
   `proofs[].trustedAssumptions` field for that module. `manifest.json` is
   already a protected, CODEOWNER-reviewed path (`protectedPaths`), so a new
   assumption needs the same review as the proof module itself.
2. **Vacuousness (syntactic, documented as such).** Independent of the
   allowlist: rejects the assumption set if any top-level conjunct (split on
   `/\` at paren depth 0, one layer of wrapping parens stripped) of any
   assumption is the literal boolean `FALSE`, or if two assumptions' top-level
   conjuncts are syntactic negations of each other (`P` / `~P` / `~(P)`, same
   or different `ASSUME` statements).

Both checks apply only to the untrusted `proofs/` module's own `ASSUME`s, not
to the trusted `modules/` it extends (those remain scoped by the existing
Spec-provenance restriction).

## Files changed (7, all directly required by the fix)

- `spec/tla/tools/check-proofs-binding.sh` — the fix itself:
  `extract_assumptions`, `top_level_conjuncts`, `negated_form`,
  `check_proof_assumptions`, wired into `theorem_invariant_binding` before the
  existing theorem-shape logic. No existing function renamed/removed.
- `spec/tla/tools/check-proofs.sh` — three new `case` arms
  (`ASSUMPTION-VACUOUS:*`, `ASSUMPTIONS-CONTRADICT:*`,
  `ASSUMPTION-NOT-TRUSTED:*`) mapping the new binding-check outputs to
  `TLA_PROOF_STATUS=FAIL reason=...`; header comment updated.
- `spec/tla/manifest.json` — added `proofs[0].trustedAssumptions` with the
  three real, pre-existing well-formedness `ASSUME`s from
  `proofs/CidxResultProof.tla` (verified byte-exact match against the
  extractor's normalization — see Self-review below).
- `spec/tla/tools/validate-manifest.py` — `trustedAssumptions` is now a
  required (string-list, empty allowed) field per `proofs[]` entry, so the
  allowlist itself is schema-enforced wherever `validate-manifest.py` already
  runs (`check-policy.sh`, `select-changed-gates.sh`/`check-gate-selection.sh`).
- `spec/tla/tools/check-gate-selection.sh` — added one
  `run_manifest_schema_failure_case` for the new required field (parity with
  the other 9 curated schema-failure cases; proves the new field is actually
  enforced, not just declared).
- `spec/tla/tools/check-proofs-binding-unit-test.sh` — extended `write_manifest`
  with an optional `trustedAssumptions` argument (default `[]`, so the 6
  existing cases are unchanged) and added 8 new cases (7-14, see Regression
  tests below).
- `spec/tla/ASSURANCE.md` — new "check-proofs.sh proof-module assumption
  binding" section documenting the mechanism and its honest syntactic limits
  (single-level negation, no arithmetic/semantic contradiction detection),
  matching the file's existing round-3 documentation convention.

Not touched: any `.cpp`/`.hpp` file, `python/` package, `tests/e2e`, `index.db`,
`.github/workflows/verification.yml` (no new CI step needed — the fix lives
inside a script already invoked by the existing `tla-proofs` job),
`check-proofs-vacuous-comment-regression.sh` (see Follow-ups: could not verify
a new fixture there without a real tlapm run).

## New mechanism: `proofs[].trustedAssumptions` (manifest field)

- **Empty input**: allowed (`allow_empty=True` in `validate-manifest.py`; a
  future proof module with zero free constants declares `[]`) — exercised
  implicitly by cases 1-6 (no ASSUME in those fixtures) and directly by the
  `missing-proof-trusted-assumptions` schema-failure case (removing the key
  entirely, not just emptying it, since the requirement is presence, not
  non-emptiness).
- **Exhausted/boundary**: an assumption present in the module but absent from
  the list → `ASSUMPTION-NOT-TRUSTED` (case 10). An assumption on the list AND
  present → accepted (case 11, case 7's real-module regression against the
  actual manifest).
- **Interaction with the pre-existing budget in this file** (the
  Spec-provenance/theorem-shape closure computation): the new check runs and
  returns *before* that logic is reached, so it cannot re-open the round-4
  fix (proven: cases 1-6, which exercise the shape/closure logic exclusively,
  are unaffected — same PASS output as before this round).
- No new cap, cursor, or cache was added; the allowlist is a plain set
  membership test and the vacuousness check is a single linear pass over a
  bounded number of conjuncts (bounded by the module's own text size, same as
  every other regex-based check already in this file).

## Regression tests (14 cases total in `check-proofs-binding-unit-test.sh`, all tlapm-free)

New cases 7-14, each proven to go red against the pre-fix
`check-proofs-binding.sh` (case 7 shown explicitly below; cases 8-14 exercise
functions — `check_proof_assumptions`, `extract_assumptions`,
`top_level_conjuncts`, `negated_form` — that do not exist at all in the
pre-fix file, so `theorem_invariant_binding` would fall through unchanged to
the theorem-shape check and return `OK:...` for every one of them, exactly
as case 7 does):

- Case 7 — the exact reviewer report: `ASSUME FALSE` added to the REAL
  `proofs/CidxResultProof.tla` shape, checked against the REAL
  `spec/tla/manifest.json`. Proven red on revert:
  `TLA_PROOF_BINDING_UNIT_STATUS=FAIL reason=assume-false-on-real-proof-module-accepted:OK:ResultInvarianceTheorem:SharedResultTypeInvariant,TrustedOutcomeInvariant`
  against the pre-fix script; green (`ASSUMPTION-VACUOUS:FALSE`) against the
  fixed script.
- Case 8 — `FALSE` as one conjunct of a larger `/\` assumption (not the whole
  assumption text) — proves the check inspects conjuncts, not literal
  whole-string equality with "FALSE".
- Case 9 — two assumptions that are direct negations of each other
  (`x = 0` / `~(x = 0)`) — unsatisfiable set with no single `FALSE` literal
  anywhere.
- Case 10 — a satisfiable, non-vacuous assumption that is simply not on the
  allowlist — proves the allowlist half independently of vacuousness.
- Case 11 — the same assumption, now allowlisted — proves no false positive
  on a legitimately reviewed, satisfiable well-formedness assumption (the
  real shape used by `proofs/CidxResultProof.tla`).
- Case 12 — `ASSUME (FALSE)` (single wrapping paren) — proves the one-layer
  paren-unwrap in `top_level_conjuncts` actually fires rather than leaving a
  parenthesized `FALSE` as opaque, unmatched text.
- Case 13 — bare-tilde negation (`Flag` / `~Flag`, no parens) — proves
  `negated_form`'s un-parenthesized branch (case 9 only exercised the
  `~(...)` branch).
- Case 14 — a lone bare negation with no positive counterpart in the module —
  proves the contradiction check does not over-fire on every `~` it sees
  (false-positive check for the branch case 13 added).

All 14 run and pass on this machine (no tlapm required):
`bash spec/tla/tools/check-proofs-binding-unit-test.sh` → 14×PASS.

Also directly exercised the real production path outside the test harness by
sourcing `check-proofs-binding.sh` and calling `theorem_invariant_binding`
against the real `spec/tla/manifest.json` + real
`spec/tla/proofs/CidxResultProof.tla` — unchanged `OK:...` result, confirming
no regression on the one real, shipped proof.

Not added: a case in `check-proofs-vacuous-comment-regression.sh` (the
real-tlapm end-to-end regression). See Follow-ups.

## Self-review (adversarial, against my own new code)

New branches introduced, and how each is tested:

- `extract_assumptions`: named (`Name == expr`) vs. bare/unnamed ASSUME —
  both reached (case 7's `ASSUME FALSE` is unnamed; cases 8-14 are named).
  Zero-ASSUME module — reached by cases 1-6 (pre-existing fixtures,
  unaffected).
- `top_level_conjuncts`: no `/\` (single conjunct) — cases 10/11/13/14; one
  `/\` split — case 8; single-layer paren unwrap — case 12; the "not a
  single wrapping pair, leave unchanged" defensive branch is *not*
  independently tested (it is a pure no-op fallback — worst case is a missed
  detection of a contrived nested-parenthesization vacuity, which the
  allowlist check still independently blocks since any such assumption still
  has to be pre-declared and reviewed; documented as a known limitation in
  ASSURANCE.md rather than silently assumed complete).
- `negated_form`: `~(...)` branch — case 9; bare `~` branch — case 13; no
  match (non-negated conjunct) — every passing case.
- `check_proof_assumptions`: FALSE-conjunct return — cases 8, 12;
  contradiction return — cases 9, 13; allowlist-miss return — case 10;
  full pass-through (all clear) — cases 1-6, 11, 14, and the real-module
  check.
- `check-proofs.sh`'s three new `case` arms: each maps 1:1 to one of the
  three `check_proof_assumptions` return prefixes; exercised indirectly
  through the binding-check unit tests (the `case` arms themselves are
  simple string-prefix dispatch with no independent logic, so the binding
  function's own return-value coverage above is the load-bearing test).
- `validate-manifest.py`'s new required field: tested end-to-end via
  `check-gate-selection.sh`'s new `missing-proof-trusted-assumptions` case
  (both `select-changed-gates.sh` and `check-policy.sh` entry points,
  matching the existing 9-case pattern) — PASS.

No workaround, quick hack, or unreviewed scope expansion: no version bump, no
schema regeneration, no `index.db` touch, no new CI workflow step (the fix
lives entirely inside scripts the existing `tla-proofs` job already runs).

## Deviations from plan

None — this is a direct review-gap fix, not a planned story; no plan.md
existed for this round.

## Follow-ups (surfaced, not actioned — tag sr-dev/architect per role boundary)

- Did not add a case to `check-proofs-vacuous-comment-regression.sh` (the
  real-tlapm end-to-end regression suite). This machine is arm64 macOS;
  `tlapm` is x86_64-linux-only and not materialized here (confirmed:
  `/tmp/tlaps-1.5.0/bin/tlapm` absent, no Java at all installed). Landing an
  untested fixture into a script whose entire purpose is "proven against a
  real tlapm run" risks introducing exactly the kind of unverified change the
  fix-discipline rules warn against. The fast, tlapm-free
  `check-proofs-binding-unit-test.sh` (case 7) exercises the identical
  reviewer-reported input against the real production
  `theorem_invariant_binding` function and is proven to go red on revert; that
  is the load-bearing regression for this round. Recommend a follow-up task,
  run on Linux/x86_64 (or in the CI container once Actions is re-enabled), to
  add the matching real-tlapm case.
- `check-verification-tamper-regression.sh` and
  `check-self-test-tamper-regression.sh` both fail on this machine with
  `TLA_TOOLCHAIN_STATUS=FAIL reason=java-major-version-unknown-expected-17`
  (no Java runtime installed at all). Confirmed via `git stash` that this is
  byte-identical pre-existing baseline behavior, unaffected by this round's
  changes — not a regression, not investigated further per the
  pre-approved-exception precedent for environment-toolchain gaps already
  documented by the prior review round.
- AC3 (CODEOWNERS/self-approval) and the AC-CI-wording item are organizational
  per the dispatch and were not touched.

## References

- Review: `gh api repos/husams/cpp-indexer/pulls/71/reviews/4789553646`
- Prior engineering review: `/tmp/pr-review-71.md`
- `spec/tla/ASSURANCE.md` (new section this round)
- `spec/tla/manifest.json`, `spec/tla/tools/check-proofs.sh`,
  `spec/tla/tools/check-proofs-binding.sh`,
  `spec/tla/tools/check-proofs-binding-unit-test.sh`,
  `spec/tla/tools/validate-manifest.py`,
  `spec/tla/tools/check-gate-selection.sh`
