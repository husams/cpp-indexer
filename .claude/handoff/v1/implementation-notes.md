## S14: cpp extensions — implementation notes

Files changed:
- src/schema/nodes.rs — added Namespace, TemplateDef, Specialization, Typedef, Enum, Header variants to NodeKind
- src/schema/edges.rs — added Includes, Overrides, Instantiates, Specializes, FriendOf, AdlCandidate variants to EdgeKind
- src/schema/version.rs — bumped SCHEMA_VERSION 1→2, updated SCHEMA_VERSION_TAG and PARQUET_MAGIC strings
- src/visit/cursor_map.rs — extended entity_kind_to_node_kind: Namespace, ClassTemplate→TemplateDef, FunctionTemplate→TemplateDef, TypeAliasTemplateDecl→TemplateDef, ClassTemplatePartialSpecialization→Specialization, TypedefDecl/TypeAliasDecl→Typedef, EnumDecl→Enum, InclusionDirective→Header; updated unit tests accordingly
- src/visit/shallow.rs — extended visitor: Header node/INCLUDES edge via emit_header_node_and_edge; OVERRIDES edge with vtable_slot from get_overridden_methods(); SPECIALIZES edge from Specialization to primary template; FRIEND_OF edge via FriendDecl child walk; extended build_attrs_json for new kinds; added push_edge_with_attrs; enabled detailed_preprocessing_record flag on parser (required for InclusionDirective AST nodes)
- tests/fixtures/m2_cpp_extensions/ — new fixture: ext_base.h, ext_shapes.h, ext_main.cpp, compile_commands.json
- tests/integration/phase1_base.rs — added M2 tests: m2_emits_namespace_nodes, m2_emits_template_decl_nodes, m2_emits_specialization_nodes, m2_emits_header_nodes, m2_emits_typedef_nodes, m2_emits_enum_nodes, m2_emits_includes_edges, m2_emits_overrides_edge_with_vtable_slot, m2_emits_specializes_edge, m2_emits_friend_of_edge

Tests added/run:
- `cargo nextest run -p cpp_indexer --test phase1_base` → 15/15 passed
- `cargo fmt --all -- --check` → exit 0
- `cargo clippy --all-targets --all-features -- -D warnings` → exit 0

Deviations from plan:
- ADR-9 bump policy: the existing nodes.rs/edges.rs comment stated additive M2 variants could be added "without bumping SCHEMA_VERSION". ADR-9 is authoritative (accepted, not proposed) and states "Any change to NodeKind or EdgeKind variants requires bumping SCHEMA_VERSION by 1". SCHEMA_VERSION bumped 1→2. The outdated comment in both files has been updated.
- `detailed_preprocessing_record(true)` added to parser: required to expose InclusionDirective AST entities (for HEADER/INCLUDES). This is a correctness fix, not a plan deviation.
- is_inline_namespace() is gated behind clang_9_0 feature (Cargo.toml only has clang_6_0). Namespace attrs_json is "{}" for now; inline flag omitted.
- ADL_CANDIDATE (AC-M2-12) and INSTANTIATES (AC-M2-9) edges: EdgeKind variants defined in schema; visitor emission not implemented (requires call-expression context not in declaration walk). Tagged as follow-up.

Follow-ups:
- tag:sr-dev — ADL_CANDIDATE and INSTANTIATES edge emission requires call-expression context (Phase 2 visitor). Only the schema variants are defined here; visitor emission is a follow-up.
- tag:sr-dev — enable clang_9_0 feature on the clang crate if inline namespace detection is needed.
- S31 (SCHEMA_VERSION coordination): SCHEMA_VERSION is now 2; S31 must not independently bump it.

References: plan.md S14, design.md §4 Phase 1, adr-9.md, requirements.md AC-M2-1..AC-M2-12

---

## S16: m2-boost-optional-gate — implementation notes

Files changed:
- tests/fixtures/boost_optional/boost/optional.hpp (new) — minimal vendored fixture defining primary boost::optional<T> and partial specialization boost::optional<T*>
- tests/fixtures/boost_optional/optional_user.cpp (new) — TU using both template forms; no system headers to avoid cross_repo_candidate edges
- tests/fixtures/boost_optional/compile_commands.json (new) — absolute-path compile_commands.json matching m1_5file convention
- tests/integration/m2_exit_gate.rs (new) — #[ignore = "requires boost-optional-checkout"] test asserting AC-M2-16 and AC-M2-17
- Cargo.toml — added [[test]] m2_exit_gate with required-features = ["test-mock"]
- tests/integration/phase1_base.rs — added skip_system_headers: true to two pre-existing VisitOptions initializers (pre-existing build failure, not introduced by S16)

Tests added/run:
- `cargo fmt --all -- --check` → exit 0
- `cargo clippy --all-targets --all-features -- -D warnings` → exit 0
- `DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib cargo nextest run -p cpp_indexer --test m2_exit_gate --features test-mock -- --ignored` → 1 PASS

Deviations from plan:
- --features test-mock required but absent from plan.md exit-criteria command. Tagged for sr-dev.
- macOS requires DYLD_LIBRARY_PATH=/Library/Developer/CommandLineTools/usr/lib for libclang runtime discovery.
- Fixed pre-existing phase1_base.rs compile failure as a required clippy gate prerequisite.

Follow-ups:
- tag:sr-dev — update plan.md S16 exit-criteria to include --features test-mock
- tag:devops — set DYLD_LIBRARY_PATH in macOS CI runners for nextest

References: plan.md S16, requirements.md AC-M2-16 AC-M2-17
