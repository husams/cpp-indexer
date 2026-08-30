#!/usr/bin/env bash
set -euo pipefail

baseline_bin=
candidate_bin=
source_root=$(cd "$(dirname "$0")/.." && pwd)
while (($#)); do
  case "$1" in
    --baseline-bin) baseline_bin=$2; shift 2 ;;
    --candidate-bin) candidate_bin=$2; shift 2 ;;
    --source-root) source_root=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [[ -z "$baseline_bin" || -z "$candidate_bin" ]]; then
  echo "usage: $0 --baseline-bin PATH --candidate-bin PATH [--source-root PATH]" >&2
  exit 2
fi

corpus="$source_root/tests/e2e/single_traversal_parity_corpus.txt"
fixtures="$source_root/tests/e2e/fixtures"
dump="$source_root/scripts/dump_layer0.sh"
[[ -f "$corpus" && -x "$dump" ]] || { echo "parity inputs are missing" >&2; exit 1; }

names=()
while IFS= read -r name; do
  names+=("$name")
done < <(sed -e 's/[[:space:]]*#.*//' -e '/^[[:space:]]*$/d' "$corpus")
expected=(basic_function_decl.cpp basic_record_decl.cpp basic_template_function.cpp
  alias_template.cpp Inheritance.cpp deep_templates_a.cpp deep_templates_b.cpp
  single_traversal_fact_extraction.cpp)
[[ "${names[*]}" == "${expected[*]}" ]] || { echo "parity corpus is not the exact eight-TU manifest" >&2; exit 1; }

tmp=$(mktemp -d "${TMPDIR:-/tmp}/cidx-layer0-parity.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

run_one() {
  local binary=$1 name=$2 out=$3 jobs=$4
  local ws="$tmp/$out-${name%.cpp}"
  mkdir -p "$ws/src" "$ws/cache"
  cp "$fixtures/$name" "$ws/src/$name"
  if [[ "$name" == single_traversal_fact_extraction.cpp ]]; then
    cp "$fixtures/single_traversal_fact_extraction.hpp" "$ws/src/"
  fi
  python3 - "$ws" "$name" <<'PY'
import json
import pathlib
import sys

ws, name = pathlib.Path(sys.argv[1]), sys.argv[2]
source = ws / "src" / name
(ws / "compile_commands.json").write_text(json.dumps([{
    "directory": str(source.parent), "file": str(source),
    "arguments": ["clang++", "-std=c++20", "-c", name],
}]) + "\n")
PY
  INDEXER_CACHE="$ws/cache" "$binary" init >/dev/null
  INDEXER_CACHE="$ws/cache" "$binary" import --db "$ws" --name fixture >/dev/null
  INDEXER_CACHE="$ws/cache" "$binary" index --jobs "$jobs" >/dev/null
  INDEXER_CACHE="$ws/cache" "$binary" resolve >/dev/null
  "$dump" "$ws/cache/index.db" |
    sed -E 's#([^[:space:]()]*/)?single_traversal_fact_extraction\.cpp#single_traversal_fact_extraction.cpp#g'
}

for name in "${names[@]}"; do
  run_one "$baseline_bin" "$name" baseline 1 >"$tmp/baseline-${name%.cpp}.txt"
  run_one "$candidate_bin" "$name" candidate-jobs1 1 >"$tmp/candidate-${name%.cpp}.txt"
  run_one "$candidate_bin" "$name" candidate-jobsN 2 >"$tmp/candidate-jobsN-${name%.cpp}.txt"
  diff -u "$tmp/baseline-${name%.cpp}.txt" "$tmp/candidate-${name%.cpp}.txt"
  diff -u "$tmp/candidate-${name%.cpp}.txt" \
    "$tmp/candidate-jobsN-${name%.cpp}.txt"
done
echo "Layer-0 parity passed for ${#names[@]} fixtures"
