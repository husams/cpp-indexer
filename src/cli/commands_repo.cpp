// component and repository commands, plus verify and realias.
// Split out of commands.cpp; run_command's dispatch is unchanged.
#include "cli/commands_detail.hpp"

namespace cidx::cli {

int cmd_component_show(const ParsedArgs &args, Context &ctx) {
  const std::string name = args.name ? *args.name : std::string();
  Storage db(ctx.index_path);
  std::optional<Component> comp = db.get_component_by_name(name);
  if (!comp) {
    *ctx.err << "error: no component named '" << name << "'\n";
    return 1;
  }
  // Output: key-value table, 14-char left-justified key col.
  // Byte-identical with Python: f"{key:<14} {value}"
  // Keys: name, kind, "base path", version, "effective root", "resolved root"
  const std::string eff = Storage::effective_root(*comp);
  const std::string resolved = db.component_abs_base(*comp);
  auto row = [&](const std::string &key, const std::string &val) {
    *ctx.out << fmt::ljust(key, 14) << " " << val << "\n";
  };
  row("name", comp->name);
  row("kind", comp->kind);
  row("base path", comp->path);
  row("version", comp->version ? *comp->version : "(none)");
  row("effective root", eff);
  row("resolved root", resolved);
  return 0;
}

int cmd_component_set_version(const ParsedArgs &args, Context &ctx) {
  const std::string name = args.name ? *args.name : std::string();
  Storage db(ctx.index_path);
  // args.version_str absent or empty means clear the version.
  const std::optional<std::string> ver =
      (args.version_str && !args.version_str->empty())
          ? args.version_str
          : std::optional<std::string>{};
  const bool ok = db.set_component_version(name, ver);
  if (!ok) {
    *ctx.err << "error: no component named '" << name << "'\n";
    return 1;
  }
  if (ver) {
    // Python: f"component '{name}' version set to {version}" — unquoted version.
    *ctx.out << "component '" << name << "' version set to " << *ver << "\n";
  } else {
    *ctx.out << "component '" << name << "' version cleared\n";
  }
  return 0;
}

// -- repo list / show / add-clone / switch / rm (v23) ----------------------


namespace {
// Resolved path of a repository's active clone, or nullopt.
std::optional<std::string> active_clone_path(Storage &db,
                                             const Repository &repo) {
  if (!repo.active_clone_id) {
    return std::nullopt;
  }
  const std::optional<Clone> cl = db.get_clone_by_id(*repo.active_clone_id);
  if (!cl) {
    return std::nullopt;
  }
  return cl->path;
}
} // namespace

int cmd_repo_list(const ParsedArgs &args, Context &ctx) {
  Storage db(ctx.index_path);
  const std::vector<Repository> repos = db.list_repositories(args.pattern,
                                                             args.kind);
  std::size_t width = 0;
  for (const Repository &r : repos) {
    width = std::max(width, r.name.size());
  }
  for (const Repository &r : repos) {
    const std::size_t ncomp = db.components_for_repository(r.id).size();
    const std::size_t nclone = db.list_clones(r.id).size();
    const std::optional<std::string> ap = active_clone_path(db, r);
    const std::string active = ap ? *ap : "-";
    // f"{r.id:>4}  {r.name:<{width}}  {r.kind:<8}  "
    // f"{ncomp} component(s)  {nclone} clone(s)  {active}"
    *ctx.out << fmt::rjust(std::to_string(r.id), 4) << "  "
             << fmt::ljust(r.name, width) << "  " << fmt::ljust(r.kind, 8)
             << "  " << ncomp << " component(s)  " << nclone << " clone(s)  "
             << active << "\n";
  }
  *ctx.out << repos.size() << " repositories\n";
  return repos.empty() ? 1 : 0;
}

int cmd_repo_show(const ParsedArgs &args, Context &ctx) {
  const std::string name = args.name ? *args.name : std::string();
  Storage db(ctx.index_path);
  const std::optional<Repository> repo = db.get_repository_by_name(name);
  if (!repo) {
    *ctx.err << "error: no repository named '" << name << "'\n";
    return 1;
  }
  const std::vector<Clone> clones = db.list_clones(repo->id);
  const std::vector<Component> comps = db.components_for_repository(repo->id);
  const std::string remote = repo->remote_url ? *repo->remote_url : "(none)";
  const std::optional<std::string> ap = active_clone_path(db, *repo);
  const std::string active = ap ? *ap : "(none)";
  auto row = [&](const std::string &key, const std::string &val) {
    *ctx.out << fmt::ljust(key, 14) << " " << val << "\n";
  };
  row("name", repo->name);
  row("kind", repo->kind);
  row("remote", remote);
  row("active clone", active);
  row("clones", std::to_string(clones.size()));
  row("components", std::to_string(comps.size()));
  if (!clones.empty()) {
    std::size_t lw = 1;
    for (const Clone &c : clones) {
      if (c.label) {
        lw = std::max(lw, c.label->size());
      }
    }
    *ctx.out << "clones:\n";
    for (const Clone &c : clones) {
      const std::string mark =
          (repo->active_clone_id && c.id == *repo->active_clone_id) ? "*"
                                                                    : " ";
      const std::string lbl = c.label ? *c.label : "-";
      // f"  {mark} {c.id:>4}  {lbl:<{lw}}  {c.path}"
      *ctx.out << "  " << mark << " " << fmt::rjust(std::to_string(c.id), 4)
               << "  " << fmt::ljust(lbl, lw) << "  " << c.path << "\n";
    }
  }
  if (!comps.empty()) {
    std::size_t cw = 0;
    for (const Component &c : comps) {
      cw = std::max(cw, c.name.size());
    }
    *ctx.out << "components:\n";
    for (const Component &c : comps) {
      // f"    {c.id:>4}  {c.name:<{cw}}  {base}" (resolved, clone-anchored)
      *ctx.out << "    " << fmt::rjust(std::to_string(c.id), 4) << "  "
               << fmt::ljust(c.name, cw) << "  " << db.component_abs_base(c)
               << "\n";
    }
  }
  return 0;
}

int cmd_repo_add_clone(const ParsedArgs &args, Context &ctx) {
  const std::string name = args.name ? *args.name : std::string();
  Storage db(ctx.index_path);
  const std::optional<Repository> repo = db.get_repository_by_name(name);
  if (!repo) {
    *ctx.err << "error: no repository named '" << name << "'\n";
    return 1;
  }
  if (!is_directory(pathutil::abspath(args.path))) {
    *ctx.err << "warning: " << args.path
             << " is not a directory (registered anyway)\n";
  }
  const int64_t clone_id = db.add_clone(repo->id, args.path, args.repo_label);
  if (!repo->active_clone_id) {
    db.set_active_clone(repo->id, clone_id);
  }
  const std::optional<Clone> cl = db.get_clone_by_id(clone_id);
  *ctx.out << "clone #" << clone_id << ": " << (cl ? cl->path : std::string())
           << " added to '" << name << "'\n";
  return 0;
}

int cmd_repo_switch(const ParsedArgs &args, Context &ctx) {
  const std::string name = args.name ? *args.name : std::string();
  Storage db(ctx.index_path);
  const std::optional<Repository> repo = db.get_repository_by_name(name);
  if (!repo) {
    *ctx.err << "error: no repository named '" << name << "'\n";
    return 1;
  }
  const std::vector<Clone> clones = db.list_clones(repo->id);
  const std::string want = pathutil::abspath(args.target);
  std::optional<Clone> target;
  for (const Clone &c : clones) {
    if (c.path == want || (c.label && *c.label == args.target)) {
      target = c;
      break;
    }
  }
  if (!target) {
    *ctx.err << "error: '" << args.target << "' is not a clone of '" << name
             << "' (add it with 'cidx repo add-clone')\n";
    return 1;
  }
  if (!is_directory(target->path)) {
    *ctx.err << "warning: clone path " << target->path
             << " is not present on disk\n";
  }
  // v24: grouped component paths are stored RELATIVE to the active clone, so a
  // switch is a single `active_clone_id` update -- no component row rewrite.
  const std::size_t ncomp = db.components_for_repository(repo->id).size();
  db.set_active_clone(repo->id, target->id);
  *ctx.out << "switched '" << name << "' to " << target->path << " (" << ncomp
           << " component(s))\n";
  return 0;
}

int cmd_repo_rm(const ParsedArgs &args, Context &ctx) {
  const std::string name = args.name ? *args.name : std::string();
  Storage db(ctx.index_path);
  const std::optional<Repository> repo = db.get_repository_by_name(name);
  if (!repo) {
    *ctx.err << "error: no repository named '" << name << "'\n";
    return 1;
  }
  const std::vector<Component> comps = db.components_for_repository(repo->id);
  if (args.delete_components) {
    for (const Component &c : comps) {
      db.delete_component(c.id);
    }
  }
  db.delete_repository(repo->id);
  if (args.delete_components) {
    *ctx.out << "removed repository '" << name << "' and " << comps.size()
             << " component(s)\n";
  } else {
    *ctx.out << "removed repository '" << name << "'; " << comps.size()
             << " component(s) detached\n";
  }
  return 0;
}

int cmd_verify(const ParsedArgs &args, Context &ctx) {
  // Byte-identical with Python cmd_verify (cli.py):
  //   component  {status:<8}  {name}  {resolved}
  //   file  MISSING   {path}    (only failures unless --all; OK = "ok" pad 8)
  // followed by the two summary lines. Exit 1 if anything is missing.
  Storage db(ctx.index_path);

  std::vector<Component> components;
  std::optional<int64_t> scope;
  if (args.component) {
    std::optional<Component> comp = db.get_component_by_name(*args.component);
    if (!comp) {
      // Parity: Python LookupError str is "no component named 'NAME'".
      *ctx.err << "error: no component named '" << *args.component << "'\n";
      return 1;
    }
    components.push_back(*comp);
    scope = comp->id;
  } else {
    components = db.list_components();
  }

  auto is_reg_file = [](const std::string &p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
  };

  int c_ok = 0;
  int c_missing = 0;
  int c_vermiss = 0;
  for (const Component &c : components) {
    const std::string resolved = db.component_abs_base(c);
    std::string status;
    if (is_directory(resolved)) {
      status = "ok";
      ++c_ok;
    } else if (c.version && is_directory([&] {
                 Component base = c;
                 base.version = std::nullopt;
                 return db.component_abs_base(base);
               }())) {
      status = "VER-MISS";
      ++c_vermiss;
    } else {
      status = "MISSING";
      ++c_missing;
    }
    *ctx.out << "component  " << fmt::ljust(status, 8) << "  " << c.name << "  "
             << resolved << "\n";
  }

  int f_ok = 0;
  int f_missing = 0;
  for (const auto &[rec, path] : db.list_files(scope)) {
    if (is_reg_file(path)) {
      ++f_ok;
      if (args.all) {
        *ctx.out << "file  ok        " << path << "\n";
      }
    } else {
      ++f_missing;
      *ctx.out << "file  MISSING   " << path << "\n";
    }
  }

  *ctx.out << "\n";
  *ctx.out << "components: " << c_ok << " ok, " << c_missing << " missing, "
           << c_vermiss << " version-mismatch\n";
  *ctx.out << "files: " << f_ok << " ok, " << f_missing << " missing\n";
  return (c_missing == 0 && c_vermiss == 0 && f_missing == 0) ? 0 : 1;
}

// cmd_realias (cli.py cmd_realias): rewrite stored include paths to <label>
// tokens via the registry. Optional COMPONENT restricts to one component.
// Port of Python cmd_realias, byte-identical output strings.
int cmd_realias(const ParsedArgs &args, Context &ctx) {
  Storage db(ctx.index_path);
  const auto pairs = db.list_alias_pairs();
  const auto label_map = CompileDb::build_label_map(
      pairs, [&db](const std::string &n) { return db.get_alias(n); });
  if (label_map.empty()) {
    *ctx.err << "error: no aliases available (add a component first)\n";
    return 1;
  }
  std::optional<int64_t> cid;
  if (args.component && !args.component->empty()) {
    const std::optional<Component> comp =
        db.get_component_by_name(*args.component);
    if (!comp) {
      *ctx.err << "error: no component named '" << *args.component << "'\n";
      return 1;
    }
    cid = comp->id;
  }
  int64_t changed = 0;
  int64_t scanned = 0;
  for (const auto &row : db.list_files(cid)) {
    const File &rec = row.first;
    if (!rec.compile_options || rec.compile_options->empty() || rec.id == 0) {
      continue;
    }
    ++scanned;
    const std::vector<std::string> cur(*rec.compile_options);
    const std::vector<std::string> nw = CompileDb::alias_options(cur, label_map);
    if (nw != cur) {
      db.update_file_compile_options(rec.id, nw);
      ++changed;
    }
  }
  *ctx.out << "realias: " << changed << " file(s) updated, " << scanned
           << " scanned\n";
  return 0;
}

} // namespace cidx::cli
