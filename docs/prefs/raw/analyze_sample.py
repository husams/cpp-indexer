import re, sys, collections

path = sys.argv[1]
lines = open(path, errors="replace").read().splitlines()

start = lines.index("Call graph:") + 1
end = next(i for i, l in enumerate(lines) if l.startswith("Total number in stack"))

node_re = re.compile(r"^([ +!:|]*?)(\d+)\s(.*)$")

class N:
    __slots__ = ("count", "sym", "kids", "parent", "depth", "loc")
    def __init__(s, c, sym, d):
        s.count, s.sym, s.kids, s.parent, s.depth = c, sym, [], None, d

roots = []
stack = []  # (col, node)
for raw in lines[start:end]:
    m = node_re.match(raw)
    if not m:
        continue
    prefix, cnt, rest = m.group(1), int(m.group(2)), m.group(3)
    col = len(prefix)
    # symbol name: strip "  + off  [0x..]  file:line"
    sym = re.sub(r"\s+\+ [\d,\.]+\s+\[[0x0-9a-f,\.]+\].*$", "", rest)
    sym = re.sub(r"\s+\[[0x0-9a-f,\.]+\].*$", "", sym).strip()
    loc = ""
    lm = re.search(r"\[0x[0-9a-f]+\]\s+(\S+:\d+)\s*$", rest)
    if lm:
        loc = lm.group(1)
    n = N(cnt, sym, col)
    n.loc = loc
    while stack and stack[-1][0] >= col:
        stack.pop()
    if stack:
        p = stack[-1][1]
        p.kids.append(n)
        n.parent = p
    else:
        roots.append(n)
    stack.append((col, n))

def walk(n, anc):
    yield n, anc
    na = anc | {n.sym}
    for k in n.kids:
        yield from walk(k, na)

incl = collections.Counter()
selfc = collections.Counter()
locs = {}
total = sum(r.count for r in roots)
for r in roots:
    for n, anc in walk(r, frozenset()):
        if n.sym not in anc:
            incl[n.sym] += n.count
        selfc[n.sym] += n.count - sum(k.count for k in n.kids)
        if n.loc and n.sym not in locs:
            locs[n.sym] = n.loc

def short(s, w=110):
    s = re.sub(r"\(in [^)]*\)", "", s).strip()
    return s if len(s) <= w else s[: w - 3] + "..."

print(f"TOTAL SAMPLES (all threads): {total}")
print(f"Main thread: {roots[0].count if roots else 0}\n")

print("=== TOP 30 BY INCLUSIVE (recursion-collapsed) ===")
for sym, c in incl.most_common(30):
    print(f"{c:7d}  {100*c/total:5.1f}%  {short(sym)}   {locs.get(sym,'')}")

print("\n=== TOP 30 BY SELF ===")
for sym, c in selfc.most_common(30):
    print(f"{c:7d}  {100*c/total:5.1f}%  {short(sym)}   {locs.get(sym,'')}")

print("\n=== HOTTEST PATH (greedy heaviest child, recursion folded) ===")
n = max(roots, key=lambda r: r.count)
seen_depth = 0
prev = None
while True:
    lbl = short(n.sym, 130)
    if lbl != prev:
        print(f"{n.count:7d}  {100*n.count/total:5.1f}%  {'  '*min(seen_depth,20)}{lbl}  {n.loc}")
        prev = lbl
        seen_depth += 1
    if not n.kids:
        break
    n = max(n.kids, key=lambda k: k.count)
