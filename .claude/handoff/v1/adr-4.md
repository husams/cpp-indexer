# ADR-4: USR canonicalisation for system headers and vendored copies

Status: accepted
Date: 2026-05-17
Resolves: AC-M4-7, AC-M4-8, cross-repo USR collision risk in plan §Cross-repo design

## Context

USRs are identical when symbols are defined identically across repos. This creates two attribution problems:

1. System-header symbols (`std::vector`, `printf`) appear in every C++ repo's TUs. Without canonicalisation, the first repo to be indexed "owns" them.
2. Vendored copies under `third_party/<pkg>/` are real local code (modified or pinned) and should not be confused with upstream.

Phase 5 must deterministically pick a canonical `REPO` for each USR before materialising `EXTERNAL_REF` edges.

## Decision

Path-rule matcher applied during Phase 5, in declaration order:

### Rule set

1. **System path prefix → `system:<lib>` REPO node**
   - `/usr/include/c++/**`, `/usr/include/x86_64-linux-gnu/c++/**`, `/usr/lib/llvm-*/include/c++/**` → `system:libstdc++`
   - `/usr/include/**` (after the libstdc++ check) → `system:libc`
   - macOS: `/Library/Developer/CommandLineTools/usr/include/c++/**` → `system:libcxx`; `/usr/include/**` → `system:libc`
   - Compiler-internal: any path matching `lib/clang/*/include/**` → `system:clang-builtins`
2. **Vendored prefix → `repo:vendored:<pkg>` REPO node**
   - Path containing a `third_party/<pkg>/` or `vendor/<pkg>/` segment → `repo:vendored:<pkg>`. The user repo and the vendored repo are distinct REPO nodes.
3. **Config override list** in `[cross_repo].canonical_priority` allows promoting a specific user repo for a given path prefix (e.g., pinning `boost::optional` to `repo:boost`).
4. **Definition vs declaration**: if multiple non-system repos contain the same USR and rule 1/2 do not match, the repo where the **definition** lives (cursor kind from libclang) wins. If multiple definitions exist (ODR violation), the lexicographically first `repo_name` wins and a `WARN cxg_odr_violation` diagnostic is logged with the colliding repos.

### Implementation

- `resolve::cross_repo::canonical_repo(usr, candidates) -> RepoRef` runs the rule set above.
- `system:*` and `repo:vendored:<pkg>` REPO nodes are auto-created on first reference; they have `commit_sha = "synthetic"`, `commit_date = NULL`.
- A `VENDORED_FROM` edge from `repo:vendored:<pkg>` to its upstream user repo (if indexed) is **manually** authored — out of scope for v1 automated emission.
- Path rule set lives in `src/resolve/canonicalise.rs` as a static table; modifiable at runtime via `[cross_repo].canonical_path_rules` config override.

## Alternatives considered

- **Hash of the source byte-range as a tie-breaker**: rejected. Source bytes for system headers vary across distros; would produce different "canonical" hashes per build machine.
- **First-indexed-wins**: rejected per scenarios.md edge cases; ordering-dependent results are not acceptable.
- **No canonicalisation, every repo owns its copy**: rejected. Produces N × duplicate `std::*` nodes in the graph, polluting queries.
- **LLM-driven canonical-repo picker**: rejected. Adds a runtime dependency on a model, is non-deterministic, and is unnecessary given the path-rule approach works.

## Consequences

Positive:
- Deterministic, replayable canonicalisation.
- System headers do not pollute user-repo attribution.
- Vendored copies are first-class (distinct REPO nodes), preserving provenance.

Negative:
- Path rules are platform-sensitive (Linux vs macOS); must be tested on both CI runners.
- `WARN cxg_odr_violation` may become noisy on real codebases; logged at WARN level by default with a config knob to suppress.

Follow-ups:
- After M4, sample the diagnostic stream on LLVM + Boost to tune rule order.
- Consider auto-emitting `VENDORED_FROM` edges in v2 when an upstream REPO is detected by a path-similarity heuristic.

Revisit if: a Windows toolchain is added (path rules need MSVC paths) or if ODR violations exceed 1 % of canonicalised symbols on a real codebase.

## References

- requirements.md AC-M4-7, AC-M4-8
- engineering plan v1.1 §Cross-repo design, §Headers shared across repos
- PRD v1.1 §6.2 FR-P4 canonicalisation, US-08, US-09
- Cognee tags: `task:cpp-indexer role:architect`
