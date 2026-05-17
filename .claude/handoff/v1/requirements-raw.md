# cpp-indexer — Raw Requirements Input

Source authoritative documents (read both in full before writing requirements.md):

1. **PRD v1.1 (draft):** `/Users/husam/workspace/wiki/pages/planning/codexgraph-cpp-prd-v1.md`
2. **Engineering plan v1.1 (draft):** `/Users/husam/workspace/wiki/pages/planning/codexgraph-cpp-libclang-rust.md`

## Scope for this dev-team run

**Entire project**, M1 through M7 (per PRD §10 milestones / engineering plan):

- M1 Foundations — Rust crate skeleton, Phase 0 compile-commands parser, Phase 0.5 auto-detect, base-schema Phase 1 libclang visitor, Parquet staging, trivial in-memory Phase 3, **both** Neo4j and IndraDB sinks behind a `GraphSink` trait. Exit: isomorphic graph vs Python reference on 5-file fixture against both sinks.
- M2 C++ extensions — namespaces, templates, headers, overrides. Exit: Boost.Optional fully resolved.
- M3 Performance & scale — parallel ingestion, memory budget, large-repo benchmarks.
- M4 Cross-repo — EXTERNAL_REF edges with USR canonicalisation for system headers; `cxg-resolve-cross-repo` binary.
- M5 Macros & modules — macro expansion handling; C++20 modules per Q3 ADR.
- M6 Agent integration — surface for cpp-mcp / MCP tool consumers (per Q5 ADR boundary).
- M7 Ops + REST API — `cxg-daemon` axum service, `/v1/ingest`, `/v1/jobs/{id}`, `/v1/reset`, `/v1/repos`, `/v1/status`, `/metrics`; bearer-token auth on writes; git2 clone manager; host allowlist.

v1 GA target: 12 weeks solo / 7–8 weeks paired.

## Locked-in decisions (do not re-litigate; see CHARTER.md for full list)

Rust + libclang 18; both Neo4j + IndraDB mandatory in default binary; USR primary key; Parquet staging; auto-detect compile_commands.json; cxg-daemon first-class; binaries `cxg-index` / `cxg-resolve-cross-repo` / `cxg-daemon`.

## Open questions (architect resolves in ADRs)

- Q5 cpp-mcp boundary — **MUST** be resolved before code (blocker). User has authorized architect to write the resolving ADR.
- Q2, Q3, Q4, Q7, Q8 — resolve in ADRs where they impact M1–M7 scope; defer with rationale otherwise.
- Q6 (SSH git auth) — accepted deferred to v2.

## Naming

Public artifacts (repo, README, future docs site) use **cpp-indexer** only. Do NOT use "codexgraph" in any user-facing string. Internal wiki planning docs may keep `codexgraph-cpp-*` slugs.

## Repo

- Local: `/Users/husam/workspace/cpp-indexer/` (git repo, default branch `main`, currently has LICENSE + README + .gitignore only).
- Remote: `https://github.com/husams/cpp-indexer` (public, Apache-2.0).

## Parallelization

Use git worktrees under `/Users/husam/workspace/cpp-indexer/.worktrees/<story-slug>/` on branch `story/<story-slug>` for any `parallel-safe: true` developer stories. See CHARTER.md "Git worktree convention" for the exact commands.
