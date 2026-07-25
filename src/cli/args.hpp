// argv grammar, parsed with the vendored CLI11 library (see args.cpp).
// The command tree, flags, defaults (limits 25/50, --kind repo), choices,
// the `ls` aliases, and the mutually-exclusive/required option groups match
// the historical grammar; help, usage, and error text are CLI11's native
// formatting (the byte-for-byte Python argparse transcription is retired).
//
// Misuse throws UsageError whose what() is
// "<Usage line>\n<prog>: error: <detail>\n" with exit code 2; main() is the
// only catch-site (D23). -h/--help fills help_text; --version sets version.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cli/version.hpp"

namespace cidx::cli {

// Product and compatibility versions are generated from
// spec/platform/version.json. `cidx --version` prints "cidx <kVersion>".
inline constexpr const char *kVersion = version::kProductVersion.data();

struct ParsedArgs {
  std::string
      command; // add-source | import | index | search | query | show | list
  std::string what; // show: symbol|file; list: components|dirs|files|symbols

  // -h/--help anywhere: when set, print to stdout and exit 0 (argparse).
  std::optional<std::string> help_text;

  // --version at the top level: print "cidx <kVersion>" to stdout, exit 0.
  bool version = false;

  std::string path;                       // add-source --path (required)
  std::optional<std::string> name;        // add-source/import --name
  std::string db;                         // import --db (required)
  std::vector<std::string> files;         // index FILE...
  std::optional<std::string> source;      // index --source COMPONENT
  std::optional<std::string> pattern;     // search (required) / list (opt.)
  std::optional<std::string> kind;        // --kind (add-source default repo)
  int limit = 0;                          // search 25 / list symbols 50
  std::string symbol;                     // show symbol (required)
  std::string file;                       // show file (required)
  std::optional<std::string> component;   // --component/-c
  std::optional<std::string> dir;         // --dir/-d
  std::optional<std::string> file_filter; // list symbols --file/-f
  bool indexed = false;                   // list files --indexed
  bool pending = false;                   // list files --pending
  bool all = false;                       // verify --all (list OK files too)
  bool force = false;                     // init --force
  bool no_git = false;                    // add-source --no-git
  std::optional<int64_t> del_id;          // delete --id
  std::optional<std::string> del_path;    // delete --path
  std::optional<std::string> usr;         // delete symbol --usr
  bool dry_run = false;                   // delete --dry-run
  bool no_graph = false;                  // index --no-graph
  bool index_status = false;              // index status
  bool index_explain = false;             // index explain
  std::optional<std::string> index_fact_set; // index status/explain --fact-set
  std::vector<std::string> assignment;    // set FIELD=VALUE [FIELD=VALUE ...]
  std::optional<std::string> index_db; // set/file/dump-cc --db (index override)
  std::string query_text;              // query CXQ expression (required)
  bool query_json = false;             // query --json (machine-readable result)
  bool query_explain = false;          // query --explain (plan only)
  std::string target;                  // file: target path or COMPONENT://PATH
  std::vector<std::string> op;         // file OP ... (REMAINDER tail)

  // Shared selector flag (graph): --first takes the closest --name match.
  bool first = false; // --first (take closest --name match)

  // -- graph sub-command fields (cidx graph callers|callees|…) ---------------
  // Shared selector: reuse usr (above), kind (above), first (above), index_db.
  std::optional<int64_t> graph_id; // --id  N (graph: numeric symbol id)
  bool graph_json = false;         // --json (emit machine-readable JSON)
  int graph_limit = 50;            // --limit N (default 50)
  std::string direction{"out"};    // --direction {in,out} (default out)
  std::optional<std::string> edge; // --edge KINDS (comma-separated)
  int graph_depth = 3;             // --depth N (walk default 3, path 8)
  // path destination selector
  std::optional<std::string> to_usr;  // --to-usr USR
  std::optional<int64_t> to_id;       // --to-id N
  std::optional<std::string> to_name; // --to-name FUZZY
  std::optional<std::string> to_kind; // --to-kind {17 kinds}
  // callers flag
  bool direct_only = false; // --direct-only (exclude virtual callers)
  // hierarchy flags
  bool transitive = false;   // --transitive (walk whole hierarchy)
  std::string access{"all"}; // --access {public,protected,private,all}

  // -- include sub-command fields (cidx include graph|check|plan|apply) -------
  // Scope: PATH... positionally, and/or --files-from FILE (one path per line,
  // absolute or repo-relative). Both may be combined; the union is used.
  std::vector<std::string> inc_paths;    // PATH...
  std::optional<std::string> files_from; // --files-from FILE
  bool inc_reverse = false;              // graph --reverse (who includes me)
  bool inc_transitive = false;           // graph --transitive
  bool inc_cycles = false;               // graph --cycles
  bool inc_system = false;               // graph --system (keep system targets)
  std::string inc_format{"text"};        // graph --format text|json|dot
  int inc_depth = 0;                     // graph --depth N (0 = unbounded)
  bool inc_duplicates = false;           // check/plan --duplicates
  bool inc_unused = false;               // check/plan --unused
  bool inc_json = false;                 // check --json
  std::optional<std::string> inc_output; // plan --output FILE (required)
  std::string inc_plan;                  // apply PLAN (positional, required)
  std::vector<std::string> inc_only;     // apply --only ID[,ID...]

  // -- portable-paths (v14) fields -------------------------------------------
  std::optional<std::string> version_str; // --version VER (add-source)
  bool no_detect_version = false;         // --no-detect-version (add-source)
  bool no_autoderive_labels = false;      // --no-autoderive-labels (index)

  // -- aliasing (v0.6.0) fields ----------------------------------------------
  bool no_alias = false; // import --no-alias (skip alias_options encoding)

  // -- repository / clone (v23) fields ---------------------------------------
  // repo subcommand (list|show|add-clone|switch|rm) is carried in `what`; the
  // repository NAME in `name`; add-clone PATH in `path`; switch TARGET in
  // `target`; list name filter in `pattern`; list kind filter in `kind`.
  std::optional<std::string> repo;     // import/add-source --repo (group name)
  std::optional<std::string> universe; // import/add-source --universe key
  std::optional<std::string> repo_label; // repo add-clone --label
  bool delete_components = false;        // repo rm --delete-components

  // -- ui graph explorer ----------------------------------------------------
  std::optional<std::string> ui_root;
  std::optional<std::string> ui_query;
  std::optional<std::string> ui_workspace;
  std::optional<std::string> ui_output;
  int ui_depth = 2;
  int ui_node_budget = 250;
  int ui_edge_budget = 500;
  int ui_site_budget = 200;
  int ui_byte_budget = 4 * 1024 * 1024;
  int ui_port = 0;
  bool ui_no_browser = false;

  // -- analyze (Souffle) fields
  // ------------------------------------------------
  // --db is carried in index_db.
  std::optional<std::string> analyze_rule;       // --rule NAME
  std::optional<std::string> analyze_rules_file; // --rules-file FILE
  bool analyze_list = false;                     // --list
  std::optional<std::string> analyze_export;     // --export-facts DIR
  int analyze_jobs = 1;                          // --jobs N (default 1)
};

// argv WITHOUT the program name. Throws UsageError on misuse.
ParsedArgs parse_args(const std::vector<std::string> &argv);

} // namespace cidx::cli
