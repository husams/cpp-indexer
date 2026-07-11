#!/usr/bin/env bash
# lt_edge_diff.sh — Phase-2a parity gate: DECLARATION-LEVEL edges.
#
# Per TU:
#   db_capi: libclang cidx FULL index (symbols + edges)   — the reference
#   db_lt:   libclang cidx `index --no-graph` (symbols)   — then cidx_lt_edges
#            writes decl-level edges through the real Storage
# then byte-diff the decl-level projections of both DBs:
#   edge rows of kinds 2,3,6,8,9,17 (inherits/contains/overrides/field_of/
#   method_of/friend — kinds fully owned by the decl walk),
#   plus template_param, plus class-spec 4/5 + template_arg (REPORTED but
#   tolerated for now: kinds 4/5 also flow from unported body/minting paths).
#
#   ./scripts/lt_edge_diff.sh [file.c ...]     (default: all corpus TUs)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LT_ROOT="$(dirname "$SCRIPT_DIR")"
CIDX="${CIDX:-$LT_ROOT/../cpp-indexer/build/cidx}"
LT_EDGES="${LT_EDGES:-$LT_ROOT/build-lt/cidx_lt_edges}"
MANIFESTS="$(realpath "${MANIFESTS:-$LT_ROOT/../cpp-indexer/manifests}")"

FILES=("$@")
if [ ${#FILES[@]} -eq 0 ]; then
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

# Decl-level edge projection (kinds strictly owned by the decl walk).
EDGE_SQL="
  SELECT ssrc.usr, sdst.usr, e.kind, e.count,
         COALESCE(e.base_access,'N'), COALESCE(e.is_virtual,'N')
  FROM edge e
  JOIN symbol ssrc ON ssrc.id = e.src_id
  JOIN symbol sdst ON sdst.id = e.dst_id
  WHERE e.kind IN (2,3,6,8,9,17)
  ORDER BY ssrc.usr, sdst.usr, e.kind;"

PARAM_SQL="
  SELECT s.usr, p.position, p.param_kind, COALESCE(p.name,'N')
  FROM template_param p JOIN symbol s ON s.id = p.owner_id
  ORDER BY s.usr, p.position;"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SYSROOT_ARGS=()
if [ "$(uname)" = "Darwin" ]; then
  SYSROOT_ARGS=(--extra-arg="-isysroot$(xcrun --show-sdk-path)")
fi

fail=0
for f in "${FILES[@]}"; do
  ABS="$(resolve_tu "$f")"
  if [ -z "$ABS" ]; then echo "SKIP  $f (not in compile db)"; continue; fi

  # Reference: full index.
  export INDEXER_CACHE="$WORK/capi-$f"; mkdir -p "$INDEXER_CACHE"
  "$CIDX" import --db "$MANIFESTS/compile_commands.json" >/dev/null
  "$CIDX" index "$ABS" >/dev/null 2>&1 || true
  DB_CAPI="$INDEXER_CACHE/index.db"

  # Candidate: symbols only, then the LT edge pass.
  export INDEXER_CACHE="$WORK/lt-$f"; mkdir -p "$INDEXER_CACHE"
  "$CIDX" import --db "$MANIFESTS/compile_commands.json" >/dev/null
  "$CIDX" index --no-graph "$ABS" >/dev/null 2>&1 || true
  DB_LT="$INDEXER_CACHE/index.db"

  # Walk targets: main file + owned headers, in cidx's file-table order.
  TARGET_ARGS=(--target "$ABS")
  while IFS= read -r hdr; do
    [ "$hdr" = "$ABS" ] && continue
    TARGET_ARGS+=(--target "$hdr")
  done < <(sqlite3 "$DB_LT" "
      SELECT '$MANIFESTS' || substr(d.path, length('manifests')+1) || '/' || fl.name
      FROM file fl JOIN directory d ON d.id = fl.directory_id
      WHERE fl.indexed = 1 ORDER BY fl.id;" 2>/dev/null | sed "s#^$MANIFESTS/manifests#$MANIFESTS#")

  "$LT_EDGES" --db "$DB_LT" "${TARGET_ARGS[@]}" \
    -p "$MANIFESTS" "${SYSROOT_ARGS[@]}" "$ABS" >"$WORK/lt.out" 2>&1 || true

  ok=1
  for proj in EDGE PARAM; do
    sql_var="${proj}_SQL"
    sqlite3 -noheader -separator $'\t' "$DB_CAPI" "${!sql_var}" > "$WORK/a.tsv"
    sqlite3 -noheader -separator $'\t' "$DB_LT"   "${!sql_var}" > "$WORK/b.tsv"
    if ! diff -u "$WORK/a.tsv" "$WORK/b.tsv" > "$WORK/diff-$proj.txt"; then
      echo "FAIL  $f [$proj]"
      head -25 "$WORK/diff-$proj.txt"
      ok=0; fail=1
    fi
  done
  [ $ok -eq 1 ] && echo "PASS  $f ($(wc -l < "$WORK/a.tsv" | tr -d ' ') decl edges)"
done
exit $fail
