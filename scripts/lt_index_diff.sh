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

# FULL Layer-0 projections: every row the clangx passes write, joined to USRs
# so ids cancel out. Stubs (file_id NULL) are included via LEFT JOIN.
SY_SQL="
  SELECT s.usr, s.spelling, s.kind, COALESCE(s.qual_name,'N'),
         COALESCE(s.display_name,'N'), COALESCE(s.type_info,'N'),
         COALESCE(fl.name,'N'), COALESCE(s.line,'N'), COALESCE(s.col,'N'),
         COALESCE(s.end_line,'N'), COALESCE(s.end_col,'N'),
         COALESCE(dfl.name,'N'), COALESCE(s.decl_line,'N'),
         COALESCE(s.decl_col,'N'), COALESCE(s.decl_path,'N'),
         s.is_definition, s.is_pure, s.is_static, s.is_instantiation,
         s.is_named_instance,
         COALESCE(s.linkage,'N'), COALESCE(s.access,'N'),
         COALESCE(s.parent_usr,'N'), s.resolved
  FROM symbol s
  LEFT JOIN file fl ON fl.id = s.file_id
  LEFT JOIN file dfl ON dfl.id = s.decl_file_id
  ORDER BY s.usr, fl.name;"

EDGE_SQL="
  SELECT ssrc.usr, sdst.usr, e.kind, e.count,
         COALESCE(e.base_access,'N'), COALESCE(e.is_virtual,'N')
  FROM edge e
  JOIN symbol ssrc ON ssrc.id = e.src_id
  JOIN symbol sdst ON sdst.id = e.dst_id
  ORDER BY ssrc.usr, sdst.usr, e.kind;"

SITE_SQL="
  SELECT ssrc.usr, sdst.usr, e.kind, es.line, es.col, es.conditional,
         COALESCE(es.recv_src_kind,'N'), COALESCE(es.recv_type_usr,'N'),
         COALESCE(es.recv_decl_usr,'N'), COALESCE(es.recv_param_pos,'N'),
         COALESCE(es.recv_type_is_value,'N')
  FROM edge_site es
  JOIN edge e ON e.id = es.edge_id
  JOIN symbol ssrc ON ssrc.id = e.src_id
  JOIN symbol sdst ON sdst.id = e.dst_id
  ORDER BY ssrc.usr, sdst.usr, e.kind, es.line, es.col;"

ARG_SQL="
  SELECT ssrc.usr, sdst.usr, ca.line, ca.col, ca.position, ca.src_kind,
         COALESCE(ca.type_usr,'N'), COALESCE(ca.decl_usr,'N'),
         COALESCE(ca.callee_usr,'N'), COALESCE(ca.type_is_value,'N')
  FROM call_arg ca
  JOIN edge e ON e.id = ca.edge_id
  JOIN symbol ssrc ON ssrc.id = e.src_id
  JOIN symbol sdst ON sdst.id = e.dst_id
  ORDER BY ssrc.usr, sdst.usr, ca.line, ca.col, ca.position;"

PARM_SQL="
  SELECT s.usr, p.position, p.param_kind, COALESCE(p.name,'N')
  FROM template_param p JOIN symbol s ON s.id = p.owner_id
  ORDER BY s.usr, p.position;"

TARG_SQL="
  SELECT s.usr, ta.position, ta.arg_kind, COALESCE(ta.literal,'N'),
         COALESCE(r.usr,'N')
  FROM template_arg ta
  JOIN symbol s ON s.id = ta.owner_id
  LEFT JOIN symbol r ON r.id = ta.ref_id
  ORDER BY s.usr, ta.position, COALESCE(ta.literal,'N');"

DEF_SQL="
  SELECT s.usr, fl.name, d.line, d.col, d.end_line, d.end_col,
         COALESCE(d.init_text,'N')
  FROM definition d
  JOIN symbol s ON s.id = d.symbol_id
  JOIN file fl ON fl.id = d.file_id
  ORDER BY s.usr, fl.name, d.line;"

DEDGE_SQL="
  SELECT s.usr, sdst.usr, de.kind, de.count
  FROM def_edge de
  JOIN definition d ON d.id = de.src_def_id
  JOIN symbol s ON s.id = d.symbol_id
  JOIN symbol sdst ON sdst.id = de.dst_id
  ORDER BY s.usr, sdst.usr, de.kind;"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SYSROOT_ARGS=()
if [ "$(uname)" = "Darwin" ]; then
  # Use the versionless MacOSX.sdk symlink spelling — cidx's driver probe
  # reports that form, and stub decl_path strings must match byte-for-byte.
  SDK="$(xcrun --show-sdk-path | sed 's/MacOSX[0-9.]*\.sdk/MacOSX.sdk/')"
  SYSROOT_ARGS=(--extra-arg="-isysroot$SDK")
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
  for proj in SY EDGE SITE ARG PARM TARG DEF DEDGE; do
    sql_var="${proj}_SQL"
    sqlite3 -noheader -separator $'\t' "$DB_CAPI" "${!sql_var}" > "$WORK/a.tsv"
    sqlite3 -noheader -separator $'\t' "$DB_LT"   "${!sql_var}" > "$WORK/b.tsv"
    if ! diff -u "$WORK/a.tsv" "$WORK/b.tsv" > "$WORK/diff-$proj.txt"; then
      echo "FAIL  $f [$proj]"
      head -20 "$WORK/diff-$proj.txt"
      ok=0; fail=1
    fi
  done
  [ $ok -eq 1 ] && echo "PASS  $f"
done
exit $fail
