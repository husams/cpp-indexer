// SQLite persistence layer — byte/semantics-compatible port of
// indexer/storage.py (schema v6, design §4/§5.3).
//
// Connection sequence (G19: migration BEFORE the schema script, because the
// schema's indexes reference migrated columns):
//   mkdir -p dirname(path)  [skipped for :memory:]
//   open -> PRAGMA foreign_keys = ON -> migrate() -> schema script
//
// Every public mutator commits unless inside a Transaction (the SQLite C API
// autocommits per statement, which is exactly Python's _commit()-unless-in-txn
// contract once Transaction issues an explicit BEGIN). The upsert SQL is
// ported character-for-character from storage.py — semantics frozen by
// tests/storage_smoke_test.cpp (the executable spec, G13/G14).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "storage/records.hpp"
#include "storage/sqlite.hpp"

namespace cidx {

constexpr int kSchemaVersion = 39;

struct IndexIdentity {
  int schema_version = kSchemaVersion;
  std::optional<std::string> source_revision;
  std::optional<std::string> source_fingerprint;
  std::optional<std::string> index_config;
  std::optional<std::string> index_config_fingerprint;
  std::string freshness = "unverifiable"; // current | stale | unverifiable
};

// Allowed symbol.kind values (storage.py SYMBOL_KINDS) — enforced by an
// application-side StorageError (§3.2). v16: kind is stored on disk as its
// CXCursorKind integer; these helpers convert name <-> stored int.
bool is_symbol_kind(std::string_view kind);
// name -> CXCursorKind int (-1 if unknown); int -> name (decimal string if
// unknown). Single source mirrored from storage.py SYMBOL_KIND_IDS.
int64_t symbol_kind_id(std::string_view name);
std::string symbol_kind_name(int64_t id);

class Storage;

// RAII transaction: BEGIN on construction, COMMIT on clean destruction,
// ROLLBACK when destroyed during exception unwind (Python _Transaction).
class Transaction {
public:
  explicit Transaction(Storage &db);
  ~Transaction();
  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;

  void commit();   // explicit early commit
  void rollback(); // explicit early rollback

private:
  Storage &db_;
  bool done_ = false;
  int uncaught_on_entry_;
};

class Storage {
public:
  // read_only opens with SQLITE_OPEN_READONLY and performs NO mutation on
  // connect: no directory creation, no migrate(), no schema script, no
  // backfill. The stored schema_version must equal kSchemaVersion (a
  // read-only open cannot migrate) or the constructor throws CidxError.
  enum class OpenMode { read_write, read_only };

  explicit Storage(const std::string &path = ":memory:",
                   OpenMode mode = OpenMode::read_write);

  // Batch many mutations into one commit (the documented 100x win):
  //   { auto txn = db.transaction(); ...; }   // commits at scope end
  Transaction transaction() { return Transaction(*this); }

  // -- semantic universes / symbol identity (v35) --------------------------
  // A universe is explicit policy: equal USRs merge only when their
  // repositories/components are assigned to the same universe. The numeric
  // id is database-local; Symbol::identity_key is the portable identity.
  int64_t add_semantic_universe(const std::string &key,
                                const std::string &name = "",
                                const std::string &policy = "explicit");
  std::optional<SemanticUniverse>
  get_semantic_universe_by_id(int64_t universe_id);
  std::optional<SemanticUniverse>
  get_semantic_universe_by_key(const std::string &key);
  std::vector<SemanticUniverse> list_semantic_universes();
  void
  set_repository_semantic_universe(int64_t repository_id,
                                   const std::optional<int64_t> &universe_id);
  void
  set_component_semantic_universe(int64_t component_id,
                                  const std::optional<int64_t> &universe_id);

  // -- components ------------------------------------------------------------
  int64_t
  add_component(const std::string &name, const std::string &path,
                const std::string &kind = "repo",
                const std::optional<std::string> &version = std::nullopt);
  // v24: refresh an EXISTING component's name/kind in place (version COALESCE-
  // kept) without touching its stored path. Mirrors Python
  // update_component_meta.
  void update_component_meta(
      int64_t component_id, const std::string &name, const std::string &kind,
      const std::optional<std::string> &version = std::nullopt);
  // Two-step lookup: first by stored BASE path, then by effective root.
  // Required because version-detection may split a trailing segment off the
  // registered path (see §2 hazard in portable_paths_contract.md).
  std::optional<Component> get_component(const std::string &path);
  std::optional<Component> get_component_by_name(const std::string &name);
  std::optional<Component> get_component_by_id(int64_t component_id);
  // Longest-prefix match computed app-side (G16); nested components resolve
  // to the deeper root. Uses resolved_root (base+version) for comparison.
  std::optional<Component> component_for_path(const std::string &abs_path);
  std::vector<Component>
  list_components(const std::optional<std::string> &name = std::nullopt,
                  const std::optional<std::string> &kind = std::nullopt);
  // Remove a component and everything derived from it: directories and files
  // via ON DELETE CASCADE, plus symbols indexed from those files (deleted
  // explicitly -- symbol file refs are ON DELETE SET NULL). For import --force.
  void delete_component(int64_t component_id);

  // v14: version management
  // Returns false when no component with that name exists.
  bool set_component_version(const std::string &name,
                             const std::optional<std::string> &version);
  // Set a component's EFFECTIVE version regardless of representation,
  // non-destructively: version-as-property -> UPDATE the column; version
  // embedded in the path -> rewrite the trailing path segment, version NULL.
  // No-op (returns false) unless the name resolves to exactly one row.
  // Mirrors Python Storage.set_component_effective_version.
  bool set_component_effective_version(const std::string &name,
                                       const std::string &version);
  // Stored effective root: version ? normpath(join(path, version)) : path.
  // NOT resolved (may contain $VAR). Static so callers can use it anywhere.
  static std::string effective_root(const Component &comp);

  // v24: absolute base directory of a component's tree (the effective root). A
  // component grouped under a repository stores its `path` RELATIVE to that
  // repository's active clone root; this anchors the (relative) effective root
  // under the resolved clone path. An ungrouped component (repository_id null),
  // an absolute path, or a portable <label>/$VAR path resolves exactly as
  // before -- clone-agnostic. The single choke point for path reconstruction.
  // Mirrors Python Storage.component_abs_base.
  std::string component_abs_base(const Component &comp);

  // v24: rewrite a grouped component's stored `path` to be RELATIVE to its
  // repository's active clone root (`.` when it IS the clone root). A portable
  // path, an already-relative path, a version-in-path representation, or a base
  // outside clone_root is left untouched. Mirrors Python relativize_component.
  void relativize_component(int64_t component_id,
                            const std::string &clone_root);

  // v23: attach (or, with nullopt, detach) a component to a repository.
  void set_component_repository(int64_t component_id,
                                const std::optional<int64_t> &repository_id);
  // Components grouped under a repository, ordered by name, path.
  std::vector<Component> components_for_repository(int64_t repository_id);

  // -- repositories / clones (v23) -------------------------------------------
  // Insert a repository; idempotent on name. remote_url updated only when a
  // non-null value is supplied (COALESCE). Returns the repository id.
  int64_t add_repository(
      const std::string &name, const std::string &kind = "repo",
      const std::optional<std::string> &remote_url = std::nullopt,
      const std::optional<int64_t> &semantic_universe_id = std::nullopt);
  std::optional<Repository> get_repository_by_name(const std::string &name);
  std::optional<Repository> get_repository_by_id(int64_t repository_id);
  std::optional<Repository>
  get_repository_by_remote(const std::string &remote_url);
  std::vector<Repository>
  list_repositories(const std::optional<std::string> &name = std::nullopt,
                    const std::optional<std::string> &kind = std::nullopt);
  void set_active_clone(int64_t repository_id,
                        const std::optional<int64_t> &clone_id);
  // Remove a repository (clones cascade; components detach via SET NULL).
  void delete_repository(int64_t repository_id);
  // Register a checkout/worktree dir; idempotent on path. Returns clone id.
  int64_t add_clone(int64_t repository_id, const std::string &path,
                    const std::optional<std::string> &label = std::nullopt);
  std::optional<Clone> get_clone_by_id(int64_t clone_id);
  std::optional<Clone> get_clone_by_path(const std::string &path);
  std::vector<Clone>
  list_clones(const std::optional<int64_t> &repository_id = std::nullopt);
  // Remove a clone; clears the repository's active pointer if it pointed here.
  void delete_clone(int64_t clone_id);

  // -- directories -----------------------------------------------------------
  int64_t add_directory(int64_t component_id, const std::string &path);
  std::optional<Directory> get_directory(int64_t component_id,
                                         const std::string &path);
  std::optional<Directory> get_directory_by_id(int64_t directory_id);
  // component.path / directory.path for a directory id, or nullopt.
  std::optional<std::string> directory_abs_path(int64_t directory_id);
  // Remove a directory, its files (ON DELETE CASCADE), and the symbols indexed
  // from those files (file refs are ON DELETE SET NULL, deleted explicitly).
  void delete_directory(int64_t directory_id);
  std::vector<std::pair<Directory, std::string>> // (row, component name)
  list_directories(const std::optional<int64_t> &component_id = std::nullopt,
                   const std::optional<std::string> &name = std::nullopt);

  // -- files -------------------------------------------------------------
  int64_t add_file(int64_t directory_id, const std::string &name,
                   const std::optional<double> &mtime = std::nullopt,
                   const std::optional<std::string> &md5 = std::nullopt,
                   const std::optional<std::vector<std::string>>
                       &compile_options = std::nullopt,
                   const std::optional<std::string> &driver = std::nullopt);
  // Throws StorageError when no component owns abs_path (add_component first).
  int64_t
  add_file_path(const std::string &abs_path,
                const std::optional<double> &mtime = std::nullopt,
                const std::optional<std::string> &md5 = std::nullopt,
                const std::optional<std::vector<std::string>> &compile_options =
                    std::nullopt,
                const std::optional<std::string> &driver = std::nullopt);
  std::optional<File> get_file(const std::string &abs_path);
  std::optional<File> get_file_by_id(int64_t file_id);
  std::optional<std::string> file_abs_path(int64_t file_id);
  // Remove a file and the symbols indexed from it (file refs are ON DELETE SET
  // NULL, so deleted explicitly to avoid file-less orphans).
  void delete_file(int64_t file_id);
  std::vector<std::pair<File, std::string>> // (row, reconstructed abs path)
  list_files(const std::optional<int64_t> &component_id = std::nullopt,
             const std::optional<std::string> &dir_path = std::nullopt,
             const std::optional<std::string> &name = std::nullopt,
             const std::optional<bool> &indexed = std::nullopt);
  void mark_file_indexed(int64_t file_id,
                         const std::optional<double> &mtime = std::nullopt,
                         const std::optional<std::string> &md5 = std::nullopt);
  // Flip the indexed/pending flag in place; symbols are untouched.
  void set_file_indexed(int64_t file_id, bool indexed);
  // Replace a file's stored compile flags (and optionally its driver) and mark
  // it args_overridden=1 so a re-import (without --force) keeps the edit. Used
  // by `cidx file -set-flag/-unset-flag/-import-args`.
  void set_file_compile_options(
      int64_t file_id, const std::vector<std::string> &options,
      const std::optional<std::string> &driver = std::nullopt,
      bool update_driver = false);

  // Replace a file's stored compile flags WITHOUT setting args_overridden.
  // Used by `cidx realias`, which rewrites include paths to <label> tokens as
  // a portability transform (not a manual edit) — a later `import` should be
  // free to re-strip + re-alias these files.
  // Port of storage.py update_file_compile_options.
  void update_file_compile_options(int64_t file_id,
                                   const std::vector<std::string> &options);
  bool is_file_indexed(const std::string &abs_path,
                       const std::optional<double> &mtime = std::nullopt,
                       const std::optional<std::string> &md5 = std::nullopt);

  // -- diagnostics (v15) -------------------------------------------------
  // Replace a file's (TU's) stored parse diagnostics wholesale; called on
  // every (re)index so a now-clean file drops its stale rows. Rows are
  // inserted in order so their ids follow TU diagnostic order.
  void replace_diagnostics(int64_t file_id,
                           const std::vector<Diagnostic> &diags);
  // Stored parse diagnostics for a file, in insertion (TU) order.
  std::vector<Diagnostic> get_diagnostics(int64_t file_id);
  // Per-file diagnostic counts grouped by severity: {file_id: {severity: n}}.
  std::map<int64_t, std::map<int, int64_t>> diagnostic_counts();

  // -- symbols -----------------------------------------------------------
  // Upsert keyed by semantic universe plus portable identity key; throws
  // StorageError on a bad kind. Definition wins over a stored declaration; a
  // declaration never downgrades a definition.
  int64_t add_symbol(const Symbol &sym);
  void delete_symbols_for_file(int64_t file_id);
  // Update named columns of the symbol with this USR; false when absent.
  // Throws StorageError on unknown columns or a bad kind value (smoke parity).
  bool update_symbol(
      const std::string &usr,
      const std::vector<std::pair<std::string, SqlValue>> &values,
      const std::optional<int64_t> &semantic_universe_id = std::nullopt,
      const std::optional<std::string> &identity_source = std::nullopt,
      const std::optional<std::string> &identity_translation_unit =
          std::nullopt);
  bool update_symbol_by_id(
      int64_t symbol_id,
      const std::vector<std::pair<std::string, SqlValue>> &values);
  // Bare lookup is unambiguous only when zero or one row matches. Call
  // lookup_symbols_by_usr() or pass a scope/source to select an ambiguous
  // match explicitly.
  std::optional<Symbol> lookup_symbol(
      const std::string &usr,
      const std::optional<int64_t> &semantic_universe_id = std::nullopt,
      const std::optional<std::string> &identity_source = std::nullopt,
      const std::optional<std::string> &identity_translation_unit =
          std::nullopt);
  std::vector<Symbol> lookup_symbols_by_usr(
      const std::string &usr,
      const std::optional<int64_t> &semantic_universe_id = std::nullopt);
  std::optional<Symbol> lookup_symbol_by_id(int64_t symbol_id);
  // Remove a single symbol row.
  void delete_symbol(int64_t symbol_id);
  std::vector<Symbol> lookup_symbols_by_name(
      const std::string &spelling,
      const std::optional<std::string> &kind = std::nullopt,
      const std::optional<int64_t> &semantic_universe_id = std::nullopt);
  // Exact match on qual_name column; mirrors lookup_symbols_by_name but keyed
  // on qual_name instead of spelling. Used to recover a callee whose USR is
  // inconsistent (member function template in a dependent template body).
  std::vector<Symbol> lookup_symbols_by_qual_name(
      const std::string &qual_name,
      const std::optional<std::string> &kind = std::nullopt,
      const std::optional<int64_t> &semantic_universe_id = std::nullopt);
  // '::'-segment fuzzy match on qual_name, ordered LENGTH(qual_name) first.
  std::vector<Symbol>
  search_symbols(const std::string &pattern,
                 const std::optional<std::string> &kind = std::nullopt,
                 const std::optional<int64_t> &config_id = std::nullopt);
  ConfiguredSymbols
  symbols_for_config(int64_t file_id, const std::vector<int64_t> &config_ids,
                     FactCoverage coverage = FactCoverage::one);
  ConfiguredFactIds
  fact_ids_for_config(int64_t file_id, const std::string &fact_kind,
                      const std::vector<int64_t> &config_ids,
                      FactCoverage coverage = FactCoverage::one);
  void associate_facts_for_file(int64_t file_id, int64_t config_id,
                                const std::vector<int64_t> &symbol_ids,
                                const std::vector<int64_t> &edge_ids,
                                const std::vector<int64_t> &definition_ids);
  // Location scope matches definition OR declaration site (§3.5).
  std::vector<Symbol>
  list_symbols(const std::optional<int64_t> &component_id = std::nullopt,
               const std::optional<std::string> &dir_path = std::nullopt,
               const std::optional<int64_t> &file_id = std::nullopt,
               const std::optional<std::string> &name = std::nullopt,
               const std::optional<std::string> &kind = std::nullopt);
  std::vector<Symbol> symbols_in_file(int64_t file_id);
  std::vector<Symbol> unresolved_symbols();

  // -- graph layer (v7) ------------------------------------------------------
  // Mint a stub symbol row (resolved=0, kind='function') for an unknown USR.
  // The reference cursor is always in hand at the call site, so its name
  // travels with the USR: a stub is born NAMED -- essential for targets whose
  // definition is never indexed (stdlib calls, implicit template
  // instantiations, defaulted ctors), where no add_symbol ever backfills it.
  // The reference cursor's declaration location travels too: when it sits in an
  // indexed file the stub is born LOCATED (e.g. a defaulted ctor anchored to
  // its `struct` line), so chain::D::D resolves to chain.hpp:25 instead of
  // `@<no-location>`. decl_file_id is nullopt for targets in unregistered
  // (system/stdlib) headers, which correctly stay location-less.
  // An existing real row is kept intact; a repeat mint only UPGRADES an empty
  // name, never clobbers a real one, and fills the location only when still
  // absent. Returns the stable symbol.id either way.
  int64_t mint_symbol_id(
      const std::string &usr, const std::string &spelling = "",
      const std::string &qual_name = "", const std::string &display_name = "",
      const std::string &kind = "function",
      const std::optional<int64_t> &decl_file_id = std::nullopt,
      const std::optional<int64_t> &decl_line = std::nullopt,
      const std::optional<int64_t> &decl_col = std::nullopt,
      const std::optional<std::string> &decl_path = std::nullopt,
      bool is_instantiation = false, bool is_named_instance = false,
      const std::optional<std::string> &type_info = std::nullopt,
      const std::optional<int64_t> &semantic_universe_id = std::nullopt,
      const std::optional<std::string> &identity_source = std::nullopt,
      const std::optional<std::string> &linkage = std::nullopt,
      const std::optional<std::string> &identity_translation_unit =
          std::nullopt);

  // UNIQUE upsert on (src_id, dst_id, kind); increments count on conflict.
  // Returns the edge.id for edge_site linkage.
  int64_t add_edge(const Edge &e);

  // Idempotent variant for STRUCTURAL relationships (specializes/instantiates):
  // inserts the edge if absent, but a conflict leaves count untouched, so a
  // relationship already recorded from the declaration is never re-counted by
  // later call sites. Returns the stable edge.id.
  int64_t ensure_edge(const Edge &e);

  // INSERT OR IGNORE: same site visited twice (e.g. re-parse) = no-op.
  void add_edge_site(const EdgeSite &s);

  // INSERT OR IGNORE a call_arg row (PK collision = same arg, harmless).
  void add_call_arg(const CallArg &a);

  // INSERT OR REPLACE keyed on (owner_id, position).
  void add_template_param(const TemplateParam &p);
  void add_template_arg(const TemplateArg &a);

  // -- v30 signature/type tier ------------------------------------------------
  // Upsert a normalized type-shape row keyed by type_key; returns the stable
  // type_node.id. A conflict refreshes ONLY canonical_id (alias nodes are
  // keyed by declaration USR while their target is mutable across reindexes:
  // `using Alias = Foo;` -> `= Bar;`); spelling and the structural columns
  // keep the first writer's values so a partial reindex can never rewrite a
  // shared node's display form.
  int64_t intern_type_node(const TypeNode &n);
  std::optional<TypeNode> type_node_by_id(int64_t type_id);
  // INSERT OR REPLACE keyed on (src_id, kind, position) -- a retargeted
  // alias's alias_of edge follows the new target.
  void add_type_edge(int64_t src_id, int64_t kind, int64_t position,
                     int64_t dst_id);
  // Wholesale per-owner refresh (DELETE + INSERT): re-index with a changed
  // arity leaves no stale positions.
  void replace_parameters(int64_t owner_id,
                          const std::vector<Parameter> &params);
  std::vector<Parameter> parameters_of(int64_t symbol_id);
  // INSERT OR REPLACE keyed on (symbol_id, kind); kind is kSymbolType*.
  void add_symbol_type(int64_t symbol_id, int64_t kind, int64_t type_id);
  std::optional<int64_t> symbol_type_of(int64_t symbol_id, int64_t kind);
  // All type_node ids from which a node naming `decl_usr` is reachable via
  // type_edge (reverse) or canonical_id links -- the "accepts/returns T
  // (including const T&, T*, aliases of T, ...)" closure. Ordered by id.
  std::vector<int64_t> type_ids_reaching(const std::string &decl_usr);
  // (owner_id, position) of parameters whose type is one of type_ids,
  // ordered by (owner_id, position).
  std::vector<std::pair<int64_t, int64_t>>
  param_owners_of_types(const std::vector<int64_t> &type_ids);
  // (symbol_id, symbol_type.kind) rows whose type is one of type_ids,
  // ordered by (symbol_id, kind).
  std::vector<std::pair<int64_t, int64_t>>
  symbol_type_owners_of_types(const std::vector<int64_t> &type_ids);

  // -- v31 include tier -------------------------------------------------------
  // Upsert keyed by (tu_file_id, digest); returns the stable include_config.id.
  // A repeat call refreshes the descriptive columns so a changed driver or
  // resource dir under an unchanged digest cannot go stale.
  int64_t add_include_config(const IncludeConfig &c);
  std::optional<IncludeConfig> include_config_by_id(int64_t config_id);
  // Configurations whose TU is `tu_file_id`, ordered by digest.
  std::vector<IncludeConfig> include_configs_for_tu(int64_t tu_file_id);

  int64_t add_translation_unit_config(const TranslationUnitConfig &input);
  std::optional<TranslationUnitConfig>
  translation_unit_config_by_id(int64_t config_id);
  std::vector<TranslationUnitConfig>
  translation_unit_configs_for_file(int64_t file_id);
  void add_file_config(const FileConfigApplicability &applicability);
  std::vector<FileConfigApplicability> file_configs_for(int64_t file_id);

  // UNIQUE upsert on (src_file_id, dst_path, config_id); ACCUMULATES count on
  // conflict (a header included twice in one file is two occurrences of one
  // collapsed edge). dst_file_id is refreshed when the caller now knows it.
  // Returns the include_edge.id for include_site linkage.
  int64_t add_include_edge(const IncludeEdge &e);
  // INSERT OR REPLACE keyed on (edge_id, begin_offset): re-indexing the same
  // directive rewrites its site rather than duplicating it.
  int64_t add_include_site(const IncludeSite &s);
  // INSERT ... ON CONFLICT: accumulates count.
  void add_include_macro_use(const IncludeMacroUse &m);

  // Drop every configuration owned by this TU (cascades to that TU's edges,
  // sites, and macro uses). Called before re-recording a TU's directives so a
  // deleted #include -- or a whole configuration retired by a changed compile
  // command -- leaves no stale row, while a shared header's facts recorded
  // under OTHER TUs' configurations are untouched.
  void delete_include_configs_for_tu(int64_t tu_file_id);

  // Direct include edges out of / into a file. `include_system` keeps
  // system-classified targets; otherwise they are filtered. Ordered by
  // (dst_path, config digest) / (src path, config digest) for determinism.
  std::vector<IncludeEdge> include_edges_from(int64_t src_file_id,
                                              bool include_system);
  std::vector<IncludeEdge>
  include_edges_from_config(int64_t src_file_id,
                            int64_t translation_unit_config_id,
                            bool include_system);
  ConfiguredIncludeEdges
  invariant_include_edges(int64_t src_file_id,
                          const std::vector<int64_t> &declared_config_ids,
                          bool include_system);
  std::vector<IncludeEdge> include_edges_to(int64_t dst_file_id);
  // Every edge whose dst_path resolves to this path (covers targets that are
  // not owned by a component, which therefore have no dst_file_id).
  std::vector<IncludeEdge> include_edges_to_path(const std::string &dst_path);
  // Every include edge in the database, ordered by (src_file_id, dst_path,
  // config digest). Whole-graph queries (cycles, transitive closure, hotspots)
  // need the full relation, and it is far cheaper to sort once here than to
  // walk per-file.
  std::vector<IncludeEdge> all_include_edges(bool include_system);
  // Sites of one collapsed edge, ordered by begin_offset.
  std::vector<IncludeSite> include_sites_for(int64_t edge_id);
  // Macros expanded in `src_file_id` that are defined in `def_path`.
  std::vector<IncludeMacroUse> include_macro_uses(int64_t src_file_id,
                                                  const std::string &def_path);
  // True once any include fact exists -- distinguishes "this DB predates the
  // v31 tier / has not been reindexed" from "this file includes nothing".
  bool include_graph_populated();
  // True if the include tier actually observed this file: it was indexed as a
  // TU (has a configuration), included something, or was included by something.
  // Lets a scoped query tell "analyzed and genuinely clean" from "never indexed
  // for includes", which otherwise both read as zero findings.
  bool include_tier_covers_file(int64_t file_id);

  // -- v31 reference-set analysis ---------------------------------------------
  // These four back the unused(S, H) definition:
  //   unused(S, H) := Refs(Owners(S)) INTERSECT Symbols(H) = {}
  // (see docs/include-hygiene.md). They are deliberately set-oriented: one
  // query per file, not one per symbol.

  // Owners(F) / Symbols(F): every symbol DECLARED or DEFINED directly in this
  // file. A symbol from a header this file includes is NOT owned by it -- that
  // separation is what makes "a reference to a transitive header's symbol does
  // not use the direct header" hold. Ordered by id.
  std::vector<int64_t> symbol_ids_for_file(int64_t file_id);

  // Targets of every persisted semantic edge out of `src_ids`, across ALL edge
  // kinds (calls, uses, inherits, overrides, construct-*, destroy, friend,
  // specializes, instantiates, field_of, method_of, ...). Sorted, unique.
  std::vector<int64_t> edge_targets_from(const std::vector<int64_t> &src_ids);

  // Targets of every def_edge out of a body DEFINED IN this file -- the
  // per-file half of Refs.
  //
  // `edge` is keyed by symbol, and symbol.usr is UNIQUE, so bodies that share a
  // USR across translation units collapse onto one row: re-indexing each TU
  // deletes the previous TU's edges and writes its own, last writer wins. Every
  // `int main(int, char**)` in a project is the same USR, so all but one lose
  // their call graph entirely -- and a file whose calls vanished has no
  // references, which makes every one of its includes look unused. The v27
  // `def_edge` table records calls/uses per BODY rather than per symbol, so it
  // survives the collapse and is the only accurate source for such a file.
  //
  // This does not replace edge_targets_from: def_edge only carries body
  // calls/uses (kinds 1/7). Declaration relations -- inherits, method_of,
  // field_of, friend, specializes -- exist only on `edge`. Refs needs both.
  std::vector<int64_t> def_edge_targets_for_file(int64_t file_id);

  // Type nodes these symbols name through the signature tier: their return
  // type, declared type, underlying type (symbol_type) and every parameter type
  // (parameter). Sorted, unique.
  std::vector<int64_t> type_ids_used_by(const std::vector<int64_t> &symbol_ids);

  // Every symbol id named by the transitive structural closure of `type_ids`:
  // follows type_edge (pointee, element, alias_of, return, param, template
  // argument) and canonical_id, then maps each node's decl_usr back to its
  // symbol. This is what makes `const Foo&`, `Foo*`, `vector<Foo>`, and an
  // alias of Foo all count as references to Foo. Sorted, unique.
  std::vector<int64_t>
  symbols_named_by_types(const std::vector<int64_t> &type_ids);

  // Delete edges whose src is a symbol defined in this file (idempotent
  // re-index: edges cascade-delete their edge_site rows).
  void delete_edges_for_file(int64_t file_id);

  // -- entity_edge (v17)
  // ------------------------------------------------------- Upsert an
  // entity_edge row (idempotent re-materialise safe).
  void add_entity_edge(int64_t src_id, int64_t dst_id, int64_t kind,
                       int64_t count = 1,
                       std::optional<int64_t> via_member_id = std::nullopt,
                       int64_t multiplicity = 1, int64_t access = 0,
                       int64_t is_virtual = 0,
                       std::optional<int64_t> create_form = std::nullopt,
                       int64_t partial = 0);
  // Delete all entity_edge rows (pre-step for idempotent re-materialise).
  void clear_entity_edges();
  // Materialise all 10 entity relation kinds from the Layer-0 graph.
  // Called by resolve_pass() after rollup_edge_counts(). Pure DB pass.
  void materialise_entity_edges();

  // Resolve pass (DB-only, no parse): roll up edge.count from edge_site for
  // calls/uses, report remaining stubs. Returns count of still-unresolved
  // stub symbols.
  int resolve_pass();

  // Roll edge.count up to the true site count for calls (kind=1) and uses
  // (kind=7) — idempotent; COUNT(*) is the source of truth.
  void rollup_edge_counts();

  // Materialise virtual-dispatch caller edges (kind 18, 'dispatch_calls'):
  // caller -> each transitive override of the virtual method it statically
  // calls. Idempotent (DELETE + rebuild). Called by resolve_pass().
  void materialize_dispatch_calls();

  // -- v27: multi-definition (per-backend redefinitions) ---------------------
  // Return the `definition` row id for this symbol's body in (component, file),
  // creating it if new. component derived from the file when not supplied.
  int64_t get_or_create_definition(
      int64_t symbol_id, std::optional<int64_t> file_id,
      std::optional<int64_t> line = std::nullopt,
      std::optional<int64_t> col = std::nullopt,
      std::optional<int64_t> end_line = std::nullopt,
      std::optional<int64_t> end_col = std::nullopt,
      const std::optional<std::string> &init_text = std::nullopt);
  // The component that owns this file (file -> directory -> component).
  std::optional<int64_t> component_id_for_file(std::optional<int64_t> file_id);
  // Upsert a per-body outgoing edge (src is a definition). kind reuses
  // edge_kind (1 calls / 7 uses). Returns the def_edge id.
  int64_t add_def_edge(int64_t src_def_id, int64_t dst_id, int64_t kind,
                       int64_t count = 1);
  // Snapshot a function body's just-emitted calls/uses (edge kind 1/7 for this
  // symbol) into def_edge keyed by def_id. Called right after body_descent.
  void copy_body_edges_to_def_edge(int64_t def_id, int64_t symbol_id);
  // Drop this file's definition rows (cascades def_edge) before re-index.
  void delete_definitions_for_file(int64_t file_id);
  // Set symbol.multi_def = COUNT(definition rows). Called by resolve_pass().
  void set_multi_def();
  // Materialise body->body possible-call fan-out. Called by resolve_pass()
  // after set_multi_def(). Idempotent (DELETE + rebuild).
  void materialize_possible_calls();

  // Edges whose ends live in different components.
  std::vector<Edge> cross_repo_edges();

  Stats stats();

  // -- graph read-only accessors (M6 — query.py parity) ----------------------
  // A1: total edge count (query.py:558)
  int64_t edge_count();

  // A2: true once `cidx resolve` has rolled up edge counts (query.py:579-583)
  bool graph_resolved();

  // A3/A4: fetch one symbol by USR / numeric id (query.py:666-668)
  std::optional<Symbol> graph_symbol_by_usr(const std::string &usr);
  std::optional<Symbol> graph_symbol_by_id(int64_t id);

  // A5: fuzzy COALESCE(qual_name,spelling) lookup (query.py:707-738, R1).
  // Escapes ONLY % and _ (matching query.py:719 -- NOT storage escape_like).
  std::vector<Symbol> find_symbols(const std::string &pattern,
                                   const std::optional<std::string> &kind,
                                   int limit);

  // v27 multi-definition readers (query.py:GraphQuery.redefined/definitions/
  // possible_callees). DefinitionRow is one `definition` (or possible-call
  // target) row; the graph layer joins in component/file for display.
  struct DefinitionRow {
    int64_t symbol_id = -1;
    std::optional<int64_t> file_id;
    std::optional<int64_t> line, col, end_line, end_col;
    std::optional<std::string>
        init_text; // v28: (static member) var initializer
  };
  std::vector<Symbol> redefined_symbols(int limit);
  std::vector<DefinitionRow> definitions_of(int64_t symbol_id);
  std::vector<DefinitionRow> possible_callees_of(int64_t symbol_id);

  // A6 result row: 8 edge columns + decoded symbol-from-offset (plan §A6).
  struct GraphEdgeRow {
    int64_t eid = -1;
    int64_t src_id = -1;
    int64_t dst_id = -1;
    int64_t ekind = 0;
    int64_t ecount = 0;
    int64_t rawcount = 0;
    std::optional<int64_t> base_access;
    std::optional<int64_t> is_virtual;
    Symbol sym; // decoded from cols 8..33 via symbol_from_offset
  };

  // A7 result row for batch site loading.
  struct EdgeSiteRow {
    int64_t edge_id = -1;
    std::optional<int64_t> file_id;
    std::optional<int64_t> line;
    std::optional<int64_t> col;
    bool conditional = false;
    std::optional<std::string> args_sig;
    std::optional<std::string> recv_src_kind;
    std::optional<std::string> recv_type_usr;
    std::optional<std::string> recv_decl_usr;
    std::optional<int64_t> recv_param_pos;
    std::optional<int64_t> recv_type_is_value;
  };

  // A6: typed-edge query (query.py:782-813)
  // direction "in"|"out"; kind_ids empty => no kind filter; count_resolved
  // controls which count expression is used (A6 plan §count_expr).
  std::vector<GraphEdgeRow> graph_edges(int64_t mine_id,
                                        const std::string &direction,
                                        const std::vector<int64_t> &kind_ids,
                                        bool count_resolved, int limit);

  // A7: batch-load edge_site rows for many edge_ids (query.py:839-870)
  std::map<int64_t, std::vector<EdgeSiteRow>>
  edge_sites_for(const std::vector<int64_t> &edge_ids);

  // A8: single-edge sites with LIMIT (query.py:884-906)
  std::vector<EdgeSiteRow> edge_sites_one(int64_t edge_id, int limit);

  // -- labels (v14) ----------------------------------------------------------
  // Upsert on name; returns the row id.
  int64_t add_label(const std::string &name, const std::string &path);
  // Returns false when name absent.
  bool remove_label(const std::string &name);
  // Returns stored path or nullopt when absent.
  std::optional<std::string> get_label(const std::string &name);
  // Sorted by name; returns (name, stored path) pairs.
  std::vector<std::pair<std::string, std::string>> list_labels();
  // Version-agnostic component alias map: name -> (base, max_version,
  // bumpable). Effective root is split into (base, version); rows grouped by
  // name; a name is kept only when all rows share ONE base (else ambiguous).
  // max_version = "" when none. bumpable = true only for a single row whose
  // stored path carries no embedded version (safe to set_component_version).
  // (Python Storage.component_alias_index.)
  std::map<std::string, std::tuple<std::string, std::string, bool>>
  component_alias_index();
  // Encode registry for include-path aliasing as (name, match_path, versioned)
  // triples: explicit labels (exact) PLUS components (version-stripped base,
  // version-agnostic). Labels win on a name collision; components with
  // conflicting bases are skipped. Decode mirror = get_alias. Sorted by name.
  // (Python Storage.list_alias_pairs.)
  std::vector<std::tuple<std::string, std::string, bool>> list_alias_pairs();
  // Decode an alias name: explicit label -> stored path; else a uniquely-based
  // component -> base joined with its highest version; nullopt otherwise.
  // (Python Storage.get_alias.)
  std::optional<std::string> get_alias(const std::string &name);

  // Raw connection — exposed for tests (schema assertions on :memory: DBs)
  // and future maintenance commands. Not part of the indexing flow.
  SqliteDb &raw_db() { return db_; }

  // Runtime qualification and maintenance APIs. Backup uses SQLite's online
  // backup API; maintenance is explicit because ANALYZE changes statistics.
  auto backup_to(const std::string &path) const -> void { db_.backup_to(path); }
  auto run_maintenance() -> void { db_.exec("PRAGMA optimize"); }
  auto refresh_statistics() -> void { db_.exec("ANALYZE"); }
  [[nodiscard]] auto integrity_ok() -> bool;
  [[nodiscard]] auto foreign_keys_ok() -> bool;

  // %c%c% char-in-order LIKE pattern with '\ % _' escaping (G18); public
  // statics so fuzzy_match_test can pin them directly.
  static std::string fuzzy_like(std::string_view text);
  // WHERE fragment matching a directory and its whole subtree; root '' -> '%'
  // (G17). Appends the two LIKE args to `args`.
  static std::string dir_scope_sql(const std::string &dir_path,
                                   std::vector<SqlValue> &args);

  // Content-addressed identity of the indexed source/configuration. Legacy
  // databases without the v35 metadata remain readable but unverifiable.
  IndexIdentity index_identity();
  void stamp_index_identity();
  std::string portable_source_identity_for_path(const std::string &path);
  std::string portable_source_identity_for_file(int64_t file_id);
  std::string portable_translation_unit_identity_for_config(int64_t config_id);
  std::string portable_translation_unit_identity_for_config(
      int64_t config_id, int64_t translation_unit_file_id);
  std::string portable_translation_unit_identity_for_file(int64_t file_id);
  int64_t semantic_universe_for_file_id(int64_t file_id);

private:
  friend class Transaction;
  friend class ArtifactStore;

  void migrate(); // column-presence detection, §4.1
  void
  reconcile_external_identities(); // resolve evidence after local rows arrive
  void reconcile_symbol_identity(int64_t symbol_id, std::string_view usr);
  void reconcile_type_identity(int64_t type_id, std::string_view decl_usr);
  void migrate_symbol_kind_to_int();    // v15 -> v16: rebuild symbol, kind->int
  void migrate_component_repo_unique(); // v23 -> v24: path UNIQUE per repo
  void migrate_symbol_identity_scope(); // v34 -> v35: scoped symbol identity
  int64_t default_semantic_universe_id();
  int64_t semantic_universe_for_file(const std::optional<int64_t> &file_id);
  std::string symbol_identity_key(
      const Symbol &sym, int64_t universe_id,
      const std::optional<int64_t> &file_id,
      const std::optional<std::string> &source = std::nullopt,
      const std::optional<std::string> &translation_unit = std::nullopt);
  // v24: resolved absolute path of a repository's active clone, or nullopt when
  // ungrouped / no live clone. Mirrors Python Storage._active_clone_root.
  std::optional<std::string>
  active_clone_root(const std::optional<int64_t> &repository_id);
  // (component_id, relative dir, file name) for an absolute path; nullopt
  // when no component owns it.
  std::optional<std::tuple<int64_t, std::string, std::string>>
  split_path(const std::string &abs_path);

  SqliteDb db_;
  bool in_txn_ = false;
  // Set by migrate() on the v21->v22 transition; consumed by the constructor to
  // backfill entity_node from existing symbols (pure-DB, no re-index/resolve).
  bool needs_entity_node_backfill_ = false;
  std::unordered_set<std::string> attached_artifact_names_;
  std::optional<bool> artifact_query_only_before_attach_;
};

} // namespace cidx
