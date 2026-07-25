#!/usr/bin/env bash
#
# Sidecar-publication conformance gate.  Mirrors check-conformance.sh's
# deterministic-replay approach but targets CidxStorageConformance.tla (the
# real CidxStorageLifecycle sidecar actions) instead of CidxBehavior, since
# sidecar publication is the third named conformance flow in HSE-89's
# acceptance criteria and does not live in CidxBehavior's coverage.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_VERSION="1.8.0"
TOOLS_URL="https://github.com/tlaplus/tlaplus/releases/download/v${TOOLS_VERSION}/tla2tools.jar"
TOOLS_SHA256="cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3"

die() {
  echo "TLA_SIDECAR_CONFORMANCE_STATUS=FAIL reason=$1" >&2
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

JAVA_BIN="${JAVA_BIN:-}"
if [[ -z "$JAVA_BIN" && -n "${JAVA_HOME:-}" ]]; then
  JAVA_BIN="$JAVA_HOME/bin/java"
fi
JAVA_BIN="${JAVA_BIN:-java}"
command -v "$JAVA_BIN" >/dev/null 2>&1 || die "java-17-not-found"

JAR="${TLA_TOOLS_JAR:-${TMPDIR:-/tmp}/tla2tools-${TOOLS_VERSION}.jar}"
if [[ ! -f "$JAR" ]]; then
  command -v curl >/dev/null 2>&1 || die "curl-not-found-and-tool-jar-is-missing"
  download="$JAR.download.$$"
  rm -f "$download"
  if ! curl --fail --location --silent --show-error \
      --retry 5 --retry-all-errors --retry-delay 2 --connect-timeout 20 \
      "$TOOLS_URL" --output "$download"; then
    rm -f "$download"
    die "download-failed"
  fi
  mv "$download" "$JAR"
fi
[[ "$(checksum "$JAR")" == "$TOOLS_SHA256" ]] || die "tla2tools-sha256-mismatch"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-sidecar.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/replay"
cp "$ROOT"/modules/CidxTypes.tla "$WORK"/
cp "$ROOT"/modules/CidxStorageLifecycle.tla "$WORK"/
cp "$ROOT"/conformance/CidxStorageConformance.tla "$WORK"/

# Kept identical to the BASE_CONSTANTS_LINES list inside the python heredoc
# below (which builds the scenarios.json-derived replay configs); this bash
# copy only feeds the hand-written illegal-order rejection seed.
BASE_CONSTANTS_LINES=(
  '    WorkspaceIds = {"workspace-1"}'
  '    RepositoryIds = {"repository-1"}'
  '    ComponentIds = {"component-1"}'
  '    SourceIds = {"source-1"}'
  '    TranslationUnitIds = {"tu-1"}'
  '    ConfigurationIds = {"config-1"}'
  '    SymbolIds = {"symbol-1"}'
  '    TypeIds = {"type-1"}'
  '    RelationIds = {"relation-1"}'
  '    EvidenceIds = {"evidence-1"}'
  '    GenerationIds = {"generation-1", "generation-2"}'
  '    TransformIds = {"transform-1"}'
  '    ArtifactIds = {"artifact-1", "artifact-2"}'
  '    QueryIds = {"query-1"}'
  '    ResultIds = {"result-1"}'
  '    WorkspaceId = "workspace-1"'
  '    CoreSchemaVersion = 39'
  '    SupportedSchemaVersion = 40'
  '    NewerSchemaVersion = 41'
  '    IncompatibleSchemaVersion = 38'
  '    InitialGeneration = 1'
  '    ReplacementGeneration = 2'
  '    SecondReplacementGeneration = 3'
  '    TraceBound = 16'
  '    Scenario = "valid"'
)

python3 - "$ROOT" "$WORK" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
work = pathlib.Path(sys.argv[2])
operations = json.loads((root / "conformance/sidecar-operation-map.json").read_text())
observation_map = json.loads((root / "conformance/sidecar-observation-map.json").read_text())
scenarios = json.loads((root / "conformance/sidecar-scenarios.json").read_text())

operation_rows = operations["operations"]
operation_names = [row["operation"] for row in operation_rows]
action_names = [row["specAction"] for row in operation_rows]
if len(operation_names) != len(set(operation_names)):
    raise SystemExit("TLA_SIDECAR_CONFORMANCE_STATUS=FAIL reason=duplicate-operation")
if len(action_names) != len(set(action_names)):
    raise SystemExit("TLA_SIDECAR_CONFORMANCE_STATUS=FAIL reason=duplicate-spec-action")

observation_rows = observation_map["observations"]
observation_specs = {row["field"]: row for row in observation_rows}
state_fields = [row["field"] for row in observation_rows]
if len(state_fields) != len(set(state_fields)):
    raise SystemExit("TLA_SIDECAR_CONFORMANCE_STATUS=FAIL reason=duplicate-observation")

scenario_rows = sorted(scenarios["scenarios"], key=lambda row: row["id"])
scenario_ids = [row["id"] for row in scenario_rows]
if len(scenario_ids) != len(set(scenario_ids)):
    raise SystemExit("TLA_SIDECAR_CONFORMANCE_STATUS=FAIL reason=duplicate-scenario")

def tla_value(value):
    if isinstance(value, bool):
        return "TRUE" if value else "FALSE"
    if isinstance(value, int):
        return str(value)
    return json.dumps(value)

expected_names = {
    "sidecarState": "ExpectedSidecarState",
    "sidecarQuality": "ExpectedSidecarQuality",
    "sidecarValidated": "ExpectedSidecarValidated",
    "sidecarAttachment": "ExpectedSidecarAttachment",
    "sidecarFilePublication": "ExpectedSidecarFilePublication",
    "readCompleteness": "ExpectedReadCompleteness",
    "currentGeneration": "ExpectedCurrentGeneration",
}

def write_replay_cfg(path, actions, observations, nondeterministic_choices=None,
                     extra_lines=()):
    if len(actions) != 7:
        raise SystemExit("TLA_SIDECAR_CONFORMANCE_STATUS=FAIL reason=scenario-must-have-7-actions")
    unknown = sorted(set(actions) - set(action_names))
    if unknown:
        raise SystemExit("TLA_SIDECAR_CONFORMANCE_STATUS=FAIL reason=unknown-action:" + ",".join(unknown))
    final_state = {}
    for observation in observations:
        field = observation.get("field")
        if field in final_state or field not in observation_specs:
            raise SystemExit("TLA_SIDECAR_CONFORMANCE_STATUS=FAIL reason=invalid-observation-field")
        value = observation.get("value")
        spec = observation_specs[field]
        if "allowed" in spec and value not in spec["allowed"]:
            raise SystemExit("TLA_SIDECAR_CONFORMANCE_STATUS=FAIL reason=invalid-observation-value:" + field)
        if spec.get("type") == "non-negative integer" and (not isinstance(value, int) or isinstance(value, bool) or value < 0):
            raise SystemExit("TLA_SIDECAR_CONFORMANCE_STATUS=FAIL reason=invalid-observation-type:" + field)
        final_state[field] = value
    if set(final_state) != set(state_fields):
        raise SystemExit("TLA_SIDECAR_CONFORMANCE_STATUS=FAIL reason=missing-final-state-field")

    lines = ["CONSTANTS"]
    lines.extend(BASE_CONSTANTS_LINES)
    for index, action in enumerate(actions, start=1):
        lines.append(f"    ScenarioAction{index} = {json.dumps(action)}")
    for field in state_fields:
        lines.append(f"    {expected_names[field]} = {tla_value(final_state[field])}")
    # PrepareStagedArtifact's own quality choice is not part of the final
    # sidecar-observation vocabulary (it is superseded by PublishCoreGeneration
    # before the trace ends), so it is supplied separately from the
    # scenario's nondeterministicChoices rather than from `observations`.
    staged_choice = (nondeterministic_choices or {}).get("PrepareStagedArtifact")
    if staged_choice is None or staged_choice.get("field") != "stagedArtifactQuality":
        raise SystemExit(
            "TLA_SIDECAR_CONFORMANCE_STATUS=FAIL reason=missing-nondeterministic-choice:PrepareStagedArtifact"
        )
    lines.append(f"    ExpectedStagedArtifactQuality = {tla_value(staged_choice['value'])}")
    lines += list(extra_lines)
    lines += [
        "",
        "SPECIFICATION ReplaySpec",
        "",
        "INVARIANT TypeInvariant",
        "INVARIANT ReplayObservationInvariant",
        "INVARIANT ReplayTraceInvariant",
        "",
        "PROPERTY ReplayCompletion",
        "",
        "CHECK_DEADLOCK TRUE",
    ]
    path.write_text("\n".join(lines) + "\n")

BASE_CONSTANTS_LINES = [
    '    WorkspaceIds = {"workspace-1"}',
    '    RepositoryIds = {"repository-1"}',
    '    ComponentIds = {"component-1"}',
    '    SourceIds = {"source-1"}',
    '    TranslationUnitIds = {"tu-1"}',
    '    ConfigurationIds = {"config-1"}',
    '    SymbolIds = {"symbol-1"}',
    '    TypeIds = {"type-1"}',
    '    RelationIds = {"relation-1"}',
    '    EvidenceIds = {"evidence-1"}',
    '    GenerationIds = {"generation-1", "generation-2"}',
    '    TransformIds = {"transform-1"}',
    '    ArtifactIds = {"artifact-1", "artifact-2"}',
    '    QueryIds = {"query-1"}',
    '    ResultIds = {"result-1"}',
    '    WorkspaceId = "workspace-1"',
    '    CoreSchemaVersion = 39',
    '    SupportedSchemaVersion = 40',
    '    NewerSchemaVersion = 41',
    '    IncompatibleSchemaVersion = 38',
    '    InitialGeneration = 1',
    '    ReplacementGeneration = 2',
    '    SecondReplacementGeneration = 3',
    '    TraceBound = 16',
    '    Scenario = "valid"',
]

for scenario in scenario_rows:
    write_replay_cfg(
        work / "replay" / f"{scenario['id']}.cfg",
        scenario["actions"],
        scenario["observations"],
        scenario.get("nondeterministicChoices"),
    )

(work / "counts").write_text(f"{len(operation_rows)} {len(state_fields)} {len(scenario_rows)}\n")
print(f"TLA_SIDECAR_CONFORMANCE_INPUT_STATUS=PASS operations={len(operation_rows)} observations={len(state_fields)} scenarios={len(scenario_rows)}")
PY

read -r OP_COUNT OBS_COUNT SCENARIO_COUNT <"$WORK/counts"

for cfg in "$WORK"/replay/*.cfg; do
  scenario="$(basename "$cfg" .cfg)"
  syntax_log="$WORK/${scenario}.syntax.log"
  tlc_log="$WORK/${scenario}.tlc.log"
  if ! (cd "$WORK" && "$JAVA_BIN" -cp "$JAR" tla2sany.SANY \
      "$WORK/CidxStorageConformance.tla") >"$syntax_log" 2>&1; then
    echo "TLA_SIDECAR_REPLAY_STATUS=FAIL scenario=$scenario phase=syntax" >&2
    cat "$syntax_log" >&2
    exit 20
  fi
  if ! (cd "$WORK" && "$JAVA_BIN" -jar "$JAR" \
      -workers 1 \
      -fp 0 \
      -seed 1 \
      -nowarning \
      -metadir "$WORK/meta-${scenario}" \
      -config "$cfg" \
      "$WORK/CidxStorageConformance.tla") >"$tlc_log" 2>&1; then
    echo "TLA_SIDECAR_REPLAY_STATUS=FAIL scenario=$scenario phase=model" >&2
    cat "$tlc_log" >&2
    exit 30
  fi
  if ! grep -q "Model checking completed. No error has been found." "$tlc_log"; then
    echo "TLA_SIDECAR_REPLAY_STATUS=FAIL scenario=$scenario reason=completion-marker-missing" >&2
    cat "$tlc_log" >&2
    exit 30
  fi
  echo "TLA_SIDECAR_REPLAY_STATUS=PASS scenario=$scenario"
done

# Seeded rejection: attempt PublishSidecar before the core generation it
# depends on is actually published.  This must fail (deadlock or invariant
# violation), proving the replay gate rejects an impossible sidecar
# publication order instead of silently accepting it.
illegal_cfg="$WORK/illegal-order.cfg"
{
  echo "CONSTANTS"
  printf '%s\n' "${BASE_CONSTANTS_LINES[@]}"
  echo '    ScenarioAction1 = "StartOneTUUpdate"'
  echo '    ScenarioAction2 = "PrepareStagedArtifact"'
  echo '    ScenarioAction3 = "BuildSidecar"'
  echo '    ScenarioAction4 = "PrepareSidecarArtifact"'
  echo '    ScenarioAction5 = "PublishSidecar"'
  echo '    ScenarioAction6 = "ValidateStagedArtifact"'
  echo '    ScenarioAction7 = "PublishCoreGeneration"'
  echo '    ExpectedSidecarState = "current"'
  echo '    ExpectedSidecarQuality = "valid"'
  echo '    ExpectedSidecarValidated = TRUE'
  echo '    ExpectedSidecarAttachment = "attached"'
  echo '    ExpectedSidecarFilePublication = "current"'
  echo '    ExpectedReadCompleteness = "complete"'
  echo '    ExpectedCurrentGeneration = 2'
  echo '    ExpectedStagedArtifactQuality = "valid"'
  echo ""
  echo "SPECIFICATION ReplaySpec"
  echo ""
  echo "INVARIANT TypeInvariant"
  echo "INVARIANT ReplayObservationInvariant"
  echo "INVARIANT ReplayTraceInvariant"
  echo ""
  echo "PROPERTY ReplayCompletion"
  echo ""
  echo "CHECK_DEADLOCK TRUE"
} >"$illegal_cfg"

illegal_log="$WORK/illegal-order.tlc.log"
set +e
(cd "$WORK" && "$JAVA_BIN" -jar "$JAR" \
    -workers 1 -fp 0 -seed 1 -nowarning \
    -metadir "$WORK/meta-illegal-order" \
    -config "$illegal_cfg" \
    "$WORK/CidxStorageConformance.tla") >"$illegal_log" 2>&1
illegal_status=$?
set -e
if [[ "$illegal_status" -eq 0 ]] \
    && grep -q "Model checking completed. No error has been found." "$illegal_log"; then
  echo "TLA_SIDECAR_REJECTION_STATUS=FAIL reason=illegal-order-accepted" >&2
  cat "$illegal_log" >&2
  exit 30
fi
if ! grep -qE "Deadlock reached|is violated" "$illegal_log"; then
  echo "TLA_SIDECAR_REJECTION_STATUS=FAIL reason=unexpected-failure-mode" >&2
  cat "$illegal_log" >&2
  exit 30
fi
echo "TLA_SIDECAR_REJECTION_STATUS=PASS scenario=illegal-order reason=publish-before-core-current"

echo "TLA_SIDECAR_CONFORMANCE_STATUS=PASS operations=$OP_COUNT observations=$OBS_COUNT scenarios=$SCENARIO_COUNT replay=$SCENARIO_COUNT"
