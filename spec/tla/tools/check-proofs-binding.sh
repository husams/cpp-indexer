#!/usr/bin/env bash
#
# Shared TLAPS theorem/invariant structural-binding check, factored out of
# check-proofs.sh so it can be exercised directly -- without installing or
# running the real tlapm toolchain -- by a fast unit test
# (check-proofs-binding-unit-test.sh), in addition to the full real-tlapm
# end-to-end regression (check-proofs-vacuous-comment-regression.sh).
#
# manifest.json's proofs[] entry declares which THEOREM a proof module is
# supposed to prove, which invariants that theorem covers, and which real,
# checked-in module the proof's Spec operator must come from ("extends").
# Nothing here re-checks that TLAPS actually verified the proof (run_proof()
# in check-proofs.sh does that, via "All N obligations proved."); this
# function only checks that a genuinely-verified proof's THEOREM says what
# manifest.json claims it says.
#
# Vulnerability history (see check-proofs.sh's own header for the round-1/2/3
# rounds this closes):
#
#   - Round 1: no binding at all -- a same-named vacuous
#     `THEOREM <declared> == TRUE OBVIOUS` plus an unrelated real theorem
#     satisfied "All N obligations proved." for any N.
#   - Round 2 (QA): bound to the declared theorem's own text, but via an
#     unscoped whole-file substring grep for "[]<Invariant>" -- a decoy
#     comment anywhere in the file, or a derived corollary theorem the
#     declared theorem doesn't itself state, could still satisfy it.
#   - Round 3 (senior-developer): scoped to the BY-citation closure of the
#     declared theorem and to each theorem's own statement text, but the
#     match within that statement was still an unscoped substring search --
#     `Negated == (~([]Inv1) /\ ~([]Inv2)) => TRUE BY <declared>` (negated
#     antecedent) and `FALSE => []Inv` (contradictory antecedent) both still
#     satisfied it.
#   - Round 3 fix: require a theorem's statement to structurally match
#     `<name> => []<Invariant>`, but "<name>" was resolved by scanning EVERY
#     .tla file check-proofs.sh had copied into its scratch work directory
#     for an operator whose body starts with the token "Init" -- and that
#     scratch directory contains the untrusted proof module itself (proofs/
#     is exactly what a PR under review controls). A proof module defining
#     its own fresh, module-local `WeakSpec == Init /\ FALSE` (a contradiction,
#     so both theorems built on it are vacuously true) was accepted as a real
#     "module-defined Spec operator": PROVEN end-to-end against real tlapm
#     1.5.0 offline (QA + senior-developer round-2 acceptance review).
#
#   Fix (this round): resolve the candidate Spec operator name(s) ONLY from
#   manifest.json's "extends" entry for this proof (a real, checked-in,
#   protected module -- never the proofs/ directory the PR controls) plus
#   that module's own transitive EXTENDS chain, each hop resolved by
#   searching only spec/tla/modules, spec/tla/conformance, and
#   spec/tla/protected (never spec/tla/proofs). A candidate operator's
#   definition must also match the full temporal-specification idiom
#   `Init /\ [][Next]_vars` (collapsed of whitespace), not merely start with
#   the bare token "Init" -- so `WeakSpec == Init /\ FALSE` can never qualify
#   regardless of where it is defined.
theorem_invariant_binding() {
  local manifest="$1"
  local module_name="$2"
  local module_file="$3"
  local spec_root="$4"
  python3 - "$manifest" "$module_name" "$module_file" "$spec_root" <<'PY'
import glob
import json
import os
import re
import sys

manifest_path, module_name, module_file, spec_root = sys.argv[1:5]
manifest = json.loads(open(manifest_path, encoding="utf-8").read())
entry = next(
    (p for p in manifest["proofs"] if p["module"] == f"proofs/{module_name}.tla"),
    None,
)
if entry is None:
    print("MANIFEST-ENTRY-MISSING")
    sys.exit(0)

theorem_name = entry["theorem"]
invariants = entry["provesInvariants"]
extends_entry = entry.get("extends")
trusted_assumptions = set(entry.get("trustedAssumptions", []))


def strip_comments(text):
    text = re.sub(r"\(\*.*?\*\)", "", text, flags=re.DOTALL)
    text = re.sub(r"\\\*.*", "", text)
    return text


text = strip_comments(open(module_file, encoding="utf-8").read())

# HSE-89 acceptance-review fix (this round): TLAPS treats every ASSUME in a
# module as a premise available for the rest of that module's proofs. The
# structural theorem/Spec-provenance checks below never looked at the proof
# module's own ASSUMEs, so a proof module could add e.g. `ASSUME FALSE` --
# anything follows from a contradiction -- and have TLAPS "prove" a
# structurally-correct-looking `Spec => []Invariant` theorem vacuously,
# passing every check below unchanged. The general invariant this closes:
# an untrusted proof module's assumption set must itself be constrained, not
# only the shape of what it claims to prove. Two independent checks, neither
# keyed to the literal text "ASSUME FALSE":
#
#   1. allowlist -- every ASSUME found in this (untrusted, proofs/-tree)
#      module must match, verbatim once whitespace is collapsed, one of
#      manifest.json's proofs[].trustedAssumptions entries for this module.
#      manifest.json is itself a protected, CODEOWNER-reviewed path, so
#      adding a new assumption requires the same review as adding one to
#      the proof module itself. Any assumption absent from that list is
#      rejected regardless of whether it happens to be sound.
#   2. vacuousness -- independent of the allowlist (defense in depth against
#      the allowlist itself declaring something unsound): the assumption
#      set is rejected if any top-level conjunct of any assumption is the
#      literal boolean FALSE, or if two assumptions' top-level conjuncts are
#      syntactic negations of each other. Both are general, decidable
#      syntactic unsatisfiability witnesses -- not a full SAT/decision
#      procedure, and not merely a string match on "FALSE" -- so
#      `ASSUME FALSE`, `ASSUME P /\ FALSE`, and `ASSUME P` alongside
#      `ASSUME ~P` (in the same or different ASSUME statements) are all
#      rejected the same way. See ASSURANCE.md for the limits of this
#      syntactic check (it is not a general theorem prover).
ASSUME_BOUNDARY_RE = re.compile(
    r"^(ASSUME\b|THEOREM\b|LEMMA\b|[A-Za-z_][A-Za-z0-9_]*\s*==|={4,})",
    re.MULTILINE,
)
NAMED_ASSUME_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*==\s*(.*)$")


def extract_assumptions(module_text):
    found = []
    for m in re.finditer(r"^ASSUME\s+", module_text, re.MULTILINE):
        start = m.end()
        end = len(module_text)
        for boundary in ASSUME_BOUNDARY_RE.finditer(module_text, start):
            end = boundary.start()
            break
        collapsed = " ".join(module_text[start:end].split())
        named = NAMED_ASSUME_RE.match(collapsed)
        expr = named.group(2) if named else collapsed
        found.append({"full": f"ASSUME {collapsed}", "expr": expr})
    return found


def top_level_conjuncts(expr):
    # Split a whitespace-collapsed boolean expression on top-level "/\"
    # connectives (paren depth 0), then strip at most one layer of fully
    # wrapping parens from each conjunct -- mirroring the "one layer of
    # wrapping" convention structurally_proves() already uses below.
    collapsed = re.sub(r"\s+", "", expr)
    parts = []
    depth = 0
    current = []
    i = 0
    while i < len(collapsed):
        ch = collapsed[i]
        if ch == "(":
            depth += 1
            current.append(ch)
            i += 1
        elif ch == ")":
            depth -= 1
            current.append(ch)
            i += 1
        elif depth == 0 and collapsed[i : i + 2] == "/\\":
            parts.append("".join(current))
            current = []
            i += 2
        else:
            current.append(ch)
            i += 1
    parts.append("".join(current))

    def unwrap(part):
        if part.startswith("(") and part.endswith(")"):
            inner_depth = 0
            for pos, ch in enumerate(part):
                if ch == "(":
                    inner_depth += 1
                elif ch == ")":
                    inner_depth -= 1
                    if inner_depth == 0 and pos != len(part) - 1:
                        return part
            return part[1:-1]
        return part

    return [unwrap(part) for part in parts if part]


def negated_form(conjunct):
    if conjunct.startswith("~(") and conjunct.endswith(")"):
        return conjunct[2:-1]
    if conjunct.startswith("~"):
        return conjunct[1:]
    return None


def check_proof_assumptions(module_text, allowlist):
    assumptions = extract_assumptions(module_text)

    all_conjuncts = []
    for assumption in assumptions:
        all_conjuncts.extend(top_level_conjuncts(assumption["expr"]))

    if "FALSE" in all_conjuncts:
        return "ASSUMPTION-VACUOUS:FALSE"

    seen = set(all_conjuncts)
    for conjunct in all_conjuncts:
        negated = negated_form(conjunct)
        if negated is not None and negated in seen:
            return f"ASSUMPTIONS-CONTRADICT:{negated}"

    for assumption in assumptions:
        if assumption["full"] not in allowlist:
            return f"ASSUMPTION-NOT-TRUSTED:{assumption['full']}"

    return None


assumption_violation = check_proof_assumptions(text, trusted_assumptions)
if assumption_violation is not None:
    print(assumption_violation)
    sys.exit(0)

theorem_re = re.compile(r"^THEOREM\s+([A-Za-z_][A-Za-z0-9_]*)\s*==(.*)$", re.MULTILINE)
boundary_re = re.compile(r"^(THEOREM\b|LEMMA\b|={4,})", re.MULTILINE)

theorems = {}
for m in theorem_re.finditer(text):
    start = m.end()
    end = len(text)
    for boundary in boundary_re.finditer(text, start):
        end = boundary.start()
        break
    theorems[m.group(1)] = m.group(2) + text[start:end]

if theorem_name not in theorems:
    print(f"THEOREM-NOT-FOUND:{theorem_name}")
    sys.exit(0)


def statement_of(block):
    # A theorem's statement is everything before its proof begins: a `BY`/
    # `PROOF` keyword or a `<1>`-style structured-proof step marker.
    m = re.search(r"\n\s*(BY\b|PROOF\b|<\d+>)", block)
    return block[: m.start()] if m else block


def proof_refs(block):
    refs = set()
    for by_clause in re.finditer(r"\bBY\b([^\n]*)", block):
        refs.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", by_clause.group(1)))
    return refs


statements = {name: statement_of(block) for name, block in theorems.items()}
refs = {name: proof_refs(block) for name, block in theorems.items()}

closure = {theorem_name}
changed = True
while changed:
    changed = False
    for name, r in refs.items():
        if name not in closure and r & closure:
            closure.add(name)
            changed = True

# Real, non-vacuous TLA+ specifications in this repository follow the
# standard idiom "Spec == Init /\ [][Next]_vars /\ Fairness" (see e.g.
# modules/CidxResult.tla). Requiring this exact shape -- not merely a body
# that starts with the token "Init" -- rejects a fabricated contradiction
# like "WeakSpec == Init /\ FALSE" even if such an operator were somehow
# discovered as a candidate.
SPEC_SHAPE_RE = re.compile(r"^Init/\\\[\]\[[A-Za-z_][A-Za-z0-9_]*\]_[A-Za-z_][A-Za-z0-9_]*")

# Only these directories hold real, checked-in, reviewed modules. proofs/ is
# deliberately excluded: it is exactly the directory a PR under review
# controls, so an operator defined only there can never be treated as this
# proof's real specification.
TRUSTED_SUBDIRS = ("modules", "conformance", "protected")


def module_header_name(text):
    m = re.search(r"^-+\s*MODULE\s+([A-Za-z_][A-Za-z0-9_]*)\s*-+", text, re.MULTILINE)
    return m.group(1) if m else None


def resolve_trusted_module(name, root):
    for subdir in TRUSTED_SUBDIRS:
        for path in glob.glob(os.path.join(root, subdir, "*.tla")):
            candidate_text = strip_comments(open(path, encoding="utf-8").read())
            if module_header_name(candidate_text) == name:
                return path, candidate_text
    return None, None


def spec_operator_names_in(text):
    names = set()
    def_re = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*==(.*)$", re.MULTILINE)
    boundary = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*\s*==|={4,})", re.MULTILINE)
    for m in def_re.finditer(text):
        start = m.end()
        end = len(text)
        for b in boundary.finditer(text, start):
            end = b.start()
            break
        body = m.group(2) + text[start:end]
        collapsed = re.sub(r"\s+", "", body)
        if SPEC_SHAPE_RE.match(collapsed):
            names.add(m.group(1))
    return names


def trusted_spec_operator_names(extends_entry, root):
    # extends_entry is a manifest-relative path, e.g. "modules/CidxResult.tla".
    if not extends_entry:
        return set()
    root_path = os.path.join(root, extends_entry)
    if not os.path.isfile(root_path):
        return set()

    visited_paths = set()
    names = set()
    queue = [root_path]
    while queue:
        path = queue.pop()
        real_path = os.path.realpath(path)
        if real_path in visited_paths:
            continue
        visited_paths.add(real_path)
        candidate_text = strip_comments(open(path, encoding="utf-8").read())
        names.update(spec_operator_names_in(candidate_text))

        extends_m = re.search(r"^EXTENDS\s+(.+)$", candidate_text, re.MULTILINE)
        if extends_m:
            for dep_name in re.split(r"\s*,\s*", extends_m.group(1).strip()):
                dep_path, _ = resolve_trusted_module(dep_name, root)
                if dep_path is not None:
                    queue.append(dep_path)
    return names


def structurally_proves(statement, invariant, spec_names):
    # Require the statement, once whitespace is collapsed, to be exactly
    # "<spec>=>[]<invariant>" (with at most one layer of wrapping parens
    # around the whole implication and/or the "[]<invariant>" term) for some
    # trusted, extends-chain-resolved spec operator -- not merely contain
    # that text as a substring anywhere (which a negation or an unrelated
    # conjunct could satisfy without the theorem proving anything of the
    # sort).
    collapsed = re.sub(r"\s+", "", statement)
    for spec in spec_names:
        pattern = (
            r"^\(*"
            + re.escape(spec)
            + r"=>\(*\[\]"
            + re.escape(invariant)
            + r"\)*$"
        )
        if re.fullmatch(pattern, collapsed):
            return True
    return False


spec_names = trusted_spec_operator_names(extends_entry, spec_root)

if not spec_names:
    print(f"NO-TRUSTED-SPEC-OPERATOR:{extends_entry}")
    sys.exit(0)

missing = [
    invariant
    for invariant in invariants
    if not any(
        structurally_proves(statements[n], invariant, spec_names) for n in closure
    )
]

if missing:
    print("INVARIANT-NOT-FOUND:" + ",".join(missing))
else:
    print(f"OK:{theorem_name}:{','.join(invariants)}")
PY
}
