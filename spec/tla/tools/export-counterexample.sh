#!/usr/bin/env bash
#
# Export a TLC counterexample trace into a stable, versioned JSON
# intermediate format so a seeded model defect yields both a readable
# counterexample and a durable artifact a C++/Python regression test can be
# generated or seeded from (HSE-89 acceptance criterion).
#
# Two modes:
#   --from-log <tlc-log> --model M --invariant I --out <path>
#       Parse an already-captured TLC stdout/stderr log (as produced by
#       tools/check.sh or tools/check-regression.sh) into JSON.
#   --demo --out <path>
#       Regenerate the one committed, representative golden example this
#       repository ships (spec/tla/counterexamples/golden/): seed the same
#       TrustedOutcomeInvariant defect tools/check-regression.sh could seed
#       for CidxResult, capture TLC's real counterexample, and export it.
#       Follow this exact pattern (mutate -> run check.sh scoped to one
#       model -> export) to add further golden examples; this script does
#       not attempt to auto-discover every seed check-regression.sh knows
#       about.
#
# HSE-89 internal-critic fix (two-generation trust-root poisoning): --demo
# invokes tools/check.sh internally to produce the TLC log it exports, so
# this script has the identical self-policing gap check-regression.sh was
# fixed for -- see that script's header for the full attack description. It
# previously resolved that invocation via "$ROOT/tools/check.sh", where ROOT
# derived from its OWN script location, so a PR could weaken check.sh and
# this exporter in the same commit and still reproduce the golden diff.
# CIDX_CHECK_SH resolves the check.sh this script invokes independently of
# CIDX_REPO_ROOT (which still points module asset resolution -- CidxResult.tla
# -- at the real checkout, since that IS legitimate PR content to seed the
# demo mutation from), so verification.yml can wire it to the same
# base-extracted check.sh both this script and the real tla-conformance gate
# trust, from the same immutable base revision.

set -euo pipefail

ROOT="${CIDX_REPO_ROOT:+$CIDX_REPO_ROOT/spec/tla}"
ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CHECK_SH="${CIDX_CHECK_SH:-$ROOT/tools/check.sh}"

usage() {
  echo "usage: $0 --from-log <path> --model <name> --invariant <name> --out <path>" >&2
  echo "       $0 --demo --out <path>" >&2
  exit 2
}

MODE=""
LOG=""
MODEL=""
INVARIANT=""
OUT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --from-log) LOG="$2"; MODE="from-log"; shift 2 ;;
    --model) MODEL="$2"; shift 2 ;;
    --invariant) INVARIANT="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --demo) MODE="demo"; shift 1 ;;
    *) usage ;;
  esac
done

[[ -n "$OUT" ]] || usage

export_log() {
  local log="$1" model="$2" invariant="$3" out="$4"
  python3 - "$log" "$model" "$invariant" "$out" <<'PY'
import json
import re
import sys

log_path, model, invariant, out_path = sys.argv[1:5]
text = open(log_path, encoding="utf-8", errors="replace").read()

if f"Invariant {invariant} is violated" not in text:
    raise SystemExit(
        f"TLA_COUNTEREXAMPLE_EXPORT_STATUS=FAIL reason=invariant-not-violated:{invariant}"
    )

state_re = re.compile(
    r"^State (\d+): <(Initial predicate)>$|"
    r"^State (\d+): <([A-Za-z_][A-Za-z0-9_]*) (line .+? of module [A-Za-z0-9_]+)>$",
    re.MULTILINE,
)
matches = list(state_re.finditer(text))
if not matches:
    raise SystemExit("TLA_COUNTEREXAMPLE_EXPORT_STATUS=FAIL reason=no-states-found")

states = []
for position, match in enumerate(matches):
    body_start = match.end()
    body_end = matches[position + 1].start() if position + 1 < len(matches) else len(text)
    body = text[body_start:body_end].strip("\n")
    # Trailing summary lines ("N states generated, ...") follow the last
    # state block after a blank line; drop them from the recorded values.
    body = body.split("\n\n")[0].rstrip()
    if match.group(1):
        index, label, action, location = match.group(1), "Initial predicate", None, None
    else:
        index, label, action, location = match.group(3), None, match.group(4), match.group(5)
    states.append({
        "index": int(index),
        "label": label,
        "action": action,
        "location": location,
        "values": body,
    })

summary_match = re.search(
    r"^(\d+) states generated, (\d+) distinct states found, (\d+) states left on queue\.$",
    text, re.MULTILINE,
)
diameter_match = re.search(
    r"^The depth of the complete state graph search is (\d+)\.$", text, re.MULTILINE
)

document = {
    "schemaVersion": "1.0.0",
    "model": model,
    "invariant": invariant,
    "toolchain": {"tool": "tla2tools", "workers": 1, "fingerprint": 0, "seed": 1},
    "states": states,
    "summary": {
        "statesGenerated": int(summary_match.group(1)) if summary_match else None,
        "distinctStatesFound": int(summary_match.group(2)) if summary_match else None,
        "statesLeftOnQueue": int(summary_match.group(3)) if summary_match else None,
        "diameter": int(diameter_match.group(1)) if diameter_match else None,
        "exhaustive": False,
    },
}
with open(out_path, "w", encoding="utf-8") as handle:
    json.dump(document, handle, indent=2, sort_keys=False)
    handle.write("\n")
print(f"TLA_COUNTEREXAMPLE_EXPORT_STATUS=PASS model={model} invariant={invariant} states={len(states)} out={out_path}")
PY
}

run_demo() {
  local out="$1"
  local work
  work="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-ce-demo.XXXXXX")"
  trap 'rm -rf "$work"' RETURN
  mkdir -p "$work/modules"
  cp "$ROOT"/modules/*.tla "$work/modules"/
  # The same seed tools/check-regression.sh would use for a CidxResult
  # defect: ProveUnderAssumptions stops recording a nonempty assumption set,
  # violating TrustedOutcomeInvariant (proved unconditionally, for every
  # constant instantiation, by proofs/CidxResultProof.tla).
  sed 's/!\.assumptions = {EvidenceId}\]/!.assumptions = {}]/' \
    "$ROOT/modules/CidxResult.tla" >"$work/modules/CidxResult.tla.mutated"
  mv "$work/modules/CidxResult.tla.mutated" "$work/modules/CidxResult.tla"

  local log="$work/check.log"
  set +e
  TLA_MODULE_DIR="$work/modules" TLA_MODELS="CidxResultSmoke" \
    "$CHECK_SH" >"$log" 2>&1
  local status=$?
  set -e
  if [[ "$status" -ne 30 ]]; then
    echo "TLA_COUNTEREXAMPLE_EXPORT_STATUS=FAIL reason=seed-did-not-fail-as-expected" >&2
    cat "$log" >&2
    exit 1
  fi
  export_log "$log" "CidxResultSmoke" "TrustedOutcomeInvariant" "$out"
}

case "$MODE" in
  from-log)
    [[ -n "$MODEL" && -n "$INVARIANT" ]] || usage
    export_log "$LOG" "$MODEL" "$INVARIANT" "$OUT"
    ;;
  demo)
    run_demo "$OUT"
    ;;
  *)
    usage
    ;;
esac
