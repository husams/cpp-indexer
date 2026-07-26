#!/usr/bin/env bash
#
# Regression proving the P1 fix (internal-critic review of HSE-89, round 5):
# the scripts that actually RUN TLC/TLAPS/conformance (check.sh,
# check-conformance.sh, check-sidecar-conformance.sh, check-proofs.sh) had
# the same self-policing gap check-protected-review.sh and
# check-gate-selection.sh were already fixed for -- a single PR could (a)
# weaken a protected invariant (e.g. drop ProtectedInvariant from a model
# config, or edit protected/CidxProtected.tla) and (b) in the same commit
# replace check.sh with a stub that exits 0, so CI would run that tampered
# copy straight from the PR's own checkout, since verification.yml
# previously invoked these scripts in-tree with no base-SHA extraction.
#
# This builds a throwaway git repo with a "base" commit (known-good check.sh
# and spec files, copied verbatim from this checkout) and a "head" commit
# that performs exactly that attack, then proves two things without any
# network access, TLA2TOOLS jar download beyond what is already cached, or
# GitHub token:
#
#   1. Head's own (stubbed) check.sh, run directly, WOULD pass -- confirming
#      the vulnerability existed absent the fix.
#   2. check.sh extracted from BASE and run with CIDX_REPO_ROOT pointed at
#      HEAD's checkout -- exactly what verification.yml now does for
#      pull_request events -- correctly resolves ROOT to HEAD's actual
#      spec/tla/ (not the extracted script's own location) and FAILS on the
#      weakened invariant. This proves both the base-pin defense and the
#      CIDX_REPO_ROOT bootstrap it depends on: a bug that silently ignored
#      CIDX_REPO_ROOT (falling back to the extracted script's own, mostly
#      empty, directory) would fail for an unrelated reason (missing
#      models/modules), not the expected invariant-mismatch, and this
#      regression checks for the exact expected reason string to catch that.
#
# Representative rather than exhaustive: check-conformance.sh,
# check-sidecar-conformance.sh, and check-proofs.sh received the identical
# CIDX_REPO_ROOT fix and are wired into verification.yml the same way this
# regression proves for check.sh. check.sh is exercised here because it can
# fail fast -- a config/invariant-list mismatch, before any TLC run -- without
# needing a full conformance-scenario TLC replay or the Linux-only TLAPS
# toolchain (check-proofs.sh's tlapm installer is x86_64-linux-gnu only and
# is not part of this repo's local/macOS gate set for that reason).
#
# Round-6 security review added tests 3-6 below, proving the extraction
# mechanism itself (not just check.sh's CIDX_REPO_ROOT behavior) closes two
# further gaps: a bootstrap fallback that fires on ANY nonzero
# `git cat-file -e BASE:PATH` cannot tell "checker is genuinely new" apart
# from "the base commit never resolved" (fixed in extract-trusted-checker.sh,
# the single shared implementation every extraction step but
# check-protected-review.sh's now calls); and an extraction destination
# inside the checked-out repository tree is a path the PR under review fully
# controls (test 4 proves the helper refuses this outright, rather than
# relying on write-ordering or symlink-following semantics to stay safe).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-verification-tamper.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

die() {
  echo "TLA_VERIFICATION_TAMPER_REGRESSION_STATUS=FAIL reason=$1" >&2
  exit 1
}

REPO="$WORK/repo"
mkdir -p "$REPO/spec/tla/tools" "$REPO/spec/tla/models" "$REPO/spec/tla/modules" \
  "$REPO/spec/tla/protected"
cp "$REPO_ROOT/spec/tla/tools/check.sh" "$REPO/spec/tla/tools/check.sh"
cp "$REPO_ROOT"/spec/tla/models/*.cfg "$REPO/spec/tla/models/"
cp "$REPO_ROOT"/spec/tla/models/*.tla "$REPO/spec/tla/models/"
cp "$REPO_ROOT"/spec/tla/modules/*.tla "$REPO/spec/tla/modules/"
cp "$REPO_ROOT"/spec/tla/protected/*.tla "$REPO/spec/tla/protected/"
chmod +x "$REPO/spec/tla/tools/check.sh"

git -C "$REPO" init --quiet --initial-branch=main
git -C "$REPO" config user.email "test@example.com"
git -C "$REPO" config user.name "test"
git -C "$REPO" add -A
git -C "$REPO" commit --quiet -m base
BASE_SHA="$(git -C "$REPO" rev-parse HEAD)"

# Simulate the attack: drop ProtectedInvariant from a model config (the same
# weakening check-regression.sh already proves check.sh detects), and stub
# check.sh so CI's own (head) copy would never notice.
sed '/^INVARIANT ProtectedInvariant$/d' \
  "$REPO/spec/tla/models/CidxRepositorySmoke.cfg" \
  >"$REPO/spec/tla/models/CidxRepositorySmoke.cfg.mutated"
mv "$REPO/spec/tla/models/CidxRepositorySmoke.cfg.mutated" \
  "$REPO/spec/tla/models/CidxRepositorySmoke.cfg"

cat >"$REPO/spec/tla/tools/check.sh" <<'STUB'
#!/usr/bin/env bash
echo "TLA_CHECK_STATUS=PASS models=stub-always-passes"
exit 0
STUB
chmod +x "$REPO/spec/tla/tools/check.sh"

git -C "$REPO" add -A
git -C "$REPO" commit --quiet \
  -m "head: drop ProtectedInvariant from CidxRepositorySmoke and stub check.sh"
HEAD_SHA="$(git -C "$REPO" rev-parse HEAD)"

# --- Test 1: head's own (stubbed) checker would have passed. ---

if ! (cd "$REPO" && ./spec/tla/tools/check.sh) >"$WORK/head-run.log" 2>&1; then
  cat "$WORK/head-run.log" >&2
  die "heads-own-stubbed-checker-unexpectedly-failed-test-setup-is-wrong"
fi
grep -q "TLA_CHECK_STATUS=PASS" "$WORK/head-run.log" \
  || { cat "$WORK/head-run.log" >&2; die "unexpected-head-output"; }
echo "TLA_VERIFICATION_TAMPER_STATUS=PASS check=heads-own-stubbed-checker-would-have-passed-confirming-the-bug-this-fix-closes"

# --- Test 2: the extracted-from-base checker, pointed at head via
# CIDX_REPO_ROOT, resolves ROOT to head's real spec/tla/ and catches the
# weakened invariant. ---

git -C "$REPO" show "${BASE_SHA}:spec/tla/tools/check.sh" >"$WORK/base-check.sh"
chmod +x "$WORK/base-check.sh"

set +e
TLA_MODELS="CidxRepositorySmoke" CIDX_REPO_ROOT="$REPO" "$WORK/base-check.sh" \
  >"$WORK/base-run.log" 2>&1
base_status=$?
set -e

if [[ "$base_status" -eq 0 ]]; then
  cat "$WORK/base-run.log" >&2
  die "extracted-base-checker-passed-against-weakened-head-model"
fi
grep -q "TLA_CONFIG_STATUS=FAIL model=CidxRepositorySmoke reason=invariant-mismatch" \
  "$WORK/base-run.log" \
  || { cat "$WORK/base-run.log" >&2; die "unexpected-failure-mode-from-base-checker"; }
echo "TLA_VERIFICATION_TAMPER_STATUS=PASS check=extracted-base-checker-caught-weakened-invariant-via-cidx-repo-root"

# --- Test 3: extract-trusted-checker.sh -- the single shared implementation
# every base-pinning extraction step now calls (round-6 security fix) --
# hard-fails on an unresolvable base commit instead of silently bootstrapping
# from head. Without this, `git cat-file -e BASE:PATH` failing for the WRONG
# reason (bad ref, unfetched base, shallow clone) would be indistinguishable
# from "checker is genuinely new in this PR" and would execute the PR's own
# (possibly rewritten-to-always-pass) copy.

EXTRACT="$REPO_ROOT/spec/tla/tools/extract-trusted-checker.sh"

set +e
(cd "$REPO" && "$EXTRACT" \
  "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef" \
  spec/tla/tools/check.sh \
  "$WORK/should-not-exist.sh") >"$WORK/bogus-base.log" 2>&1
bogus_base_status=$?
set -e

if [[ "$bogus_base_status" -eq 0 ]]; then
  cat "$WORK/bogus-base.log" >&2
  die "extract-trusted-checker-bootstrapped-from-an-unresolvable-base"
fi
if [[ -e "$WORK/should-not-exist.sh" ]]; then
  die "extract-trusted-checker-wrote-a-destination-despite-failing"
fi
grep -q "TLA_TRUSTED_EXTRACT_STATUS=FAIL reason=base-commit-unresolvable:deadbeef" \
  "$WORK/bogus-base.log" \
  || { cat "$WORK/bogus-base.log" >&2; die "unexpected-bogus-base-failure-mode"; }
echo "TLA_VERIFICATION_TAMPER_STATUS=PASS check=extract-trusted-checker-hard-fails-on-unresolvable-base-instead-of-bootstrapping"

# --- Test 4: extract-trusted-checker.sh refuses to write inside the
# checkout at all -- structurally closing the symlink/regular-file/directory
# attack surface a PR-controlled in-tree destination would otherwise open,
# rather than relying on write-ordering or filesystem symlink-following
# semantics to keep it safe. ---

run_in_tree_destination_case() {
  local name="$1" destination="$2"
  local log="$WORK/in-tree-dest-${name}.log"
  set +e
  (cd "$REPO" && "$EXTRACT" \
    "$BASE_SHA" \
    spec/tla/tools/check.sh \
    "$destination") >"$log" 2>&1
  local status=$?
  set -e
  if [[ "$status" -eq 0 ]]; then
    cat "$log" >&2
    die "extract-trusted-checker-accepted-an-in-tree-destination:${name}"
  fi
  grep -q "TLA_TRUSTED_EXTRACT_STATUS=FAIL reason=destination-must-be-outside-checkout" \
    "$log" \
    || { cat "$log" >&2; die "unexpected-in-tree-destination-failure-mode:${name}"; }
}

run_in_tree_destination_case \
  relative \
  "spec/tla/tools/.trusted-check-relative.sh"
run_in_tree_destination_case \
  absolute \
  "$REPO/spec/tla/tools/.trusted-check-absolute.sh"
ln -s "$REPO/spec/tla/tools" "$WORK/symlinked-checkout-tools"
run_in_tree_destination_case \
  symlink-parent \
  "$WORK/symlinked-checkout-tools/.trusted-check-symlink.sh"

if compgen -G "$REPO/spec/tla/tools/.trusted-check-*.sh" >/dev/null; then
  die "extract-trusted-checker-wrote-inside-the-checkout"
fi
echo "TLA_VERIFICATION_TAMPER_STATUS=PASS check=extract-trusted-checker-refuses-relative-absolute-and-symlinked-in-tree-destinations"

# --- Test 5: the two legitimate outcomes -- extracting an existing base
# copy versus bootstrapping a genuinely-new-at-that-base checker -- are each
# exercised directly against real (not simulated) base commits of this very
# repository, proving mode=base never silently substitutes head content and
# mode=bootstrap only ever fires when the base commit itself resolved. ---

REAL_BASE="$(git -C "$REPO_ROOT" rev-parse HEAD~3)"

"$EXTRACT" "$REAL_BASE" spec/tla/tools/check.sh "$WORK/real-base-existing.sh" \
  >"$WORK/real-base-existing.log" 2>&1
grep -q "TLA_TRUSTED_EXTRACT_STATUS=PASS mode=base ref=${REAL_BASE}" \
  "$WORK/real-base-existing.log" \
  || { cat "$WORK/real-base-existing.log" >&2; die "unexpected-real-base-existing-output"; }
diff <(git -C "$REPO_ROOT" show "${REAL_BASE}:spec/tla/tools/check.sh") \
  "$WORK/real-base-existing.sh" \
  || die "real-base-existing-content-does-not-match-base-not-head"
echo "TLA_VERIFICATION_TAMPER_STATUS=PASS check=extract-trusted-checker-extracts-base-content-for-a-checker-that-exists-at-that-base"

"$EXTRACT" "$REAL_BASE" spec/tla/tools/check-verification-tamper-regression.sh \
  "$WORK/real-base-bootstrap.sh" >"$WORK/real-base-bootstrap.log" 2>&1
grep -q "TLA_TRUSTED_EXTRACT_STATUS=PASS mode=bootstrap ref=$" \
  "$WORK/real-base-bootstrap.log" \
  || { cat "$WORK/real-base-bootstrap.log" >&2; die "unexpected-real-base-bootstrap-output"; }
diff spec/tla/tools/check-verification-tamper-regression.sh \
  "$WORK/real-base-bootstrap.sh" \
  || die "real-base-bootstrap-content-does-not-match-head"
echo "TLA_VERIFICATION_TAMPER_STATUS=PASS check=extract-trusted-checker-bootstraps-from-head-only-for-a-checker-genuinely-absent-at-that-base"

# --- Test 6: every workflow extraction step delegates to
# extract-trusted-checker.sh and writes to $RUNNER_TEMP (outside the
# checkout), except check-protected-review.sh, which stays unconditional,
# hand-written, and destined for $RUNNER_TEMP too -- it never calls the
# shared helper's first-introduction bootstrap branch at all. This is a
# static workflow contract because GitHub expression interpolation is not
# available to local shell tests; the executable proofs above cover the
# extraction logic's actual runtime behavior.

python3 - "$REPO_ROOT/.github/workflows/verification.yml" <<'PY'
import pathlib
import re
import sys

workflow = pathlib.Path(sys.argv[1]).read_text()
# Expected extraction-call count per checker (default 1). check.sh is
# extracted twice: once in tla-syntax-and-model for the real gate (and to
# wire check-regression.sh's CIDX_CHECK_SH there), and again in
# tla-conformance solely so export-counterexample.sh's --demo path has a
# base-extracted check.sh to invoke via its own CIDX_CHECK_SH. Both
# check-regression.sh and export-counterexample.sh are self-tests of
# check.sh's detection logic with the identical two-generation
# trust-root-poisoning gap (see check-self-test-tamper-regression.sh); each
# is extracted once, alongside the check.sh it is wired to via CIDX_CHECK_SH.
checkers = {
    "check-gate-selection.sh": 1,
    "check-gate-selection-defense-regression.sh": 1,
    "check.sh": 2,
    "check-proofs.sh": 1,
    "check-conformance.sh": 1,
    "check-sidecar-conformance.sh": 1,
    "check-regression.sh": 1,
    "export-counterexample.sh": 1,
}

for checker, expected_count in checkers.items():
    relative = f"spec/tla/tools/{checker}"
    marker = f"extract-trusted-checker.sh \\\n            \"${{{{ github.event.pull_request.base.sha }}}}\" \\\n            {relative} \\\n"
    actual_count = workflow.count(marker)
    if actual_count != expected_count:
        raise SystemExit(
            "TLA_VERIFICATION_TAMPER_REGRESSION_STATUS=FAIL "
            f"reason=shared-helper-call-count-mismatch:{checker}:expected={expected_count}:actual={actual_count}"
        )
    search_from = 0
    for _ in range(expected_count):
        start = workflow.index(marker, search_from)
        destination_match = re.search(
            r'"(\$RUNNER_TEMP/[^"]+)"', workflow[start : start + len(marker) + 200]
        )
        if destination_match is None or destination_match.group(1) != f"$RUNNER_TEMP/{checker}":
            raise SystemExit(
                "TLA_VERIFICATION_TAMPER_REGRESSION_STATUS=FAIL "
                f"reason=shared-helper-destination-not-runner-temp:{checker}"
            )
        search_from = start + len(marker)
    # export-counterexample.sh's run step passes --demo/--out arguments after
    # the quoted path (it is a multi-line `run: |` block, unlike the other
    # single-command checkers), so its execution marker cannot require the
    # closing quote to be followed immediately by end-of-line.
    execution_marker = (
        f'"$RUNNER_TEMP/{checker}" --demo'
        if checker == "export-counterexample.sh"
        else f'run: "$RUNNER_TEMP/{checker}"'
    )
    if execution_marker not in workflow:
        raise SystemExit(
            "TLA_VERIFICATION_TAMPER_REGRESSION_STATUS=FAIL "
            f"reason=shared-helper-execution-not-runner-temp:{checker}"
        )

# check-regression.sh and export-counterexample.sh must each pass
# CIDX_CHECK_SH pointing at a $RUNNER_TEMP-extracted check.sh on their
# pull_request run step, not merely extract it and leave it unused -- that
# would silently reintroduce the CIDX_CHECK_SH-shaped hole this fix closes.
for checker in ("check-regression.sh", "export-counterexample.sh"):
    run_marker = (
        f'"$RUNNER_TEMP/{checker}" --demo'
        if checker == "export-counterexample.sh"
        else f'run: "$RUNNER_TEMP/{checker}"'
    )
    run_start = workflow.rindex(run_marker)
    step_start = workflow.rindex("- name:", 0, run_start)
    step_block = workflow[step_start:run_start]
    if "CIDX_CHECK_SH: ${{ runner.temp }}/check.sh" not in step_block:
        raise SystemExit(
            "TLA_VERIFICATION_TAMPER_REGRESSION_STATUS=FAIL "
            f"reason=cidx-check-sh-not-wired-to-runner-temp:{checker}"
        )

# check-protected-review.sh must never call the shared helper: it is the one
# gate that enforces human review of every other protected path (including
# its own CODEOWNERS/protectedPaths entries), so its extraction step stays
# unconditional and hand-written rather than sharing any conditional logic
# that could become a lever to weaken it.
protected_review_marker = 'git show "${{ github.event.pull_request.base.sha }}:spec/tla/tools/check-protected-review.sh"'
if workflow.count(protected_review_marker) != 1:
    raise SystemExit(
        "TLA_VERIFICATION_TAMPER_REGRESSION_STATUS=FAIL "
        "reason=protected-review-extraction-missing-or-duplicated"
    )
protected_review_start = workflow.index(protected_review_marker)
protected_review_end = workflow.find("\n      - name:", protected_review_start)
protected_review_block = workflow[protected_review_start:protected_review_end]
if "extract-trusted-checker.sh" in protected_review_block:
    raise SystemExit(
        "TLA_VERIFICATION_TAMPER_REGRESSION_STATUS=FAIL "
        "reason=protected-review-must-not-use-shared-bootstrap-helper"
    )
if (
    '> "$RUNNER_TEMP/check-protected-review.sh"' not in protected_review_block
    or 'chmod +x "$RUNNER_TEMP/check-protected-review.sh"' not in protected_review_block
):
    raise SystemExit(
        "TLA_VERIFICATION_TAMPER_REGRESSION_STATUS=FAIL "
        "reason=protected-review-destination-not-runner-temp"
    )
if 'run: "$RUNNER_TEMP/check-protected-review.sh"' not in workflow:
    raise SystemExit(
        "TLA_VERIFICATION_TAMPER_REGRESSION_STATUS=FAIL "
        "reason=protected-review-execution-not-runner-temp"
    )

# No extraction destination anywhere in the workflow may resolve inside the
# checked-out repository tree (the P1 symlink/regular-file/directory attack
# surface a PR fully controls).
if re.search(r'"\$GITHUB_WORKSPACE/spec/tla/tools/', workflow):
    raise SystemExit(
        "TLA_VERIFICATION_TAMPER_REGRESSION_STATUS=FAIL "
        "reason=in-tree-extraction-destination-present"
    )

defense = "check-gate-selection-defense-regression.sh"
defense_start = workflow.index(f"extract-trusted-checker.sh \\\n            \"${{{{ github.event.pull_request.base.sha }}}}\" \\\n            spec/tla/tools/{defense} \\\n")
defense_end = workflow.index("\n      - name:", defense_start)
defense_block = workflow[defense_start:defense_end]
for output in (
    'echo "fixture_ref=${{ github.event.pull_request.base.sha }}" >> "$GITHUB_OUTPUT"',
    'echo "fixture_ref=" >> "$GITHUB_OUTPUT"',
):
    if output not in defense_block:
        raise SystemExit(
            "TLA_VERIFICATION_TAMPER_REGRESSION_STATUS=FAIL "
            "reason=gate-defense-fixture-ref-bootstrap-missing"
        )

print(
    "TLA_VERIFICATION_TAMPER_STATUS=PASS "
    f"check=workflow-checker-bootstrap-and-base-pin checkers={len(checkers)}"
)
PY

echo "TLA_VERIFICATION_TAMPER_REGRESSION_STATUS=PASS"
