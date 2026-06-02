#!/usr/bin/env python3
r"""
Advanced CodexGraph queries (IndraDB) — transitive call graphs, class
hierarchies, type-usage, namespace contents, orphan-interface detection, fan-out
ranking, and compact-ingest id resolution.

    PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python \
    CXG_DB_URI=indradb://localhost:27652 \
    CXG_SYMBOL_DB=/path/to/cxg-symbols.db \      # optional, enables example 7
    python3 advanced_graph_queries.py

Every traversal here is bounded by what the indexer emitted. CALLS-edge coverage
is partial and UNEVEN: some functions resolve their callees fully (e.g.
rd_kafka_new has 24), others show none even though their body calls indexed
functions (e.g. rd_kafka_poll). So call trees here are a lower bound — the `code`
property on each function node is the textual ground truth.

Note: example 8 sweeps every METHOD/FUNCTION node (one round-trip each), so it is
intentionally O(N) and slow on a large graph; it is a demonstration, not a
hot-path pattern.
"""
from __future__ import annotations

import collections

from cxg_graph import Cxg, SymbolMap, TYPE_REF_EDGES


# ── 1. Fan-in / fan-out of a single function ─────────────────────────────────
def q1_fan(g: Cxg, name: str):
    callees, callers = g.callees(name), g.callers(name)
    print(f"1) {name!r}: fan-out={len(callees)} callees, fan-in={len(callers)} callers")
    print(f"     callees: {callees[:8]}")
    print(f"     callers: {callers[:8]}")


# ── 2. Transitive call tree (BFS, depth-limited, cycle-safe) ─────────────────
def q2_call_tree(g: Cxg, root: str, max_depth: int = 3):
    """All functions transitively reachable from `root` via CALLS."""
    seen = {root}
    frontier = [(root, 0)]
    edges = []
    while frontier:
        name, depth = frontier.pop()
        if depth >= max_depth:
            continue
        for callee in g.callees(name):
            edges.append((name, callee, depth))
            if callee not in seen:
                seen.add(callee)
                frontier.append((callee, depth + 1))
    print(f"\n2) transitive callees of {root!r} (depth<= {max_depth}): {len(seen) - 1} reachable")
    for caller, callee, d in edges[:20]:
        print(f"     {'  ' * d}{caller} -> {callee}")


# ── 3. Class hierarchy: ancestors (up) and descendants (down) ────────────────
def q3_hierarchy(g: Cxg, qn: str):
    def walk(start_vid, direction):
        seen, out, frontier = set(), [], [start_vid]
        while frontier:
            vid = frontier.pop()
            for r in g.neighbors(vid, direction, "INHERITS"):
                nid, nm = r["node"]["id"], r["node"]["properties"].get("name")
                if nid not in seen:
                    seen.add(nid); out.append(nm); frontier.append(nid)
        return out
    cls = g.one_class(qn)
    if not cls:
        print(f"\n3) no CLASS {qn}"); return
    ancestors = walk(cls["id"], "outbound")    # what qn inherits, transitively
    descendants = walk(cls["id"], "inbound")   # what inherits qn, transitively
    print(f"\n3) hierarchy of {qn}: ancestors={ancestors} descendants={descendants}")


# ── 4. "Who uses this type?" (inbound USES/OF_TYPE/POINTS_TO) ─────────────────
def q4_type_users(g: Cxg, type_name: str):
    rows = [r for r in g.by_name(type_name) if r["t"] in ("CLASS", "TYPE")]
    users: set[str] = set()
    for node in rows:
        for ek in TYPE_REF_EDGES:
            for r in g.neighbors(node["id"], "inbound", ek):
                p = r["node"]["properties"]
                users.add(f"{r['node']['t']}:{p.get('name') or p.get('qualified_name')}")
    print(f"\n4) users of type {type_name!r}: {len(users)}")
    for u in sorted(users)[:12]:
        print("     ", u)


# ── 5. Namespace contents (outbound CONTAINS from a NAMESPACE node) ──────────
def q5_namespace_members(g: Cxg, ns="RdKafka"):
    node = next((r for r in g.by_name(ns) if r["t"] == "NAMESPACE"), None)
    if not node:
        print(f"\n5) no NAMESPACE {ns}"); return
    members = collections.Counter()
    names = collections.defaultdict(list)
    for r in g.neighbors(node["id"], "outbound", "CONTAINS"):
        t = r["node"]["t"]; members[t] += 1
        names[t].append(r["node"]["properties"].get("name"))
    print(f"\n5) namespace {ns} contains: {dict(members)}")
    print(f"     classes: {sorted(set(names.get('CLASS', [])))[:10]}")


# ── 6. Orphan interfaces: abstract classes with no in-repo implementation ────
def q6_orphan_interfaces(g: Cxg):
    orphans = []
    for r in g.of_kind("CLASS"):
        p = r["properties"]
        if not p.get("is_abstract"):
            continue
        if not g.neighbors(r["id"], "inbound", "INHERITS"):   # no subclass indexed
            orphans.append(p.get("name"))
    print(f"\n6) abstract classes with no indexed subclass ({len(orphans)}):")
    print("     ", sorted(orphans)[:15])


# ── 7. Resolve compact-ingest ids -> file path / USR ─────────────────────────
def q7_resolve_ids(g: Cxg, name="offsets_store"):
    sm = SymbolMap()
    if not sm.conn:
        print("\n7) skipped (set CXG_SYMBOL_DB to a cxg-symbols.db)"); return
    for fn in g.functions(name):
        p = fn["properties"]
        print(f"\n7) {name}: file_id={p.get('file_id')} -> {sm.file(p.get('file_id'))}")
        print(f"     symbol_id={p.get('symbol_id')} -> {sm.usr(p.get('symbol_id'))}")


# ── 8. Fan-out ranking: which functions delegate the most ────────────────────
def q8_fanout_ranking(g: Cxg, top=10):
    ranking = []
    for r in g.of_kind("METHOD") + g.of_kind("FUNCTION"):
        n = len(g.neighbors(r["id"], "outbound", "CALLS"))
        if n:
            ranking.append((n, r["properties"].get("qualified_name") or r["properties"].get("name")))
    ranking.sort(reverse=True)
    print(f"\n8) top {top} functions by CALLS fan-out:")
    for n, qn in ranking[:top]:
        print(f"     {n:3}  {qn}")


if __name__ == "__main__":
    g = Cxg()
    q1_fan(g, "offsets_store")
    q2_call_tree(g, "offsets_store", max_depth=3)
    q3_hierarchy(g, "KafkaConsumerImpl")
    q4_type_users(g, "TopicPartition")
    q5_namespace_members(g, "RdKafka")
    q6_orphan_interfaces(g)
    q7_resolve_ids(g, "offsets_store")
    q8_fanout_ranking(g, top=10)
