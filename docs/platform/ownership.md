# CIDX capability ownership and compatibility matrix

This matrix is the public ownership contract for HSE-60. “C++ authority” means
the semantic source of truth is the C++23 LibTooling implementation. “Python
client/model only” permits Python readers, builders, notebook helpers, and
result models without requiring a second extractor. “Shared generated
contract” is the only row that promises cross-language executor behavior.

| Surface | C++ location | Python location | Authority / promise | Compatibility owner |
|---|---|---|---|---|
| Production source extraction and indexing | `src/ast/`, `src/toolchain/` | `python/indexer/clang/` | C++ authority; Python libclang extractor is a compatibility adapter pending removal | Platform / HSE-60 |
| SQLite schema, migrations, and storage rows | `src/storage/` | `python/indexer/storage.py` | Shared generated/schema contract; both readers preserve the documented migration window | Storage / HSE-60 |
| Graph and entity read queries | `src/graph/` | `python/indexer/query.py`, `entity_graph.py` | C++ authority for product CLI; Python SDK reader/model is supported; no byte-parity promise | Query / HSE-24 |
| QueryPlan/CXQ builders and canonical JSON | `src/query/` | `python/indexer/queryplan.py` | Shared generated contract; canonical JSON is byte-identical and covered by the manifest golden vector | Query / HSE-24 |
| CLI parsing, commands, text, and JSON formatting | `src/cli/` | `python/indexer/cli.py` | C++ authority; Python CLI is a compatibility adapter during migration | Product / HSE-69 |
| Result envelopes, errors, and statuses | `src/query/`, `src/cli/` | `python/indexer/queryplan.py`, query readers | Shared schema contract; status/error codes are stable, prose is not | Platform / HSE-70 |
| Symbol/type/edge/entity catalogs | C++ catalog consumers | Python catalog consumers | Shared generated contract from the HSE-59 catalog source; numeric IDs and names are stable | Catalog / HSE-59 |
| AST graph / per-TU facts | `src/astgraph/` | none | C++ authority; Python may read declared artifacts but does not reimplement the executor | Extraction / HSE-60 |
| Soufflé analyses | `src/cli/analyze.cpp`, `src/astgraph/` | `python/indexer/souffle.py` | Shared declared fact/result contract; each executor is an adapter, not semantic authority | Analysis / HSE-70 |
| Include hygiene | `src/include_hygiene/` | none | C++ authority; Python client may consume artifacts | Refactor / HSE-69 |
| Source/index/config diff | `src/diff/` | none | C++ authority; JSON report schema is an artifact contract | Product / HSE-70 |
| Storage-backed models and notebook helpers | `src/storage/` as producer | `python/indexer/model.py`, `python/examples/` | Python-only developer/SDK convenience over declared read contracts | SDK / HSE-60 |
| Release/version metadata | generated `src/cli/version.hpp` | generated `python/indexer/_version.py` | One source: `spec/platform/version.json`; generated outputs must pass the release gate | Release / HSE-60 |

The retired Rust, Neo4j, IndraDB, daemon, and libclang C API surfaces are not
compatibility targets. They must not reappear as alternate implementations.

## Rules for new work

1. Add a matrix row before adding a public C++ or Python surface.
2. A new indexing or semantic rule belongs in C++ unless the row explicitly
   says `shared generated contract with dual executor`.
3. A Python feature may be a client/model/notebook convenience when it only
   reads a declared artifact or QueryPlan contract.
4. Any schema, catalog, artifact, API, or CLI compatibility change requires a
   migration entry, a golden/schema update, and an owner review.
