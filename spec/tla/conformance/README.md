# CIDX conformance strategy

The TLA+ modules define abstract actions and observable state. An implementation
adapter records CIDX operation boundaries, maps each boundary to the
`specAction` in `operation-map.json`, and emits only fields listed in
`observation-map.json`. It then checks the observed state transition against the
enabled action and named invariants for the selected model configuration.

The adapter must not import C++ types, SQLite tables, traversal order, or file
formats into the specification. Those details are translated at the boundary.
Unknown, partial, stale, ambiguous, and failed observations remain explicit;
the adapter may not coerce them into `complete` or `current`.

`scenarios.json` is the deterministic interchange format for TLC counterexample
traces and conformance regressions. Each scenario contains a complete final
observation record. `tools/check-conformance.sh` generates a replay config for
every scenario, constrains the model's nondeterministic choices to that record,
and runs SANY/TLC with the pinned deterministic worker/fingerprint/seed. An
impossible action order, invalid observation value, or mismatched final state
fails the gate. Action names are canonical, scenario IDs are sorted, and every
trace is short enough for the bounded model. A counterexample exporter can
append the observed fields to one of these action sequences and run the same
adapter without inventing a second scenario vocabulary.
