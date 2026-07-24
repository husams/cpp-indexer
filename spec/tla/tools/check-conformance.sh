#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 - "$ROOT" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
manifest = json.loads((root / "manifest.json").read_text())
operations = json.loads((root / "conformance/operation-map.json").read_text())
observations = json.loads((root / "conformance/observation-map.json").read_text())
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

observation_rows = observations["observations"]
observation_names = [row["field"] for row in observation_rows]
if len(observation_names) != len(set(observation_names)):
    raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=duplicate-observation")

scenario_rows = scenarios["scenarios"]
scenario_ids = [row["id"] for row in scenario_rows]
if scenario_ids != sorted(scenario_ids):
    raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=scenarios-not-sorted")
if len(scenario_ids) != len(set(scenario_ids)):
    raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=duplicate-scenario")
known_actions = set(action_names) | {
    "IndexSuccessfully", "IndexFails", "InterruptPublication", "RejectIncompleteTransform",
    "BeginMigration", "CompleteMigration", "InterruptMigration",
    "RecoverMigration"
}
for scenario in scenario_rows:
    if not scenario["actions"] or len(scenario["actions"]) > 8:
        raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=invalid-trace-length:" + scenario["id"])
    unknown = sorted(set(scenario["actions"]) - known_actions)
    if unknown:
        raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=unknown-action:" + scenario["id"])
    if not scenario["observations"]:
        raise SystemExit("TLA_CONFORMANCE_STATUS=FAIL reason=missing-observation:" + scenario["id"])

print(f"TLA_CONFORMANCE_STATUS=PASS operations={len(operation_rows)} observations={len(observation_rows)} scenarios={len(scenario_rows)}")
PY
