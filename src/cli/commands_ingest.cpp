// init, migrate, add-source, import and index: the ingest side.
// Split out of commands.cpp; run_command's dispatch is unchanged.
#include "cli/commands_detail.hpp"

namespace cidx::cli {

std::string resolve_cache_dir() {
  // os.path.expanduser(os.environ.get("INDEXER_CACHE") or "~/.cache/cidx")
  std::optional<std::string> env = get_env("INDEXER_CACHE");
  const std::string raw =
      (env && !env->empty()) ? *env : std::string("~/.cache/cidx");
  return pathutil::expanduser(raw);
}

// -- write commands ----------------------------------------------------------

// cmd_init (cli.py cmd_init): create a blank index database (schema v6, no
// rows) at the cache path. Constructing a Storage applies the schema, so this
// just materializes an empty index.db. Refuses to clobber an existing
// database unless --force; with --force the old file is removed first.
int cmd_init(const ParsedArgs &args, Context &ctx) {
  const bool existed = path_exists(ctx.index_path);
  if (existed && !args.force) {
    *ctx.err << "error: index database already exists at " << ctx.index_path
             << " (use --force to recreate)\n";
    return 1;
  }
  if (existed && std::remove(ctx.index_path.c_str()) != 0) {
    // os.remove raises on failure -> propagates to main() (exit 1).
    throw CidxError("cannot remove " + ctx.index_path);
  }
  {
    Storage db(ctx.index_path);
  } // constructing Storage applies the schema
  *ctx.out << (existed ? "recreated" : "initialized")
           << " empty index database at " << ctx.index_path << "\n";
  return 0;
}

// cmd_migrate (cli.py cmd_migrate): upgrade an existing index DB to the current
// schema in place. Constructing a Storage runs the migration (e.g. v15 -> v16
// converts symbol.kind from a TEXT name to its CXCursorKind integer); this just
// does it deliberately and reports the before/after version. Preserves all data
// (symbols, edges) and never re-indexes. --db targets a non-standard index.
int cmd_migrate(const ParsedArgs &args, Context &ctx) {
  (void)args; // --db already folded into ctx.index_path by main()
  if (!path_exists(ctx.index_path)) {
    *ctx.err << "error: no index database at " << ctx.index_path
             << " (run `cidx init` / `cidx import` first)\n";
    return 1;
  }
  // Read schema_version WITHOUT opening a Storage (whose ctor would migrate).
  auto schema_version = [&]() -> std::optional<int> {
    SqliteDb db(ctx.index_path);
    SqliteStmt st =
        db.prepare("SELECT value FROM meta WHERE key = 'schema_version'");
    if (st.step() && !st.col_is_null(0)) {
      const std::string v = st.col_text(0);
      if (!v.empty()) {
        return std::stoi(v);
      }
    }
    return std::nullopt;
  };
  auto vstr = [](const std::optional<int> &v) {
    return v ? std::to_string(*v) : std::string("None"); // Python f"v{None}"
  };
  // A leftover 'nests' row (removed relation) or 'realizes' row (renamed to
  // 'implements') marks a DB whose entity_edge_kind seed must be refreshed,
  // even when schema_version is already current (an earlier build bumped the
  // version without reconciling the seed).
  auto entity_kinds_stale = [&]() -> bool {
    SqliteDb db(ctx.index_path);
    try {
      SqliteStmt st = db.prepare("SELECT 1 FROM entity_edge_kind "
                                 "WHERE name IN ('nests', 'realizes') LIMIT 1");
      return st.step();
    } catch (const std::exception &) {
      return false; // no entity_edge_kind table (pre-v17 DB)
    }
  };
  const std::optional<int> before = schema_version();
  if (before && *before > kSchemaVersion) {
    *ctx.err << "index at " << ctx.index_path << " is schema v" << *before
             << ", newer than this build (v" << kSchemaVersion
             << "); refusing to touch it\n";
    return 1;
  }
  const bool stale = entity_kinds_stale();
  {
    Storage db(ctx.index_path);
  } // constructing Storage applies the migration
  const std::optional<int> after = schema_version();
  if (before != after) {
    *ctx.out << "migrated " << ctx.index_path << ": schema v" << vstr(before)
             << " -> v" << vstr(after) << "\n";
  } else if (stale) {
    *ctx.out << "migrated " << ctx.index_path
             << ": refreshed entity relation kinds (schema v" << vstr(after)
             << ")\n";
  } else {
    *ctx.out << ctx.index_path << " already at schema v" << vstr(after)
             << "; nothing to migrate\n";
  }
  return 0;
}

int cmd_add_source(const ParsedArgs &args, Context &ctx) {
  const std::string kind = args.kind ? *args.kind : "repo";
  std::string path = pathutil::abspath(args.path);
  if (!is_directory(path)) {
    *ctx.err << "error: " << path << " is not a directory\n";
    return 1;
  }
  const bool use_git = kind == "repo" && !args.no_git;
  std::optional<std::string> git_root_opt;
  if (use_git) {
    git_root_opt = repo::git_root(path);
    if (git_root_opt) {
      path = *git_root_opt;
    }
  }
  const std::string name =
      args.name ? *args.name
                : (use_git ? repo::repo_name(path) : pathutil::basename(path));
  // v23: the (pre-version-split) source/repo dir is the repository's clone
  // path.
  const std::string clone_path = path;
  // v14: version auto-detection (split_base_version) then explicit override.
  std::optional<std::string> version_to_store;
  if (args.version_str) {
    version_to_store = *args.version_str;
  } else if (!args.no_detect_version) {
    const auto [base, seg] = CompileDb::split_base_version(path);
    if (!seg.empty()) {
      path = base; // store base without version segment
      version_to_store = seg;
    }
  }
  Storage db(ctx.index_path);
  const std::string repo_name_val = args.repo ? *args.repo : name;
  std::optional<std::string> remote_url =
      git_root_opt ? repo::git_remote_url(*git_root_opt)
                   : std::optional<std::string>{};
  // A direct source registration has no compile database, so use its declared
  // remote/path identity unless --universe explicitly composes it.
  const std::optional<int64_t> universe_id =
      db.add_semantic_universe(args.universe.value_or(
          "workspace:" + remote_url.value_or("path:" + path)));
  const std::optional<int64_t> repository_scope =
      args.universe || !db.get_repository_by_name(repo_name_val) ? universe_id
                                                                 : std::nullopt;
  // v24: a grouped component stores a clone-relative path, so re-adding the
  // same source resolves the EXISTING component clone-aware (its stored path is
  // no longer the absolute base) and refreshes its metadata in place; only a
  // genuinely new source mints a row. Mirrors Python cmd_add_source.
  int64_t cid;
  if (const auto existing = db.get_component(path); existing) {
    cid = existing->id;
    db.update_component_meta(cid, name, kind, version_to_store);
  } else {
    cid = db.add_component(name, path, kind, version_to_store);
  }
  // v23: group the component under a repository (same kind). The source dir is
  // its first clone and becomes active. Mirrors Python cmd_add_source.
  const int64_t rid =
      db.add_repository(repo_name_val, kind, remote_url, repository_scope);
  const int64_t clone_id = db.add_clone(rid, clone_path);
  const std::optional<Repository> repo = db.get_repository_by_id(rid);
  if (repo && !repo->active_clone_id) {
    db.set_active_clone(rid, clone_id);
  }
  db.set_component_repository(cid, rid);
  // v24: store the component path RELATIVE to its clone root so a later
  // `repo switch` only repoints the active clone (no path rewrite).
  db.relativize_component(cid, clone_path);
  *ctx.out << "component #" << cid << ": " << name << " (" << kind << ") at "
           << path << "\n";
  return 0;
}

// Advance a component's stored version when a ported compile command's -I sits
// under its version-stripped base and carries a numerically HIGHER version than
// the one registered AND that version directory EXISTS on disk ("check the path
// exists before you replace, even when it is newer"). An include whose version
// is missing on disk (or not higher) leaves the version as registered. The
// write goes through set_component_effective_version, which handles both the
// version-as-property and version-embedded-in-path representations and is a
// no-op for ambiguous multi-row names. Mirrors Python
// cli._bump_component_versions.
void bump_component_versions(Storage &db,
                             const std::vector<CompileCommand> &commands,
                             const std::vector<AliasEntry> &label_map) {
  const auto idx = db.component_alias_index();
  std::map<std::string, std::string> seen; // name -> highest version seen
  for (const CompileCommand &cmd : commands) {
    for (const std::string &val : CompileDb::include_values(cmd.args)) {
      if (val.contains('<') || val.contains('$') || !pathutil::isabs(val)) {
        continue;
      }
      const auto m = CompileDb::match_alias(pathutil::normpath(val), label_map);
      if (!m.has_value()) {
        continue;
      }
      const std::string &name = std::get<0>(*m);
      const std::string &vseg = std::get<1>(*m);
      if (vseg.empty()) {
        continue;
      }
      auto it = seen.find(name);
      if (it == seen.end() ||
          CompileDb::version_key(vseg) > CompileDb::version_key(it->second)) {
        seen[name] = vseg;
      }
    }
  }
  for (const auto &[name, vseg] : seen) {
    const auto it = idx.find(name);
    if (it == idx.end()) {
      continue;
    }
    const auto &[base, maxver, bumpable] = it->second;
    (void)bumpable;
    // Only a strictly-higher version is a candidate for replacement.
    if (!maxver.empty() &&
        CompileDb::version_key(vseg) <= CompileDb::version_key(maxver)) {
      continue;
    }
    // Existence guard: never repoint a component at a version dir absent from
    // disk, even when newer. `base` is the resolved, absolute, version-stripped
    // component base.
    std::error_code ec;
    if (!std::filesystem::is_directory(pathutil::join(base, vseg), ec)) {
      continue;
    }
    db.set_component_effective_version(name, vseg);
  }
}

int cmd_import(const ParsedArgs &args, Context &ctx) {
  std::vector<CompileCommand> commands;
  try {
    commands = CompileDb::load(args.db);
  } catch (const CidxError &) {
    // Python prints the cindex exception text; CompilationDatabaseError
    // formats as "Error 1: CompilationDatabase loading failed" for every
    // fromDirectory failure — reproduced verbatim for golden parity.
    *ctx.err << "error: cannot load compilation database from " << args.db
             << ": Error 1: CompilationDatabase loading failed\n";
    return 1;
  }
  if (commands.empty()) {
    *ctx.err << "error: compilation database is empty\n";
    return 1;
  }

  // Component root: the git repo owning the sources, else the directory
  // holding compile_commands.json (its basename names the component). The db
  // dir — not the first source's dir — keeps git-worktree checkouts, whose
  // `.git` is a file rather than a directory, rooted where their build db
  // lives.
  const std::string first_src = source_path(commands[0]);
  const std::optional<std::string> groot = repo::git_root(first_src);
  const std::string root =
      groot ? *groot : pathutil::abspath(CompileDb::db_dir_from_arg(args.db));
  const std::string name =
      args.name ? *args.name
                : (groot ? repo::repo_name(root) : pathutil::basename(root));

  // Version is a per-component property: import only AUTO-DETECTS a trailing
  // version segment (e.g. .../1.4.0). Manual version control lives in
  // `cidx component set-version`, not on import. Mirrors Python cmd_import.
  std::string stored_root = root;
  std::optional<std::string> version_to_store;
  {
    const auto [base, seg] = CompileDb::split_base_version(root);
    if (!seg.empty()) {
      stored_root = base;
      version_to_store = seg;
    }
  }

  int imported = 0;
  int skipped = 0;
  Storage db(ctx.index_path);
  const std::string repo_name_val = args.repo ? *args.repo : name;
  const std::optional<std::string> remote_url =
      groot ? repo::git_remote_url(*groot) : std::optional<std::string>{};
  const std::optional<int64_t> universe_id =
      db.add_semantic_universe(args.universe.value_or(
          build_evidence_universe_key(commands, root, remote_url)));
  const std::optional<int64_t> repository_scope =
      args.universe || !db.get_repository_by_name(repo_name_val) ? universe_id
                                                                 : std::nullopt;

  // Encode include paths against the alias registry unless --no-alias. The
  // registry is uniquely-named components (plus any stored labels), so an -I
  // under a component root auto-aliases to <component-name>. Decode
  // (get_alias) mirrors this same registry.
  // Mirrors Python cmd_import: build_label_map(db.list_alias_pairs(),
  // db.get_alias).
  std::vector<AliasEntry> label_map;
  if (!args.no_alias) {
    const auto pairs = db.list_alias_pairs();
    if (!pairs.empty()) {
      label_map = CompileDb::build_label_map(
          pairs, [&db](const std::string &n) { return db.get_alias(n); });
    }
  }
  // Version-agnostic port: a ported -I under a versioned component base may
  // carry a HIGHER version than the registered one — advance the stored
  // component version so <name> decodes to it (bumpable components only).
  if (!label_map.empty()) {
    bump_component_versions(db, commands, label_map);
  }

  if (args.force) {
    const std::optional<Component> existing = db.get_component(stored_root);
    if (existing) {
      // Resolved base (stored path is clone-relative for a grouped component,
      // v24); show the absolute path either way. Mirrors Python cmd_import.
      const std::string existing_base = db.component_abs_base(*existing);
      db.delete_component(existing->id);
      *ctx.out << "force: removed existing component #" << existing->id
               << " at " << existing_base << " (files and indexed symbols)\n";
    }
  }
  // The db-dir/git-root component is created LAZILY: only when a source matches
  // no already-registered component. Matching first means an import whose
  // sources are already covered by existing components (e.g. sub-components
  // Comp_1/Comp_2) does not spawn a spurious project component and re-home
  // those files under it. Mirrors Python cmd_import.
  std::optional<int64_t> root_cid;
  {
    Transaction txn = db.transaction();
    for (const CompileCommand &cmd : commands) {
      const std::string src = source_path(cmd);
      if (!db.component_for_path(src)) {
        if (!root_cid) {
          root_cid =
              db.add_component(name, stored_root, "repo", version_to_store);
          *ctx.out << "component #" << *root_cid << ": " << name << " at "
                   << stored_root << "\n";
          // The label_map was built BEFORE this lazily-created component
          // existed, so its own -I paths would store as absolute. Rebuild the
          // registry (now including it) and re-run the version bump so its
          // includes encode to <name>. Mirrors Python cmd_import.
          if (!args.no_alias) {
            const auto pairs = db.list_alias_pairs();
            label_map = pairs.empty() ? std::vector<AliasEntry>{}
                                      : CompileDb::build_label_map(
                                            pairs, [&db](const std::string &n) {
                                              return db.get_alias(n);
                                            });
            if (!label_map.empty()) {
              bump_component_versions(db, commands, label_map);
            }
          }
        }
        if (!db.component_for_path(src)) {
          *ctx.err << "  skip (outside any component): " << src << "\n";
          ++skipped;
          continue;
        }
      }
      // Apply alias_options after stripping (encode include paths).
      std::vector<std::string> opts = cmd.args;
      if (!label_map.empty()) {
        opts = CompileDb::alias_options(opts, label_map);
      }
      db.add_file_path(src, file_mtime(src), md5_of(src), opts, cmd.driver);
      ++imported;
    }
    txn.commit(); // R2: explicit commit so a COMMIT failure is not swallowed
  }
  // v23: group the imported components under a repository. Identity = --repo if
  // given, else the git/dir-derived name; remote_url (git checkout) lets two
  // worktrees map to one repository. The checkout dir (`root`) is registered as
  // a clone and made active when the repository has none. Mirrors Python.
  {
    const int64_t rid =
        db.add_repository(repo_name_val, "repo", remote_url, repository_scope);
    const int64_t clone_id = db.add_clone(rid, root);
    const std::optional<Repository> repo = db.get_repository_by_id(rid);
    if (repo && !repo->active_clone_id) {
      db.set_active_clone(rid, clone_id);
    }
    std::set<int64_t> attached;
    for (const CompileCommand &cmd : commands) {
      const std::optional<Component> comp =
          db.component_for_path(source_path(cmd));
      if (!comp || (attached.contains(comp->id))) {
        continue;
      }
      attached.insert(comp->id);
      if (!comp->repository_id) {
        db.set_component_repository(comp->id, rid);
      }
      // v24: store each grouped component's path RELATIVE to the clone root,
      // so `repo switch` repoints one pointer instead of N rows.
      db.relativize_component(comp->id, root);
    }
    *ctx.out << "repository '" << repo_name_val << "': " << attached.size()
             << " component(s)\n";
  }
  *ctx.out << "imported " << imported << " file(s), skipped " << skipped
           << "\n";
  return 0;
}

// cmd_index (cli.py:234-245) — the full §6.1 pipeline. libclang is NOT
// loaded eagerly: Parser::parse() loads it on first use (S05), so an index
// run with nothing to do succeeds without a libclang — exactly like the
// Python tool, whose cindex library loads lazily on the first parse.
int cmd_index(const ParsedArgs &args, Context &ctx) {
  Logger &log = ctx.logger != nullptr ? *ctx.logger : Logger::root();
  int rc = 0;
  {
    Storage db(ctx.index_path);
    // _source_root (cli.py:174-177): unknown --source name -> error, exit 1
    // (the warning-count line is NOT printed on this path — Python returns
    // from inside the `with` block before reaching it).
    std::optional<Component> comp;
    if (!lookup_component(db, args.source, comp, *ctx.err)) {
      return 1;
    }
    const std::optional<std::string> root =
        comp ? std::optional<std::string>(db.component_abs_base(*comp))
             : std::nullopt;
    // v7: --no-graph disables edge extraction for this run. Indexing runs
    // through the LibTooling engine (ast::run_index_one), which builds its own
    // toolchain + parse internally — no Parser/AstIndexer needed here.
    const bool graph_enabled = !args.no_graph;
    rc = !args.files.empty()
             ? index_files(db, args.files, root, graph_enabled, ctx)
             : index_pending(db, graph_enabled, ctx);
    if (rc == 0 && graph_enabled) {
      const TransformReport report = db.run_transform_pipeline();
      const bool failed =
          std::ranges::any_of(report.runs, [](const TransformRun &run) {
            return run.status == TransformRunStatus::failed;
          });
      if (failed) {
        for (const auto &run : report.runs) {
          if (run.status == TransformRunStatus::failed) {
            *ctx.err << "error: transform '" << run.transform_id
                     << "' failed: " << run.diagnostic << "\n";
          }
        }
        rc = 1;
      } else if (all_files_current(db)) {
        db.stamp_graph_resolved();
        db.stamp_index_identity();
      }
    }
  }
  // cli.py:243-244 — only when the file-sink warning counter is > 0 (G27).
  if (log.warning_count() > 0) {
    *ctx.out << log.warning_count() << " warning(s)/error(s) logged to "
             << pathutil::join(ctx.cache_dir, "cidx.log") << "\n";
  }
  return rc;
}

// -- query commands ------------------------------------------------------

} // namespace cidx::cli
