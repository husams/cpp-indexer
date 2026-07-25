// Command handlers (design §5.9, §6.1-§6.3) — 1:1 port of the cmd_*
// functions in cli.py. Each handler returns the process exit code; output
// goes to Context streams so tests can capture full stdout/stderr strings.
//
// cmd_index (S08) runs the full §6.1 pipeline: target list (all pending via
// the md5-only skip, or resolved FILE args), per-file sanitize -> parse ->
// index_symbols -> index_headers -> mark_file_indexed -> per-file line,
// warning-count summary, exit 1 iff any file failed/unknown.
#pragma once

#include <functional>
#include <ostream>
#include <string>

#include "cli/args.hpp"
#include "util/logger.hpp"

namespace cidx::ast {
struct IndexOneOutcome;
}

namespace cidx::cli {
using IndexOutcomeSink =
    std::function<void(const cidx::ast::IndexOneOutcome &outcome)>;

// Cache-dir policy (analysis §1.3): $INDEXER_CACHE else ~/.cache/cidx,
// expanduser'd, NOT abspath'd (Python parity). All generated files live
// there: index.db + cidx.log, never the CWD.
std::string resolve_cache_dir();

struct Context {
  std::string cache_dir;       // resolved (caller created it, mkdir -p)
  std::string index_path;      // <cache_dir>/index.db
  Logger *logger = nullptr;    // Logger::root() in main; file sink lazy
  std::ostream *out = nullptr; // stdout
  std::ostream *err = nullptr; // stderr
  IndexOutcomeSink index_outcome_sink;
};

int cmd_init(const ParsedArgs &args, Context &ctx);
int cmd_migrate(const ParsedArgs &args, Context &ctx);
int cmd_add_source(const ParsedArgs &args, Context &ctx);
int cmd_import(const ParsedArgs &args, Context &ctx);
int cmd_index(const ParsedArgs &args, Context &ctx);
int cmd_search(const ParsedArgs &args, Context &ctx);
int cmd_query(const ParsedArgs &args, Context &ctx);
int cmd_show_symbol(const ParsedArgs &args, Context &ctx);
int cmd_show_file(const ParsedArgs &args, Context &ctx);
int cmd_list_components(const ParsedArgs &args, Context &ctx);
int cmd_list_dirs(const ParsedArgs &args, Context &ctx);
int cmd_list_files(const ParsedArgs &args, Context &ctx);
int cmd_list_symbols(const ParsedArgs &args, Context &ctx);
int cmd_delete_component(const ParsedArgs &args, Context &ctx);
int cmd_delete_dir(const ParsedArgs &args, Context &ctx);
int cmd_delete_file(const ParsedArgs &args, Context &ctx);
int cmd_delete_symbol(const ParsedArgs &args, Context &ctx);
int cmd_file(const ParsedArgs &args, Context &ctx);
int cmd_dump_compile_commands(const ParsedArgs &args, Context &ctx);

// Portable-paths commands (v14): component show/set-version, label
// add/rm/list/resolve
int cmd_component_show(const ParsedArgs &args, Context &ctx);
int cmd_component_set_version(const ParsedArgs &args, Context &ctx);
int cmd_label_add(const ParsedArgs &args, Context &ctx);
int cmd_label_rm(const ParsedArgs &args, Context &ctx);
int cmd_label_list(const ParsedArgs &args, Context &ctx);
int cmd_label_resolve(const ParsedArgs &args, Context &ctx);

// repository / clone commands (v23): repo list/show/add-clone/switch/rm
int cmd_repo_list(const ParsedArgs &args, Context &ctx);
int cmd_repo_show(const ParsedArgs &args, Context &ctx);
int cmd_repo_add_clone(const ParsedArgs &args, Context &ctx);
int cmd_repo_switch(const ParsedArgs &args, Context &ctx);
int cmd_repo_rm(const ParsedArgs &args, Context &ctx);

// verify: check that component roots (incl. version) and files exist on disk.
int cmd_verify(const ParsedArgs &args, Context &ctx);

// analyze: Souffle Datalog analyses over the semantic index
// (src/cli/analyze.cpp).
int cmd_analyze(const ParsedArgs &args, Context &ctx);

int cmd_ui_export(const ParsedArgs &args, Context &ctx);
int cmd_ui_open(const ParsedArgs &args, Context &ctx);
int cmd_ui_status(const ParsedArgs &args, Context &ctx);

// Graph query commands (cidx graph callers|callees|refs|neighbors|walk|path|
//                            hierarchy|dispatch)
int cmd_graph_callers(const ParsedArgs &args, Context &ctx);
int cmd_graph_callees(const ParsedArgs &args, Context &ctx);
int cmd_graph_refs(const ParsedArgs &args, Context &ctx);
int cmd_graph_neighbors(const ParsedArgs &args, Context &ctx);
int cmd_graph_walk(const ParsedArgs &args, Context &ctx);
int cmd_graph_path(const ParsedArgs &args, Context &ctx);
int cmd_graph_hierarchy(const ParsedArgs &args, Context &ctx);
int cmd_graph_dispatch(const ParsedArgs &args, Context &ctx);
// v30 signature/type tier
int cmd_graph_signature(const ParsedArgs &args, Context &ctx);
int cmd_graph_template(const ParsedArgs &args, Context &ctx);
int cmd_graph_typeusers(const ParsedArgs &args, Context &ctx);

// v31 include hygiene (cli/include_cmd.cpp). graph/check/plan are read-only;
// apply is the only command in cidx that edits source.
int cmd_include_graph(const ParsedArgs &args, Context &ctx);
int cmd_include_check(const ParsedArgs &args, Context &ctx);
int cmd_include_plan(const ParsedArgs &args, Context &ctx);
int cmd_include_apply(const ParsedArgs &args, Context &ctx);

// Dispatch on args.command/args.what (args.help_text handled by the caller).
int run_command(const ParsedArgs &args, Context &ctx);

} // namespace cidx::cli
