# Exported counterexamples

`golden/` holds counterexample traces exported from a real, seeded TLC
model-check failure via `../tools/export-counterexample.sh`, in the stable
JSON format documented in that script. Unlike `../generated/`, this
directory is committed: these are durable, reviewable artifacts, not
disposable tool output.

Each file records:

- `model` / `invariant` -- which finite smoke model and named invariant the
  trace violates.
- `states` -- the ordered list of states TLC printed, each with the action
  that produced it (`label`/`action`/`location`) and the exact printed
  variable values (`values`, kept as TLC's own pretty-printed text rather
  than re-parsed into JSON, so the file stays a faithful, human-readable
  record of what the tool actually reported).
- `summary` -- the state-generation/diameter counters TLC reported for that
  run, with `exhaustive: false` always present as an explicit reminder that
  a counterexample proves a defect exists; its *absence* on an unrelated run
  never proves the property holds beyond the explored bound (see
  `../ASSURANCE.md`).

## Regenerating

```bash
spec/tla/tools/export-counterexample.sh --demo --out /tmp/out.json
diff /tmp/out.json spec/tla/counterexamples/golden/trusted-outcome-violation.json
```

Regeneration is deterministic (TLC workers=1, fingerprint=0, seed=1 --
`TOOLCHAIN.md`), so this diff should be empty. Add further golden examples by
following the same mutate-one-module -> run `tools/check.sh` scoped to one
model -> `export-counterexample.sh --from-log` pattern; there is no
auto-discovery of every seed `tools/check-regression.sh` knows about.

## From counterexample to C++ regression

`trusted-outcome-violation.json` is the seed for
`tests/tla_counterexample_regression_test.cpp`: the C++ test carries the same
"assumptions/evidence-empty conditional result" shape into the real
`cidx::protocol::ResultEnvelope::valid()` validation path and asserts it is
rejected there too, then asserts the corresponding "healthy" shape (a
required diagnostic present, mirroring a nonempty assumption set) is
accepted. See that test file's header comment for the exact mapping and its
stated limits -- it is a seeded regression scenario, not a formal
conformance replay.
