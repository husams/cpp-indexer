# Trusted-assumption register

This register is normative policy for what the current TLA+ foundation does
not prove. Each assumption is explicit, scoped, and reviewable.

| ID | Trusted assumption | Scope | Required review |
| --- | --- | --- | --- |
| TA-001 | Java 17 and the pinned TLA+ tools execute SANY/TLC according to their released semantics. | `spec/tla/tools/check.sh` and CI | Any toolchain pin or checker change |
| TA-002 | The model's finite identifier sets stand for the selected workspace snapshot. | `.cfg` smoke models | Any model-size or constant change |
| TA-003 | A repository/source/configuration discovery pipeline supplies the abstract records consumed by later models. | `CidxTypes` and future indexing models | Any domain or identity change |
| TA-004 | Compiler, filesystem, operating-system, and external-library behavior is not inferred from this M0 foundation. | All current modules | Any new implementation-facing axiom |
| TA-005 | A successful smoke model is evidence about the model only, not proof that C++/Python/SQLite conform. | All smoke models | Any conformance claim or new adapter |

Do not add an assumption to make a failed invariant pass. First decide whether
the behavior belongs in the normative model, then record only the remaining
external dependency with a bounded scope and named review owner.
