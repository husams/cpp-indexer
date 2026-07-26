#!/usr/bin/env bash
#
# Regression proving the P1 fix (internal-critic review of HSE-89): the
# gate-selection defense (check-gate-selection.sh, ci-dependency-map.json)
# had the exact self-policing gap check-protected-review.sh was already
# fixed for -- a single PR could (a) narrow ci-dependency-map.json's
# sidecar-publication flow to drop tla-sidecar-conformance, silently
# reverting the round-3 fix, and (b) delete the matching run_case assertion
# in check-gate-selection.sh in the same change, so CI would run the
# tampered checker from that PR's own head and see it pass.
#
# This builds a throwaway git repo with a "base" commit (known-good policy
# files, copied verbatim from this checkout) and a "head" commit that
# performs exactly that attack, then proves two things without any network
# access or GitHub token:
#
#   1. Executing verification.yml's actual defense -- extract
#      check-gate-selection.sh from base, run THAT copy with CIDX_REPO_ROOT
#      pointed at head's checkout -- FAILS (catches the weakened map), even
#      though head's own (self-neutered) copy of the same script, run the
#      same way, WOULD pass. This proves the extracted-from-base checker,
#      not head's, is what must run and does catch the regression.
#   2. Executing verification.yml's other defense -- extract
#      check-protected-review.sh from base and run it against the base/head
#      diff -- reports the change as requiring CODEOWNER approval of the
#      PR's head SHA, and FAILS when (as in this test) no such approval
#      exists. This proves a head-side edit to either
#      spec/tla/ci-dependency-map.json or
#      spec/tla/tools/check-gate-selection.sh cannot merge without owner
#      approval, now that both are in manifest.json's protectedPaths and
#      CODEOWNERS.
#
# A real GitHub token/API is not exercised (GitHub Actions is disabled in
# this repo); a fake `curl` on PATH stands in for the reviews API and always
# reports zero reviews, which is sufficient to prove the fail-closed
# behavior in (2). check-protected-review.sh's own env/argument contract,
# CODEOWNERS-matching, and manifest-matching code is exercised unmodified.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-gate-selection-defense.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

die() {
  echo "TLA_GATE_SELECTION_DEFENSE_REGRESSION_STATUS=FAIL reason=$1" >&2
  exit 1
}

REPO="$WORK/repo"
mkdir -p "$REPO/spec/tla/tools" "$REPO/.github"
cp "$REPO_ROOT/spec/tla/manifest.json" "$REPO/spec/tla/manifest.json"
cp "$REPO_ROOT/.github/CODEOWNERS" "$REPO/.github/CODEOWNERS"
cp "$REPO_ROOT/spec/tla/ci-dependency-map.json" "$REPO/spec/tla/ci-dependency-map.json"
cp "$REPO_ROOT/spec/tla/tools/check-gate-selection.sh" "$REPO/spec/tla/tools/check-gate-selection.sh"
cp "$REPO_ROOT/spec/tla/tools/select-changed-gates.sh" "$REPO/spec/tla/tools/select-changed-gates.sh"
cp "$REPO_ROOT/spec/tla/tools/check-protected-review.sh" "$REPO/spec/tla/tools/check-protected-review.sh"
chmod +x "$REPO"/spec/tla/tools/*.sh

git -C "$REPO" init --quiet --initial-branch=main
git -C "$REPO" config user.email "test@example.com"
git -C "$REPO" config user.name "test"
git -C "$REPO" add -A
git -C "$REPO" commit --quiet -m base
BASE_SHA="$(git -C "$REPO" rev-parse HEAD)"

# Simulate the attack: narrow the sidecar-publication flow's gates, and
# delete the run_case assertion that would otherwise catch it.
python3 - "$REPO/spec/tla/ci-dependency-map.json" <<'PY'
import json
import sys

path = sys.argv[1]
manifest = json.loads(open(path).read())
for flow in manifest["flows"]:
    if flow["name"] == "sidecar-publication":
        flow["gates"] = [g for g in flow["gates"] if g != "tla-sidecar-conformance"]
        break
else:
    raise SystemExit("test setup error: sidecar-publication flow not found")
open(path, "w").write(json.dumps(manifest, indent=2) + "\n")
PY

python3 - "$REPO/spec/tla/tools/check-gate-selection.sh" <<'PY'
import sys

path = sys.argv[1]
text = open(path).read()
# Both run_case blocks assert tla_sidecar_conformance=true for a path this
# flow covers (conformance_recorder.cpp and conformance_schema.hpp are both
# listed in the sidecar-publication flow); a real attacker hiding the
# ci-dependency-map.json narrowing from this self-test would need to delete
# both, not just one.
needles = (
    'run_case conformance-recorder-selects-sidecar-gate \\\n'
    '  "tla_conformance,tla_sidecar_conformance,cpp_default" \\\n'
    '  "tla_syntax_and_model,tla_proofs,tla_policy" \\\n'
    '  "src/application/conformance_recorder.cpp"\n',
    'run_case conformance_schema-selects-both-conformance-gates \\\n'
    '  "tla_conformance,tla_sidecar_conformance,cpp_default" \\\n'
    '  "tla_syntax_and_model,tla_proofs,tla_policy" \\\n'
    '  "src/application/conformance_schema.hpp"\n',
)
for needle in needles:
    if needle not in text:
        raise SystemExit("test setup error: expected run_case block not found verbatim")
    text = text.replace(needle, "")
open(path, "w").write(text)
PY

git -C "$REPO" add -A
git -C "$REPO" commit --quiet -m "head: narrow sidecar-publication mapping and its own assertion"
HEAD_SHA="$(git -C "$REPO" rev-parse HEAD)"

# --- Test 1: the extracted-from-base gate-selection checker catches the
# weakened head map; head's own (self-neutered) copy would not. ---

git -C "$REPO" show "${BASE_SHA}:spec/tla/tools/check-gate-selection.sh" \
  >"$WORK/base-check-gate-selection.sh"
chmod +x "$WORK/base-check-gate-selection.sh"

if CIDX_REPO_ROOT="$REPO" "$WORK/base-check-gate-selection.sh" \
    >"$WORK/base-run.log" 2>&1; then
  cat "$WORK/base-run.log" >&2
  die "extracted-base-checker-passed-against-weakened-head-map"
fi
grep -q "missing-true-tla_sidecar_conformance" "$WORK/base-run.log" \
  || { cat "$WORK/base-run.log" >&2; die "unexpected-failure-mode-from-base-checker"; }
echo "TLA_GATE_SELECTION_DEFENSE_STATUS=PASS check=extracted-base-checker-caught-weakened-map"

if ! CIDX_REPO_ROOT="$REPO" "$REPO/spec/tla/tools/check-gate-selection.sh" \
    >"$WORK/head-run.log" 2>&1; then
  cat "$WORK/head-run.log" >&2
  die "heads-own-neutered-checker-unexpectedly-failed-test-setup-is-wrong"
fi
echo "TLA_GATE_SELECTION_DEFENSE_STATUS=PASS check=heads-own-neutered-checker-would-have-passed-confirming-the-bug-this-fix-closes"

# --- Test 2: the extracted-from-base protected-review checker requires
# CODEOWNER approval of both tampered paths, and fails absent one. ---

git -C "$REPO" show "${BASE_SHA}:spec/tla/tools/check-protected-review.sh" \
  >"$WORK/base-check-protected-review.sh"
chmod +x "$WORK/base-check-protected-review.sh"

FAKE_BIN="$WORK/fake-bin"
mkdir -p "$FAKE_BIN"
cat >"$FAKE_BIN/curl" <<'SH'
#!/usr/bin/env bash
# Stand-in for the GitHub reviews API: reports zero reviews. No network
# access or GITHUB_TOKEN is exercised or required by this local regression.
echo "[]"
SH
chmod +x "$FAKE_BIN/curl"

set +e
PATH="$FAKE_BIN:$PATH" \
  GITHUB_TOKEN=fake-token \
  GITHUB_REPOSITORY=cidx-test/cidx-test \
  GITHUB_PR_NUMBER=1 \
  GITHUB_BASE_SHA="$BASE_SHA" \
  GITHUB_HEAD_SHA="$HEAD_SHA" \
  CIDX_REPO_ROOT="$REPO" \
  "$WORK/base-check-protected-review.sh" >"$WORK/protected-review.log" 2>&1
protected_review_status=$?
set -e

[[ "$protected_review_status" -ne 0 ]] \
  || { cat "$WORK/protected-review.log" >&2; die "protected-review-passed-without-approval"; }
grep -q "no-codeowner-approval-of-head-sha-for-paths" "$WORK/protected-review.log" \
  || { cat "$WORK/protected-review.log" >&2; die "unexpected-failure-mode-from-protected-review"; }
grep -q "spec/tla/ci-dependency-map.json" "$WORK/protected-review.log" \
  || { cat "$WORK/protected-review.log" >&2; die "ci-dependency-map.json-not-flagged-as-protected"; }
grep -q "spec/tla/tools/check-gate-selection.sh" "$WORK/protected-review.log" \
  || { cat "$WORK/protected-review.log" >&2; die "check-gate-selection.sh-not-flagged-as-protected"; }
echo "TLA_GATE_SELECTION_DEFENSE_STATUS=PASS check=protected-review-blocks-merge-without-codeowner-approval"

echo "TLA_GATE_SELECTION_DEFENSE_REGRESSION_STATUS=PASS"
