// argv grammar, built on the vendored CLI11 library (third_party/CLI11.hpp).
// parse_args() constructs the App tree per call (no global state), binds
// options straight into ParsedArgs, and maps every CLI11 parse failure to
// UsageError (exit 2, D23: main() is the only catch-site) with the message
//   "<Usage line>\n<prog>: error: <detail>\n"
// -h/--help anywhere fills ParsedArgs::help_text with CLI11's generated help;
// --version at the top level sets ParsedArgs::version. Help/usage/error text
// is CLI11's native formatting — the former byte-for-byte Python argparse
// transcription is retired.
#include "cli/args.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "CLI11.hpp"
#include "util/errors.hpp"
#include "util/pathutil.hpp"

namespace cidx::cli {
namespace {

const std::vector<std::string> kSymbolKinds = {
    "class",   "class-template", "constructor", "destructor",
    "enum",    "enum-constant",  "function",    "function-template",
    "macro",   "member",         "method",      "namespace",
    "struct",  "type-alias",     "typedef",     "union",
    "variable"};
const std::vector<std::string> kComponentKinds = {"repo", "external"};
const std::vector<std::string> kDirections = {"in", "out"};
const std::vector<std::string> kAccessLevels = {"public", "protected",
                                                "private", "all"};

const char kDbHelpText[] = "index database (default: the standard cache index)";
const char kAltDbHelpText[] =
    "operate on this index DB (default: the standard index)";
const char kPatternHelpText[] =
    "optional free-text fuzzy filter: characters must appear in order, "
    "e.g. 'shp' matches shapes.c";

// Deepest parsed subcommand and its "cidx file set"-style program path.
std::pair<const CLI::App *, std::string> parsed_leaf(const CLI::App &root) {
  const CLI::App *leaf = &root;
  std::string prog = root.get_name();
  while (true) {
    const std::vector<CLI::App *> subs = leaf->get_subcommands();
    if (subs.empty()) {
      break;
    }
    leaf = subs[0];
    prog += " " + leaf->get_name();
  }
  return {leaf, prog};
}

[[noreturn]] void usage_fail(const CLI::App &root, const CLI::ParseError &e) {
  const auto [leaf, prog] = parsed_leaf(root);
  std::string usage;
  if (const auto fmt = std::dynamic_pointer_cast<const CLI::Formatter>(
          leaf->get_formatter())) {
    usage = fmt->make_usage(leaf, prog);
  }
  std::string detail = e.what();
  // Tokens nothing recognized (a typo'd subcommand, an unknown flag) end up
  // in the leaf's leftovers; name them instead of erroring opaquely.
  // ExtrasError already lists them itself.
  const std::vector<std::string> extras =
      dynamic_cast<const CLI::ExtrasError *>(&e) != nullptr
          ? std::vector<std::string>{}
          : leaf->remaining(/*recurse=*/false);
  if (!extras.empty()) {
    detail += " (unrecognized: ";
    for (std::size_t i = 0; i < extras.size(); ++i) {
      detail += (i != 0 ? " " : "") + extras[i];
    }
    detail += ")";
  }
  throw UsageError(usage + prog + ": error: " + detail + "\n", 2);
}

// Shared symbol selector of the graph sub-commands: exactly one of
// --usr | --id | --name, plus the common query options.
void add_graph_selector(CLI::App *sub, ParsedArgs &pa) {
  CLI::Option_group *sel =
      sub->add_option_group("selector", "symbol to start from");
  sel->add_option("--usr", pa.usr, "exact clang USR");
  sel->add_option("--id", pa.graph_id, "numeric symbol id");
  sel->add_option("--name", pa.name,
                  "fuzzy qualified-name match ('conf::set')");
  sel->require_option(1);
  sub->add_option("--kind", pa.kind,
                  "restrict a --name match to one symbol kind")
      ->check(CLI::IsMember(kSymbolKinds));
  sub->add_flag("--first", pa.first,
                "if --name is ambiguous, take the closest match");
  sub->add_option(
      "--db", pa.index_db,
      "index database to query (default: the standard cache index)");
  sub->add_flag("--json", pa.graph_json, "emit stable machine-readable JSON");
  sub->add_option("--limit", pa.graph_limit,
                  "cap the number of results (default 50)");
}

void build_top_level(CLI::App &app, ParsedArgs &pa) {
  CLI::App *init = app.add_subcommand("init", "create a blank index database");
  init->add_flag("--force", pa.force, "overwrite an existing index database");
  init->callback([&pa] { pa.command = "init"; });

  CLI::App *import =
      app.add_subcommand("import", "import a compile_commands.json");
  import
      ->add_option("--db", pa.db,
                   "compile_commands.json (or the directory holding it)")
      ->required();
  import->add_option("--name", pa.name, "component name override");
  import->add_option("--repo", pa.repo,
                     "repository name to group the imported components under "
                     "(default: the git/dir-derived name)");
  import->add_option("--universe", pa.universe,
                     "portable semantic universe key for this repository");
  import->add_flag("--force", pa.force,
                   "reimport: delete the existing component (its files and "
                   "indexed symbols) before importing");
  import->add_flag("--no-alias", pa.no_alias,
                   "do not rewrite include paths to <label> tokens via the "
                   "registry");
  import->callback([&pa] { pa.command = "import"; });

  CLI::App *index = app.add_subcommand("index", "index imported C/C++ files");
  index->add_option("files", pa.files,
                    "restrict to these files (default: all pending)");
  index->add_option("--source", pa.source,
                    "resolve relative FILE paths against this component's "
                    "root");
  index->add_flag("--no-graph", pa.no_graph,
                  "skip relationship-graph extraction (calls, inherits, ...)");
  index->add_flag("--no-autoderive-labels", pa.no_autoderive_labels,
                  "disable label autoderive fallback at parse time");
  index->callback([&pa] { pa.command = "index"; });

  CLI::App *resolve = app.add_subcommand(
      "resolve", "finalize cross-repo edges and roll up edge counts");
  resolve->callback([&pa] { pa.command = "resolve"; });

  CLI::App *search =
      app.add_subcommand("search", "fuzzy-search symbols by qualified name");
  search
      ->add_option("pattern", pa.pattern,
                   "'::'-separated substrings matched in order, e.g. "
                   "'conf::set' hits RdKafka::Conf::set")
      ->required();
  search->add_option("--kind", pa.kind, "restrict to one symbol kind")
      ->check(CLI::IsMember(kSymbolKinds));
  search->add_option("--limit", pa.limit,
                     "show at most N matches (0 = all; default 25)");
  search->preparse_callback([&pa](std::size_t) { pa.limit = 25; });
  search->callback([&pa] { pa.command = "search"; });

  CLI::App *analyze = app.add_subcommand(
      "analyze", "run Souffle Datalog analyses over the index");
  analyze->add_option("--rule", pa.analyze_rule,
                      "built-in rule to run (see --list)");
  analyze->add_option("--rules-file", pa.analyze_rules_file,
                      "user Souffle .dl program; the fact declarations are "
                      "prepended automatically");
  analyze->add_flag("--list", pa.analyze_list,
                    "list the built-in rules as JSON");
  analyze->add_option("--export-facts", pa.analyze_export,
                      "write TSV fact files and the cidx_facts.dl prelude to "
                      "DIR");
  analyze->add_option("--jobs", pa.analyze_jobs,
                      "Souffle worker count (default 1)");
  analyze->add_option("--db", pa.index_db, kDbHelpText);
  analyze->callback([&pa] { pa.command = "analyze"; });
}

void build_db(CLI::App &app, ParsedArgs &pa) {
  CLI::App *db =
      app.add_subcommand("db", "database maintenance (migrate, verify)");
  db->require_subcommand(1);

  CLI::App *migrate = db->add_subcommand(
      "migrate",
      "upgrade an existing index to the current schema (in place, no "
      "re-index)");
  migrate->add_option("--db", pa.index_db, kDbHelpText);
  migrate->callback([&pa] { pa.command = "migrate"; });

  CLI::App *verify = db->add_subcommand(
      "verify", "check that component roots and files exist on disk");
  verify->add_option("--component,-c", pa.component,
                     "restrict to one component (default: all)");
  verify->add_flag("--all", pa.all,
                   "also list files that exist (default: only failures)");
  verify->add_option("--db", pa.index_db, kDbHelpText);
  verify->callback([&pa] { pa.command = "verify"; });
}

void build_component(CLI::App &app, ParsedArgs &pa) {
  CLI::App *component = app.add_subcommand(
      "component",
      "manage components (add, list, show, set-version, compile-commands, "
      "rm)");
  component->require_subcommand(1);

  CLI::App *add = component->add_subcommand("add", "register a component");
  add->add_option("--path", pa.path, "repo root or library header dir")
      ->required();
  add->add_option("--name", pa.name,
                  "component name (default: from .git/config)");
  add->add_option("--repo", pa.repo,
                  "repository name to group under (default: component name)");
  add->add_option("--universe", pa.universe,
                  "portable semantic universe key for this repository");
  add->add_option("--kind", pa.kind, "component kind")
      ->check(CLI::IsMember(kComponentKinds));
  add->add_flag("--no-git", pa.no_git,
                "use --path as-is; do not promote to the enclosing git root");
  add->add_option("--version", pa.version_str,
                  "set component version to V (overrides auto-detection; '' "
                  "clears)");
  add->add_flag("--no-detect-version", pa.no_detect_version,
                "disable trailing-segment version detection");
  add->preparse_callback([&pa](std::size_t) { pa.kind = "repo"; });
  add->callback([&pa] { pa.command = "add-source"; });

  CLI::App *list =
      component->add_subcommand("list", "list registered components");
  list->alias("ls");
  list->add_option("pattern", pa.pattern, kPatternHelpText);
  list->add_option("--kind", pa.kind, "restrict to one component kind")
      ->check(CLI::IsMember(kComponentKinds));
  list->callback([&pa] {
    pa.command = "list";
    pa.what = "components";
  });

  CLI::App *show =
      component->add_subcommand("show", "show details for a component");
  show->add_option("NAME", pa.name, "component name")->required();
  show->add_option("--db", pa.index_db, kDbHelpText);
  show->callback([&pa] {
    pa.command = "component";
    pa.what = "show";
  });

  CLI::App *set_version = component->add_subcommand(
      "set-version", "set or clear a component's version");
  set_version->add_option("NAME", pa.name, "component name")->required();
  set_version->add_option("V", pa.version_str,
                          "version string; omit or pass '' to clear");
  set_version->add_option("--db", pa.index_db, kDbHelpText);
  set_version->callback([&pa] {
    pa.command = "component";
    pa.what = "set-version";
  });

  CLI::App *cc = component->add_subcommand(
      "compile-commands", "emit a compile_commands.json for the component");
  cc->add_option("COMPONENT", pa.component, "component whose files to emit")
      ->required();
  cc->add_option("--db", pa.index_db, kAltDbHelpText);
  cc->callback([&pa] { pa.command = "dump-compile-commands"; });

  CLI::App *rm = component->add_subcommand(
      "rm", "delete a component and everything indexed from it");
  CLI::Option_group *sel = rm->add_option_group("selector");
  sel->add_option("--id", pa.del_id, "component id");
  sel->add_option("--name", pa.name, "component name");
  sel->add_option("--path", pa.del_path, "component root path");
  sel->require_option(1);
  rm->add_flag("--dry-run", pa.dry_run,
               "preview the matches without deleting anything");
  rm->callback([&pa] {
    pa.command = "delete";
    pa.what = "component";
  });
}

void build_repo(CLI::App &app, ParsedArgs &pa) {
  CLI::App *repo = app.add_subcommand(
      "repo", "group components into repositories; switch clones");
  repo->require_subcommand(1);

  CLI::App *list = repo->add_subcommand("list", "list repositories");
  list->alias("ls");
  list->add_option("pattern", pa.pattern, "fuzzy-filter by repository name");
  list->add_option("--kind", pa.kind, "filter by repository kind")
      ->check(CLI::IsMember(kComponentKinds));
  list->add_option("--db", pa.index_db, kDbHelpText);
  list->callback([&pa] {
    pa.command = "repo";
    pa.what = "list";
  });

  CLI::App *show =
      repo->add_subcommand("show", "show a repository's clones and components");
  show->add_option("NAME", pa.name, "repository name")->required();
  show->add_option("--db", pa.index_db, kDbHelpText);
  show->callback([&pa] {
    pa.command = "repo";
    pa.what = "show";
  });

  CLI::App *add_clone =
      repo->add_subcommand("add-clone", "register another checkout directory");
  add_clone->add_option("NAME", pa.name, "repository name")->required();
  add_clone->add_option("PATH", pa.path, "checkout/worktree directory")
      ->required();
  add_clone->add_option("--label", pa.repo_label,
                        "optional label (e.g. branch/worktree name)");
  add_clone->add_option("--db", pa.index_db, kDbHelpText);
  add_clone->callback([&pa] {
    pa.command = "repo";
    pa.what = "add-clone";
  });

  CLI::App *sw = repo->add_subcommand(
      "switch", "rebase the repository onto another clone (by path/label)");
  sw->add_option("NAME", pa.name, "repository name")->required();
  sw->add_option("TARGET", pa.target, "clone path or label")->required();
  sw->add_option("--db", pa.index_db, kDbHelpText);
  sw->callback([&pa] {
    pa.command = "repo";
    pa.what = "switch";
  });

  CLI::App *realias = repo->add_subcommand(
      "realias",
      "rewrite stored include paths to <label> tokens via the registry");
  realias->add_option("COMPONENT", pa.component,
                      "restrict to one component (default: all files)");
  realias->add_option("--db", pa.index_db, kDbHelpText);
  realias->callback([&pa] {
    pa.command = "realias";
    pa.what = "realias";
  });

  CLI::App *rm = repo->add_subcommand("rm", "remove a repository");
  rm->add_option("NAME", pa.name, "repository name")->required();
  rm->add_flag("--delete-components", pa.delete_components,
               "also delete the grouped components and their indexed symbols");
  rm->add_option("--db", pa.index_db, kDbHelpText);
  rm->callback([&pa] {
    pa.command = "repo";
    pa.what = "rm";
  });
}

void build_dir(CLI::App &app, ParsedArgs &pa) {
  CLI::App *dir =
      app.add_subcommand("dir", "browse or delete indexed directories");
  dir->require_subcommand(1);

  CLI::App *list =
      dir->add_subcommand("list", "list directories (all, or one component's)");
  list->alias("ls");
  list->add_option("pattern", pa.pattern, kPatternHelpText);
  list->add_option("--component,-c", pa.component,
                   "restrict to this component");
  list->callback([&pa] {
    pa.command = "list";
    pa.what = "dirs";
  });

  CLI::App *rm = dir->add_subcommand(
      "rm", "delete a directory, its files, and their symbols");
  CLI::Option_group *sel = rm->add_option_group("selector");
  sel->add_option("--id", pa.del_id, "directory id");
  sel->add_option("--path", pa.del_path, "directory path");
  sel->require_option(1);
  rm->add_option("--component,-c", pa.component,
                 "restrict the match to this component");
  rm->add_flag("--dry-run", pa.dry_run,
               "preview the matches without deleting anything");
  rm->callback([&pa] {
    pa.command = "delete";
    pa.what = "dir";
  });
}

void build_file(CLI::App &app, ParsedArgs &pa) {
  CLI::App *file = app.add_subcommand(
      "file", "manage indexed files (list, show, flags, set, rm)");
  file->require_subcommand(1);

  CLI::App *list = file->add_subcommand(
      "list", "list files for a component or a directory in it");
  list->alias("ls");
  list->add_option("pattern", pa.pattern, kPatternHelpText);
  list->add_option("--component,-c", pa.component,
                   "restrict to this component");
  list->add_option("--dir,-d", pa.dir,
                   "directory (relative to the component root) including its "
                   "subtree; needs --component");
  CLI::Option *indexed =
      list->add_flag("--indexed", pa.indexed, "only files already indexed");
  CLI::Option *pending =
      list->add_flag("--pending", pa.pending, "only files not yet indexed");
  indexed->excludes(pending);
  pending->excludes(indexed);
  list->callback([&pa] {
    pa.command = "list";
    pa.what = "files";
  });

  CLI::App *show =
      file->add_subcommand("show", "show full details of one file");
  show->add_option("file", pa.file,
                   "numeric id (first column of 'file list') or a path; "
                   "relative paths resolve against the --component root (else "
                   "the current directory)")
      ->required();
  show->add_option("--component,-c", pa.component,
                   "component root for resolving a relative path");
  show->callback([&pa] {
    pa.command = "show";
    pa.what = "file";
  });

  // `file flags TARGET [OP ...]`: everything after the first unrecognized
  // token is the operation tail, captured verbatim (git-style prefix
  // command), e.g. `-set-flag FLAG | -unset-flag FLAG | -import-args JSON |
  // -dump-args`.
  CLI::App *flags = file->add_subcommand(
      "flags", "inspect or edit one file's stored compile flags");
  flags->prefix_command();
  flags
      ->add_option("TARGET", pa.target,
                   "file address, e.g. 'mylib://src/foo.c'")
      ->required();
  flags->add_option("--db", pa.index_db, kAltDbHelpText);
  flags->callback([&pa, flags] {
    pa.command = "file";
    pa.op = flags->remaining();
  });

  CLI::App *set = file->add_subcommand(
      "set", "set a mutable file attribute (e.g. pending status)");
  set->add_option("ASSIGNMENT", pa.assignment,
                  "attribute assignment, e.g. 'pending=False' (fields: "
                  "pending, indexed)")
      ->required();
  set->add_option("--component,-c", pa.component,
                  "restrict to this component's files");
  set->add_option("--file", pa.file_filter,
                  "restrict to one file (path relative to component root)");
  set->add_option("--db", pa.index_db, kAltDbHelpText);
  set->add_flag("--dry-run", pa.dry_run,
                "preview the matches without changing anything");
  set->callback([&pa] { pa.command = "set"; });

  CLI::App *rm = file->add_subcommand("rm", "delete a file and its symbols");
  CLI::Option_group *sel = rm->add_option_group("selector");
  sel->add_option("--id", pa.del_id, "file id");
  sel->add_option("--name", pa.name, "file basename");
  sel->add_option("--path", pa.del_path, "file path");
  sel->require_option(1);
  rm->add_option("--component,-c", pa.component,
                 "restrict the match to this component");
  rm->add_flag("--dry-run", pa.dry_run,
               "preview the matches without deleting anything");
  rm->callback([&pa] {
    pa.command = "delete";
    pa.what = "file";
  });
}

void build_symbol(CLI::App &app, ParsedArgs &pa) {
  CLI::App *symbol =
      app.add_subcommand("symbol", "inspect indexed symbols (list, show, rm)");
  symbol->require_subcommand(1);

  CLI::App *list = symbol->add_subcommand(
      "list", "list symbols for a component, directory, or file");
  list->alias("ls");
  list->add_option("pattern", pa.pattern,
                   std::string(kPatternHelpText) +
                       " (matched against the qualified name)");
  list->add_option("--component,-c", pa.component,
                   "restrict to this component");
  list->add_option("--dir,-d", pa.dir,
                   "directory (relative to the component root) including its "
                   "subtree; needs --component");
  list->add_option("--file,-f", pa.file_filter,
                   "one file; relative paths resolve against the --component "
                   "root (else the current directory)");
  list->add_option("--kind", pa.kind, "restrict to one symbol kind")
      ->check(CLI::IsMember(kSymbolKinds));
  list->add_option("--limit", pa.limit,
                   "show at most N matches (0 = all; default 50)");
  list->preparse_callback([&pa](std::size_t) { pa.limit = 50; });
  list->callback([&pa] {
    pa.command = "list";
    pa.what = "symbols";
  });

  CLI::App *show =
      symbol->add_subcommand("show", "show full details of one symbol");
  show->add_option("symbol", pa.symbol,
                   "numeric id (first column of 'search') or a clang USR; "
                   "USRs contain $ and * so single-quote them in the shell")
      ->required();
  show->callback([&pa] {
    pa.command = "show";
    pa.what = "symbol";
  });

  CLI::App *rm = symbol->add_subcommand("rm", "delete a symbol");
  CLI::Option_group *sel = rm->add_option_group("selector");
  sel->add_option("--id", pa.del_id, "symbol id");
  sel->add_option("--name", pa.name, "symbol spelling");
  sel->add_option("--usr", pa.usr, "clang USR");
  sel->require_option(1);
  rm->add_option("--component,-c", pa.component,
                 "restrict the match to this component");
  rm->add_flag("--dry-run", pa.dry_run,
               "preview the matches without deleting anything");
  rm->callback([&pa] {
    pa.command = "delete";
    pa.what = "symbol";
  });
}

// Scope selector shared by every include verb: PATH... and/or --files-from.
// A list file keeps a large or scripted scope off the command line, which
// matters once a cleanup spans hundreds of files.
void add_include_scope(CLI::App *sub, ParsedArgs &pa) {
  sub->add_option("paths", pa.inc_paths,
                  "files to scope to (default: every indexed file)");
  sub->add_option("--files-from", pa.files_from,
                  "read scope paths from FILE, one per line (absolute or "
                  "relative to the current directory); '-' reads stdin. "
                  "Blank lines and #-comments are ignored. Combined with any "
                  "PATH arguments.")
      ->type_name("FILE");
  sub->add_option("--db", pa.index_db, "index database (default: the cache)");
}

void build_include(CLI::App &app, ParsedArgs &pa) {
  CLI::App *inc = app.add_subcommand(
      "include", "inspect and clean up #include directives (graph, check, "
                 "plan, apply)");
  inc->require_subcommand(1);

  const auto leaf = [&pa, inc](const std::string &name,
                               const std::string &desc) {
    CLI::App *sub = inc->add_subcommand(name, desc);
    sub->callback([&pa, name] {
      pa.command = "include";
      pa.what = name;
    });
    return sub;
  };

  CLI::App *graph = leaf("graph", "show the include dependency graph");
  add_include_scope(graph, pa);
  graph->add_flag("--reverse", pa.inc_reverse,
                  "show files that include the target, not what it includes");
  graph->add_flag("--transitive", pa.inc_transitive,
                  "follow the graph transitively, not just direct edges");
  graph->add_option("--depth", pa.inc_depth,
                    "bound --transitive to N hops (0 = unbounded)");
  graph->add_flag("--cycles", pa.inc_cycles,
                  "report include cycles (strongly connected components) "
                  "instead of edges");
  graph->add_flag("--system", pa.inc_system,
                  "keep direct system-header targets (their internals are "
                  "never indexed, so they are always leaves)");
  graph->add_option("--format", pa.inc_format, "text|json|dot")
      ->check(CLI::IsMember({"text", "json", "dot"}));

  CLI::App *check = leaf("check", "report duplicate and unused includes");
  add_include_scope(check, pa);
  check->add_flag("--duplicates", pa.inc_duplicates,
                  "report duplicate includes (default: both)");
  check->add_flag(
      "--unused", pa.inc_unused,
      "report includes with zero symbol references (default: both)");
  check->add_flag("--json", pa.inc_json, "emit machine-readable JSON");

  CLI::App *plan = leaf("plan", "write a reviewable cleanup plan (no edits)");
  add_include_scope(plan, pa);
  plan->add_option("--output", pa.inc_output, "write the plan to FILE")
      ->required()
      ->type_name("FILE");
  plan->add_flag("--duplicates", pa.inc_duplicates, "plan duplicate removals");
  plan->add_flag("--unused", pa.inc_unused, "plan unused removals");

  CLI::App *apply =
      leaf("apply", "apply an approved cleanup plan (EDITS SOURCE)");
  apply->add_option("plan", pa.inc_plan, "the cleanup plan to apply")
      ->required()
      ->type_name("PLAN");
  apply->add_option("--db", pa.index_db, "index database (default: the cache)");
  apply->add_flag("--dry-run", pa.dry_run,
                  "validate and report, but write nothing");
  apply
      ->add_option("--only", pa.inc_only,
                   "apply only these candidate ids (comma-separated); the "
                   "final combined validation still covers every applied edit")
      ->delimiter(',')
      ->type_name("ID");
}

void build_graph(CLI::App &app, ParsedArgs &pa) {
  CLI::App *graph = app.add_subcommand(
      "graph",
      "query the relationship graph (callers, callees, refs, neighbors, "
      "walk, path, hierarchy, dispatch)");
  graph->require_subcommand(1);

  const auto leaf = [&pa, graph](const std::string &name,
                                 const std::string &desc) {
    CLI::App *sub = graph->add_subcommand(name, desc);
    sub->callback([&pa, name] {
      pa.command = "graph";
      pa.what = name;
    });
    return sub;
  };

  CLI::App *callers = leaf("callers", "functions that call the symbol");
  add_graph_selector(callers, pa);
  callers->add_flag("--direct-only", pa.direct_only,
                    "only literal incoming calls (exclude virtual-dispatch "
                    "callers reached via materialised dispatch_calls edges, "
                    "which are on by default)");

  CLI::App *callees = leaf("callees", "functions the symbol calls");
  add_graph_selector(callees, pa);
  callees->add_flag("--direct-only", pa.direct_only,
                    "only literal outgoing calls (exclude virtual-dispatch "
                    "targets reached via materialised dispatch_calls edges, "
                    "which are on by default)");

  CLI::App *refs = leaf(
      "refs", "incoming references (calls + uses + alias_of) to the symbol");
  add_graph_selector(refs, pa);

  CLI::App *neighbors = leaf("neighbors", "one-hop typed neighbors");
  add_graph_selector(neighbors, pa);
  neighbors->add_option("--edge", pa.edge, "edge kinds (comma-separated)");
  neighbors
      ->add_option("--direction", pa.direction, "edge direction (default out)")
      ->check(CLI::IsMember(kDirections));

  CLI::App *walk = leaf("walk", "bounded BFS over typed edges");
  add_graph_selector(walk, pa);
  walk->add_option("--edge", pa.edge, "edge kinds (comma-separated)");
  walk->add_option("--direction", pa.direction, "edge direction (default out)")
      ->check(CLI::IsMember(kDirections));
  walk->add_option("--depth", pa.graph_depth, "walk depth (default 3)");

  CLI::App *path = leaf("path", "shortest path between two symbols, or none");
  add_graph_selector(path, pa);
  CLI::Option_group *to =
      path->add_option_group("destination", "symbol to reach");
  to->add_option("--to-usr", pa.to_usr, "exact clang USR");
  to->add_option("--to-id", pa.to_id, "numeric symbol id");
  to->add_option("--to-name", pa.to_name, "fuzzy qualified-name match");
  to->require_option(1);
  path->add_option("--to-kind", pa.to_kind,
                   "restrict a --to-name match to one symbol kind")
      ->check(CLI::IsMember(kSymbolKinds));
  path->add_option("--edge", pa.edge, "edge kinds (comma-separated)");
  path->add_option("--direction", pa.direction, "edge direction (default out)")
      ->check(CLI::IsMember(kDirections));
  path->add_option("--depth", pa.graph_depth, "search depth (default 8)");
  path->preparse_callback([&pa](std::size_t) { pa.graph_depth = 8; });

  CLI::App *hierarchy =
      leaf("hierarchy", "class bases, subclasses, and members");
  add_graph_selector(hierarchy, pa);
  hierarchy->add_flag("--transitive", pa.transitive,
                      "walk the whole hierarchy");
  hierarchy
      ->add_option("--access", pa.access, "member access filter (default all)")
      ->check(CLI::IsMember(kAccessLevels));

  CLI::App *dispatch =
      leaf("dispatch", "run-time targets of a virtual-method call");
  add_graph_selector(dispatch, pa);

  CLI::App *redefined = leaf(
      "redefined", "symbols defined in more than one backend (multi_def>1)");
  redefined->add_option("--db", pa.index_db,
                        "index database to query (default: the standard "
                        "cache index)");
  redefined->add_flag("--json", pa.graph_json,
                      "emit stable machine-readable JSON");
  redefined->add_option("--limit", pa.graph_limit,
                        "cap the number of results (default 200)");
  redefined->preparse_callback([&pa](std::size_t) { pa.graph_limit = 200; });

  CLI::App *definitions =
      leaf("definitions",
           "each backend body of a symbol + its possible-call fan-out");
  add_graph_selector(definitions, pa);
  definitions->add_flag("--direct-only", pa.direct_only,
                        "list the backend bodies only (omit the "
                        "possible-call fan-out, which is shown by default)");

  CLI::App *signature =
      leaf("signature",
           "signature/type facts of a symbol (return + parameter types, "
           "variable/field type, alias target)");
  add_graph_selector(signature, pa);

  CLI::App *template_graph =
      leaf("template", "template provenance, relationships, and arguments");
  add_graph_selector(template_graph, pa);

  CLI::App *typeusers =
      leaf("typeusers",
           "callables accepting/returning the type + variables/fields/aliases "
           "of it (through pointer/reference/array/alias layers)");
  add_graph_selector(typeusers, pa);
}

} // namespace

ParsedArgs parse_args(const std::vector<std::string> &argv) {
  ParsedArgs pa;
  CLI::App app{"cidx command-line skeleton", "cidx"};
  app.require_subcommand(1);
  app.set_version_flag("--version", std::string("cidx ") + kVersion,
                       "show program's version number and exit");

  build_top_level(app, pa);
  build_db(app, pa);
  build_component(app, pa);
  build_repo(app, pa);
  build_dir(app, pa);
  build_file(app, pa);
  build_symbol(app, pa);
  build_graph(app, pa);
  build_include(app, pa);

  // CLI11's vector-parse consumes tokens from the back.
  std::vector<std::string> tokens(argv.rbegin(), argv.rend());
  try {
    app.parse(tokens);
  } catch (const CLI::CallForHelp &) {
    pa.help_text = app.help(); // delegates to the parsed subcommand
    return pa;
  } catch (const CLI::CallForVersion &) {
    // --version fires from the option callback, which runs before help
    // processing; keep argparse's encounter-order contract that a -h seen
    // first wins.
    if (app.get_help_ptr() != nullptr && app.get_help_ptr()->count() > 0) {
      pa.help_text = app.help();
    } else {
      pa.version = true;
    }
    return pa;
  } catch (const CLI::ParseError &e) {
    usage_fail(app, e);
  }

  if ((pa.command == "graph" || pa.command == "include") && pa.index_db) {
    pa.index_db = pathutil::abspath(pathutil::expanduser(*pa.index_db));
  }
  if (pa.command == "include") {
    // Scope paths are matched against the index's absolute paths, so resolve
    // them once here rather than in each verb.
    for (std::string &p : pa.inc_paths) {
      p = pathutil::abspath(pathutil::expanduser(p));
    }
    // Neither --duplicates nor --unused means BOTH: the useful default for a
    // report is everything, and asking for one must not silently widen to two.
    if (!pa.inc_duplicates && !pa.inc_unused) {
      pa.inc_duplicates = pa.inc_unused = true;
    }
  }
  return pa;
}

} // namespace cidx::cli
