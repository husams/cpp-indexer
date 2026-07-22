# cidx BDD end-to-end suite

Black-box tests for the whole indexing pipeline: index small C++ fixtures with
the **`cidx` command line only**, then assert the result through the **Python
graph-query API**. No Python indexing, no direct `Storage` writes, no in-process
libclang.

```
tests/e2e/
  features/     Gherkin specs -- the expected symbols, edges and signature facts
  fixtures/     the C++ inputs, one translation unit per feature
  steps/        step definitions and their helpers, one module per concern
  conftest.py   the two pytest fixtures (cidx_bin, workspace) and nothing else
  test_*.py     one thin binder per feature file
```

`conftest.py` star-imports `steps`, which is what registers the step
definitions with pytest-bdd. Inside `steps/`:

| module              | holds                                                  |
| ------------------- | ------------------------------------------------------ |
| `pipeline_steps.py` | `Given` the fixture workspace, `When` the CLI runs      |
| `index_steps.py`    | database, resolve pass, file/symbol/unresolved totals   |
| `symbol_steps.py`   | symbol rows: membership, exhaustiveness, spans          |
| `signature_steps.py`| types, parameters, templates, instantiations, definitions |
| `edge_steps.py`     | edge counts, relationship tables, per-kind totals       |
| `edge_site_steps.py`| edge sites -- where a relationship is written           |
| `callgraph_steps.py`| callers and callees                                     |
| `cli_steps.py`      | CLI output and read-only `cidx` subcommands             |
| `paths.py`          | filesystem anchors; puts `python/` on `sys.path`        |
| `workspace.py`      | the per-scenario `Workspace`: CLI runner + query handle |
| `tables.py`         | Gherkin table parsing and row-list comparison           |
| `symbol_facts.py`   | the comparable view of a symbol, and row matching       |
| `edge_facts.py`     | rendering and lookup helpers for edges and sites        |

Add a new step to the module that owns its concern -- never to `conftest.py`,
and never by growing one catch-all step file.

## Running

```bash
cmake -S . -B build && cmake --build build -j        # the CLI under test
uv run --project python pytest tests/e2e             # or: pytest tests/e2e
```

Set `CIDX_BIN` to test a binary other than `build/cidx`. Without a usable
binary the suite fails immediately so a missing build cannot produce a false-green run.

## What each scenario does

Every scenario gets a **fresh, isolated `index.db`** under its own `tmp_path`,
built by the same four commands a user runs:

```
cidx init
cidx import --db <workspace> --name fixture
cidx index
cidx resolve
```

`INDEXER_CACHE` points at the scenario's private cache directory, so no
scenario can see another's rows and nothing touches the repository's committed
`index.db`.

Assertions then go through `indexer.query.GraphQuery`: symbol rows, the
signature/type tier (`signature`, `template_params`, `template_args`,
`template_of`, `instantiations`), the edge graph (`edges_out` with sites), the
call graph (`callers` / `callees`) and `definitions`.

## Where the expectations live

**In the feature files, never in Python.** The step modules know only how to
compare; they never hard-code what a fixture should contain. A scenario states
its expectation as a Gherkin data table:

```gherkin
  Scenario: The signature tier records the return type and both parameters
    Then symbol "add" returns "int"
    And symbol "add" takes the parameters:
      | position | name | type |
      | 0        | a    | int  |
      | 1        | b    | int  |
```

### Table cell conventions

| cell            | means                    |
| --------------- | ------------------------ |
| `true` / `false`| boolean                  |
| `-` or empty    | `None` (absent / not set)|
| `123`           | integer                  |
| anything else   | string                   |

### Symbol selectors

The quoted name in a scalar step, and the `src`/`dst` cells of an edge table:

| selector                | resolves to                                       |
| ----------------------- | ------------------------------------------------- |
| `add`                   | qualified name, else spelling; must be unique      |
| `add@11`                | ... narrowed to the symbol declared on line 11     |
| `add@11:1`              | ... narrowed to line 11, column 1                  |
| `usr:c:@F@add<#d>#d#d#` | exact USR -- always unambiguous                    |

Qualified name beats spelling, so `PointClass` is the class and not its
constructor. A function or method's qualified name carries its full signature
(parameter types plus any `const`/ref qualifiers), so overloads stay distinct:
`PointClass::getX() const`, `PointClass::setX(int)`. The bare spelling
`getValue` still names three different symbols (the pattern member, the `int`
member and the `double` member), which the qualified names
`MyClass::getValue() const`, `MyClass<int>::getValue() const` and
`MyClass<double>::getValue() const` — or a USR selector — disambiguate.

## Step vocabulary

**Given / When**

- `Given a clean index workspace for fixture "<file>.cpp"`
- `When I build the index with the cidx CLI`

**Index-level**

- `the index database exists`
- `the entity graph is resolved`
- `the index holds <n> indexed file(s)`
- `the index holds exactly <n> symbols` / `... <n> edges`
- `the index holds no edges` / `the index holds no <kind> edges`
- `the CLI output contains "<text>"`

**Symbols**

- `the index holds the symbols:` -- each row must match exactly one symbol
- `the index holds exactly these symbols:` -- and the table must cover them all
- `symbol "<sel>" spans lines <a> to <b>`
- `symbol "<sel>" returns "<type>"` / `has type "<type>"`
- `symbol "<sel>" takes the parameters:` / `takes no parameters`
- `symbol "<sel>" declares the template parameters:`
- `symbol "<sel>" binds the template arguments:` / `binds no template arguments`
- `symbol "<sel>" is an instantiation of "<sel>"`
- `symbol "<sel>" has <n> instantiation(s)`
- `symbol "<sel>" has the definitions:`

Symbol table columns: `usr`, `spelling`, `qual_name`, `kind`, `type_info`,
`file`, `line`, `col`, `end_line`, `end_col`, `access`, `is_definition`,
`is_instantiation`, `is_static`, `is_pure`, `is_stub`.

**Edges and the call graph**

- `the index holds the edges:` / `the index holds exactly these edges:`
  (columns `src | kind | dst | count | sites`, sites as `line:col`)
- `the edge kind totals are:` (columns `kind | total`, exhaustive)
- `symbol "<sel>" calls:` / `is called by:` / `is called by nothing`

## Adding a fixture

1. Drop the `.cpp` in `fixtures/`.
2. Write `features/<name>.feature` stating what you expect.
3. Add `test_<name>.py` with `scenarios("features/<name>.feature")`.

Derive the expected values from the indexer's actual output only after reading
it critically -- a table copied blindly from a buggy run locks the bug in.
