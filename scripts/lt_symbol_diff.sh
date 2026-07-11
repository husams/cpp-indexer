#!/usr/bin/env bash
# lt_symbol_diff.sh — Phase-1 parity gate for the LibTooling symbol pass.
#
# For each corpus file: run the libclang-based cidx (symbol rows in index.db)
# and the LibTooling probe (TSV stream folded through lt_merge.py, which
# replays add_symbol's upsert), then byte-diff the two.
#
#   ./scripts/lt_symbol_diff.sh [file.c ...]     (default: a starter set)
#
# Env: CIDX — libclang cidx binary (default ../cpp-indexer/build/cidx)
#      PROBE — LibTooling probe (default ./build-lt/cidx_lt_probe)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LT_ROOT="$(dirname "$SCRIPT_DIR")"
CIDX="${CIDX:-$LT_ROOT/../cpp-indexer/build/cidx}"
PROBE="${PROBE:-$LT_ROOT/build-lt/cidx_lt_probe}"
# The committed compile_commands.json carries absolute paths into the MAIN
# repo checkout — use that manifests dir so both tools see the same files.
# realpath: the probe reports files by their canonical compile-db spelling, and
# lt_merge.py compares path strings.
MANIFESTS="$(realpath "${MANIFESTS:-$LT_ROOT/../cpp-indexer/manifests}")"
MERGE="$SCRIPT_DIR/lt_merge.py"

FILES=("$@")
if [ ${#FILES[@]} -eq 0 ]; then
  # Default: every TU in the corpus compile db (basenames).
  while IFS= read -r line; do FILES+=("$line"); done < <(python3 - "$MANIFESTS/compile_commands.json" <<'EOF'
import json, os, sys
seen = set()
for e in json.load(open(sys.argv[1])):
    b = os.path.basename(e["file"])
    if b not in seen:
        seen.add(b)
        print(b)
EOF
)
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Resolve a TU basename to its absolute path via the compile db (entries live
# in subdirs — graphlab/, project/, versioned/ — with their own `directory`).
resolve_tu() {
  python3 - "$MANIFESTS/compile_commands.json" "$1" <<'EOF'
import json, os, sys
for e in json.load(open(sys.argv[1])):
    if os.path.basename(e["file"]) == sys.argv[2]:
        p = e["file"]
        if not os.path.isabs(p):
            p = os.path.join(e["directory"], p)
        print(os.path.realpath(p))
        break
EOF
}

fail=0
for f in "${FILES[@]}"; do
  ABS="$(resolve_tu "$f")"
  if [ -z "$ABS" ]; then echo "SKIP  $f (not in compile db)"; continue; fi
  # --- libclang side: ONE file per FRESH cache, symbols only ------------------
  # The probe is single-TU, so cross-TU effects must be excluded: another TU
  # claiming a shared USR (several corpus files define `main`), and the edge
  # pass backfilling callee decl sites. --no-graph runs the symbol pass alone.
  export INDEXER_CACHE="$WORK/cache-$f"
  mkdir -p "$INDEXER_CACHE"
  "$CIDX" import --db "$MANIFESTS/compile_commands.json" >/dev/null
  "$CIDX" index --no-graph "$ABS" >/dev/null 2>&1 || true
  DB="$INDEXER_CACHE/index.db"

  # C-API rows for this file (minted instantiations excluded; stubs have no
  # file_id and drop out via the join).
  sqlite3 -noheader -nullvalue '\N' -separator $'\t' "$DB" "
    SELECT s.usr, s.spelling, s.kind, s.qual_name, s.display_name, s.type_info,
           s.line, s.col, s.end_line, s.end_col, s.decl_line, s.decl_col,
           s.is_definition, s.is_pure, s.is_static, s.linkage, s.access,
           s.parent_usr, s.resolved
    FROM symbol s JOIN file fl ON fl.id = s.file_id
    WHERE fl.name = '$(basename "$f")' AND s.is_instantiation = 0
    ORDER BY s.usr;" > "$WORK/capi.tsv"

  # LibTooling side: probe stream folded through the upsert merger. On macOS
  # the SDK sysroot must be passed explicitly (the plain `cc` argv in the
  # compile db does not survive into ClangTool's driver invocation). The probe
  # exits non-zero on missing-header fatals; symbols still stream, so tolerate.
  SYSROOT_ARGS=()
  if [ "$(uname)" = "Darwin" ]; then
    SYSROOT_ARGS=(--extra-arg="-isysroot$(xcrun --show-sdk-path)")
  fi
  "$PROBE" -p "$MANIFESTS" "${SYSROOT_ARGS[@]}" "$ABS" \
    2>"$WORK/probe.err" \
    | python3 "$MERGE" --main "$ABS" --root "$MANIFESTS" \
    > "$WORK/lt.tsv" || true

  if diff -u "$WORK/capi.tsv" "$WORK/lt.tsv" > "$WORK/diff.txt"; then
    echo "PASS  $f ($(wc -l < "$WORK/capi.tsv" | tr -d ' ') symbols)"
  else
    echo "FAIL  $f"
    head -40 "$WORK/diff.txt"
    fail=1
  fi
done
exit $fail
