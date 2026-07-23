# Adding a CIDX module or dependency

HSE-58 and ADR-011 define the repository contract. Start with the manifest and
run the bootstrap gate before editing code:

```bash
python3 scripts/check_architecture.py \
  --manifest architecture/cidx-module-manifest.json
python3 tests/architecture_test.py
```

## New source module

Choose exactly one owner and layer for every new production source. Add a
non-overlapping `paths` rule to `architecture/cidx-module-manifest.json`, or an
explicit path entry for a small contract file. Declare only downward dependencies.
If the source needs Clang/LLVM, SQLite, CLI, filesystem, or process APIs, place that
code in the corresponding adapter/product module rather than leaking the include
into a model or port.

## New dependency edge

Prefer an existing port or standard-library facility. If the edge is part of the
target architecture, add it to the owning module's `allowedDependencies` and cover
it with a positive test. If it is transitional, add an exception with all of:

- owner;
- affected boundary (`from -> to` and the source path);
- rationale;
- expiry date;
- Linear removal issue.

An exception is a tracked migration item and must not be reused for an unrelated
new edge. Expired exceptions fail the gate.

## New CMake target or link

Assign every production `add_library`, `add_executable`, and custom target in the
manifest. Keep Clang/LLVM object-library isolation and SQLite ownership explicit.
Record target links in the manifest so a target change cannot silently widen the
build graph.

## Required checks and review note

Run the architecture checker and mutation suite, then the relevant C++ default/
Clang tests and Python tests. In the change description, state the layer owner,
new edges, adapter boundary, compatibility impact, and exact checks. A new public
query/result surface must extend QueryPlan/CXQ rather than introduce an independent
SQL or command-specific semantic path.
