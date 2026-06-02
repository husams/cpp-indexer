#!/usr/bin/env python3
r"""
Basic CodexGraph queries (IndraDB).  Run after indexing a repo into IndraDB:

    PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python \
    CXG_DB_URI=indradb://localhost:27652 \
    python3 graph_query_examples.py

`PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python` is required: the `indradb`
driver's generated protobufs predate protobuf 7.x and otherwise fail to import.

See cxg_graph.py for the schema cheat-sheet and the helper API used here.
"""
import collections

from cxg_graph import Cxg

KINDS = ("CLASS", "FUNCTION", "METHOD", "FIELD", "ENUM", "NAMESPACE", "MODULE")


def example_A_inventory(g: Cxg):
    """A) Node inventory — count every node kind in one scan."""
    counts = collections.Counter(p.get("kind") for p in g.all_props().values())
    print("A) node kinds:", dict(counts.most_common()))


def example_B_classes_vs_structs(g: Cxg):
    """B) C++ classes vs C structs (record_kind splits CLASS-kind nodes)."""
    rk = collections.defaultdict(list)
    for r in g.of_kind("CLASS"):
        p = r["properties"]
        rk[p.get("record_kind")].append(p.get("qualified_name") or p.get("name"))
    for k in sorted(rk, key=lambda x: x or ""):
        print(f"B) record_kind={k}: {len(rk[k])}  e.g. {sorted(rk[k])[:5]}")


def example_C_lookup_symbol(g: Cxg, qn="KafkaConsumer"):
    """C) Resolve a symbol by qualified_name and show its raw kinds."""
    rows = g.by_qualified(qn)
    print(f"\nC) lookup qualified_name={qn!r}: {len(rows)} node(s)")
    for r in rows:
        p = r["properties"]
        print(f"   {r['t']:9} name={p.get('name')} abstract={p.get('is_abstract')}")


def example_D_class_relationships(g: Cxg, qn="KafkaConsumer"):
    """D) One-hop structure around a class: base, subclasses, users."""
    cls = g.one_class(qn)
    if not cls:
        print(f"\nD) no CLASS {qn}"); return
    vid = cls["id"]
    bases = [r["node"]["properties"].get("name") for r in g.neighbors(vid, "outbound", "INHERITS")]
    subs = [r["node"]["properties"].get("name") for r in g.neighbors(vid, "inbound", "INHERITS")]
    users = [r["node"]["properties"].get("name") or r["node"]["t"]
             for r in g.neighbors(vid, "inbound", "USES")]
    print(f"\nD) class {qn}: bases={bases} subclasses={subs} #users={len(users)}")


def example_E_callees(g: Cxg, name="offsets_store"):
    """E) Functions/methods called by `name` (outbound CALLS)."""
    print(f"\nE) callees of {name!r}: {g.callees(name)}")


def example_F_method_body(g: Cxg, name="offsets_store"):
    """F) Graph-native source: the `code` property holds the full body."""
    fns = g.functions(name)
    if fns:
        code = fns[0]["properties"].get("code", "")
        print(f"\nF) body of {name!r} (from graph `code` property):")
        print("   " + code.replace("\n", "\n   "))


if __name__ == "__main__":
    g = Cxg()
    example_A_inventory(g)
    example_B_classes_vs_structs(g)
    example_C_lookup_symbol(g)
    example_D_class_relationships(g)
    example_E_callees(g)
    example_F_method_body(g)
