// delete-component/dir/file/symbol and resolve.
// Split out of commands.cpp; run_command's dispatch is unchanged.
#include "cli/commands_detail.hpp"

namespace cidx::cli {

int cmd_delete_component(const ParsedArgs &args, Context &ctx) {
  Storage db(ctx.index_path);
  std::vector<Component> matches;
  if (args.del_id) {
    if (std::optional<Component> c = db.get_component_by_id(*args.del_id)) {
      matches.push_back(*c);
    }
  } else if (args.del_path) {
    if (std::optional<Component> c =
            db.get_component(pathutil::abspath(*args.del_path))) {
      matches.push_back(*c);
    }
  } else { // name
    for (const Component &c : db.list_components()) {
      if (c.name == *args.name) {
        matches.push_back(c);
      }
    }
  }
  if (matches.empty()) {
    *ctx.err << "error: no component matches " << selector_str(args) << "\n";
    return 1;
  }
  std::vector<int64_t> ids;
  std::vector<std::string> lines;
  for (const Component &c : matches) {
    ids.push_back(c.id);
    lines.push_back("  #" + std::to_string(c.id) + "  " + c.name + " (" +
                    c.kind + ")  " + c.path);
  }
  return finish_delete(
      args, ctx, ids, lines, [&db](int64_t id) { db.delete_component(id); },
      "component", "components");
}

int cmd_delete_dir(const ParsedArgs &args, Context &ctx) {
  Storage db(ctx.index_path);
  std::optional<Component> comp;
  if (!lookup_component(db, args.component, comp, *ctx.err)) {
    return 1;
  }
  std::vector<Directory> matches;
  if (args.del_id) {
    if (std::optional<Directory> d = db.get_directory_by_id(*args.del_id)) {
      if (under_component(db, db.directory_abs_path(d->id), comp)) {
        matches.push_back(*d);
      }
    }
  } else { // path
    const std::string target = pathutil::abspath(*args.del_path);
    const std::optional<int64_t> scope =
        comp ? std::optional<int64_t>(comp->id) : std::nullopt;
    for (const std::pair<Directory, std::string> &pr :
         db.list_directories(scope)) {
      const std::optional<std::string> ap = db.directory_abs_path(pr.first.id);
      if (ap && *ap == target) {
        matches.push_back(pr.first);
      }
    }
  }
  if (matches.empty()) {
    *ctx.err << "error: no directory matches " << selector_str(args) << "\n";
    return 1;
  }
  std::vector<int64_t> ids;
  std::vector<std::string> lines;
  for (const Directory &d : matches) {
    ids.push_back(d.id);
    lines.push_back("  #" + std::to_string(d.id) + "  " +
                    db.directory_abs_path(d.id).value_or(""));
  }
  return finish_delete(
      args, ctx, ids, lines, [&db](int64_t id) { db.delete_directory(id); },
      "directory", "directories");
}

int cmd_delete_file(const ParsedArgs &args, Context &ctx) {
  Storage db(ctx.index_path);
  std::optional<Component> comp;
  if (!lookup_component(db, args.component, comp, *ctx.err)) {
    return 1;
  }
  std::vector<std::pair<int64_t, std::string>> matches; // (id, abs path)
  if (args.del_id) {
    if (std::optional<File> rec = db.get_file_by_id(*args.del_id)) {
      const std::optional<std::string> ap = db.file_abs_path(rec->id);
      if (under_component(db, ap, comp)) {
        matches.emplace_back(rec->id, ap.value_or(""));
      }
    }
  } else if (args.del_path) {
    const std::string ap = files::resolve_file_arg(
        *args.del_path,
        comp ? std::optional<std::string>(db.component_abs_base(*comp))
             : std::nullopt);
    if (std::optional<File> rec = db.get_file(ap)) {
      if (under_component(db, ap, comp)) {
        matches.emplace_back(rec->id, ap);
      }
    }
  } else { // name (basename)
    for (const std::pair<File, std::string> &pr : db.list_files()) {
      if (pathutil::basename(pr.second) == *args.name &&
          under_component(db, pr.second, comp)) {
        matches.emplace_back(pr.first.id, pr.second);
      }
    }
  }
  if (matches.empty()) {
    *ctx.err << "error: no file matches " << selector_str(args) << "\n";
    return 1;
  }
  std::vector<int64_t> ids;
  std::vector<std::string> lines;
  for (const std::pair<int64_t, std::string> &m : matches) {
    ids.push_back(m.first);
    lines.push_back("  #" + std::to_string(m.first) + "  " + m.second);
  }
  return finish_delete(
      args, ctx, ids, lines, [&db](int64_t id) { db.delete_file(id); }, "file",
      "files");
}

int cmd_delete_symbol(const ParsedArgs &args, Context &ctx) {
  Storage db(ctx.index_path);
  std::optional<Component> comp;
  if (!lookup_component(db, args.component, comp, *ctx.err)) {
    return 1;
  }
  std::vector<Symbol> matches;
  if (args.del_id) {
    if (std::optional<Symbol> s = db.lookup_symbol_by_id(*args.del_id)) {
      matches.push_back(*s);
    }
  } else if (args.usr) {
    if (std::optional<Symbol> s = db.lookup_symbol(*args.usr)) {
      matches.push_back(*s);
    }
  } else { // name (spelling)
    matches = db.lookup_symbols_by_name(*args.name);
  }
  if (comp) {
    std::vector<Symbol> kept;
    for (const Symbol &s : matches) {
      const std::optional<std::string> here =
          s.file_id ? db.file_abs_path(*s.file_id) : std::nullopt;
      const std::optional<std::string> decl =
          s.decl_file_id ? db.file_abs_path(*s.decl_file_id) : std::nullopt;
      if ((here && under_component(db, here, comp)) ||
          (decl && under_component(db, decl, comp))) {
        kept.push_back(s);
      }
    }
    matches = kept;
  }
  if (matches.empty()) {
    *ctx.err << "error: no symbol matches " << selector_str(args) << "\n";
    return 1;
  }
  std::vector<int64_t> ids;
  std::vector<std::string> lines;
  for (const Symbol &s : matches) {
    ids.push_back(s.id);
    const std::string qual =
        (s.qual_name && !s.qual_name->empty()) ? *s.qual_name : s.spelling;
    lines.push_back("  #" + std::to_string(s.id) + "  " + s.kind + "  " + qual);
  }
  return finish_delete(
      args, ctx, ids, lines, [&db](int64_t id) { db.delete_symbol(id); },
      "symbol", "symbols");
}

int cmd_resolve(const ParsedArgs &args, Context &ctx) {
  (void)args;
  Storage db(ctx.index_path);
  const int stubs = db.resolve_pass();
  const std::vector<Edge> cross = db.cross_repo_edges();
  db.stamp_graph_resolved();
  *ctx.out << "resolve: " << stubs << " still-stub, " << cross.size()
           << " cross-repo edge(s)\n";
  return 0;
}

} // namespace cidx::cli
