# Self-hosting the CIDX architecture contract (HSE-71)

HSE-58/ADR-011 and `architecture/cidx-module-manifest.json` define the
enforceable module/dependency contract; `scripts/check_architecture.py` and
`scripts/check_platform_contract.py` are the stdlib-only bootstrap checkers
that validate it from source text, CMake, and Python imports before a
self-index exists.

HSE-71 adds a second, independent signal: index CIDX's own repository with
CIDX, then check the same contract against the **resolved semantic call
graph** rather than source text. This is what "self-hosting" means here —
CIDX becomes a continuously versioned architecture and compatibility corpus
for its own repository.

## Two independent checkers, one contract

| | Bootstrap (`scripts/check_architecture.py`) | Self-host (`scripts/self_host_architecture_report.py`) |
| --- | --- | --- |
| Input | source text, CMake, Python AST | resolved `calls`/`dispatch_calls` edges from a real self-index |
| Can see | `#include`, `target_link_libraries`, imports | actual call edges, independent of whether a matching `#include` exists |
| Cannot see | whether an include is ever actually used | anything not captured as a `calls`/`dispatch_calls` edge (function pointers, unresolved virtual dispatch) |
| Runs | always, no build required | requires a built `cidx` and a self-index (`scripts/self_host_index.sh`) |

Both consume the **same** `architecture/cidx-module-manifest.json` allowed-
dependency graph and `exceptions` ledger, so they can be compared for
agreement wherever they both have evidence for the same (caller file, callee
module) pair — see "Cross-check" below.

## What the self-host report checks

1. **Module boundary violations** — every resolved cross-module call edge is
   checked against the manifest's `allowedDependencies` and `exceptions`
   ledger. A new call from one module into another that is not declared
   allowed, and not covered by a matching file exception, fails the gate.
2. **Module cycles** — cycles found in the *actual resolved call graph*
   (independent of the declared-dependency cycle check already in
   `scripts/check_architecture.py`).
3. **Legacy façade violations** — `architecture/cidx-self-host-policy.json`
   names two legacy compatibility classes and freezes their current call
   sites as a baseline:
   - `cidx::Storage` / `cidx::SqliteStorageService` (the "Compatibility
     facade for legacy application code", HSE-62 cutover) — any new call
     site from outside `persistence.sqlite`/`product.application` fails.
   - `cidx::graph::GraphQuery` (the pre-QueryPlan graph read adapter,
     HSE-24 migration) — any new call site from outside
     `analysis.graph`/`query.plan` that is not already in the frozen
     baseline fails. Paying down an existing baseline entry only requires
     deleting it; it never needs to be re-added.
4. **Catalog duplication guard** — rejects a hand-authored redeclaration of a
   generated catalog `(id, name)` pair (HSE-59) anywhere outside the files
   `scripts/generate_catalogs.py` actually writes.
5. **Unclassified sources** — reuses the bootstrap classification; any
   production source matching zero or more than one module is surfaced.

Every finding carries a **witness**: caller symbol/file/line, callee
symbol/file/line, and the exact call-site line/column, so a developer can
navigate directly from the policy finding to the offending source location.

## Cross-check: bootstrap vs. semantic agreement

For every `(caller file, callee module)` pair where **both** a textual
`#include` (bootstrap evidence) and a resolved call edge (semantic evidence)
exist, the two checkers must agree on whether it is a violation. A
disagreement — one checker flags it, the other doesn't, despite both having
direct evidence — blocks the gate (`crossCheck.status: "blocked"`) instead of
being silently resolved by either side. See
`tests/self_host_architecture_test.py::test_disagreement_between_bootstrap_and_semantic_exception_matching_blocks_the_gate`
for a real example this design surfaces: a manifest exception with a specific
`include` spelling only suppresses the bootstrap finding when the spelling
matches exactly, while the self-host report's exceptions are keyed only by
`(source, fromModule, toModule)`. A future manifest change that adds an
`include`-scoped exception must keep both checkers' view of "excepted"
consistent, or the cross-check will (by design) fail closed.

## Absence of evidence is not evidence of absence

The report always records `completeness` and `unresolvedLimitations`
explicitly:

- If the self-index produced zero `calls`/`dispatch_calls` edges, the report
  says so and treats the semantic layer as incomplete rather than clean.
- If the index's `component` path does not match the expected repository
  root (e.g. a stale or mismatched self-index), `completeness.identityIssues`
  is populated and `completeness.semantic` becomes `"partial"`.
- Virtual dispatch through an unresolved base pointer, calls through function
  pointers/`std::function`, and the catalog guard's textual matching are
  named as permanent, structural limitations — a clean report never implies
  these are covered.

## Running it

```bash
# Fast, no build required: bootstrap + platform contract + mutation tests.
python3 scripts/check_architecture.py --manifest architecture/cidx-module-manifest.json
python3 scripts/check_platform_contract.py --module-manifest architecture/cidx-module-manifest.json
python3 tests/architecture_test.py
python3 tests/platform_contract_test.py
python3 tests/self_host_architecture_test.py

# Full self-host run: builds cidx (unless --build-dir is given), indexes this
# repository into a throwaway /tmp cache (never the checked-in index.db),
# and writes the deterministic report.
scripts/self_host_index.sh --out /tmp/self-host-report.json
```

The report format is `cidx.self-host-architecture-report/v1`; every run
records `sourceRevision` (git SHA), `index.{schemaVersion,catalogVersion,
catalogHash,graphResolvedAt,fileCount,symbolCount,edgeCount}`, `config.
{manifestVersion,policyVersion,packageHash}`, and an overall `status` of
`"pass"` or `"fail"`. A release gate should require a current report with
`status: "pass"` and treat any change to `architecture/cidx-self-host-
policy.json`'s `baseline` entries as requiring the same review as a manifest
exception (owner, rationale, expiry, removal issue).

Two top-level fields are wall-clock stamps of *when this run happened*, not
of what it found, and legitimately differ between two runs over the exact
same, byte-identical checkout: `generatedAt` (this script's own invocation
time) and `index.graphResolvedAt` (whenever `cidx resolve` last ran on the
self-index being read). Comparing two reports field-by-field for "did
anything real change" would always see spurious differences there. The
top-level `canonicalHash` is a hash of the report with exactly those two
fields excluded, so two reports over byte-identical checkouts always have
the same `canonicalHash` even when their raw timestamps differ -- use it
(not the raw JSON) for release-gate diffing or reproducibility checks.

## Regenerating the legacy-façade baseline

The `baseline` arrays in `architecture/cidx-self-host-policy.json` are a
point-in-time snapshot of known call sites, not something to hand-maintain.
Regenerate them by running a self-host report with an empty baseline, taking
the resulting `legacyFacadeViolations`, and turning each into a baseline
entry with `callerFile`, `calleeQualName`, `owner`, `rationale`,
`expiresOn`, and `removalIssue` (mirroring
`architecture/cidx-module-manifest.json`'s exception metadata). Never widen
a baseline to cover a *new* call site introduced by the same change that
regenerates it — the whole point is that growth requires an explicit,
reviewed exception.

### Manual/textual-audit fallback

The preferred path above needs a *completed* real self-index run
(`scripts/self_host_index.sh`), which indexes this repository's own ~140
translation units with the real LibTooling engine. On a resource-constrained
or contended machine that run can take a very long time and may not finish
in a practical window. When it doesn't, the baseline may instead be seeded
by a manual/textual audit: grep each facade's `calleeQualNamePrefixes` root
class name (`Storage`, `cidx::graph::GraphQuery`) for direct constructions
in non-exempt-module source files, and record the real `callerFile`/`line`/
`col` read directly off the source, with the same required owner/rationale/
expiresOn/removalIssue metadata as the report-derived path.

This fallback is strictly narrower than a real resolved call graph: it can
only find **direct constructions** (`Storage db(...)`, `graph::GraphQuery
g(...)`) by grep, not member-function calls made through an *already-held*
`Storage`/`GraphQuery` reference (e.g. `db.some_method()` on a reference
received as a parameter) — those require a real resolved call edge to
enumerate soundly and are not claimed to be covered. A baseline entry
captured this way must say so in its facade's top-level `description` (see
the existing `storage-facade`/`graph-query-bypass` entries in
`architecture/cidx-self-host-policy.json`), so a reader never mistakes a
textual audit for a semantically complete one. Replace it with a
report-derived baseline (the preferred path above) once a real self-index
run completes.
