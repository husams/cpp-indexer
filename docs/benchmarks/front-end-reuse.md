# S-075 front-end reuse qualification

The qualification contract is intentionally reproducible and keeps raw output
outside the checkout. The C++ setup probe is built and run with:

```sh
cmake -S . -B build
cmake --build build --target front_end_reuse_probe -j
ctest --test-dir build -R front_end_reuse_probe_test --output-on-failure
```

It retains three trials for each concrete path: the no-reuse syntax-only
control, an LLVM-driver-generated umbrella PCH loaded with `-include-pch`, and
fresh `ASTUnit` construction. The generated PCH path records setup/load time
and its explicit builtin-module compatibility failures; the ASTUnit path
records successful isolated construction without retaining a reusable owner.
Neither rejected candidate is presented as shipped acceleration.

The production corpus harness records the qualification matrix and its
three-trial fields in the report:

```sh
python3 benchmarks/indexing/production.py --help
python3 -m unittest benchmarks.indexing.test_frontend_reuse_qualification
```

The report contract retains wall time, CPU time, peak RSS, exclusive Clang
front-end time, disk use, semantic/diagnostic/catalog/integrity parity, and the
versioned `front-end-reuse/v1` none identity. `--front-end-reuse none` is the
only shipped choice; `--no-front-end-reuse` makes the control explicit for
diagnosis.

The evidence-supported outcome is `do-not-ship`: the no-reuse control is
semantically stable, while neither acceleration candidate has qualified the
required configuration identity, artifact integrity, compatibility, and
worker-failure guarantees. A future attempt must update ADR-014 and retain
the same S-076-consumable identity before claiming a benefit.
