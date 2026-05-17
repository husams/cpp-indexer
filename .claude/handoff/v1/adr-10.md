# ADR-10: Build-config handling (Q2) and multi-tenant daemon (Q7) for v1

Status: accepted
Date: 2026-05-17
Resolves: PRD Q2, PRD Q7

## Context

Two open questions affect M3 and M7 scope but do not block the demo gates:

- **Q2 (build config handling)**: should `(Debug, -DFOO)` and `(Release, -DBAR)` produce one merged graph with config-tagged edges, or separate graphs per `(profile, defines)` tuple?
- **Q7 (multi-tenant daemon)**: should v1 cxg-daemon support multiple teams / users with isolated workspaces, auth tokens, and sink configs, or stay single-tenant?

The PRD's default (per CHARTER) is "separate graphs per config" and "single-tenant for v1". This ADR confirms those defaults with explicit rationale and Revisit-if conditions so the decision is durable rather than implicit.

## Decision

### Q2 — Separate graph per `(build_profile, defines_hash)` tuple

- Each `cxg-index` invocation writes to a **single logical graph** keyed by `(repo_name, build_profile, defines_hash)` where:
  - `build_profile` is taken from `[index].build_profile` config (default `"default"`; user sets `"debug"`, `"release"`, etc.).
  - `defines_hash` is `blake3(sorted(args matching '-D*'))[..8]`.
- The REPO node's `name` is `<repo_name>` and a separate attribute `config_tag = "<profile>:<defines_hash>"` distinguishes config variants.
- Multi-config queries are out of scope for v1. If a user wants to query across configs, they query each REPO independently and union results in the agent layer.
- Phase 5 cross-repo resolution treats each `(repo_name, config_tag)` as an independent REPO for `EXTERNAL_REF` purposes.

### Q7 — Single-tenant daemon for v1

- One `cxg-daemon` process = one bearer token, one workspace dir, one sink config, one shared job queue.
- Multi-tenancy (per-team isolation) is **explicitly out of scope** for v1 GA.
- Operators needing multi-team isolation run multiple `cxg-daemon` processes on different ports with different configs. Acceptable because the daemon is light (one libclang pool, one tokio runtime).

## Alternatives considered

### Q2 alternatives
- **Merged graph with config-tagged edges**: rejected for v1. Edge cardinality multiplies by N configs; query authors must always filter by config which is easy to forget; no requested user story actually needs cross-config queries. Revisit only when a user does.
- **One graph period, last-write-wins**: rejected. Silently loses the other config's data; debugging-hostile.

### Q7 alternatives
- **Multi-tenant via per-request workspace selection**: rejected. Requires a tenant-ID concept, per-tenant token store, per-tenant sink credentials, per-tenant resource quotas. Each of those is a feature in its own right and would push M7 out by 3–4 weeks.
- **Multi-tenant via Kubernetes Deployment per tenant**: viable today already (one daemon per Deployment). This is the recommended pattern for ops that need isolation; no code change required.

## Consequences

### Q2
Positive:
- Simple to reason about: one binary invocation → one graph.
- No silent ambiguity about which config a query is hitting.
- Phase 5 logic does not need a config-axis tie-breaker.

Negative:
- Disk usage scales with config count (each config has its own Parquet shards + DB nodes).
- A user wanting "find all uses of `Foo` across debug and release" must run two queries.

### Q7
Positive:
- Single-tenant is the simplest auth model: one token, one workspace, one allowlist.
- No tenant-isolation security audit needed for v1.
- Operators retain isolation by running N daemons (proven pattern).

Negative:
- Operators who want one daemon serving many teams must wait for v2 or use per-tenant Deployments.

## Follow-ups

- Q2: track requests for cross-config queries; if ≥2 users ask, design a v2 merged-graph mode with explicit config dimension.
- Q7: design a tenant model (token store, per-tenant workspace, per-tenant rate limit) only after at least one operator presents a concrete multi-team scenario.

Revisit if:
- (Q2) A user story requires cross-config queries inside a single graph.
- (Q7) Operating multiple daemons per host becomes burdensome (e.g., >5 daemons on one box) or a multi-tenant audit requirement appears.

## References

- PRD v1.1 Q2, Q7
- requirements.md story M3-S1 (Q2 reference), story M7-S1 (Q7 reference)
- engineering plan v1.1 §M5 Build configuration handling; §M7
- Cognee tags: `task:cpp-indexer role:architect`
