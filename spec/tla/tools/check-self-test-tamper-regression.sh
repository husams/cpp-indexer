#!/usr/bin/env bash
#
# Regression proving the two-generation trust-root-poisoning fix
# (internal-critic finding on HSE-89): check-regression.sh and
# export-counterexample.sh are CODEOWNERS-protected self-tests of
# tools/check.sh's own detection logic, but until this fix they always
# invoked "$ROOT/tools/check.sh", where ROOT derived from their OWN script
# location -- i.e. whichever check.sh happened to sit beside wherever they
# were currently running from. verification.yml already extracts
# check-regression.sh itself from GITHUB_BASE_SHA for pull_request events
# (mirroring check.sh, check-conformance.sh, and the other base-pinned
# checkers), but that alone did not close the gap:
#
#   1. A PR edits tools/check.sh to quietly break detection (e.g. weaken a
#      grep or exit-code path so a seeded mutation no longer fails).
#   2. The same PR neuters check-regression.sh -- strips the seeded-mutation
#      assertions, or hardcodes the expected exit codes -- so the self-test
#      still prints PASS.
#   3. Every gate goes green, because the real per-PR content gate is fine
#      (this PR did not weaken a protected invariant), and the self-test
#      that would have caught "check.sh regressed" is disarmed.
#   4. It merges and becomes the new base. The next PR's extraction step
#      faithfully extracts the already-broken check.sh from that base, and
#      nothing in the chain ever proves check.sh actually detects anything.
#
# The fix: check-regression.sh (and export-counterexample.sh) now resolve
# the check.sh they invoke via CIDX_CHECK_SH, independent of CIDX_REPO_ROOT
# (which still points model/module asset resolution at the real checkout,
# since those ARE legitimate PR content to seed mutations from).
# verification.yml sets CIDX_CHECK_SH to the SAME base-extracted,
# untamperable check.sh it already runs for the real gate, extracted from
# the same GITHUB_BASE_SHA as check-regression.sh itself.
#
# This builds a throwaway git repo with a "base" commit (known-good check.sh
# and check-regression.sh, copied verbatim from this checkout, alongside the
# full real models/modules/protected assets check-regression.sh's suite
# needs) and a "head" commit performing exactly the attack above, then
# proves, without network access or a TLA2TOOLS download beyond what is
# already cached:
#
#   1. Head's own neutered check-regression.sh -- run with no CIDX_CHECK_SH
#      override, i.e. exactly the pre-fix wiring -- WOULD pass, confirming
#      the vulnerability existed absent the fix.
#   2. The base-extracted, un-neutered check-regression.sh, given
#      CIDX_CHECK_SH pointed at the base-extracted, unweakened check.sh
#      (exactly what verification.yml now wires for pull_request events),
#      runs its real suite end to end and correctly detects every seeded
#      mutation -- proving detection survives even though HEAD's own copies
#      of both files are compromised.
#   3. Pointing that SAME base-extracted check-regression.sh's CIDX_CHECK_SH
#      at head's weakened check.sh instead (simulating the base-pin being
#      reverted) makes its very first assertion fail closed for the WRONG
#      reason (the weakened check.sh no longer detects the seeded
#      invariant-mismatch at all) -- proving test 2 is not vacuously true
#      and genuinely depends on both scripts coming from the same immutable
#      base revision, not merely on check-regression.sh being "some"
#      extracted copy.
#   4. The identical CIDX_CHECK_SH fix in export-counterexample.sh: a
#      base-extracted, un-neutered export-counterexample.sh, given
#      CIDX_CHECK_SH pointed at the base-extracted check.sh, still
#      reproduces the exact golden counterexample; pointed at head's
#      weakened check.sh instead, its seed no longer fails as expected and
#      the demo pipeline itself refuses to export a (potentially dishonest)
#      counterexample.
#
# Cost note: test 2 and test 4's "defense holds" runs execute the REAL
# check-regression.sh suite and a real TLC counterexample export to
# completion (not a scoped reimplementation), because the attack this fix
# closes is specifically about self-tests being neutered -- a scoped
# stand-in would not exercise the real assertions the attack disarms. Every
# OTHER path below fails closed on its very first (TLC-free or single-model)
# check, so only one full suite run and one single-model TLC run are paid.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-self-test-tamper.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

die() {
  echo "TLA_SELF_TEST_TAMPER_REGRESSION_STATUS=FAIL reason=$1" >&2
  exit 1
}

REPO="$WORK/repo"
mkdir -p "$REPO/spec/tla/tools" "$REPO/spec/tla/models" "$REPO/spec/tla/modules" \
  "$REPO/spec/tla/protected" "$REPO/spec/tla/counterexamples/golden"
cp "$REPO_ROOT/spec/tla/tools/check.sh" "$REPO/spec/tla/tools/check.sh"
cp "$REPO_ROOT/spec/tla/tools/check-regression.sh" "$REPO/spec/tla/tools/check-regression.sh"
cp "$REPO_ROOT/spec/tla/tools/export-counterexample.sh" "$REPO/spec/tla/tools/export-counterexample.sh"
cp "$REPO_ROOT"/spec/tla/models/*.cfg "$REPO/spec/tla/models/"
cp "$REPO_ROOT"/spec/tla/models/*.tla "$REPO/spec/tla/models/"
cp "$REPO_ROOT"/spec/tla/modules/*.tla "$REPO/spec/tla/modules/"
cp "$REPO_ROOT"/spec/tla/protected/*.tla "$REPO/spec/tla/protected/"
cp "$REPO_ROOT/spec/tla/counterexamples/golden/trusted-outcome-violation.json" \
  "$REPO/spec/tla/counterexamples/golden/trusted-outcome-violation.json"
chmod +x "$REPO/spec/tla/tools/check.sh" \
  "$REPO/spec/tla/tools/check-regression.sh" \
  "$REPO/spec/tla/tools/export-counterexample.sh"

git -C "$REPO" init --quiet --initial-branch=main
git -C "$REPO" config user.email "test@example.com"
git -C "$REPO" config user.name "test"
git -C "$REPO" add -A
git -C "$REPO" commit --quiet -m base
BASE_SHA="$(git -C "$REPO" rev-parse HEAD)"

# Simulate the attack: stub check.sh so it always passes (step 1), and
# neuter check-regression.sh so it hardcodes the exact success line the real
# suite would print, without running anything (step 2).
cat >"$REPO/spec/tla/tools/check.sh" <<'STUB'
#!/usr/bin/env bash
echo "TLA_CHECK_STATUS=PASS models=stub-always-passes"
exit 0
STUB
cat >"$REPO/spec/tla/tools/check-regression.sh" <<'STUB'
#!/usr/bin/env bash
echo "TLA_REGRESSION_STATUS=PASS mutation=missing-ProtectedInvariant seeded=7"
exit 0
STUB
chmod +x "$REPO/spec/tla/tools/check.sh" "$REPO/spec/tla/tools/check-regression.sh"

git -C "$REPO" add -A
git -C "$REPO" commit --quiet \
  -m "head: stub check.sh and neuter check-regression.sh in the same commit"

# --- Test 1: head's own neutered check-regression.sh, run with the pre-fix
# wiring (no CIDX_CHECK_SH override), would have passed. ---

if ! (cd "$REPO" && ./spec/tla/tools/check-regression.sh) \
    >"$WORK/head-run.log" 2>&1; then
  cat "$WORK/head-run.log" >&2
  die "heads-own-neutered-self-test-unexpectedly-failed-test-setup-is-wrong"
fi
grep -q "TLA_REGRESSION_STATUS=PASS mutation=missing-ProtectedInvariant seeded=7" \
  "$WORK/head-run.log" \
  || { cat "$WORK/head-run.log" >&2; die "unexpected-head-output"; }
echo "TLA_SELF_TEST_TAMPER_STATUS=PASS check=heads-own-neutered-self-test-would-have-passed-confirming-the-bug-this-fix-closes"

# --- Test 2: the base-extracted, un-neutered check-regression.sh, wired via
# CIDX_CHECK_SH to the base-extracted, unweakened check.sh, runs the real
# suite to completion and detects every seeded mutation. ---

EXTRACT="$REPO_ROOT/spec/tla/tools/extract-trusted-checker.sh"

(cd "$REPO" && "$EXTRACT" "$BASE_SHA" spec/tla/tools/check.sh "$WORK/base-check.sh") \
  >"$WORK/extract-check.log" 2>&1 \
  || { cat "$WORK/extract-check.log" >&2; die "failed-to-extract-base-check.sh"; }
(cd "$REPO" && "$EXTRACT" "$BASE_SHA" spec/tla/tools/check-regression.sh \
  "$WORK/base-check-regression.sh") >"$WORK/extract-check-regression.log" 2>&1 \
  || { cat "$WORK/extract-check-regression.log" >&2; die "failed-to-extract-base-check-regression.sh"; }

set +e
CIDX_REPO_ROOT="$REPO" CIDX_CHECK_SH="$WORK/base-check.sh" \
  "$WORK/base-check-regression.sh" >"$WORK/defense-run.log" 2>&1
defense_status=$?
set -e

if [[ "$defense_status" -ne 0 ]]; then
  cat "$WORK/defense-run.log" >&2
  die "base-extracted-check-regression-did-not-pass-against-the-real-check.sh"
fi
grep -q "TLA_REGRESSION_STATUS=PASS mutation=missing-ProtectedInvariant seeded=7" \
  "$WORK/defense-run.log" \
  || { cat "$WORK/defense-run.log" >&2; die "unexpected-defense-run-output"; }
echo "TLA_SELF_TEST_TAMPER_STATUS=PASS check=base-extracted-check-regression-detects-every-seeded-mutation-via-cidx-check-sh"

# --- Test 3: the SAME base-extracted check-regression.sh, pointed instead
# at head's weakened check.sh (simulating the base-pin being reverted),
# fails closed for the WRONG reason -- the weakened check.sh no longer
# detects the seeded invariant-mismatch at all -- proving test 2 genuinely
# depends on both scripts sharing the same base revision. ---

set +e
CIDX_REPO_ROOT="$REPO" CIDX_CHECK_SH="$REPO/spec/tla/tools/check.sh" \
  "$WORK/base-check-regression.sh" >"$WORK/reverted-run.log" 2>&1
reverted_status=$?
set -e

if [[ "$reverted_status" -eq 0 ]]; then
  cat "$WORK/reverted-run.log" >&2
  die "base-extracted-check-regression-passed-against-heads-weakened-check.sh"
fi
grep -q "TLA_REGRESSION_STATUS=FAIL reason=unexpected-exit-0" "$WORK/reverted-run.log" \
  || { cat "$WORK/reverted-run.log" >&2; die "unexpected-reverted-run-failure-mode"; }
echo "TLA_SELF_TEST_TAMPER_STATUS=PASS check=reverting-cidx-check-sh-to-heads-weakened-copy-breaks-detection-proving-test-2-is-not-vacuous"

# --- Test 4: the identical CIDX_CHECK_SH fix in export-counterexample.sh.
# Base-extracted, un-neutered, wired to the base-extracted check.sh, it
# still reproduces the exact golden counterexample; wired to head's
# weakened check.sh instead, its seed no longer fails as expected and it
# refuses to export at all. ---

(cd "$REPO" && "$EXTRACT" "$BASE_SHA" spec/tla/tools/export-counterexample.sh \
  "$WORK/base-export-counterexample.sh") >"$WORK/extract-export.log" 2>&1 \
  || { cat "$WORK/extract-export.log" >&2; die "failed-to-extract-base-export-counterexample.sh"; }

set +e
CIDX_REPO_ROOT="$REPO" CIDX_CHECK_SH="$WORK/base-check.sh" \
  "$WORK/base-export-counterexample.sh" --demo --out "$WORK/demo-defense.json" \
  >"$WORK/demo-defense.log" 2>&1
demo_defense_status=$?
set -e

if [[ "$demo_defense_status" -ne 0 ]]; then
  cat "$WORK/demo-defense.log" >&2
  die "base-extracted-export-counterexample-did-not-reproduce-the-demo-against-the-real-check.sh"
fi
diff "$WORK/demo-defense.json" \
  "$REPO_ROOT/spec/tla/counterexamples/golden/trusted-outcome-violation.json" \
  || die "base-extracted-export-counterexample-demo-does-not-match-the-golden-file"
echo "TLA_SELF_TEST_TAMPER_STATUS=PASS check=base-extracted-export-counterexample-reproduces-the-golden-file-via-cidx-check-sh"

set +e
CIDX_REPO_ROOT="$REPO" CIDX_CHECK_SH="$REPO/spec/tla/tools/check.sh" \
  "$WORK/base-export-counterexample.sh" --demo --out "$WORK/demo-reverted.json" \
  >"$WORK/demo-reverted.log" 2>&1
demo_reverted_status=$?
set -e

if [[ "$demo_reverted_status" -eq 0 ]]; then
  cat "$WORK/demo-reverted.log" >&2
  die "base-extracted-export-counterexample-exported-a-demo-against-heads-weakened-check.sh"
fi
grep -q "TLA_COUNTEREXAMPLE_EXPORT_STATUS=FAIL reason=seed-did-not-fail-as-expected" \
  "$WORK/demo-reverted.log" \
  || { cat "$WORK/demo-reverted.log" >&2; die "unexpected-demo-reverted-failure-mode"; }
echo "TLA_SELF_TEST_TAMPER_STATUS=PASS check=reverting-cidx-check-sh-makes-export-counterexample-refuse-to-export-a-demo"

echo "TLA_SELF_TEST_TAMPER_REGRESSION_STATUS=PASS"
