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
# SECURITY (two layers, both required):
#
# 1. Data: the protected-path set (manifest.json's protectedPaths) and the
#    CODEOWNERS ownership list are both read from GITHUB_BASE_SHA via
#    `git show`, never from the checked-out worktree (the PR head). Reading
#    them from HEAD would let the very PR being policed delete a
#    protectedPaths entry, or rewrite CODEOWNERS to name itself as owner.
#
# 2. Code: this SCRIPT ITSELF must also be executed from a trusted revision,
#    not the PR's own checkout -- otherwise a PR could rewrite
#    check-protected-review.sh to always print PASS, and the CI step that
#    runs "spec/tla/tools/check-protected-review.sh" would run that edited
#    copy instead of the honest one, regardless of what data-source fix (1)
#    uses internally. The workflow (verification.yml) is responsible for
#    extracting this file from GITHUB_BASE_SHA and executing THAT copy, not
#    the checked-out one. CIDX_REPO_ROOT lets that extracted copy -- which no
#    longer lives inside the repository tree -- still find and operate on the
#    real checkout for `git diff`/`git show`/reading .github/CODEOWNERS.
#
# Required env: GITHUB_TOKEN, GITHUB_REPOSITORY (owner/repo),
#               GITHUB_PR_NUMBER, GITHUB_BASE_SHA, GITHUB_HEAD_SHA.
# Optional env: CIDX_REPO_ROOT (defaults to this script's own repo, only
#               correct when the script is run from a trusted revision of the
#               tree it polices; set explicitly when running an extracted
#               copy from elsewhere).
# Requires the checkout to have both SHAs available (fetch-depth: 0, or a
# fetch of the base ref) and `curl` + `python3`.

set -euo pipefail

ROOT="${CIDX_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"

die() {
  echo "TLA_PROTECTED_REVIEW_STATUS=FAIL reason=$1" >&2
  exit 1
}

: "${GITHUB_TOKEN:?GITHUB_TOKEN required}"
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY required}"
: "${GITHUB_PR_NUMBER:?GITHUB_PR_NUMBER required}"
: "${GITHUB_BASE_SHA:?GITHUB_BASE_SHA required}"
: "${GITHUB_HEAD_SHA:?GITHUB_HEAD_SHA required}"

BOOTSTRAP="${TLA_PROTECTED_REVIEW_BOOTSTRAP:-false}"

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
#
# Owners are resolved per matched path, not "every @handle anywhere in the
# file": CODEOWNERS is a sequence of `pattern owner...` rows, and GitHub's own
# semantics are that the LAST row whose pattern matches a given path is the
# one that owns it. Scanning the whole file for handles would (a) let an
# owner declared only for an unrelated path approve here, (b) let a comment
# mentioning a handle silently enlarge the approval pool, and (c) mis-extract
# team handles ("@org/team" -> "org", which can never match a review login).
python3 - "$WORK/base-codeowners" "$WORK/matched.txt" "$WORK/path_owners.json" <<'PY'
import fnmatch
import json
import pathlib
import sys

codeowners_path, matched_path, path_owners_path = sys.argv[1:4]
matched = [line.strip() for line in open(matched_path) if line.strip()]

rows = []
for line in pathlib.Path(codeowners_path).read_text().splitlines():
    stripped = line.strip()
    if not stripped or stripped.startswith("#"):
        continue
    parts = stripped.split()
    if len(parts) < 2:
        continue
    rows.append((parts[0], parts[1:]))

def pattern_matches(pattern: str, path: str) -> bool:
    anchored = pattern.startswith("/")
    trimmed = pattern[1:] if anchored else pattern
    if trimmed.endswith("/"):
        return path == trimmed.rstrip("/") or path.startswith(trimmed)
    if "*" in trimmed or "?" in trimmed:
        return fnmatch.fnmatch(path, trimmed) or fnmatch.fnmatch(
            path, "*/" + trimmed
        )
    if anchored:
        return path == trimmed
    return path == trimmed or path.endswith("/" + trimmed)

# Owners are resolved PER matched path (not a flat union across every matched
# path): GitHub's CODEOWNERS semantics require an approval from an owner of
# EACH owned path that changed, not just any owner of any changed path. A
# flat union would let an owner of path A approve a diff that also edits
# path B, which that owner does not own -- latent today (one owner for
# everything) but live the moment a second, narrower CODEOWNER is added.
path_owners = {}
unowned = []
team_only = []
for path in matched:
    winner = None
    for pattern, row_owners in rows:
        if pattern_matches(pattern, path):
            winner = row_owners
    if winner is None:
        unowned.append(path)
        continue
    individuals = [
        owner.lstrip("@") for owner in winner if "/" not in owner.lstrip("@")
    ]
    teams = [owner for owner in winner if "/" in owner.lstrip("@")]
    if individuals:
        path_owners[path] = individuals
    elif teams:
        team_only.append((path, teams))
    else:
        unowned.append(path)

if unowned:
    raise SystemExit(
        "TLA_PROTECTED_REVIEW_STATUS=FAIL reason=no-codeowners-pattern-matches:"
        + ",".join(unowned)
    )
if team_only:
    detail = ";".join(f"{path}={','.join(teams)}" for path, teams in team_only)
    raise SystemExit(
        "TLA_PROTECTED_REVIEW_STATUS=FAIL "
        "reason=team-only-codeowners-unsupported:" + detail
    )

pathlib.Path(path_owners_path).write_text(json.dumps(path_owners))
PY

# GitHub caps this endpoint at 100 reviews per page. Approval state must be
# reduced over the COMPLETE chronological history: otherwise an approval at
# the end of page 1 can incorrectly survive a superseding decision from the
# same reviewer on page 2. Fetch pages until the first short page (including
# the required empty page when the total is an exact multiple of 100), and
# validate each response before trusting its length.
REVIEWS_PAGES="$WORK/review-pages"
mkdir -p "$REVIEWS_PAGES"
page=1
while :; do
  page_file="$REVIEWS_PAGES/page-${page}.json"
  reviews_url="https://api.github.com/repos/${GITHUB_REPOSITORY}/pulls/${GITHUB_PR_NUMBER}/reviews?per_page=100&page=${page}"
  if ! curl --fail --location --silent --show-error \
    --header "Authorization: Bearer $GITHUB_TOKEN" \
    --header "Accept: application/vnd.github+json" \
    "$reviews_url" >"$page_file"; then
    die "reviews-api-page-${page}-unreadable"
  fi

  if ! review_count="$(
    python3 - "$page_file" <<'PY'
import json
import pathlib
import sys

try:
    reviews = json.loads(pathlib.Path(sys.argv[1]).read_text())
except (OSError, json.JSONDecodeError):
    raise SystemExit(1)
if not isinstance(reviews, list) or len(reviews) > 100:
    raise SystemExit(1)
print(len(reviews))
PY
  )"; then
    die "reviews-api-page-${page}-invalid-json"
  fi

  if (( review_count < 100 )); then
    break
  fi
  page=$((page + 1))
done

if ! python3 - "$REVIEWS_PAGES" "$WORK/reviews.json" <<'PY'
import json
import pathlib
import sys

pages_dir = pathlib.Path(sys.argv[1])
reviews_path = pathlib.Path(sys.argv[2])
pages = sorted(
    pages_dir.glob("page-*.json"),
    key=lambda path: int(path.stem.removeprefix("page-")),
)
reviews = []
for page in pages:
    reviews.extend(json.loads(page.read_text()))
reviews_path.write_text(json.dumps(reviews))
PY
then
  die "reviews-api-pages-could-not-be-combined"
fi

# A review only carries a live approval if (a) among that reviewer's DECISION
# reviews -- APPROVED, CHANGES_REQUESTED, or DISMISSED; a COMMENTED or PENDING
# review never changes GitHub's own review-decision state, and ignoring that
# would let a routine follow-up comment silently revoke a valid approval --
# it is their MOST RECENT one (an earlier approval does not survive a later
# CHANGES_REQUESTED from the same person), and (b) that latest decision
# review's commit_id is the current head SHA (an approval left on an earlier
# commit does not carry forward to a new commit pushed after it -- the
# classic approve-then-push-a-protected-edit bypass).
python3 - "$WORK/reviews.json" "$GITHUB_HEAD_SHA" "$WORK/approved.json" <<'PY'
import json
import pathlib
import sys

reviews_path, head_sha, approved_path = sys.argv[1:4]
reviews = json.loads(pathlib.Path(reviews_path).read_text())

DECISION_STATES = {"APPROVED", "CHANGES_REQUESTED", "DISMISSED"}

latest_by_user = {}
for review in reviews:
    if review.get("state") not in DECISION_STATES:
        continue
    user = (review.get("user") or {}).get("login")
    review_id = review.get("id", 0)
    if not user:
        continue
    # Reviews are returned in submission order, but sort defensively by id
    # (monotonically increasing) rather than assuming list order.
    previous = latest_by_user.get(user)
    if previous is None or review_id >= previous.get("id", 0):
        latest_by_user[user] = review

approvers = sorted(
    user
    for user, review in latest_by_user.items()
    if review.get("state") == "APPROVED" and review.get("commit_id") == head_sha
)
pathlib.Path(approved_path).write_text(json.dumps(approvers))
PY

# The checker itself is first introduced in this PR, so no immutable base copy
# exists yet. In that one bootstrap case, do not trust the base CODEOWNERS rows
# to create an approval path for a newly introduced enforcement boundary. The
# only accepted attestation is a current-head APPROVED review by a real
# reviewer other than the PR author. This preserves the fail-closed property:
# the author cannot self-approve, and an absent independent reviewer remains a
# visible CI failure until repository administration supplies one.
if [[ "$BOOTSTRAP" == "true" ]]; then
  if ! curl --fail --location --silent --show-error \
    --header "Authorization: Bearer $GITHUB_TOKEN" \
    --header "Accept: application/vnd.github+json" \
    "https://api.github.com/repos/${GITHUB_REPOSITORY}/pulls/${GITHUB_PR_NUMBER}" \
    >"$WORK/pull.json"; then
    die "pull-api-unreadable-during-bootstrap"
  fi
  python3 - "$WORK/pull.json" "$WORK/approved.json" <<'PY'
import json
import pathlib
import sys

pull_path, approved_path = sys.argv[1:3]
try:
    pull = json.loads(pathlib.Path(pull_path).read_text())
    approved = json.loads(pathlib.Path(approved_path).read_text())
except (OSError, json.JSONDecodeError):
    raise SystemExit(
        "TLA_PROTECTED_REVIEW_STATUS=FAIL "
        "reason=bootstrap-review-state-invalid"
    )
author = ((pull.get("user") or {}).get("login"))
if not author:
    raise SystemExit(
        "TLA_PROTECTED_REVIEW_STATUS=FAIL "
        "reason=bootstrap-pr-author-unreadable"
    )
independent = sorted({login for login in approved if login != author})
if not independent:
    raise SystemExit(
        "TLA_PROTECTED_REVIEW_STATUS=FAIL "
        "reason=bootstrap-requires-independent-head-approval"
    )
print(
    "TLA_PROTECTED_REVIEW_STATUS=PASS "
    "reason=bootstrap-independent-head-approval:" + ",".join(independent)
)
PY
  exit 0
fi

# Every matched path needs an approval from one of ITS OWN owners, not just
# any approval anywhere -- fixed alongside the union-of-owners bug above.
python3 - "$WORK/path_owners.json" "$WORK/approved.json" <<'PY'
import json
import pathlib
import sys

path_owners_path, approved_path = sys.argv[1:3]
path_owners = json.loads(pathlib.Path(path_owners_path).read_text())
approved = set(json.loads(pathlib.Path(approved_path).read_text()))

uncovered = []
covered = []
for path, owners in path_owners.items():
    matching = sorted(set(owners) & approved)
    if matching:
        covered.append(f"{path}:{','.join(matching)}")
    else:
        uncovered.append(path)

if uncovered:
    raise SystemExit(
        "TLA_PROTECTED_REVIEW_STATUS=FAIL "
        "reason=no-codeowner-approval-of-head-sha-for-paths:"
        + ",".join(sorted(uncovered))
    )
print("TLA_PROTECTED_REVIEW_STATUS=PASS " + ";".join(covered))
PY
