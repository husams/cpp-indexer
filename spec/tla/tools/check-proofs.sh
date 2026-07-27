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
# UNSCOPED whole-file grep for the literal text "[]<InvariantName>" -- so a
# vacuous `THEOREM <declared-theorem> == TRUE OBVIOUS` still passed as long
# as a decoy TLA+ comment ANYWHERE in the file happened to contain that
# literal text. QA also proved the round-1 fix was never truly binding the
# invariant to the declared theorem even in the legitimate case: this
# module's declared theorem (ResultInvarianceTheorem == Spec => []Invariant)
# does not itself literally say "[]SharedResultTypeInvariant" or
# "[]TrustedOutcomeInvariant" -- those strings only appear in two derived
# corollary theorems (ResultTypeAlwaysHolds, TrustedOutcomeAlwaysHolds)
# further down the file, each proved `BY <declared-theorem>, ...`.
#
# Fixed binding: strip all TLA+ comments (so decoy text can never satisfy
# the check), locate every top-level THEOREM's own statement (the text
# between its `==` and its proof/next top-level THEOREM/LEMMA/module
# boundary), then compute the closure of theorems reachable from the
# declared theorem by following `BY <name>` proof references (i.e. the
# declared theorem itself plus any corollary theorem whose proof cites it,
# directly or transitively). A declared invariant is proved only if some
# theorem's STATEMENT (not a comment, not an unrelated theorem) in that
# closure literally states `[]<Invariant>`.
theorem_invariant_binding() {
  local module_name="$1"
  local module_file="$2"
  python3 - "$MANIFEST" "$module_name" "$module_file" <<'PY'
import json
import re
import sys

manifest_path, module_name, module_file = sys.argv[1], sys.argv[2], sys.argv[3]
manifest = json.loads(open(manifest_path, encoding="utf-8").read())
entry = next(
    (p for p in manifest["proofs"] if p["module"] == f"proofs/{module_name}.tla"),
    None,
)
if entry is None:
    print("MANIFEST-ENTRY-MISSING")
    sys.exit(0)

theorem_name = entry["theorem"]
invariants = entry["provesInvariants"]

text = open(module_file, encoding="utf-8").read()
# Strip TLA+ block comments (* ... *) and line comments \* ... to end of
# line, so decoy text placed only in a comment can never satisfy the check.
text = re.sub(r"\(\*.*?\*\)", "", text, flags=re.DOTALL)
text = re.sub(r"\\\*.*", "", text)

theorem_re = re.compile(r"^THEOREM\s+([A-Za-z_][A-Za-z0-9_]*)\s*==(.*)$", re.MULTILINE)
boundary_re = re.compile(r"^(THEOREM\b|LEMMA\b|={4,})", re.MULTILINE)

theorems = {}
for m in theorem_re.finditer(text):
    start = m.end()
    end = len(text)
    for boundary in boundary_re.finditer(text, start):
        end = boundary.start()
        break
    theorems[m.group(1)] = m.group(2) + text[start:end]

if theorem_name not in theorems:
    print(f"THEOREM-NOT-FOUND:{theorem_name}")
    sys.exit(0)


def statement_of(block):
    # A theorem's statement is everything before its proof begins: a `BY`/
    # `PROOF` keyword or a `<1>`-style structured-proof step marker.
    m = re.search(r"\n\s*(BY\b|PROOF\b|<\d+>)", block)
    return block[: m.start()] if m else block


def proof_refs(block):
    refs = set()
    for by_clause in re.finditer(r"\bBY\b([^\n]*)", block):
        refs.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", by_clause.group(1)))
    return refs


statements = {name: statement_of(block) for name, block in theorems.items()}
refs = {name: proof_refs(block) for name, block in theorems.items()}

closure = {theorem_name}
changed = True
while changed:
    changed = False
    for name, r in refs.items():
        if name not in closure and r & closure:
            closure.add(name)
            changed = True

missing = [
    invariant
    for invariant in invariants
    if not any(
        re.search(r"\[\]\s*" + re.escape(invariant) + r"(?![A-Za-z0-9_])", statements[n])
        for n in closure
    )
]

if missing:
    print("INVARIANT-NOT-FOUND:" + ",".join(missing))
else:
    print(f"OK:{theorem_name}:{','.join(invariants)}")
PY
}

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
  binding="$(theorem_invariant_binding "$module" "$module_file")"
  case "$binding" in
    MANIFEST-ENTRY-MISSING)
      echo "TLA_PROOF_STATUS=FAIL module=$module reason=manifest-entry-missing" >&2
      exit 30
      ;;
    THEOREM-NOT-FOUND:*)
      echo "TLA_PROOF_STATUS=FAIL module=$module reason=declared-theorem-not-found:${binding#THEOREM-NOT-FOUND:}" >&2
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
