// search plus the list/show commands for components, dirs, files and symbols.
// Split out of commands.cpp; run_command's dispatch is unchanged.
#include "cli/commands_detail.hpp"

namespace cidx::cli {

int cmd_search(const ParsedArgs &args, Context &ctx) {
  Storage db(ctx.index_path);
  const std::vector<Symbol> hits = db.search_symbols(*args.pattern, args.kind);
  fmt::print_symbols(db, hits, args.limit, *ctx.out);
  return hits.empty() ? 1 : 0;
}

int cmd_list_components(const ParsedArgs &args, Context &ctx) {
  Storage db(ctx.index_path);
  const std::vector<Component> comps =
      db.list_components(args.pattern, args.kind);
  // v23: repository_id -> repository name, "-" when ungrouped.
  std::map<int64_t, std::string> repos;
  for (const Repository &r : db.list_repositories()) {
    repos[r.id] = r.name;
  }
  auto repo_name_of = [&](const Component &c) -> std::string {
    if (c.repository_id) {
      const auto it = repos.find(*c.repository_id);
      if (it != repos.end()) {
        return it->second;
      }
    }
    return "-";
  };
  std::size_t width = 0;
  std::size_t vw = 1;
  std::size_t rw = 1;
  for (const Component &c : comps) {
    width = std::max(width, c.name.size());
    if (c.version) {
      vw = std::max(vw, c.version->size());
    }
    rw = std::max(rw, repo_name_of(c).size());
  }
  for (const Component &c : comps) {
    const std::string ver = c.version ? *c.version : "-";
    const std::string rep = repo_name_of(c);
    // Show the resolved (clone-anchored) base, not the stored path, which is
    // relative for a grouped component (v24).
    // f"{c.id:>4}  {c.name:<{width}}  {c.kind:<8}  {ver:<{vw}}  {rep:<{rw}}  {base}"
    *ctx.out << fmt::rjust(std::to_string(c.id), 4) << "  "
             << fmt::ljust(c.name, width) << "  " << fmt::ljust(c.kind, 8)
             << "  " << fmt::ljust(ver, vw) << "  " << fmt::ljust(rep, rw)
             << "  " << db.component_abs_base(c) << "\n";
  }
  *ctx.out << comps.size() << " component(s)\n";
  return comps.empty() ? 1 : 0;
}

int cmd_list_dirs(const ParsedArgs &args, Context &ctx) {
  Storage db(ctx.index_path);
  std::optional<Component> comp;
  if (!lookup_component(db, args.component, comp, *ctx.err)) {
    return 1;
  }
  const auto rows = db.list_directories(
      comp ? std::optional<int64_t>(comp->id) : std::nullopt, args.pattern);
  std::size_t width = 0;
  for (const auto &row : rows) {
    width = std::max(width, row.second.size());
  }
  for (const auto &row : rows) {
    const Directory &d = row.first;
    // f"{d.id:>4}  {cname:<{width}}  {d.path or '.'}"
    *ctx.out << fmt::rjust(std::to_string(d.id), 4) << "  "
             << fmt::ljust(row.second, width) << "  "
             << (d.path.empty() ? "." : d.path) << "\n";
  }
  *ctx.out << rows.size() << " directory(ies)\n";
  return rows.empty() ? 1 : 0;
}

int cmd_list_files(const ParsedArgs &args, Context &ctx) {
  if (args.dir && (!args.component || args.component->empty())) {
    *ctx.err << kDirNeedsComponent << "\n";
    return 1;
  }
  Storage db(ctx.index_path);
  std::optional<Component> comp;
  if (!lookup_component(db, args.component, comp, *ctx.err)) {
    return 1;
  }
  // indexed = True if --indexed else False if --pending else None
  std::optional<bool> indexed;
  if (args.indexed) {
    indexed = true;
  } else if (args.pending) {
    indexed = false;
  }
  const auto rows =
      db.list_files(comp ? std::optional<int64_t>(comp->id) : std::nullopt,
                    args.dir, args.pattern, indexed);
  // Version is a per-component property; show each file's owning-component
  // version. Map file -> directory -> component -> version (two queries).
  std::unordered_map<int64_t, std::optional<std::string>> comp_ver;
  for (const Component &c : db.list_components()) {
    comp_ver[c.id] = c.version;
  }
  std::unordered_map<int64_t, int64_t> dir_comp;
  for (const auto &pr : db.list_directories()) {
    dir_comp[pr.first.id] = pr.first.component_id;
  }
  std::vector<std::string> vers;
  vers.reserve(rows.size());
  std::size_t vw = 1;
  for (const auto &row : rows) {
    std::string v = "-";
    const auto dit = dir_comp.find(row.first.directory_id);
    if (dit != dir_comp.end()) {
      const auto cit = comp_ver.find(dit->second);
      if (cit != comp_ver.end() && cit->second && !cit->second->empty()) {
        v = *cit->second;
      }
    }
    vw = std::max(vw, v.size());
    vers.push_back(v);
  }
  // Parse-diagnostic indicator: "-" clean, else e.g. "2E"/"3W"/"1E2W".
  const auto diag_counts = db.diagnostic_counts();
  std::vector<std::string> flags;
  flags.reserve(rows.size());
  std::size_t fw = 1;
  for (const auto &row : rows) {
    std::string flag = "-";
    const auto dit = diag_counts.find(row.first.id);
    if (dit != diag_counts.end()) {
      flag = diag_flag(dit->second);
    }
    fw = std::max(fw, flag.size());
    flags.push_back(flag);
  }
  for (std::size_t k = 0; k < rows.size(); ++k) {
    const File &rec = rows[k].first;
    const char *mark = rec.indexed ? "idx " : "pend";
    // f"{rec.id:>4}  {mark}  {flag:<{fw}}  {ver:<{vw}}  {path}"
    *ctx.out << fmt::rjust(std::to_string(rec.id), 4) << "  " << mark << "  "
             << fmt::ljust(flags[k], fw) << "  " << fmt::ljust(vers[k], vw)
             << "  " << rows[k].second << "\n";
  }
  *ctx.out << rows.size() << " file(s)\n";
  return rows.empty() ? 1 : 0;
}

int cmd_list_symbols(const ParsedArgs &args, Context &ctx) {
  if (args.dir && (!args.component || args.component->empty())) {
    *ctx.err << kDirNeedsComponent << "\n";
    return 1;
  }
  Storage db(ctx.index_path);
  std::optional<Component> comp;
  if (!lookup_component(db, args.component, comp, *ctx.err)) {
    return 1;
  }
  std::optional<int64_t> file_id;
  if (args.file_filter && !args.file_filter->empty()) {
    const std::string path = files::resolve_file_arg(
        *args.file_filter,
        comp ? std::optional<std::string>(db.component_abs_base(*comp)) : std::nullopt);
    const std::optional<File> rec = db.get_file(path);
    if (!rec) {
      *ctx.err << "error: not in index database: " << path << "\n";
      return 1;
    }
    file_id = rec->id;
  }
  const std::vector<Symbol> hits =
      db.list_symbols(comp ? std::optional<int64_t>(comp->id) : std::nullopt,
                      args.dir, file_id, args.pattern, args.kind);
  fmt::print_symbols(db, hits, args.limit, *ctx.out);
  return hits.empty() ? 1 : 0;
}

int cmd_show_symbol(const ParsedArgs &args, Context &ctx) {
  Storage db(ctx.index_path);
  const std::string &ref = args.symbol;
  const std::optional<Symbol> s =
      is_digits(ref)
          ? db.lookup_symbol_by_id(std::strtoll(ref.c_str(), nullptr, 10))
          : db.lookup_symbol(ref);
  if (!s) {
    *ctx.err << "error: no symbol with id/USR " << fmt::py_repr(ref) << "\n";
    return 1;
  }

  const auto loc =
      [&db](const std::optional<int64_t> &file_id,
            const std::optional<int64_t> &line,
            const std::optional<int64_t> &col) -> std::optional<std::string> {
    if (!file_id) {
      return std::nullopt;
    }
    return fmt::py_str(db.file_abs_path(*file_id)) + ":" + fmt::py_str(line) +
           ":" + fmt::py_str(col);
  };

  const std::optional<Symbol> parent =
      (s->parent_usr && !s->parent_usr->empty())
          ? db.lookup_symbol(*s->parent_usr)
          : std::nullopt;

  std::optional<std::string> visibility;
  if (s->linkage) {
    if (*s->linkage == "external") {
      visibility = "program-wide (usable from any .cpp)";
    } else if (*s->linkage == "internal") {
      visibility = "file-local (static / anonymous namespace)";
    } else if (*s->linkage == "no-linkage") {
      visibility = "local scope only";
    } else {
      visibility = *s->linkage;
    }
  }

  std::optional<std::string> parent_field = s->parent_usr;
  if (parent) {
    parent_field =
        fmt::py_str(parent->qual_name) + "  [" + *s->parent_usr + "]";
  }

  // declaration: a registered decl site, else the raw external decl_path of a
  // stub whose target lives in an unregistered (system/stdlib) file.
  std::optional<std::string> declaration =
      loc(s->decl_file_id, s->decl_line, s->decl_col);
  if (!declaration && s->decl_path) {
    declaration = *s->decl_path + ":" + fmt::py_str(s->decl_line) + ":" +
                  fmt::py_str(s->decl_col);
  }

  const std::vector<std::pair<const char *, std::optional<std::string>>>
      fields = {
          {"id", std::to_string(s->id)},
          {"usr", s->usr},
          {"name", s->spelling},
          {"qualified", s->qual_name},
          {"display", s->display_name},
          {"kind", s->kind},
          {"type", s->type_info},
          {"visibility", visibility},
          {"access", s->access},
          {"parent", parent_field},
          {"pure", s->is_pure ? std::optional<std::string>(
                                    "yes (pure virtual; implemented by "
                                    "overriders)")
                              : std::nullopt},
          {"definition",
           s->is_definition ? loc(s->file_id, s->line, s->col) : std::nullopt},
          {"declaration", declaration},
          {"resolved", s->resolved  ? std::string("yes")
                       : s->is_pure ? std::string("n/a (pure virtual)")
                                    : std::string("no (definition not seen)")},
      };
  for (const auto &field : fields) {
    if (field.second) {
      fmt::print_field(*ctx.out, field.first, *field.second);
    }
  }
  return 0;
}

int cmd_show_file(const ParsedArgs &args, Context &ctx) {
  Storage db(ctx.index_path);
  std::optional<Component> comp;
  if (!lookup_component(db, args.component, comp, *ctx.err)) {
    return 1;
  }
  const std::string &ref = args.file;
  std::optional<File> rec;
  std::optional<std::string> path;
  if (is_digits(ref)) { // first column of 'list files'
    rec = db.get_file_by_id(std::strtoll(ref.c_str(), nullptr, 10));
    if (rec) {
      path = db.file_abs_path(rec->id);
    }
  } else {
    path = files::resolve_file_arg(
        ref, comp ? std::optional<std::string>(db.component_abs_base(*comp)) : std::nullopt);
    rec = db.get_file(*path);
  }
  if (!rec || !path) {
    *ctx.err << "error: not in index database: " << ref << "\n";
    return 1;
  }

  const std::optional<Directory> d = db.get_directory_by_id(rec->directory_id);
  const std::optional<Component> owner =
      d ? db.get_component_by_id(d->component_id) : std::nullopt;
  const std::vector<Symbol> syms =
      db.list_symbols(std::nullopt, std::nullopt, rec->id);
  int64_t defined = 0;
  int64_t declared = 0;
  std::map<std::string, int64_t> by_kind;
  for (const Symbol &s : syms) {
    if (s.file_id && *s.file_id == rec->id && s.is_definition) {
      ++defined;
    }
    if (s.decl_file_id && *s.decl_file_id == rec->id) {
      ++declared;
    }
    ++by_kind[s.kind];
  }
  const std::vector<Diagnostic> diags = db.get_diagnostics(rec->id);
  std::map<int, int64_t> diag_counts;
  for (const Diagnostic &dg : diags) {
    ++diag_counts[dg.severity];
  }
  std::optional<std::string> by_kind_field;
  if (!by_kind.empty()) { // std::map iterates sorted — Python sorted(items)
    std::string joined;
    for (const auto &entry : by_kind) {
      if (!joined.empty()) {
        joined += ", ";
      }
      joined += entry.first + ": " + std::to_string(entry.second);
    }
    by_kind_field = joined;
  }
  std::optional<std::string> options_field;
  if (rec->compile_options && !rec->compile_options->empty()) {
    std::string joined;
    for (const std::string &opt : *rec->compile_options) {
      if (!joined.empty()) {
        joined += " ";
      }
      joined += opt;
    }
    options_field = joined;
  } else {
    options_field = "(none -- header indexed via an including TU)";
  }

  const std::vector<std::pair<const char *, std::optional<std::string>>>
      fields = {
          {"id", std::to_string(rec->id)},
          {"path", *path},
          {"component",
           owner ? std::optional<std::string>(owner->name + " (" + owner->kind +
                                              ")  " + owner->path)
                 : std::nullopt},
          {"directory",
           d ? std::optional<std::string>(d->path.empty() ? "." : d->path)
             : std::nullopt},
          {"mtime", rec->mtime ? std::optional<std::string>(
                                     fmt::format_mtime(*rec->mtime))
                               : std::nullopt},
          {"md5", rec->md5},
          {"driver", rec->driver},
          {"options", options_field},
          {"indexed", std::string(files::index_status_reason(
                          files::index_status(*rec, *path)))},
          {"indexed at", rec->indexed_at ? std::optional<std::string>(
                                               *rec->indexed_at + " UTC")
                                         : std::nullopt},
          {"symbols", std::to_string(syms.size()) + " (" +
                          std::to_string(defined) + " defined here, " +
                          std::to_string(declared) + " declared here)"},
          {"by kind", by_kind_field},
          {"diagnostics", diags.empty()
                              ? std::nullopt
                              : std::optional<std::string>(
                                    diag_summary(diag_counts))},
      };
  for (const auto &field : fields) {
    if (field.second) {
      fmt::print_field(*ctx.out, field.first, *field.second);
    }
  }
  // Each captured parse diagnostic, in TU order, under the summary.
  for (const Diagnostic &dg : diags) {
    const char *lbl = diag_severity_label(dg.severity);
    const std::string label =
        lbl != nullptr ? std::string(lbl) : std::to_string(dg.severity);
    const std::string locstr =
        dg.file_path ? (*dg.file_path + ":" + std::to_string(*dg.line) + ":" +
                        std::to_string(*dg.col))
                     : std::string("<no location>");
    *ctx.out << "  " << fmt::ljust(label, 7) << " " << locstr << ": "
             << dg.spelling << "\n";
  }
  return 0;
}

} // namespace cidx::cli
