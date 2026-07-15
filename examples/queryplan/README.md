# CXQ QueryPlan DSL examples

Runnable samples for the CXQ QueryPlan tier — the unified declarative query
DSL over a cidx `index.db`. One question = one immutable pipeline:

```
start(<source>) | <stage> | <stage> | ...
```

The same pipeline builder exists in both languages and produces the same
QueryPlan IR (byte-identical canonical JSON), so everything shown here
translates 1:1 between C++ and Python.

- Contract (the normative spec): [`docs/query-plan.md`](../../docs/query-plan.md)
- Usage guide (start here): [`docs/query-dsl.md`](../../docs/query-dsl.md)

## The samples

| Sample | Language | Shows |
|---|---|---|
| [`../../python/examples/08_queryplan_basics.py`](../../python/examples/08_queryplan_basics.py) | Python | build a plan, canonical JSON, run it, `where()` filters, E_* validation errors |
| [`../../python/examples/09_queryplan_advanced.py`](../../python/examples/09_queryplan_advanced.py) | Python | `codebase()` enumeration, the entity view, depth windows, set algebra, budgets/truncation |
| [`queryplan_example.cpp`](queryplan_example.cpp) | C++ | the same surface end-to-end from `src/query/plan.hpp` + `exec.hpp` |

All three default to the repo's checked-in self-index (`index.db` at the repo
root — cidx indexing its own sources), so they run out of the box and their
outputs can be compared line by line.

## Run the Python samples

```bash
# from the repo root
uv run --project python python python/examples/08_queryplan_basics.py
uv run --project python python python/examples/09_queryplan_advanced.py

# against another index
CIDX_DB=/path/to/index.db uv run --project python python python/examples/08_queryplan_basics.py
```

## Build and run the C++ sample

The example target is gated behind `CIDX_BUILD_EXAMPLES` (OFF by default):

```bash
cmake -S . -B build -DCIDX_BUILD_EXAMPLES=ON
cmake --build build -j --target cidx-queryplan-example
./build/cidx-queryplan-example              # uses ./index.db
./build/cidx-queryplan-example /path/to/index.db
```
