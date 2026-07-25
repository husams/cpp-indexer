#include "cli/commands_detail.hpp"

#include <charconv>
#include <fstream>
#include <limits>
#include <map>
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

int bounded_int(std::string_view value, int lo, int hi,
               std::string_view field) {
  int parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() ||
      parsed < lo || parsed > hi) {
    throw std::invalid_argument(std::string(field) + " out of range");
  }
  return parsed;
}

std::vector<std::string> split_comma_list(std::string_view text) {
  std::vector<std::string> items;
  std::string current;
  for (const char c : text) {
    if (c == ',') {
      if (!current.empty()) {
        items.push_back(current);
        current.clear();
      }
    } else if (c != ' ') {
      current += c;
    }
  }
  if (!current.empty()) {
    items.push_back(std::move(current));
  }
  return items;
}

std::optional<std::vector<std::string>>
comma_list_param(std::string_view target, std::string_view name) {
  const auto raw = query_parameter(target, name);
  if (!raw) {
    return std::nullopt;
  }
  auto items = split_comma_list(*raw);
  if (items.empty()) {
    return std::nullopt;
  }
  return items;
}

// Every filter/budget/typed-input field a live `/api/graph` request may
// override on top of the base request `cidx ui open` was started with. This
// is the SAME GraphViewRequest surface `ui_request()` builds from CLI flags
// below, so live navigation and the offline exporter share one filter
// vocabulary (HSE-92 preserves the HSE-90/HSE-91 GraphView contract).
ui::GraphViewRequest live_request(const ui::GraphViewRequest &base,
                                  std::string_view target) {
  ui::GraphViewRequest request = base;
  if (const auto input_kind = query_parameter(target, "input_kind")) {
    static const std::map<std::string, ui::GraphInputKind> kinds{
        {"symbol", ui::GraphInputKind::Symbol},
        {"file", ui::GraphInputKind::File},
        {"entity", ui::GraphInputKind::Entity},
        {"type", ui::GraphInputKind::Type},
        {"cxq", ui::GraphInputKind::Cxq},
        {"plan", ui::GraphInputKind::QueryPlan},
        {"path", ui::GraphInputKind::Path},
    };
    const auto kind = kinds.find(*input_kind);
    if (kind == kinds.end()) {
      throw std::invalid_argument("unknown input_kind '" + *input_kind + "'");
    }
    const auto input = query_parameter(target, "input");
    if (!input) {
      throw std::invalid_argument("input_kind requires input");
    }
    request.input = ui::GraphViewInput{.kind = kind->second, .value = *input};
    request.root.reset();
    request.query.reset();
  } else if (const auto root = query_parameter(target, "root")) {
    request.root = *root;
    request.query.reset();
    request.input =
        ui::GraphViewInput{.kind = ui::GraphInputKind::Symbol, .value = *root};
  } else if (const auto query = query_parameter(target, "query")) {
    request.query = *query;
    request.root.reset();
    request.input =
        ui::GraphViewInput{.kind = ui::GraphInputKind::Symbol, .value = *query};
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
  if (const auto value = query_parameter(target, "limit")) {
    request.node_budget = bounded_int(*value, 1, 10000, "limit");
  }
  if (const auto value = query_parameter(target, "edge_limit")) {
    request.edge_budget = bounded_int(*value, 1, 20000, "edge_limit");
  }
  if (const auto value = query_parameter(target, "site_limit")) {
    request.site_budget = bounded_int(*value, 0, 20000, "site_limit");
  }
  if (const auto value = query_parameter(target, "byte_limit")) {
    request.byte_budget =
        bounded_int(*value, 1024, 64 * 1024 * 1024, "byte_limit");
  }
  if (const auto edge = comma_list_param(target, "edge")) {
    request.edge_kinds = edge;
  }
  if (const auto node_kind = comma_list_param(target, "node_kind")) {
    request.node_kinds = node_kind;
  }
  if (const auto file = comma_list_param(target, "file")) {
    request.files = file;
  }
  if (const auto component = comma_list_param(target, "component")) {
    request.components = component;
  }
  if (const auto repository = comma_list_param(target, "repository")) {
    request.repositories = repository;
  }
  if (const auto status = query_parameter(target, "status")) {
    request.status_filter = *status;
  }
  if (const auto applicability = query_parameter(target, "applicability")) {
    request.applicability_filter = *applicability;
  }
  if (const auto continuation = query_parameter(target, "continuation")) {
    request.continuation = *continuation;
  } else {
    request.continuation.reset();
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

struct LiveSearchProvider {
  Storage *db = nullptr;
  std::optional<std::string> workspace;

  std::optional<std::string>
  operator()(std::string_view target) const noexcept {
    try {
      const auto text = query_parameter(target, "q");
      if (!text || text->empty()) {
        return std::nullopt;
      }
      const auto kind = query_parameter(target, "kind");
      int limit = 25;
      if (const auto value = query_parameter(target, "limit")) {
        limit = bounded_int(*value, 1, 500, "limit");
      }
      return json_out::dumps_indent2(
          ui::search_candidates(*db, *text, kind, workspace, limit));
    } catch (const std::exception &) {
      return std::nullopt;
    }
  }
};

struct LiveEvidenceProvider {
  Storage *db = nullptr;
  std::optional<std::string> workspace;

  std::optional<std::string>
  operator()(std::string_view target) const noexcept {
    try {
      const auto edge_id = query_parameter(target, "edge");
      if (!edge_id || edge_id->empty()) {
        return std::nullopt;
      }
      int site_offset = 0;
      if (const auto value = query_parameter(target, "site_offset")) {
        site_offset = bounded_int(*value, 0, 1'000'000, "site_offset");
      }
      int site_limit = 200;
      if (const auto value = query_parameter(target, "site_limit")) {
        site_limit = bounded_int(*value, 1, 5000, "site_limit");
      }
      return json_out::dumps_indent2(ui::load_edge_evidence(
          *db, *edge_id, workspace, site_offset, site_limit));
    } catch (const std::exception &) {
      return std::nullopt;
    }
  }
};

ui::GraphViewRequest ui_request(const ParsedArgs &args) {
  ui::GraphViewRequest request;
  request.root = args.ui_root;
  request.query = args.ui_query;
  if (args.ui_input) {
    static const std::map<std::string, ui::GraphInputKind> kinds{
        {"symbol", ui::GraphInputKind::Symbol},
        {"file", ui::GraphInputKind::File},
        {"entity", ui::GraphInputKind::Entity},
        {"type", ui::GraphInputKind::Type},
        {"cxq", ui::GraphInputKind::Cxq},
        {"plan", ui::GraphInputKind::QueryPlan},
        {"path", ui::GraphInputKind::Path},
    };
    const auto kind = kinds.find(args.ui_input_kind);
    if (kind == kinds.end()) {
      throw ui::GraphViewError(ui::GraphViewFailureKind::InvalidInput,
                               "unknown --input-kind '" + args.ui_input_kind +
                                   "'",
                               "choose a supported typed GraphView input kind");
    }
    if (request.root || request.query) {
      throw ui::GraphViewError(
          ui::GraphViewFailureKind::InvalidInput,
          "--input cannot be combined with --root or --query",
          "provide exactly one normalized input");
    }
    request.input =
        ui::GraphViewInput{.kind = kind->second, .value = *args.ui_input};
  } else if (request.root || request.query) {
    request.input = ui::GraphViewInput{.kind = ui::GraphInputKind::Symbol,
                                       .value = request.root ? *request.root
                                                             : *request.query};
  }
  request.workspace = args.ui_workspace;
  request.direction = args.direction;
  request.depth = args.ui_depth;
  request.node_budget = args.ui_node_budget;
  request.edge_budget = args.ui_edge_budget;
  request.site_budget = args.ui_site_budget;
  request.byte_budget = args.ui_byte_budget;
  if (args.edge) {
    auto kinds = split_comma_list(*args.edge);
    if (!kinds.empty()) {
      request.edge_kinds = std::move(kinds);
    }
  }
  if (args.ui_node_kind) {
    auto kinds = split_comma_list(*args.ui_node_kind);
    if (!kinds.empty()) {
      request.node_kinds = std::move(kinds);
    }
  }
  if (args.ui_file) {
    auto files = split_comma_list(*args.ui_file);
    if (!files.empty()) {
      request.files = std::move(files);
    }
  }
  if (args.ui_component) {
    auto components = split_comma_list(*args.ui_component);
    if (!components.empty()) {
      request.components = std::move(components);
    }
  }
  if (args.ui_repository) {
    auto repositories = split_comma_list(*args.ui_repository);
    if (!repositories.empty()) {
      request.repositories = std::move(repositories);
    }
  }
  request.status_filter = args.ui_status;
  request.applicability_filter = args.ui_applicability;
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
  if (!args.ui_root && !args.ui_query && !args.ui_input) {
    *ctx.err << "error: cidx ui export: a bounded typed input is required "
                "(--root/--query or --input-kind + --input)\n";
    return 2;
  }
  auto db = open_ui_storage(args, ctx);
  if (!db) {
    return 1;
  }
  try {
    auto request = ui_request(args);
    request.strict = true;
    const auto view = ui::build_graph_view(*db, request);
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
  } catch (const ui::GraphViewError &error) {
    *ctx.err << "error: cidx ui export: " << error.code() << ": "
             << error.what() << "; next: " << error.next_action() << "\n";
    return 2;
  } catch (const std::exception &error) {
    *ctx.err << "error: cidx ui export: " << error.what() << "\n";
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
    const ui::GraphProvider search_provider =
        LiveSearchProvider{.db = db.get(), .workspace = args.ui_workspace};
    const ui::GraphProvider evidence_provider =
        LiveEvidenceProvider{.db = db.get(), .workspace = args.ui_workspace};
    return ui::serve_live(
        html, graph_provider, search_provider, evidence_provider,
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
           << "  \"live_transport\": \"loopback\",\n"
           << "  \"live_operations\": ["
              "\"graph\", \"search\", \"evidence\", \"shutdown\"]\n"
           << "}\n";
  return 0;
}

} // namespace cidx::cli
