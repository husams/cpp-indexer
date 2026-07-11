#!/usr/bin/env bash
# lt_index_diff.sh — Phase-2 parity gate: interleaved LT indexer vs libclang cidx.
#
# Per TU:
#   db_capi: libclang cidx FULL index                       — the reference
#   db_lt:   libclang cidx `index --no-graph` (creates file rows + md5s),
#            symbol/decl_site rows CLEARED, then cidx_lt_index rebuilds
#            symbols + decl-level edges with AstIndexer's exact per-file
#            interleave (symbols/edges main, then two-pass headers)
# then diff three projections:
#   SYw  — symbol rows with a file_id (real indexed symbols; body-minted
#          stubs and instantiation mints belong to unported phases)
#   EDGE — edge rows of kinds 2,3,6,8,9,17 between non-instantiation symbols
#   PARM — template_param rows
#
#   ./scripts/lt_index_diff.sh [file.c ...]     (default: all corpus TUs)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LT_ROOT="$(dirname "$SCRIPT_DIR")"
CIDX="${CIDX:-$LT_ROOT/../cpp-indexer/build/cidx}"
LT_INDEX="${LT_INDEX:-$LT_ROOT/build-lt/cidx_lt_index}"
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

# decl_line/decl_col are EXCLUDED here: the unported body pass backfills decl
# sites onto definitions (callee-stub ensures, instance mints). Symbol-pass
# decl sites are separately proven byte-identical by lt_symbol_diff.sh.
# USRs flagged is_instantiation / is_named_instance in the REFERENCE db are
# excluded from both sides (ref.symbol via ATTACH): those flags are set by the
# unported template/minting passes, so the same symbol legitimately differs.
SY_SQL="
  SELECT s.usr, s.spelling, s.kind, COALESCE(s.qual_name,'N'),
         COALESCE(s.display_name,'N'), COALESCE(s.type_info,'N'),
         fl.name, s.line, s.col, s.end_line, s.end_col,
         s.is_definition, s.is_pure, s.is_static,
         COALESCE(s.linkage,'N'), COALESCE(s.access,'N'),
         COALESCE(s.parent_usr,'N'), s.resolved
  FROM symbol s JOIN file fl ON fl.id = s.file_id
  WHERE s.usr NOT IN (SELECT usr FROM ref.symbol
                      WHERE is_instantiation = 1 OR is_named_instance = 1)
  ORDER BY s.usr, fl.name;"

EDGE_SQL="
  SELECT ssrc.usr, sdst.usr, e.kind, e.count,
         COALESCE(e.base_access,'N'), COALESCE(e.is_virtual,'N')
  FROM edge e
  JOIN symbol ssrc ON ssrc.id = e.src_id
  JOIN symbol sdst ON sdst.id = e.dst_id
  WHERE e.kind IN (2,3,6,8,9,17)
    AND ssrc.usr NOT IN (SELECT usr FROM ref.symbol
                         WHERE is_instantiation = 1 OR is_named_instance = 1)
    AND sdst.usr NOT IN (SELECT usr FROM ref.symbol
                         WHERE is_instantiation = 1 OR is_named_instance = 1)
  ORDER BY ssrc.usr, sdst.usr, e.kind;"

PARM_SQL="
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

  export INDEXER_CACHE="$WORK/capi-$f"; mkdir -p "$INDEXER_CACHE"
  "$CIDX" import --db "$MANIFESTS/compile_commands.json" >/dev/null
  "$CIDX" index "$ABS" >/dev/null 2>&1 || true
  DB_CAPI="$INDEXER_CACHE/index.db"

  export INDEXER_CACHE="$WORK/lt-$f"; mkdir -p "$INDEXER_CACHE"
  "$CIDX" import --db "$MANIFESTS/compile_commands.json" >/dev/null
  "$CIDX" index --no-graph "$ABS" >/dev/null 2>&1 || true
  DB_LT="$INDEXER_CACHE/index.db"
  sqlite3 "$DB_LT" "DELETE FROM decl_site; DELETE FROM symbol;"

  # Walk targets: main + owned headers in cidx's discovery order (file table).
  TARGET_ARGS=(--target "$ABS")
  while IFS= read -r hdr; do
    [ "$hdr" = "$ABS" ] && continue
    TARGET_ARGS+=(--target "$hdr")
  done < <(sqlite3 "$DB_LT" "
      SELECT d.path || '/' || fl.name
      FROM file fl JOIN directory d ON d.id = fl.directory_id
      WHERE fl.indexed = 1 ORDER BY fl.id;" \
    | sed "s#^manifests#$MANIFESTS#")

  "$LT_INDEX" --db "$DB_LT" "${TARGET_ARGS[@]}" \
    -p "$MANIFESTS" "${SYSROOT_ARGS[@]}" "$ABS" >"$WORK/lt.out" 2>&1 || true

  ok=1
  for proj in SY EDGE PARM; do
    sql_var="${proj}_SQL"
    sqlite3 -noheader -separator $'\t' "$DB_CAPI" \
      "ATTACH '$DB_CAPI' AS ref; ${!sql_var}" > "$WORK/a.tsv"
    sqlite3 -noheader -separator $'\t' "$DB_LT" \
      "ATTACH '$DB_CAPI' AS ref; ${!sql_var}" > "$WORK/b.tsv"
    if ! diff -u "$WORK/a.tsv" "$WORK/b.tsv" > "$WORK/diff-$proj.txt"; then
      echo "FAIL  $f [$proj]"
      head -20 "$WORK/diff-$proj.txt"
      ok=0; fail=1
    fi
  done
  [ $ok -eq 1 ] && echo "PASS  $f"
done
exit $fail
