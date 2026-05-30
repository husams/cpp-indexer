# Agent Playbook for cpp-indexer

This document provides AI coding assistants (like Claude, Gemini, etc.) with the architectural map, development workflows, testing patterns, and operating constraints for working within the `cpp-indexer` codebase.

---

## 1. Codebase Architecture Map

`cpp-indexer` is a Rust + libclang 18 code indexer that outputs code structure to graph databases (Neo4j or IndraDB). It is split into a library (`cpp_indexer`) and three primary binaries.

### Crate Layout
* **`src/bin/`**: Core entry points:
  * [`index.rs`](file:///Users/husam/workspace/cpp-indexer/src/bin/index.rs): `cxg-index` CLI for one-shot indexing of local codebases.
  * [`resolve_cross_repo.rs`](file:///Users/husam/workspace/cpp-indexer/src/bin/resolve_cross_repo.rs): `cxg-resolve-cross-repo` CLI to link dependencies across different indexed repositories.
  * [`daemon.rs`](file:///Users/husam/workspace/cpp-indexer/src/bin/daemon.rs): `cxg-daemon` REST control plane.
* **`src/schema/`**: Defines the node and edge definitions, `SCHEMA_VERSION` (currently `v5`), and Arrow data structures.
* **`src/bootstrap/`**: Handles discovery and sanitization of `compile_commands.json` arguments (crucial for filtering driver tokens and `-c`/`-o` file flags).
* **`src/visit/`**: Coordinates the AST traversal using `libclang`. Supports Best-Effort C++20 module parsing.
* **`src/stage/`**: Stages extracted AST nodes/edges to local Parquet files per parallel thread, avoiding lock contention.
* **`src/resolve/`**: Resolves symbol references within the repo (Phase 3) and implements cross-repo resolution (Phase 5).
* **`src/sink/`**: Implements `GraphSink` interface with `Neo4jSink` (using `neo4rs` batch `UNWIND`) and `IndraDbSink` (using gRPC).
* **`src/api/`**: Axum REST endpoints, Prometheus metrics, and job queue management.
* **`prompt/`**: Contains the MCP schema definition and idiom query examples.

---

## 2. Graph Schema & Agent Guidance

The graph schema is built to support the downstream translation of natural language queries to database queries (e.g., Cypher).

* **Schema Definition**: [`prompt/graph_database/cpp/schema.txt`](file:///Users/husam/workspace/cpp-indexer/prompt/graph_database/cpp/schema.txt) is automatically updated by `build.rs` at compilation time. Do NOT edit it manually.
* **Idiom examples**: [`prompt/graph_database/cpp/example.txt`](file:///Users/husam/workspace/cpp-indexer/prompt/graph_database/cpp/example.txt) contains canonical Cypher queries for class inheritance, template instantiation, virtual method override chains, include graphs, macros, and cross-repo lookups. Refer to this file when writing new Cypher translation logic.

---

## 3. Development & Testing Commands

### Prerequisites
Working with this project requires **libclang 18**.
* Ensure `LIBCLANG_PATH` points to the LLVM 18 libraries (e.g. `/usr/lib/llvm-18/lib` on Linux, `/opt/homebrew/opt/llvm@18/lib` on macOS).

### Testing Gates
Run these verification gates in order before proposing commits or changes:

```bash
# Gate 1: Format check
cargo fmt --all -- --check

# Gate 2: Clippy Lints (requires LIBCLANG_PATH and DYLD_LIBRARY_PATH on macOS)
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib \
DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
  cargo clippy --all-targets --all-features -- -D warnings

# Gate 3: Build targets
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib \
DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
  cargo build --all-targets --all-features

# Gate 4: Test suite (must include test-mock feature for benchmark compile)
LIBCLANG_PATH=/Library/Developer/CommandLineTools/usr/lib \
DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib \
  cargo nextest run --lib --tests --features test-mock
```

> [!NOTE]
> Integration tests that depend on a live database are ignored by default. To run them, spin up target databases using `tests/compose/` and set `NEO4J_URI` / `NEO4J_PASSWORD` or `INDRADB_ENDPOINT`.

---

## 4. Key Constraints & Operational Rules

* **No Secret Exposure**: Credentials must never be written to configuration files or exposed in commands. Use indirect configuration mapping (e.g., `password_env` config fields naming the environment variable).
* **Exit Code Protocol**:
  * For CLI tools (`cxg-index`), exit code `0` is for successful ingestion. Exit code `2` indicates that compilation/parsing errors exceeded the limit configured via `--fail-on-tu-error <RATIO|never>` (default `1.0`).
  * Exit code `2` is also used for CLI parse failures (from `clap`).
* **REST API Errors**: All daemon control-plane errors must return RFC-7807 `application/problem+json` formatted responses.
* **C++20 Modules**: Handle Best-Effort skip-with-warning when libclang doesn't support target modules.
