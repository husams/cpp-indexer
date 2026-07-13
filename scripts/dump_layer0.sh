#!/bin/sh
# Normalized, ordered projection of the seven Layer-0 tables of a cidx index —
# the same projection index_golden_test compares (surrogate keys resolved to
# USR/kind-name/basename, rows ORDERed), usable on any database for reviewing
# semantic deltas between refactor phases:
#
#   scripts/dump_layer0.sh path/to/index.db > before.txt
#   ... change + reindex ...
#   scripts/dump_layer0.sh path/to/index.db > after.txt
#   diff -u before.txt after.txt
set -eu

db="${1:?usage: dump_layer0.sh <index.db>}"

q() {
  name="$1"
  sql="$2"
  printf '== %s ==\n' "$name"
  sqlite3 -tabs "$db" "$sql"
}

q file "SELECT f.name, COALESCE(f.compile_options,''), COALESCE(f.driver,''),
        f.indexed, COALESCE(f.md5,'') FROM file f ORDER BY f.name"

q symbol "SELECT s.usr, s.spelling, COALESCE(s.qual_name,''),
          COALESCE(s.display_name,''), COALESCE(sk.name, CAST(s.kind AS TEXT)),
          COALESCE(s.type_info,''), COALESCE(ff.name,''), COALESCE(s.line,''),
          COALESCE(s.col,''), COALESCE(df.name,''), COALESCE(s.decl_line,''),
          COALESCE(s.decl_col,''), s.is_definition, s.is_pure, s.is_static,
          s.is_instantiation, COALESCE(s.linkage,''), COALESCE(s.access,''),
          COALESCE(s.parent_usr,''), s.resolved
          FROM symbol s
          LEFT JOIN symbol_kind sk ON sk.id = s.kind
          LEFT JOIN file ff ON ff.id = s.file_id
          LEFT JOIN file df ON df.id = s.decl_file_id
          ORDER BY s.usr"

q decl_site "SELECT s.usr, COALESCE(f.name,''), COALESCE(d.line,''),
             COALESCE(d.col,''), d.is_definition
             FROM decl_site d JOIN symbol s ON s.id = d.symbol_id
             LEFT JOIN file f ON f.id = d.file_id
             ORDER BY s.usr, f.name, d.line, d.col"

q edge "SELECT ss.usr, ds.usr, COALESCE(ek.name, CAST(e.kind AS TEXT)),
        e.count, COALESCE(e.base_access,''), COALESCE(e.is_virtual,'')
        FROM edge e JOIN symbol ss ON ss.id = e.src_id
        JOIN symbol ds ON ds.id = e.dst_id
        LEFT JOIN edge_kind ek ON ek.id = e.kind
        ORDER BY ss.usr, ds.usr, e.kind"

q edge_site "SELECT ss.usr, ds.usr, COALESCE(ek.name, CAST(e.kind AS TEXT)),
             COALESCE(f.name,''), COALESCE(es.line,''), COALESCE(es.col,''),
             es.conditional, COALESCE(es.args_sig,''),
             COALESCE(es.recv_src_kind,''), COALESCE(es.recv_type_usr,''),
             COALESCE(es.recv_decl_usr,''), COALESCE(es.recv_param_pos,''),
             COALESCE(es.recv_type_is_value,'')
             FROM edge_site es JOIN edge e ON e.id = es.edge_id
             JOIN symbol ss ON ss.id = e.src_id
             JOIN symbol ds ON ds.id = e.dst_id
             LEFT JOIN edge_kind ek ON ek.id = e.kind
             LEFT JOIN file f ON f.id = es.file_id
             ORDER BY ss.usr, ds.usr, e.kind, f.name, es.line, es.col"

q call_arg "SELECT ss.usr, ds.usr, COALESCE(ek.name, CAST(e.kind AS TEXT)),
            COALESCE(f.name,''), ca.line, ca.col, ca.position, ca.src_kind,
            COALESCE(ca.type_usr,''), COALESCE(ca.decl_usr,''),
            COALESCE(ca.callee_usr,''), COALESCE(ca.type_is_value,'')
            FROM call_arg ca JOIN edge e ON e.id = ca.edge_id
            JOIN symbol ss ON ss.id = e.src_id
            JOIN symbol ds ON ds.id = e.dst_id
            LEFT JOIN edge_kind ek ON ek.id = e.kind
            LEFT JOIN file f ON f.id = ca.file_id
            ORDER BY ss.usr, ds.usr, e.kind, f.name, ca.line, ca.col,
            ca.position"

q template_arg "SELECT os.usr, ta.position, ta.arg_kind, COALESCE(rs.usr,''),
                COALESCE(ta.literal,'')
                FROM template_arg ta JOIN symbol os ON os.id = ta.owner_id
                LEFT JOIN symbol rs ON rs.id = ta.ref_id
                ORDER BY os.usr, ta.position"
