#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_VERSION="1.8.0"
TOOLS_URL="https://github.com/tlaplus/tlaplus/releases/download/v${TOOLS_VERSION}/tla2tools.jar"
TOOLS_SHA256="cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3"

die() {
  echo "TLA_TOOLCHAIN_STATUS=FAIL reason=$1" >&2
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
JAVA_VERSION_OUTPUT="$($JAVA_BIN -version 2>&1 || true)"
JAVA_MAJOR="$(printf '%s\n' "$JAVA_VERSION_OUTPUT" | sed -n 's/.*version "\([0-9][0-9]*\).*/\1/p' | head -n 1)"
[[ "$JAVA_MAJOR" == "17" ]] || die "java-major-version-${JAVA_MAJOR:-unknown}-expected-17"

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

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cidx-tla-conformance.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/replay"

python3 - "$ROOT" "$WORK" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
work = pathlib.Path(sys.argv[2])
manifest = json.loads((root / "manifest.json").read_text())
operations = json.loads((root / "conformance/operation-map.json").read_text())
observation_map = json.loads((root / "conformance/observation-map.json").read_text())
scenarios = json.loads((root / "conformance/scenarios.json").read_text())

required_modules = [root / path for path in manifest["modules"]]
required_models = [root / model[key] for model in manifest["models"] for key in ("module", "config")]
missing = [str(path.relative_to(root)) for path in required_modules + required_models if not path.is_file()]
if missing:
    raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=missing-files:" + ",".join(missing))

operation_rows = operations["operations"]
operation_names = [row["operation"] for row in operation_rows]
action_names = [row["specAction"] for row in operation_rows]
if len(operation_names) != len(set(operation_names)):
    raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=duplicate-operation")
if len(action_names) != len(set(action_names)):
    raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=duplicate-spec-action")

observation_rows = observation_map["observations"]
observation_specs = {row["field"]: row for row in observation_rows}
state_fields = [row["field"] for row in observation_rows]
if len(state_fields) != len(set(state_fields)):
    raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=duplicate-observation")

scenario_rows = sorted(scenarios["scenarios"], key=lambda row: row["id"])
scenario_ids = [row["id"] for row in scenario_rows]
if len(scenario_ids) != len(set(scenario_ids)):
    raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=duplicate-scenario")

base_constants = [
    ("WorkspaceIds", '{"workspace-1"}'),
    ("RepositoryIds", '{"repository-1"}'),
    ("ComponentIds", '{"component-1"}'),
    ("SourceIds", '{"source-1"}'),
    ("TranslationUnitIds", '{"tu-1"}'),
    ("ConfigurationIds", '{"config-1"}'),
    ("SymbolIds", '{"symbol-1"}'),
    ("TypeIds", '{"type-1"}'),
    ("RelationIds", '{"relation-1"}'),
    ("EvidenceIds", '{"evidence-1"}'),
    ("GenerationIds", '{"generation-1"}'),
    ("TransformIds", '{"transform-1"}'),
    ("ArtifactIds", '{"artifact-1"}'),
    ("QueryIds", '{"query-1"}'),
    ("ResultIds", '{"result-1"}'),
    ("WorkspaceId", '"workspace-1"'),
    ("RepositoryId", '"repository-1"'),
    ("TranslationUnitId", '"tu-1"'),
    ("ConfigurationId", '"config-1"'),
    ("GenerationId", '"generation-1"'),
    ("ArtifactId", '"artifact-1"'),
    ("QueryId", '"query-1"'),
    ("RelationId", '"relation-1"'),
    ("EvidenceId", '"evidence-1"'),
    ("SemanticUniverseId", '"universe-1"'),
    ("SharedUniverseId", '"universe-1"'),
    ("TraceBound", "9"),
]

expected_names = {
    "workspaceState": "ExpectedWorkspaceState",
    "configState": "ExpectedConfigState",
    "indexState": "ExpectedIndexState",
    "publicationState": "ExpectedPublicationState",
    "transformState": "ExpectedTransformState",
    "queryState": "ExpectedQueryState",
    "storageState": "ExpectedStorageState",
    "artifactState": "ExpectedArtifactState",
    "failureMode": "ExpectedFailureMode",
    "generation": "ExpectedGeneration",
    "currentGeneration": "ExpectedCurrentGeneration",
    "migrationBaseline": "ExpectedMigrationBaseline",
    "invalidated": "ExpectedInvalidated",
    "evidenceState": "ExpectedEvidenceState",
    "queryResultState": "ExpectedQueryResultState",
    "queryTruncated": "ExpectedQueryTruncated",
    "queryWrites": "ExpectedQueryWrites",
    "identityMode": "ExpectedIdentityMode",
    "graphState": "ExpectedGraphState",
    "graphEvidenceState": "ExpectedGraphEvidenceState",
    "graphTargetState": "ExpectedGraphTargetState",
    "graphConfigurationState": "ExpectedGraphConfigurationState",
    "queryPlanState": "ExpectedQueryPlanState",
    "queryStreamShape": "ExpectedQueryStreamShape",
    "queryTraversalBound": "ExpectedQueryTraversalBound",
    "queryViewState": "ExpectedQueryViewState",
    "includePlanState": "ExpectedIncludePlanState",
    "includeConfigurationState": "ExpectedIncludeConfigurationState",
    "includeEditState": "ExpectedIncludeEditState",
    "destructiveEditAuthorized": "ExpectedDestructiveEditAuthorized",
}

def tla_value(value):
    if isinstance(value, bool):
        return "TRUE" if value else "FALSE"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, list):
        if not value:
            return "<< >>"
        return "<<" + ", ".join(tla_value(item) for item in value) + ">>"
    return json.dumps(value)

for scenario in scenario_rows:
    actions = scenario.get("actions", [])
    observations = scenario.get("observations", [])
    if not actions or len(actions) > 8:
        raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=invalid-trace-length:" + scenario["id"])
    unknown = sorted(set(actions) - set(action_names))
    if unknown:
        raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=unknown-action:" + scenario["id"])
    if len(observations) != len(state_fields):
        raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=incomplete-final-state:" + scenario["id"])
    final_state = {}
    for observation in observations:
        field = observation.get("field")
        if field in final_state or field not in observation_specs:
            raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=invalid-observation-field:" + scenario["id"])
        value = observation.get("value")
        spec = observation_specs[field]
        if "allowed" in spec and value not in spec["allowed"]:
            raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=invalid-observation-value:" + scenario["id"])
        if spec.get("type") == "non-negative integer" and (not isinstance(value, int) or isinstance(value, bool) or value < 0):
            raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=invalid-observation-type:" + scenario["id"])
        if spec.get("type") == "sequence of non-negative integers" and (
            not isinstance(value, list)
            or any(not isinstance(item, int) or isinstance(item, bool) or item < 0 for item in value)
        ):
            raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=invalid-observation-type:" + scenario["id"])
        final_state[field] = value
    if set(final_state) != set(state_fields):
        raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=missing-final-state-field:" + scenario["id"])

    constants = list(base_constants)
    padded_actions = actions + ["NoOp"] * (8 - len(actions))
    constants.extend((f"ScenarioAction{index}", json.dumps(action))
                     for index, action in enumerate(padded_actions, start=1))
    constants.append(("ScenarioLength", str(len(actions))))
    for field in state_fields:
        if field == "graphArgumentOrder":
            order = final_state[field]
            constants.extend([
                ("ExpectedGraphArgumentOrderLength", str(len(order))),
                ("ExpectedGraphArgumentOrder1", str(order[0] if len(order) > 0 else 0)),
                ("ExpectedGraphArgumentOrder2", str(order[1] if len(order) > 1 else 0)),
            ])
        else:
            constants.append((expected_names[field], tla_value(final_state[field])))
    lines = ["CONSTANTS"] + [f"    {name} = {value}" for name, value in constants]
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
    (work / "replay" / f"{scenario['id']}.cfg").write_text("\n".join(lines) + "\n")

(work / "counts").write_text(f"{len(operation_rows)} {len(state_fields)} {len(scenario_rows)}\n")
print(f"TLA_CONFORMANCE_INPUT_STATUS=PASS operations={len(operation_rows)} observations={len(state_fields)} scenarios={len(scenario_rows)}")
PY

read -r OP_COUNT OBS_COUNT SCENARIO_COUNT <"$WORK/counts"
cp "$ROOT"/modules/CidxTypes.tla "$WORK"/
cp "$ROOT"/protected/CidxProtected.tla "$WORK"/
cp "$ROOT"/modules/CidxBehavior.tla "$WORK"/
cp "$ROOT"/conformance/CidxConformance.tla "$WORK"/

for cfg in "$WORK"/replay/*.cfg; do
  scenario="$(basename "$cfg" .cfg)"
  syntax_log="$WORK/${scenario}.syntax.log"
  tlc_log="$WORK/${scenario}.tlc.log"
  if ! (cd "$WORK" && "$JAVA_BIN" -cp "$JAR" tla2sany.SANY \
      "$WORK/CidxConformance.tla") >"$syntax_log" 2>&1; then
    echo "TLA_REPLAY_STATUS=FAIL scenario=$scenario phase=syntax" >&2
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
      "$WORK/CidxConformance.tla") >"$tlc_log" 2>&1; then
    echo "TLA_REPLAY_STATUS=FAIL scenario=$scenario phase=model" >&2
    cat "$tlc_log" >&2
    exit 30
  fi
  if ! grep -q "Model checking completed. No error has been found." "$tlc_log"; then
    echo "TLA_REPLAY_STATUS=FAIL scenario=$scenario reason=completion-marker-missing" >&2
    cat "$tlc_log" >&2
    exit 30
  fi
  echo "TLA_REPLAY_STATUS=PASS scenario=$scenario"
done

echo "TLA_CONFORMANCE_STATUS=PASS operations=$OP_COUNT observations=$OBS_COUNT scenarios=$SCENARIO_COUNT replay=$SCENARIO_COUNT"
