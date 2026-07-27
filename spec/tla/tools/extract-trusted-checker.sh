#!/usr/bin/env bash
#
# Single, shared implementation of "extract a checker script from the PR's
# base commit so the PR's own (possibly tampered) head copy is never what
# polices the PR." Used by every verification.yml job that needs this for a
# checker introduced by THIS chain of PRs (HSE-89) and therefore genuinely
# absent from some earlier base commit: check.sh, check-conformance.sh,
# check-sidecar-conformance.sh, check-proofs.sh, check-proofs-binding.sh,
# check-gate-selection.sh, check-gate-selection-defense-regression.sh, and
# check-protected-review.sh.
#
# check-protected-review.sh also uses this helper. Its first-introduction
# bootstrap is fail-closed: the extracted checker requires an independent
# current-head reviewer before accepting bootstrap, so extraction is never an
# approval or a self-approval escape hatch.
#
# HSE-89 round-6 security fix (internal-critic + review): a bootstrap
# fallback that falls back to the head copy on ANY nonzero
# `git cat-file -e BASE:PATH` cannot distinguish "the checker is genuinely
# new in this PR" from "the base commit itself never resolved" (unfetched
# ref, garbage SHA, shallow clone) -- the latter must hard-fail, not silently
# execute the PR's own copy. This script checks base-commit resolvability
# FIRST and exits nonzero, loudly, if it does not; only a checker path that
# is missing from a base commit that DOES resolve counts as legitimate
# first-introduction.
#
# The extraction destination is always outside the checked-out repository
# tree (the caller passes a path under $RUNNER_TEMP). A PR fully controls
# every path inside its own checkout, including one it could commit as a
# regular file, a symlink, or a directory; writing the trusted copy inside
# that tree would make its integrity depend on write-ordering and
# filesystem-symlink semantics instead of being structurally impossible to
# influence. The helper canonicalizes the destination and rejects any path
# inside the checkout; verification.yml also pins every caller to
# $RUNNER_TEMP, which check-verification-tamper-regression.sh checks.
#
# The protected-review checker also uses this helper. Its first-introduction
# bootstrap is not an approval: check-protected-review.sh requires an
# independent current-head reviewer before accepting the bootstrap path.
#
# Usage: extract-trusted-checker.sh <base-sha> <repo-relative-checker-path> <dest-path>
# Prints, on success: TLA_TRUSTED_EXTRACT_STATUS=PASS mode=(base|bootstrap) ref=<base-sha-or-empty>

set -euo pipefail

BASE_SHA="${1:?usage: extract-trusted-checker.sh <base-sha> <checker-path> <dest-path>}"
CHECKER="${2:?usage: extract-trusted-checker.sh <base-sha> <checker-path> <dest-path>}"
DEST="${3:?usage: extract-trusted-checker.sh <base-sha> <checker-path> <dest-path>}"

die() {
  echo "TLA_TRUSTED_EXTRACT_STATUS=FAIL reason=$1" >&2
  exit 1
}

# Resolve the checkout and the destination's existing parent physically before
# comparing. The destination itself need not exist yet, but it must not already
# be a file/symlink and its parent must resolve. Canonicalizing the parent also
# closes a $RUNNER_TEMP/symlink -> checkout bypass.
CHECKOUT="$(git rev-parse --show-toplevel 2>/dev/null)" \
  || die "checkout-root-unresolvable"
CHECKOUT="$(cd "$CHECKOUT" && pwd -P)" \
  || die "checkout-root-unresolvable"
DEST_PARENT="$(dirname -- "$DEST")"
DEST_NAME="$(basename -- "$DEST")"
CANONICAL_PARENT="$(cd "$DEST_PARENT" 2>/dev/null && pwd -P)" \
  || die "destination-parent-unresolvable:${DEST_PARENT}"
ABS_DEST="${CANONICAL_PARENT}/${DEST_NAME}"
if [[ -e "$DEST" || -L "$DEST" ]]; then
  die "destination-must-not-exist:${DEST}"
fi
if [[ "$ABS_DEST" == "$CHECKOUT" || "$ABS_DEST" == "$CHECKOUT"/* ]]; then
  die "destination-must-be-outside-checkout:${DEST}"
fi

# Resolvability first: a base SHA that does not resolve to a commit (bad
# ref, unfetched, garbage input) must hard-fail here, not fall through to
# "path missing" below and bootstrap from head.
if ! git cat-file -e "${BASE_SHA}^{commit}" 2>/dev/null; then
  die "base-commit-unresolvable:${BASE_SHA}"
fi

if git cat-file -e "${BASE_SHA}:${CHECKER}" 2>/dev/null; then
  git show "${BASE_SHA}:${CHECKER}" > "$DEST"
  chmod +x "$DEST"
  echo "TLA_TRUSTED_EXTRACT_STATUS=PASS mode=base ref=${BASE_SHA}"
else
  # First-addition bootstrap: legitimate only because the base commit
  # itself resolved (checked above) and the checker is genuinely absent from
  # it -- not because the base ref failed to fetch or resolve.
  cp "$CHECKER" "$DEST"
  chmod +x "$DEST"
  echo "TLA_TRUSTED_EXTRACT_STATUS=PASS mode=bootstrap ref="
fi
