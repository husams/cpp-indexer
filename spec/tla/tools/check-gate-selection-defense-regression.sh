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
#
# P2 fix (internal-critic review, round 5): this script is CODEOWNERS-
# protected, but a PR that edited both ci-dependency-map.json and this
# script together could previously make this self-test's own "known good"
# fixtures self-consistent with the tampering (fixtures were `cp`-ed from the
# checked-out working tree, i.e. the PR's own head, not from an
# independently-trusted commit) -- the same assurance gap already closed for
# check-gate-selection.sh's DATA (as opposed to its LOGIC; see that script's
# own "Judgement call" comment on why its data intentionally stays at head).
# Unlike check-gate-selection.sh, this script's whole job is to prove the
# defense mechanism itself still works, independent of whatever the PR under
# test did -- so its fixtures should come from a fixed, trusted reference,
# not from head. When CIDX_FIXTURE_REF is set (verification.yml sets it to
# GITHUB_BASE_SHA for pull_request events, alongside extracting this script
# itself from the same base commit), fixtures are read via `git show
# $CIDX_FIXTURE_REF:<path>` instead of `cp` from the working tree, so a
# same-commit edit to both files can no longer make the self-test green.
# Default (unset, e.g. push events or local runs): unchanged `cp`-from-
# working-tree behavior.

set -euo pipefail

REPO_ROOT="${CIDX_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-gate-selection-defense.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

die() {
  echo "TLA_GATE_SELECTION_DEFENSE_REGRESSION_STATUS=FAIL reason=$1" >&2
  exit 1
}

fetch_fixture() {
  local rel="$1" dest="$2"
  if [[ -n "${CIDX_FIXTURE_REF:-}" ]]; then
    git -C "$REPO_ROOT" show "${CIDX_FIXTURE_REF}:${rel}" >"$dest"
  else
    cp "$REPO_ROOT/$rel" "$dest"
  fi
}

REPO="$WORK/repo"
mkdir -p "$REPO/spec/tla/tools" "$REPO/spec/tla/models" "$REPO/.github"
fetch_fixture spec/tla/manifest.json "$REPO/spec/tla/manifest.json"
fetch_fixture spec/tla/POLICY.md "$REPO/spec/tla/POLICY.md"
fetch_fixture spec/tla/ASSURANCE.md "$REPO/spec/tla/ASSURANCE.md"
fetch_fixture .github/CODEOWNERS "$REPO/.github/CODEOWNERS"
fetch_fixture spec/tla/ci-dependency-map.json "$REPO/spec/tla/ci-dependency-map.json"
fetch_fixture spec/tla/tools/check-gate-selection.sh "$REPO/spec/tla/tools/check-gate-selection.sh"
fetch_fixture spec/tla/tools/check-policy.sh "$REPO/spec/tla/tools/check-policy.sh"
fetch_fixture spec/tla/tools/select-changed-gates.sh "$REPO/spec/tla/tools/select-changed-gates.sh"
fetch_fixture spec/tla/tools/check-protected-review.sh "$REPO/spec/tla/tools/check-protected-review.sh"
fetch_fixture spec/tla/tools/validate-manifest.py "$REPO/spec/tla/tools/validate-manifest.py"
fetch_fixture spec/tla/tools/check.sh "$REPO/spec/tla/tools/check.sh"
for model in \
  CidxRepositorySmoke \
  CidxResultSmoke \
  CidxWorkspaceLifecycleSmoke \
  CidxBehaviorSmoke \
  CidxSemanticGraphSmoke \
  CidxStorageLifecycleSmoke; do
  fetch_fixture "spec/tla/models/${model}.cfg" "$REPO/spec/tla/models/${model}.cfg"
done
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
# All three run_case blocks assert tla_sidecar_conformance=true for a path
# this flow covers (conformance_recorder.cpp, conformance_schema.hpp, and
# CidxTypes.tla -- the last added by the round-2 critic P1-2 fix, since
# CidxStorageConformance.tla EXTENDS CidxTypes -- are all listed in the
# sidecar-publication flow); a real attacker hiding the ci-dependency-map.json
# narrowing from this self-test would need to delete all three, not just two.
needles = (
    'run_case conformance-recorder-selects-sidecar-gate \\\n'
    '  "tla_conformance,tla_sidecar_conformance,cpp_default" \\\n'
    '  "tla_syntax_and_model,tla_proofs,tla_policy" \\\n'
    '  "src/application/conformance_recorder.cpp"\n',
    'run_case conformance_schema-selects-both-conformance-gates \\\n'
    '  "tla_conformance,tla_sidecar_conformance,cpp_default" \\\n'
    '  "tla_syntax_and_model,tla_proofs,tla_policy" \\\n'
    '  "src/application/conformance_schema.hpp"\n',
    'run_case cidxtypes-selects-both-conformance-gates \\\n'
    '  "tla_conformance,tla_sidecar_conformance,tla_proofs,tla_syntax_and_model,cpp_default" \\\n'
    '  "tla_policy" \\\n'
    '  "spec/tla/modules/CidxTypes.tla"\n',
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
# Stand-in for the GitHub reviews API. No network access or real token is
# exercised by this local regression.
url=""
for argument in "$@"; do
  case "$argument" in
    https://*) url="$argument" ;;
  esac
done
[[ -n "$url" ]] || exit 2
page="${url##*page=}"
if [[ -n "${CIDX_FAKE_CURL_LOG:-}" ]]; then
  printf '%s\n' "$url" >>"$CIDX_FAKE_CURL_LOG"
fi

case "${CIDX_FAKE_REVIEWS_MODE:-empty}" in
  api-error) exit 22 ;;
  invalid-json)
    echo "{ invalid"
    exit 0
    ;;
  empty)
    echo "[]"
    exit 0
    ;;
  pagination) ;;
  *) exit 2 ;;
esac

python3 - "$page" <<'PY'
import json
import os
import sys

page = int(sys.argv[1])
head = os.environ["GITHUB_HEAD_SHA"]
if page == 1:
    reviews = [
        {
            "id": review_id,
            "state": "COMMENTED",
            "commit_id": head,
            "user": {"login": "reviewer"},
        }
        for review_id in range(1, 100)
    ]
    reviews.append(
        {
            "id": 100,
            "state": "APPROVED",
            "commit_id": head,
            "user": {"login": "husams"},
        }
    )
elif page == 2:
    reviews = [
        {
            "id": 101,
            "state": "CHANGES_REQUESTED",
            "commit_id": head,
            "user": {"login": "husams"},
        }
    ]
else:
    reviews = []
print(json.dumps(reviews))
PY
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

# --- Test 3: a superseding decision beyond GitHub's 100-review page is
# included before approval state is reduced. Page 1 ends with an approval of
# the current head; page 2 revokes it. A one-page implementation incorrectly
# passes this exact case.

: >"$WORK/fake-curl.log"
set +e
PATH="$FAKE_BIN:$PATH" \
  CIDX_FAKE_REVIEWS_MODE=pagination \
  CIDX_FAKE_CURL_LOG="$WORK/fake-curl.log" \
  GITHUB_TOKEN=fake-token \
  GITHUB_REPOSITORY=cidx-test/cidx-test \
  GITHUB_PR_NUMBER=1 \
  GITHUB_BASE_SHA="$BASE_SHA" \
  GITHUB_HEAD_SHA="$HEAD_SHA" \
  CIDX_REPO_ROOT="$REPO" \
  "$WORK/base-check-protected-review.sh" >"$WORK/paginated-review.log" 2>&1
paginated_review_status=$?
set -e

[[ "$paginated_review_status" -ne 0 ]] \
  || { cat "$WORK/paginated-review.log" >&2; die "protected-review-ignored-page-2-superseding-decision"; }
grep -q "no-codeowner-approval-of-head-sha-for-paths" "$WORK/paginated-review.log" \
  || { cat "$WORK/paginated-review.log" >&2; die "unexpected-paginated-review-failure-mode"; }
grep -q "per_page=100&page=1" "$WORK/fake-curl.log" \
  || { cat "$WORK/fake-curl.log" >&2; die "reviews-page-1-not-requested"; }
grep -q "per_page=100&page=2" "$WORK/fake-curl.log" \
  || { cat "$WORK/fake-curl.log" >&2; die "reviews-page-2-not-requested"; }
echo "TLA_GATE_SELECTION_DEFENSE_STATUS=PASS check=protected-review-reduces-superseding-decision-after-page-100"

# --- Test 4: every API page fails closed on transport/HTTP errors and on a
# successful HTTP response that is not a JSON review array.

run_reviews_failure_case() {
  local mode="$1" expected_reason="$2"
  set +e
  PATH="$FAKE_BIN:$PATH" \
    CIDX_FAKE_REVIEWS_MODE="$mode" \
    GITHUB_TOKEN=fake-token \
    GITHUB_REPOSITORY=cidx-test/cidx-test \
    GITHUB_PR_NUMBER=1 \
    GITHUB_BASE_SHA="$BASE_SHA" \
    GITHUB_HEAD_SHA="$HEAD_SHA" \
    CIDX_REPO_ROOT="$REPO" \
    "$WORK/base-check-protected-review.sh" \
    >"$WORK/reviews-failure-${mode}.log" 2>&1
  local status=$?
  set -e
  [[ "$status" -ne 0 ]] \
    || { cat "$WORK/reviews-failure-${mode}.log" >&2; die "reviews-${mode}-passed"; }
  grep -q "$expected_reason" "$WORK/reviews-failure-${mode}.log" \
    || { cat "$WORK/reviews-failure-${mode}.log" >&2; die "reviews-${mode}-unexpected-failure"; }
  echo "TLA_GATE_SELECTION_DEFENSE_STATUS=PASS check=protected-review-${mode}-fails-closed"
}

run_reviews_failure_case api-error "reviews-api-page-1-unreadable"
run_reviews_failure_case invalid-json "reviews-api-page-1-invalid-json"

echo "TLA_GATE_SELECTION_DEFENSE_REGRESSION_STATUS=PASS"
