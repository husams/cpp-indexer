#!/usr/bin/env bash
#
# TLAPS proof-checking gate.  This is a distinct assurance level from
# tools/check.sh (finite TLC model checking): a proof checked here holds for
# every constant instantiation permitted by the module's ASSUMEs, not only
# the one finite sample a smoke model explores.  See ../ASSURANCE.md for
# which invariants get this treatment and why.
#
# TLAPS has its own toolchain independent of the pinned Java/tla2tools used
# by check.sh: it is a native OCaml (Zenon) + Isabelle/ML toolchain that
# needs a C toolchain (`cc`, `make`) to compile its bundled Isabelle theories
# on first install, but no JVM.  A syntax/model failure here is reported
# distinctly from a TLC model failure, a conformance mismatch, or a C++ test
# failure, per the CI-outcome contract in ../README.md.
#
# HSE-89 internal-critic P1 (round 5): verification.yml extracts this script
# from GITHUB_BASE_SHA for pull_request events and runs THAT copy rather than
# the PR's own checkout, exactly as it already does for
# check-protected-review.sh and check-gate-selection.sh -- a PR that weakens
# a proof obligation could otherwise replace this script with a stub that
# exits 0 in the same commit. CIDX_REPO_ROOT lets that extracted copy -- which
# no longer lives inside the repository tree -- still resolve ROOT to the
# real checkout's spec/tla/ (this PR's head), so it checks the PR's actual
# modules/proofs, not files relative to its own $RUNNER_TEMP location.

set -euo pipefail

ROOT="${CIDX_REPO_ROOT:+$CIDX_REPO_ROOT/spec/tla}"
ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
MODULE_DIR="${TLA_MODULE_DIR:-$ROOT/modules}"
PROOF_DIR="${TLA_PROOF_DIR:-$ROOT/proofs}"
MANIFEST="${TLA_MANIFEST:-$ROOT/manifest.json}"
TOOLS_VERSION="1.5.0"
TOOLS_URL="https://github.com/tlaplus/tlapm/releases/download/202210041448/tlaps-${TOOLS_VERSION}-x86_64-linux-gnu-inst.bin"
TOOLS_SHA256="ebb7a3f271bdb564f74cb0a2767ef7b9ff7045621a9be7c50d363a03c2e6f08a"

die() {
  echo "TLA_PROOF_TOOLCHAIN_STATUS=FAIL reason=$1" >&2
  exit 10
}

checksum() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    die "no SHA-256 utility (need sha256sum or shasum)"
  fi
}

command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 \
  || die "c-compiler-not-found"
command -v make >/dev/null 2>&1 || die "make-not-found"

# AC6/F2 fix (senior-developer acceptance review): materialize the installer
# ONLY when the install prefix does not already have a working tlapm.
# Previously the download+checksum-verify block ran unconditionally, ahead of
# the `[[ ! -x "$TLAPM_BIN" ]]` guard below -- so a CI cache hit on
# TLA_PROOFS_PREFIX (verification.yml caches exactly that path) still forced
# a ~50 MB fetch from github.com on every run, breaking the "clean-environment
# replay needs no network once dependencies are materialized" criterion.
PREFIX="${TLA_PROOFS_PREFIX:-${TMPDIR:-/tmp}/tlaps-${TOOLS_VERSION}}"
TLAPM_BIN="$PREFIX/bin/tlapm"
if [[ ! -x "$TLAPM_BIN" ]]; then
  INSTALLER="${TLA_PROOFS_INSTALLER:-${TMPDIR:-/tmp}/tlaps-${TOOLS_VERSION}-x86_64-linux-gnu-inst.bin}"
  if [[ ! -f "$INSTALLER" ]]; then
    command -v curl >/dev/null 2>&1 || die "curl-not-found-and-installer-is-missing"
    download="$INSTALLER.download.$$"
    rm -f "$download"
    if ! curl --fail --location --silent --show-error \
        --retry 5 --retry-all-errors --retry-delay 2 --connect-timeout 20 \
        "$TOOLS_URL" --output "$download"; then
      rm -f "$download"
      die "download-failed"
    fi
    mv "$download" "$INSTALLER"
  fi
  [[ "$(checksum "$INSTALLER")" == "$TOOLS_SHA256" ]] || die "tlaps-installer-sha256-mismatch"

  chmod +x "$INSTALLER"
  rm -rf "$PREFIX"
  if ! "$INSTALLER" -d "$PREFIX" >"${TMPDIR:-/tmp}/tlaps-install.$$.log" 2>&1; then
    cat "${TMPDIR:-/tmp}/tlaps-install.$$.log" >&2
    die "install-failed"
  fi
fi
[[ -x "$TLAPM_BIN" ]] || die "tlapm-binary-missing-after-install"

VERSION_OUTPUT="$("$TLAPM_BIN" --version 2>&1 || true)"
[[ "$VERSION_OUTPUT" == "$TOOLS_VERSION" ]] || die "tlapm-version-${VERSION_OUTPUT:-unknown}-expected-$TOOLS_VERSION"

echo "TLA_PROOF_TOOLCHAIN_STATUS=PASS version=$TOOLS_VERSION"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tlaps.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cp "$MODULE_DIR"/*.tla "$WORK"/
cp "$PROOF_DIR"/*.tla "$WORK"/

# F1 fix (senior-developer acceptance review, round 1): manifest.json's
# proofs[] entry declares which THEOREM this module's checker is supposed to
# prove and which invariants that theorem covers. Previously nothing bound
# the module's actual content to that declaration -- a two-line module
# (`THEOREM Trivial1 == 1 = 1 OBVIOUS` plus a same-named but vacuous
# `THEOREM <declared-theorem> == TRUE OBVIOUS`) satisfied the "All N
# obligations proved." log-line check with N=2 and exited 0.
#
# F1 residual gap (QA round 2): the round-1 fix's invariant check was an
# UNSCOPED whole-file grep for the literal text "[]<InvariantName>" -- a
# decoy comment anywhere in the file could satisfy it.
#
# F1 residual gap #2 (senior-developer acceptance review, round 3): scoping
# the search to a theorem's own statement inside the BY-citation closure of
# the declared theorem still used a bare substring match -- a negated
# antecedent, or a bare `FALSE => []Inv`, still satisfied it.
#
# F1 residual gap #3 (QA + senior-developer round 2 acceptance review): the
# round-3 fix required a theorem's statement to structurally match
# `<Spec> => []<Invariant>`, but resolved `<Spec>` by scanning EVERY .tla
# file check-proofs.sh had copied into ITS OWN SCRATCH WORK DIRECTORY for an
# operator whose body merely started with the token "Init" -- and that
# scratch directory includes the untrusted proofs/ tree the PR under review
# controls. A proof module defining its own fresh, module-local
# `WeakSpec == Init /\ FALSE` (a contradiction, so proofs built on it are
# vacuously true) was accepted as a real "module-defined Spec operator":
# PROVEN end-to-end against real tlapm 1.5.0, offline.
#
# The full binding logic (closure computation, statement extraction, and the
# fixed spec-operator resolution restricted to manifest.json's "extends"
# module and its transitive EXTENDS chain -- resolved only from
# modules/conformance/protected, never from proofs/ -- with the tightened
# `Init /\ [][Next]_vars` shape requirement) lives in
# check-proofs-binding.sh so it can also be exercised directly, without a
# real tlapm run, by check-proofs-binding-unit-test.sh. See that file's
# header for the full vulnerability history and the fix rationale.
#
# HSE-89 acceptance-review fix (this round): a proof module's THEOREM can
# have the correct `Spec => []Invariant` shape and still be vacuous if the
# module also carries an unconstrained ASSUME (e.g. `ASSUME FALSE`) -- TLAPS
# treats ASSUMEs as premises available to every later proof in the module.
# check-proofs-binding.sh now also rejects any proof module whose ASSUMEs
# are not on manifest.json's declared per-module trustedAssumptions
# allowlist, or whose assumption set is syntactically vacuous/contradictory
# (a literal FALSE conjunct, or two assumptions that are negations of each
# other), before this function ever looks at theorem shape. The binding is an
# exact equality of normalized proof and policy assumption sets: extra,
# changed, missing, or duplicate entries fail closed.
# shellcheck source=check-proofs-binding.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/check-proofs-binding.sh"

run_proof() {
  local module="$1"
  local log="$WORK/${module}.tlapm.log"
  # --nofp: never trust a cached fingerprint across runs; a clean environment
  # always re-checks every obligation, so a weakened proof cannot hide behind
  # stale cached success.
  if ! (cd "$WORK" && "$TLAPM_BIN" --toolbox 0 0 --nofp "${module}.tla") \
      >"$log" 2>&1; then
    echo "TLA_PROOF_STATUS=FAIL module=$module reason=backend-error" >&2
    cat "$log" >&2
    exit 30
  fi
  local summary
  summary="$(grep -E '^\[INFO\]: All [0-9]+ obligations proved\.$' "$log" | tail -n 1 || true)"
  if [[ -z "$summary" ]]; then
    echo "TLA_PROOF_STATUS=FAIL module=$module reason=incomplete-proof" >&2
    cat "$log" >&2
    exit 30
  fi
  local obligations
  obligations="$(sed -nE 's/^\[INFO\]: All ([0-9]+) obligations proved\.$/\1/p' <<<"$summary")"

  local module_file="$WORK/${module}.tla"
  local binding
  binding="$(theorem_invariant_binding "$MANIFEST" "$module" "$module_file" "$ROOT")"
  case "$binding" in
    MANIFEST-ENTRY-MISSING)
      echo "TLA_PROOF_STATUS=FAIL module=$module reason=manifest-entry-missing" >&2
      exit 30
      ;;
    ASSUMPTION-VACUOUS:*)
      echo "TLA_PROOF_STATUS=FAIL module=$module reason=vacuous-proof-assumption:${binding#ASSUMPTION-VACUOUS:}" >&2
      exit 30
      ;;
    ASSUMPTIONS-CONTRADICT:*)
      echo "TLA_PROOF_STATUS=FAIL module=$module reason=contradictory-proof-assumptions:${binding#ASSUMPTIONS-CONTRADICT:}" >&2
      exit 30
      ;;
    ASSUMPTION-NOT-TRUSTED:*)
      echo "TLA_PROOF_STATUS=FAIL module=$module reason=untrusted-proof-assumption:${binding#ASSUMPTION-NOT-TRUSTED:}" >&2
      exit 30
      ;;
    ASSUMPTION-POLICY-MISMATCH:*)
      echo "TLA_PROOF_STATUS=FAIL module=$module reason=proof-assumption-policy-mismatch:${binding#ASSUMPTION-POLICY-MISMATCH:}" >&2
      exit 30
      ;;
    ASSUMPTION-POLICY-DUPLICATE:*)
      echo "TLA_PROOF_STATUS=FAIL module=$module reason=duplicate-proof-assumption-policy-entry:${binding#ASSUMPTION-POLICY-DUPLICATE:}" >&2
      exit 30
      ;;
    THEOREM-NOT-FOUND:*)
      echo "TLA_PROOF_STATUS=FAIL module=$module reason=declared-theorem-not-found:${binding#THEOREM-NOT-FOUND:}" >&2
      exit 30
      ;;
    NO-TRUSTED-SPEC-OPERATOR:*)
      echo "TLA_PROOF_STATUS=FAIL module=$module reason=no-trusted-spec-operator:${binding#NO-TRUSTED-SPEC-OPERATOR:}" >&2
      exit 30
      ;;
    INVARIANT-NOT-FOUND:*)
      echo "TLA_PROOF_STATUS=FAIL module=$module reason=proves-invariant-not-found:${binding#INVARIANT-NOT-FOUND:}" >&2
      exit 30
      ;;
    OK:*) ;;
    *)
      echo "TLA_PROOF_STATUS=FAIL module=$module reason=binding-check-internal-error" >&2
      echo "$binding" >&2
      exit 30
      ;;
  esac

  local theorem invariants_csv
  theorem="$(cut -d: -f2 <<<"$binding")"
  invariants_csv="$(cut -d: -f3- <<<"$binding")"

  echo "TLA_PROOF_STATUS=PASS module=$module obligations=$obligations theorem=$theorem invariants=$invariants_csv"
}

for module in ${TLA_PROOFS:-CidxResultProof}; do
  run_proof "$module"
done

echo "TLA_PROOF_CHECK_STATUS=PASS modules=${TLA_PROOFS:-CidxResultProof}"
