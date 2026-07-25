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

python3 - "$ROOT" "$WORK/changed.txt" "$WORK/matched.txt" <<'PY'
import json
import pathlib
import sys

root, changed_path, matched_path = (pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3])
manifest = json.loads((root / "spec/tla/manifest.json").read_text())
protected = manifest["protectedPaths"]
changed = [line.strip() for line in open(changed_path) if line.strip()]

def is_protected(path: str) -> bool:
    for entry in protected:
        full = f"spec/tla/{entry}"
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

owners="$(grep -ohE '@[A-Za-z0-9_-]+' "$ROOT/.github/CODEOWNERS" | tr -d '@' | sort -u)"
[[ -n "$owners" ]] || die "no-codeowners-configured"

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
