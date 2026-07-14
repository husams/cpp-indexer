# Canonical `template_arg.arg_kind` contract

Status: adopted (Phase 0 of `refactoring.md`)
Applies to: every writer of the `template_arg` table under `src/ast/`.

## The contract

The storage schema already declares the read-side contract, in both
`src/storage/storage.cpp` and `python/indexer/storage.py`:

```
arg_kind INTEGER NOT NULL,  -- 1=type 2=non-type value 3=template-template 4=pack
```

and `python/indexer/query.py` exposes exactly those four codes
(`TEMPLATE_PARAM_KINDS = {1: "type", 2: "non-type", 3: "template-template",
4: "pack"}`), shared with `template_param.param_kind`. That mapping is the
canonical contract. Every `clang::TemplateArgument::ArgKind` maps as follows,
on **every** extraction path:

| `clang::TemplateArgument::ArgKind` | `arg_kind` | `literal` | `ref_id` |
|---|---|---|---|
| `Null` | no row emitted (unfilled slot) | — | — |
| `Type` | 1 | type spelling via the single `PrintingPolicy` | USR of the typed `TagDecl`, when indexed |
| `Declaration` | 2 | NULL | NULL |
| `NullPtr` | 2 | NULL | NULL |
| `Integral` | 2 | value text (`llvm_compat` integral_to_string) | NULL |
| `StructuralValue` | 2 | NULL | NULL |
| `Template` | 3 | NULL | NULL |
| `TemplateExpansion` | 3 | NULL | NULL |
| `Expression` | 2 | NULL | NULL |
| `Pack` | 4 | NULL (elements not expanded) | NULL |

Rules:

- The switch over `ArgKind` must be exhaustive with no `default:` so a Clang
  upgrade that adds a kind fails the build instead of silently mis-storing.
- `ref_id` resolution is typed only (`TagDecl` → USR → symbol id). The
  string-spelling fallback (`TemplateArgResolver::resolve` over `base_name`)
  is removed in Phase 3.
- Paths that today emit Type-only rows (`instance_minter.cpp`,
  `instantiation_edges.cpp`) and the method-call-site path that skips
  Declaration/NullPtr/Template kinds (`call_template_args.cpp` `emit_arg`)
  emit full-kind rows per this table. The new rows are a reviewed golden
  delta.

## Divergence being corrected (pre-Phase-3 state)

| Path | Type | Integral | Decl | NullPtr | Template | TmplExpansion | Expr | Pack | Null |
|---|---|---|---|---|---|---|---|---|---|
| `edge_visitor.cpp` (class-spec edges) | 1 | 2 | 2 | **3** | **5** | **6** | **7** | **8** | **0** |
| `call_template_args.cpp` free-function | 1 | 2 | 2 | 2 | 3 | 3 | 2 | 4 | skip |
| `call_template_args.cpp` method site | 1 | 2 | skip | skip | skip | skip | skip | 4 | skip |
| `instance_minter.cpp` | 1 | skip | skip | skip | skip | skip | skip | skip | skip |
| `instantiation_edges.cpp` | 1 | skip | skip | skip | skip | skip | skip | skip | skip |

Only the class-spec path deviates from the declared schema meaning: it stores
raw libclang `CXTemplateArgumentKind` values in its non-Type/Integral branch.
Values 5, 6, 7, 8 and 0 are out-of-contract; its 3 means NullPtr while the
contract's 3 means template-template.

## Schema decision: bump 28 → 29 (Phase 3)

Normalizing the class-spec path changes the persisted meaning of existing
rows, so per `refactoring.md` this is an on-disk semantic change:

- bump `schema_version` 28 → 29 in `src/storage/storage.cpp` and
  `python/indexer/storage.py` together;
- migration for existing databases. Legacy out-of-contract values were only
  ever written for **class/struct-kind owners** (the `edge_visitor.cpp` path);
  callable owners were always in-contract. The migration therefore
  disambiguates by owner symbol kind:
  - owner is a record-like symbol: `5→3`, `6→3`, `7→2`, `8→4`, `3→2`
    (legacy NullPtr), delete `arg_kind = 0` rows;
  - owner is a callable symbol: rows are already in-contract, untouched;
- old-database tests covering both owner classes;
- reindex recommended (migration preserves meaning but not the newly emitted
  full-kind rows); refresh the committed `index.db`.
