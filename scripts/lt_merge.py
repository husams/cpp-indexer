#!/usr/bin/env python3
"""Replay cidx add_symbol upsert semantics over the cidx_lt_probe TSV stream.

The probe emits one row per visited cursor across every non-system file of the
TU (leading `file` column); cidx's symbol phase indexes the MAIN file first,
then owned headers, folding rows per USR through add_symbol's ON CONFLICT merge
(storage.cpp:2298). This replays that: rows are filtered to files under --root,
partitioned main-file-first, merged per USR (the file column follows the same
definition-wins CASE as the location block), and the final rows whose winning
file == --main are printed WITHOUT the file column, sorted by USR — directly
diffable against the `symbol` table dump.

Input columns (20, tab-separated, \\N = NULL):
file usr spelling kind qual_name display_name type_info line col end_line
end_col decl_line decl_col is_definition is_pure is_static linkage access
parent_usr resolved
"""

import argparse
import sys

NULL = "\\N"
COLS = 20
(FILE, USR, SPELLING, KIND, QUAL, DISPLAY, TYPEINFO, LINE, COL, ELINE, ECOL,
 DLINE, DCOL, ISDEF, ISPURE, ISSTATIC, LINKAGE, ACCESS, PARENT,
 RESOLVED) = range(COLS)


def coalesce(new, old):
    return new if new != NULL else old


def merge(old, new):
    out = list(old)
    out[SPELLING] = new[SPELLING]                       # excluded.spelling
    out[QUAL] = coalesce(new[QUAL], old[QUAL])
    out[DISPLAY] = coalesce(new[DISPLAY], old[DISPLAY])
    out[KIND] = new[KIND]                               # excluded.kind
    out[TYPEINFO] = coalesce(new[TYPEINFO], old[TYPEINFO])
    # file/location block moves when excluded.is_definition >= symbol.is_definition
    if int(new[ISDEF]) >= int(old[ISDEF]):
        for i in (FILE, LINE, COL, ELINE, ECOL):
            out[i] = new[i]
    out[DLINE] = coalesce(new[DLINE], old[DLINE])
    out[DCOL] = coalesce(new[DCOL], old[DCOL])
    for i in (ISDEF, ISPURE, ISSTATIC, RESOLVED):       # MAX()
        out[i] = str(max(int(new[i]), int(old[i])))
    out[LINKAGE] = coalesce(new[LINKAGE], old[LINKAGE])
    out[ACCESS] = coalesce(new[ACCESS], old[ACCESS])
    out[PARENT] = coalesce(new[PARENT], old[PARENT])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--main", required=True, help="main file path (compared)")
    ap.add_argument("--root", required=True,
                    help="component root; rows outside it are not indexed")
    args = ap.parse_args()
    root = args.root.rstrip("/") + "/"

    main_rows, header_rows = [], []
    for line in sys.stdin:
        line = line.rstrip("\n")
        if not line:
            continue
        f = line.split("\t")
        if len(f) != COLS:
            raise SystemExit(f"bad row ({len(f)} cols): {line!r}")
        if f[FILE] == args.main:
            main_rows.append(f)
        elif f[FILE].startswith(root):  # owned header
            header_rows.append(f)
        # else: not owned by the component -> not indexed by cidx

    rows = {}
    for f in main_rows + header_rows:   # cidx order: main file, then headers
        usr = f[USR]
        rows[usr] = merge(rows[usr], f) if usr in rows else f

    for usr in sorted(rows):
        row = rows[usr]
        if row[FILE] == args.main:      # compare the main file's rows only
            sys.stdout.write("\t".join(row[1:]) + "\n")


if __name__ == "__main__":
    main()
