# TLA+ specification policy

## Naming and dependency rules

- Normative modules use the `Cidx*.tla` prefix and PascalCase operator names.
- A module has one responsibility and imports lower-level vocabulary only.
- Shared domains and statuses live only in `modules/CidxTypes.tla`.
- Constants describe model inputs; finite values and model sizes belong in
  `.cfg` files and must not be hidden in implementation modules.
- Actions use verb phrases (`InitializeRepository`, `MarkUnknown`) and are
  composed into `Next`. A module exposes `Init`, `Next`, and `Spec` when it is
  a complete behavior model.
- Safety claims are named `*Invariant`; liveness obligations are named
  `Fairness` or `*Liveness`. Invariants must be listed explicitly in each
  model config.
- Fairness is never implied by a successful finite state exploration. If an
  action requires fairness, the formula must be explicit and checked as a
  `PROPERTY`.

## Model-size and determinism rules

- Smoke models use finite singleton or otherwise explicitly bounded domains.
- TLC runs with one worker and a fixed fingerprint polynomial.
- Model configs are checked in beside the model they parameterize.
- A checker reports stable model and invariant names; raw TLC logs are
  diagnostic output, not a machine-readable contract.
- A timeout, parser error, tool download/hash error, or model violation fails
  the TLA+ gate. None may be reported as implementation test success.

## Normative, implementation, and assumption policy

Normative TLA+ files define behavior and reviewable contracts. C++/Python code,
SQLite schema, generated modules, model-checker metadata, and CI plumbing are
implementation details unless a future module explicitly specifies their
observable behavior. A passing model is not conformance evidence by itself.

External facts that the model needs but does not prove—compiler behavior,
filesystem durability, Java/TLC correctness, complete source discovery, and
closed-world boundaries—belong in the trusted-assumption register. They must be
named, scoped, and reviewed rather than silently encoded as axioms.

## Protected files and review ownership

AI-generated implementation changes may consume this contract but may not
weaken, delete, or rewrite a protected invariant or trusted assumption without
explicit human review. The repository owner rules below are the minimum review
boundary; branch protection must require those owners for changes under these
paths.

The following paths are protected human-authored inputs:

- `spec/tla/protected/`
- `spec/tla/trusted/`
- `spec/tla/modules/CidxTypes.tla`
- `spec/tla/models/*.cfg`

Generators may write only below `spec/tla/generated/`. They must not replace,
delete, or rewrite protected or trusted files. Changes to protected files
require the explicit review owner in `.github/CODEOWNERS` and a review of the
invariant/assumption diff. CI must run the checker from the committed tree so a
generated file cannot silently redefine the checked-in policy.
