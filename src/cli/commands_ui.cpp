#include "cli/commands_detail.hpp"

#include <charconv>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "ui/assets.hpp"
#include "ui/graph_view.hpp"
#include "ui/server.hpp"

namespace cidx::cli {
namespace {

int hex_value(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  return -1;
}

std::string url_decode(std::string_view value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '+') {
      decoded.push_back(' ');
      continue;
    }
    if (value[index] == '%') {
      if (index + 2 >= value.size()) {
        throw std::invalid_argument("malformed URL escape");
      }
      const int high = hex_value(value[index + 1]);
      const int low = hex_value(value[index + 2]);
      if (high < 0 || low < 0) {
        throw std::invalid_argument("malformed URL escape");
      }
      decoded.push_back(static_cast<char>((high << 4) | low));
      index += 2;
      continue;
    }
    decoded.push_back(value[index]);
  }
  return decoded;
}

std::optional<std::string> query_parameter(std::string_view target,
                                           std::string_view name) {
  const std::size_t question = target.find('?');
  if (question == std::string_view::npos) {
    return std::nullopt;
  }
  std::size_t start = question + 1;
  while (start <= target.size()) {
    const std::size_t end = target.find('&', start);
    const std::string_view field = target.substr(
        start,
        end == std::string_view::npos ? target.size() - start : end - start);
    const std::size_t equals = field.find('=');
    const std::string_view encoded_key = field.substr(
        0, equals == std::string_view::npos ? field.size() : equals);
    if (url_decode(encoded_key) == name) {
      return equals == std::string_view::npos
                 ? std::string{}
                 : url_decode(field.substr(equals + 1));
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return std::nullopt;
}

int bounded_depth(std::string_view value) {
  int depth = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), depth);
  if (error != std::errc{} || end != value.data() + value.size() || depth < 0 ||
      depth > 32) {
    throw std::invalid_argument("depth must be between 0 and 32");
  }
  return depth;
}

ui::GraphViewRequest live_request(const ui::GraphViewRequest &base,
                                  std::string_view target) {
  ui::GraphViewRequest request = base;
  if (const auto root = query_parameter(target, "root")) {
    request.root = *root;
    request.query.reset();
  }
  if (const auto query = query_parameter(target, "query")) {
    request.query = *query;
    request.root.reset();
  }
  if (const auto direction = query_parameter(target, "direction")) {
    if (*direction != "in" && *direction != "out") {
      throw std::invalid_argument("direction must be 'in' or 'out'");
    }
    request.direction = *direction;
  }
  if (const auto depth = query_parameter(target, "depth")) {
    request.depth = bounded_depth(*depth);
  }
  return request;
}

struct LiveGraphProvider {
  Storage *db = nullptr;
  ui::GraphViewRequest base_request;

  std::optional<std::string>
  operator()(std::string_view target) const noexcept {
    try {
      const ui::GraphViewRequest request = live_request(base_request, target);
      return json_out::dumps_indent2(ui::build_graph_view(*db, request));
    } catch (const std::exception &) {
      return std::nullopt;
    }
  }
};

ui::GraphViewRequest ui_request(const ParsedArgs &args) {
  ui::GraphViewRequest request;
  request.root = args.ui_root;
  request.query = args.ui_query;
  request.workspace = args.ui_workspace;
  request.direction = args.direction;
  request.depth = args.ui_depth;
  request.node_budget = args.ui_node_budget;
  request.edge_budget = args.ui_edge_budget;
  request.site_budget = args.ui_site_budget;
  request.byte_budget = args.ui_byte_budget;
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
  if (!args.ui_output) {
    *ctx.err << "error: cidx ui export: --output is required\n";
    return 2;
  }
  if (!args.ui_root && !args.ui_query) {
    *ctx.err << "error: cidx ui export: a bounded --root or --query is "
                "required\n";
    return 2;
  }
  auto db = open_ui_storage(args, ctx);
  if (!db) {
    return 1;
  }
  try {
    const auto view = ui::build_graph_view(*db, ui_request(args));
    const std::string html =
        ui::render_html(view, ui::RenderMode::OfflineExport);
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
    const ui::GraphViewRequest base_request = ui_request(args);
    const auto view = ui::build_graph_view(*db, base_request);
    const std::string html =
        ui::render_html(view, ui::RenderMode::LoopbackLive);
    const ui::GraphProvider graph_provider =
        LiveGraphProvider{.db = db.get(), .base_request = base_request};
    return ui::serve_live(
        html, graph_provider,
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
