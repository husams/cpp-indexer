# The ExtractionPlan IR (HSE-64)

## Purpose

`ExtractionPlan` is the stable, versioned compatibility surface for CIDX's
declarative AST extraction DSL: a way to recognize project-specific C++
constructs and emit custom, source-backed extension facts without writing a
new C++ visitor or loading arbitrary native code into the indexer.

```text
text/builder
  -> parse
  -> matcher + emission type checking     (validate_matchers)
  -> safety/budget validation             (validate_structure)
  -> execute against a pinned FrontendSession   (execute_plan)
  -> immutable extension fact/evidence artifact (ExtensionFactSink)
```

The IR itself — not any particular textual syntax or builder API — is the
stable contract. Canonical JSON and a rule's matcher-expression grammar may
each evolve; see [ADR-013](adr/ADR-013-extraction-plan-matcher-vocabulary.md)
for why the matcher grammar is Clang's own dynamic AST matcher expression
language rather than a bespoke CIDX parser.

C++ types: `src/extract/plan_ir.hpp` (`cidx::extract` namespace). JSON Schema:
[`spec/contracts/extraction-plan.schema.json`](../spec/contracts/extraction-plan.schema.json).

## Shape

```json
{
  "schema_version": 1,
  "plan_id": "banking.audit.plan",
  "plan_version": 1,
  "catalog_versions": [1],
  "rules": [
    {
      "id": "audit.logs_to",
      "version": 1,
      "matcher_expression": "callExpr(callee(functionDecl(hasName(\"log_audit_event\")).bind(\"callee\"))).bind(\"call\")",
      "bindings": [
        {"name": "call", "domain": "expression"},
        {"name": "callee", "domain": "declaration"}
      ],
      "emits": [
        {"relation": {"namespace": "audit", "relation_kind": "logs_to",
                     "from_binding": "call", "to_binding": "callee",
                     "with_evidence": true}}
      ],
      "scope": "main_file",
      "traversal": "as_is",
      "completeness": "complete",
      "budget": {"max_matches": 1000, "max_emitted_facts": 1000,
                "max_visited_nodes": 100000, "declared": true},
      "producer_package": "banking.audit",
      "producer_version": 1
    }
  ]
}
```

Field reference:

| Field | Meaning |
| --- | --- |
| `schema_version` | Must equal the compiled `kExtractionPlanSchemaVersion`. |
| `rule.matcher_expression` | Clang dynamic AST matcher expression (ADR-013); parsed, never executed, during validation. |
| `rule.bindings[].domain` | `declaration \| expression \| type \| custom_node` — the endpoint type validation checks every emit operation's use of the binding against. |
| `rule.scope` | `main_file \| translation_unit \| workspace`. `workspace` gets a tighter budget ceiling (unbounded-scope check). |
| `rule.traversal` | `as_is` (every AST node including compiler-generated ones) or `ignore_unless_spelled` (skips nodes with no direct source spelling — implicit constructors/conversions, some template-instantiation-only nodes). The two modes are required to disagree on implicit/template constructs. |
| `rule.completeness` | `complete \| partial \| unknown_capable` — a DECLARED applicability, never a promise every construct is covered; see "Completeness and provenance" below. |
| `rule.budget` | Every rule must declare non-zero, bounded `max_matches` / `max_emitted_facts` / `max_visited_nodes`; `declared` must be `true`. Validation rejects a missing or excessive budget before any Clang execution. |
| `emits[].node/relation/attribute/unknown` | Exactly one payload per emit operation. `node` mints a namespaced custom node with an `IdentityRecipe`; `relation` connects two bindings; `attribute` reads one allow-listed typed AST property; `unknown` records an explicit "recognized but unclassifiable" finding. |
| `identity.kind` | `usr \| source_anchor \| owner_position \| type_key \| composed`. There is deliberately no "AST pointer" kind — process-local Clang handles can never become a fact's identity. |

## Validation (before Clang execution)

`validate(plan)` = `validate_structure(plan)` (Clang-free: schema version,
budgets, scope bounds, binding references, identity-recipe well-formedness,
a forbidden-capability text scan) merged with `validate_matchers(plan)`
(constructs — never executes — each rule's matcher via Clang's dynamic
parser, then checks the CIDX allow-list and property/domain catalog,
`src/extract/matcher_catalog.hpp`). `execute_plan()` re-validates internally
and throws `PlanNotValidated` rather than ever running Clang against a plan
with validation errors — see `tests/extraction_engine_test.cpp`, case
"execute_plan refuses to run a plan that fails validation and emits
nothing".

Validation error codes: `malformed_plan`, `unsupported_schema_version`,
`unknown_matcher`, `unknown_property`, `invalid_binding`,
`endpoint_type_mismatch`, `unstable_identity`, `unbounded_scope`,
`excessive_budget`, `forbidden_capability`.

## No arbitrary code execution

A rule cannot execute SQL, a shell command, Python, a shared library, or a
user C++ callback. This is structural, not only a validation-time check: the
IR has no field that can name an executable, interpreter, or shared library,
`ExtractionRule.matcher_expression` is parsed by Clang's own registry (which
performs no I/O), and every bound node is converted to a safe identity
primitive before an emit operation runs — the engine never serializes or
stores a process-local AST pointer. `validate_structure()` additionally scans
every free-text field for the same forbidden tokens the HSE-65 package SDK
rejects (`sql`, `executor`, `shell`, `python`, `shared_library`,
`native_plugin`) as a belt-and-braces check.

## Determinism and identity

`plan_hash(plan) = "sha256:" + sha256(canonical_json(plan))`
(`src/extract/plan_identity.hpp`). Canonical JSON has a fixed field order and
no floating point, so two plans are identical iff their hashes are. Changing
a rule's matcher expression, bindings, emits, scope, traversal mode, budgets,
catalog versions, or producer/package identity changes the canonical JSON and
therefore the plan hash (`tests/extraction_plan_test.cpp`, the "plan hash
changes when ..." cases).

Re-running an unchanged plan against an unchanged translation unit produces a
byte-identical canonical fact batch: `ExtensionFactSink`'s in-memory recorder
sorts and dedups each record family (`InMemoryExtensionFactSink::
canonicalize()`), so Clang's traversal order never leaks into the artifact
(`tests/extraction_engine_test.cpp`, "re-running an unchanged plan ...").

## Extension facts vs. core facts

Every extension fact (`src/extract/extension_facts.hpp`) carries an
`ExtensionProvenance`: the producing plan hash, rule id, producer
package/version, and the rule's declared completeness. Extension facts are
never written into core symbol/edge tables and are always distinguishable
from core static facts, inferred analyses, runtime evidence, or proofs by
this provenance envelope. An omitted matcher case or an incomplete rule can
mark its own output `partial`/`unknown_capable` and emit explicit `unknown`
findings, but it can never claim `complete` core coverage — completeness is a
property the rule author declares and the engine copies onto every fact it
emits, not something the engine infers from what it happened to match.

## Example corpus

`tests/extraction_engine_test.cpp` is both the conformance suite and the
worked example corpus for this iteration:

- **Logging boundary** (`audit.logs_to`): a positive call site is matched and
  emitted as one `audit/logs_to` relation with source evidence; a near-miss
  call to a similarly-named function is silently absent (no false match).
- **Unclassifiable dispatch** (`audit.unclassified_dispatch`,
  `unknown_capable`): demonstrates an explicit `unknown` finding, distinct
  from silent absence, for a recognized-but-unclassifiable construct.
- **Banking application-state boundary** (`banking.appstate.boundary` +
  `banking.appstate.unverifiable_registration`): a class with both `commit`
  and `rollback` methods is recognized structurally as a boundary; a
  near-miss class with only `commit` is absent; a class using an
  unverifiable runtime-registration pattern is reported `unknown` rather than
  guessed at.

Deferred for a follow-up iteration: additional worked categories (framework
annotations, repository/DAO wrappers, callback registration, lock-call
conventions beyond the demonstrated relation/unknown mechanics), a standalone
`cidx extract validate/explain/preview` CLI surface, and continuous
libFuzzer-style fuzzing (`tests/extraction_plan_test.cpp`'s parser-fuzz case
is a deterministic 500-iteration seeded-mutation proxy, not a continuous
fuzzer). None of these affect the IR, validation, execution, or identity
contract documented above.
