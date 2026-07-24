#include "cli/commands_detail.hpp"

#include <fstream>
#include <limits>

#include "ui/assets.hpp"
#include "ui/graph_view.hpp"
#include "ui/server.hpp"

namespace cidx::cli {
namespace {

ui::GraphViewRequest ui_request(const ParsedArgs &args) {
  ui::GraphViewRequest request;
  request.root = args.ui_root;
  request.query = args.ui_query;
  request.workspace = args.ui_workspace;
  request.direction = args.direction;
  request.depth = args.ui_depth;
  request.node_budget = args.ui_node_budget;
  request.edge_budget = args.ui_edge_budget;
  if (args.edge) {
    std::vector<std::string> kinds;
    std::string current;
    for (const char c : *args.edge) {
      if (c == ',') {
        if (!current.empty()) {
          kinds.push_back(current);
          current.clear();
        }
      } else if (c != ' ') {
        current += c;
      }
    }
    if (!current.empty()) {
      kinds.push_back(std::move(current));
    }
    if (!kinds.empty()) {
      request.edge_kinds = std::move(kinds);
    }
  }
  return request;
}

std::unique_ptr<Storage> open_ui_storage(const ParsedArgs & /*args*/,
                                         Context &ctx) {
  struct stat st{};
  if (::stat(ctx.index_path.c_str(), &st) != 0) {
    *ctx.err << "error: cidx ui: no index at " << ctx.index_path << "\n";
    return nullptr;
  }
  try {
    return std::make_unique<Storage>(ctx.index_path,
                                     Storage::OpenMode::read_only);
  } catch (const std::exception &error) {
    *ctx.err << "error: cidx ui: " << error.what() << "\n";
    return nullptr;
  }
}

} // namespace

int cmd_ui_export(const ParsedArgs &args, Context &ctx) {
  auto db = open_ui_storage(args, ctx);
  if (!db || !args.ui_output) {
    return 1;
  }
  try {
    const auto view = ui::build_graph_view(*db, ui_request(args));
    const std::string html = ui::render_html(view);
    std::ofstream output(*args.ui_output, std::ios::binary | std::ios::trunc);
    if (!output) {
      *ctx.err << "error: cidx ui: cannot write " << *args.ui_output << "\n";
      return 1;
    }
    output << html;
    if (!output) {
      *ctx.err << "error: cidx ui: failed writing " << *args.ui_output << "\n";
      return 1;
    }
    *ctx.out << *args.ui_output << "\n";
    return 0;
  } catch (const std::exception &error) {
    *ctx.err << "error: cidx ui: " << error.what() << "\n";
    return 1;
  }
}

int cmd_ui_open(const ParsedArgs &args, Context &ctx) {
  auto db = open_ui_storage(args, ctx);
  if (!db) {
    return 1;
  }
  try {
    const auto view = ui::build_graph_view(*db, ui_request(args));
    const std::string html = ui::render_html(view);
    const std::string graph_json = json_out::dumps_indent2(view);
    return ui::serve_live(
        html, graph_json,
        ui::ServerOptions{.port = args.ui_port,
                          .launch_browser = !args.ui_no_browser},
        *ctx.out, *ctx.err);
  } catch (const std::exception &error) {
    *ctx.err << "error: cidx ui: " << error.what() << "\n";
    return 1;
  }
}

int cmd_ui_status(const ParsedArgs & /*args*/, Context &ctx) {
  *ctx.out << "{\n"
           << "  \"contract\": \"cidx.graph-view.v1\",\n"
           << "  \"cytoscape\": \"3.31.2\",\n"
           << "  \"offline\": true,\n"
           << "  \"live_transport\": \"loopback\"\n"
           << "}\n";
  return 0;
}

} // namespace cidx::cli
