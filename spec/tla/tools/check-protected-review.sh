#!/usr/bin/env bash
#
# Protected-path human-review gate (HSE-89 acceptance criterion: "CI fails
# when a protected invariant or trusted assumption changes without the
# required review path"). CODEOWNERS + branch protection is the primary
# enforcement (a repo setting outside this script's control); this script is
# the CI-visible, fail-closed check that a pull request touching a protected
# path has an approving review from a CODEOWNER before it is allowed to
# report success, so the requirement is enforced even if branch protection is
# ever misconfigured.
#
# SECURITY: the protected-path set (manifest.json's protectedPaths) and the
# CODEOWNERS ownership list are both read from GITHUB_BASE_SHA via `git show`,
# never from the checked-out worktree (the PR head). Reading them from HEAD
# would let the very PR being policed delete a protectedPaths entry, or
# rewrite CODEOWNERS to name itself as owner, and have the gate evaluate
# itself against its own edited rules. The base ref is the immutable,
# pre-PR source of truth for "what was protected and who owns it" this gate
# is supposed to enforce.
#
# Required env: GITHUB_TOKEN, GITHUB_REPOSITORY (owner/repo),
#               GITHUB_PR_NUMBER, GITHUB_BASE_SHA, GITHUB_HEAD_SHA.
# Requires the checkout to have both SHAs available (fetch-depth: 0, or a
# fetch of the base ref) and `curl` + `python3`.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

die() {
  echo "TLA_PROTECTED_REVIEW_STATUS=FAIL reason=$1" >&2
  exit 1
}

: "${GITHUB_TOKEN:?GITHUB_TOKEN required}"
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY required}"
: "${GITHUB_PR_NUMBER:?GITHUB_PR_NUMBER required}"
: "${GITHUB_BASE_SHA:?GITHUB_BASE_SHA required}"
: "${GITHUB_HEAD_SHA:?GITHUB_HEAD_SHA required}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-protected-review.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

git -C "$ROOT" diff --name-only "$GITHUB_BASE_SHA" "$GITHUB_HEAD_SHA" >"$WORK/changed.txt"

# Read the policy files as they existed at the base commit -- not from the
# worktree, which is checked out at the PR's head and could have edited them.
git -C "$ROOT" show "${GITHUB_BASE_SHA}:spec/tla/manifest.json" >"$WORK/base-manifest.json" \
  || die "base-manifest-unreadable"
git -C "$ROOT" show "${GITHUB_BASE_SHA}:.github/CODEOWNERS" >"$WORK/base-codeowners" \
  || die "base-codeowners-unreadable"

python3 - "$WORK/base-manifest.json" "$WORK/changed.txt" "$WORK/matched.txt" <<'PY'
import json
import pathlib
import sys

manifest_path, changed_path, matched_path = sys.argv[1:4]
manifest = json.loads(pathlib.Path(manifest_path).read_text())
protected = manifest["protectedPaths"]
changed = [line.strip() for line in open(changed_path) if line.strip()]

def is_protected(path: str) -> bool:
    for entry in protected:
        # A leading "/" is repo-root-relative (e.g. "/.github/CODEOWNERS",
        # "/spec/tla/manifest.json" -- the policy files that define this very
        # protected set and its ownership); anything else is spec/tla/-relative,
        # matching the existing convention.
        full = entry[1:] if entry.startswith("/") else f"spec/tla/{entry}"
        if full.endswith("*.cfg"):
            directory = full[: -len("*.cfg")]
            if path.startswith(directory) and path.endswith(".cfg"):
                return True
        elif full.endswith("/"):
            if path.startswith(full):
                return True
        elif path == full:
            return True
    return False

matched = [path for path in changed if is_protected(path)]
pathlib.Path(matched_path).write_text("\n".join(matched))
if matched:
    print("TLA_PROTECTED_REVIEW_STATUS=PENDING matched=" + ",".join(matched))
else:
    print("TLA_PROTECTED_REVIEW_STATUS=NOT_APPLICABLE")
PY

if [[ ! -s "$WORK/matched.txt" ]]; then
  echo "TLA_PROTECTED_REVIEW_STATUS=PASS reason=no-protected-paths-changed"
  exit 0
fi

# manifest.json and CODEOWNERS are themselves both listed in manifest.json's
# protectedPaths policy-and-ownership coverage; if THIS diff touches either
# one, the base-commit ownership list still governs -- a PR cannot expand its
# own approval pool by adding owners in the same change.
owners="$(grep -ohE '@[A-Za-z0-9_-]+' "$WORK/base-codeowners" | tr -d '@' | sort -u)"
[[ -n "$owners" ]] || die "no-codeowners-configured-at-base"

curl --fail --silent --show-error \
  --header "Authorization: Bearer $GITHUB_TOKEN" \
  --header "Accept: application/vnd.github+json" \
  "https://api.github.com/repos/${GITHUB_REPOSITORY}/pulls/${GITHUB_PR_NUMBER}/reviews?per_page=100" \
  >"$WORK/reviews.json"

python3 - "$WORK/reviews.json" "$WORK/approved.txt" <<'PY'
import json
import pathlib
import sys

reviews = json.loads(pathlib.Path(sys.argv[1]).read_text())
approvers = []
for review in reviews:
    if review.get("state") == "APPROVED":
        user = (review.get("user") or {}).get("login")
        if user:
            approvers.append(user)
pathlib.Path(sys.argv[2]).write_text("\n".join(approvers))
PY

for owner in $owners; do
  if grep -qxF "$owner" "$WORK/approved.txt"; then
    echo "TLA_PROTECTED_REVIEW_STATUS=PASS reviewer=$owner"
    exit 0
  fi
done

echo "TLA_PROTECTED_REVIEW_STATUS=FAIL reason=no-codeowner-approval matched=$(paste -sd, "$WORK/matched.txt")" >&2
exit 1
