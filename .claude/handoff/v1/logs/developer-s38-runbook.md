# Developer Session Log — S38-runbook

## Skills loaded
- rust-conventions

## Skills considered but not loaded
- cpp-conventions — no C++ files in scope (docs-only story)
- implement-story — docs-only, no code to implement
- simplify — no existing code to simplify

## Commands run and outcomes

1. Read CHARTER.md — confirmed worktree convention and path layout
2. Read plan.md lines 627-640 — confirmed S38 scope: one file (staging-recovery.md), AC-M7-23/24, exit criteria: cargo fmt check + markdown-link-check
3. Read requirements.md M7-S6 section — confirmed AC-M7-23 (corrupted .cxg-cache/ recovery) and AC-M7-24 (confirm_token derivation)
4. Read design.md §5.4 (observability metrics list), §5.5 (soak note), §5.7 (Docker/libclang)
5. Checked worktrees: /Users/husam/workspace/cpp-indexer/.worktrees/s38-runbook exists on branch story/s38-runbook, clean
6. Grepped src/api/reset.rs — found exact sha256_hex() implementation; confirmed: target_name = repo_name for Repo, literal "ALL" for All; output is hex(sha256(s)); known test vector sha256_hex("ALL") = "b5c7aed7cd2a308523e7d2847b7815909e864b2fd9c4ea88b00d35adb2ecdfd7"
7. Grepped src/resolve/spill.rs — confirmed RocksDB path: <stage_dir>/.cxg-cache/usr_map.rocks
8. Grepped src/api/reset.rs clear_staging_cache() — confirmed Repo removes stage_root/<name>/, All removes entire stage_root
9. Read src/bin/daemon.rs — confirmed config file name (cxg-daemon.toml), listen default (127.0.0.1:7878), auth_token_env pattern, sink preflight call
10. Read src/sink/factory.rs — confirmed "neo4j" and "indradb" are the only valid backend strings
11. Read src/config/mod.rs — confirmed stage_dir field name in [index] section
12. Read README.md — confirmed LIBCLANG_PATH is required; libclang.so/libclang.dylib on dynamic loader path
13. Called advisor — resolved scope conflict (plan vs dispatch); confirmed approach of primary file + sibling files; warned about token derivation verification (done in step 6)
14. Created docs/runbooks/staging-recovery.md (AC-M7-23/24 primary)
15. Created docs/runbooks/daemon-ops.md
16. Created docs/runbooks/sink-failover.md
17. Created docs/runbooks/observability.md
18. Created docs/runbooks/libclang-setup.md
19. cargo fmt --all -- --check — exit 0 (no Rust changes)
20. npx --yes markdown-link-check docs/runbooks/staging-recovery.md — 0 links, pass
21. npx --yes markdown-link-check (all other files) — all links pass including https://apt.llvm.org
22. git add docs/runbooks/ && git -c commit.gpgsign=false commit -m "S38: runbook" — commit e2f84ab

## Deviations from plan.md
- Plan names only staging-recovery.md; dispatch elaborates to include daemon-ops, sink-failover, observability, libclang-setup. Created all four siblings. Link-check run on all files, not just the one in exit-criteria.

## Tool failures or retries
- None. All gates passed on first run.

## Key facts verified from source before writing
- sha256_hex("ALL") = b5c7aed7cd2a308523e7d2847b7815909e864b2fd9c4ea88b00d35adb2ecdfd7 (from unit test in reset.rs)
- RocksDB spill path: <stage_dir>/.cxg-cache/usr_map.rocks (from spill.rs line 124)
- stage_root per-repo clear: rm -rf <stage_root>/<repo_name>/ (from clear_staging_cache in reset.rs)
- stage_root all clear: rm -rf <stage_root>/ (same)
- Daemon config file: cxg-daemon.toml (from daemon.rs default_value)
- Default listen: 127.0.0.1:7878 (from daemon.rs line 79)
- Valid backend strings: "neo4j", "indradb" only (from factory.rs match arm)
- Prometheus metrics confirmed from design.md §5.4: cxg_nodes_total, cxg_edges_total, cxg_libclang_errors_total, cxg_cache_hit_ratio, cxg_queue_depth, cxg_nodes_per_second, cxg_edges_per_second
