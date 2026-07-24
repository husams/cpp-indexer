// Command dispatch. The command bodies live in the commands_*.cpp units.
#include "cli/commands_detail.hpp"

namespace cidx::cli {

int run_command(const ParsedArgs &args, Context &ctx) {
  if (args.command == "init") {
    return cmd_init(args, ctx);
  }
  if (args.command == "migrate") {
    return cmd_migrate(args, ctx);
  }
  if (args.command == "add-source") {
    return cmd_add_source(args, ctx);
  }
  if (args.command == "import") {
    return cmd_import(args, ctx);
  }
  if (args.command == "realias") {
    return cmd_realias(args, ctx);
  }
  if (args.command == "index") {
    return cmd_index(args, ctx);
  }
  if (args.command == "resolve") {
    return cmd_resolve(args, ctx);
  }
  if (args.command == "set") {
    return cmd_set(args, ctx);
  }
  if (args.command == "file") {
    return cmd_file(args, ctx);
  }
  if (args.command == "dump-compile-commands") {
    return cmd_dump_compile_commands(args, ctx);
  }
  if (args.command == "search") {
    return cmd_search(args, ctx);
  }
  if (args.command == "query") {
    return cmd_query(args, ctx);
  }
  if (args.command == "show") {
    return args.what == "symbol" ? cmd_show_symbol(args, ctx)
                                 : cmd_show_file(args, ctx);
  }
  if (args.command == "delete") {
    if (args.what == "component") {
      return cmd_delete_component(args, ctx);
    }
    if (args.what == "dir") {
      return cmd_delete_dir(args, ctx);
    }
    if (args.what == "file") {
      return cmd_delete_file(args, ctx);
    }
    return cmd_delete_symbol(args, ctx);
  }
  if (args.command == "graph") {
    if (args.what == "callers") {
      return cmd_graph_callers(args, ctx);
    }
    if (args.what == "callees") {
      return cmd_graph_callees(args, ctx);
    }
    if (args.what == "refs") {
      return cmd_graph_refs(args, ctx);
    }
    if (args.what == "neighbors") {
      return cmd_graph_neighbors(args, ctx);
    }
    if (args.what == "walk") {
      return cmd_graph_walk(args, ctx);
    }
    if (args.what == "path") {
      return cmd_graph_path(args, ctx);
    }
    if (args.what == "hierarchy") {
      return cmd_graph_hierarchy(args, ctx);
    }
    if (args.what == "dispatch") {
      return cmd_graph_dispatch(args, ctx);
    }
    if (args.what == "redefined") {
      return cmd_graph_redefined(args, ctx);
    }
    if (args.what == "definitions") {
      return cmd_graph_definitions(args, ctx);
    }
    if (args.what == "signature") {
      return cmd_graph_signature(args, ctx);
    }
    if (args.what == "template") {
      return cmd_graph_template(args, ctx);
    }
    if (args.what == "typeusers") {
      return cmd_graph_typeusers(args, ctx);
    }
  }
  if (args.command == "include") {
    if (args.what == "graph") {
      return cmd_include_graph(args, ctx);
    }
    if (args.what == "check") {
      return cmd_include_check(args, ctx);
    }
    if (args.what == "plan") {
      return cmd_include_plan(args, ctx);
    }
    if (args.what == "apply") {
      return cmd_include_apply(args, ctx);
    }
  }
  if (args.command == "component") {
    if (args.what == "show") {
      return cmd_component_show(args, ctx);
    }
    return cmd_component_set_version(args, ctx);
  }
  if (args.command == "repo") {
    if (args.what == "show") {
      return cmd_repo_show(args, ctx);
    }
    if (args.what == "add-clone") {
      return cmd_repo_add_clone(args, ctx);
    }
    if (args.what == "switch") {
      return cmd_repo_switch(args, ctx);
    }
    if (args.what == "rm") {
      return cmd_repo_rm(args, ctx);
    }
    return cmd_repo_list(args, ctx); // list (and its `ls` alias)
  }
  if (args.command == "verify") {
    return cmd_verify(args, ctx);
  }
  if (args.command == "analyze") {
    return cmd_analyze(args, ctx);
  }
  if (args.command == "ui") {
    if (args.what == "open") {
      return cmd_ui_open(args, ctx);
    }
    if (args.what == "export") {
      return cmd_ui_export(args, ctx);
    }
    return cmd_ui_status(args, ctx);
  }
  // list
  if (args.what == "components") {
    return cmd_list_components(args, ctx);
  }
  if (args.what == "dirs") {
    return cmd_list_dirs(args, ctx);
  }
  if (args.what == "files") {
    return cmd_list_files(args, ctx);
  }
  return cmd_list_symbols(args, ctx);
}

} // namespace cidx::cli
