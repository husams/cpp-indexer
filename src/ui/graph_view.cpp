#include "ui/graph_view.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <string_view>
#include <utility>

#include "graph/query.hpp"
#include "graph/records.hpp"
#include "util/pathutil.hpp"

namespace cidx::ui {
namespace {

using json_out::Array;
using json_out::Object;
using json_out::Value;

constexpr int kGraphViewVersion = 1;

Value optional_string(const std::optional<std::string> &value) {
  return value ? Value::of(*value) : Value::null();
}

Value optional_int(const std::optional<int64_t> &value) {
  return value ? Value::of(*value) : Value::null();
}

std::string redacted_path(const std::string &path,
                          const std::optional<std::string> &workspace) {
  if (workspace && !workspace->empty()) {
    const std::string prefix = *workspace + "/";
    if (path.starts_with(prefix)) {
      return path.substr(prefix.size());
    }
  }
  return pathutil::basename(path);
}

std::string location(const graph::Sym &sym,
                     const std::optional<std::string> &workspace) {
  if (!sym.file) {
    return "<no-location>";
  }
  std::string out = redacted_path(*sym.file, workspace);
  if (sym.line) {
    out += ":" + std::to_string(*sym.line);
    if (sym.col) {
      out += ":" + std::to_string(*sym.col);
    }
  }
  return out;
}

std::string symbol_color(const graph::Sym &sym) {
  if (sym.external) {
    return "#f37777";
  }
  return sym.resolved ? "#65d6c3" : "#f0b35e";
}

Value status(const graph::Sym &sym, const std::string &freshness,
             bool truncated) {
  Object out;
  out.emplace_back("completeness", Value::of(std::string(
                                       sym.resolved ? "complete" : "partial")));
  out.emplace_back("freshness", Value::of(freshness));
  out.emplace_back("resolved", Value::of(sym.resolved));
  out.emplace_back("external", Value::of(sym.external));
  out.emplace_back("stub", Value::of(sym.is_stub()));
  out.emplace_back("truncated", Value::of(truncated));
  out.emplace_back("inferred", Value::of(false));
  out.emplace_back("assumed", Value::of(false));
  out.emplace_back("refuted", Value::of(false));
  out.emplace_back("proved", Value::of(false));
  return Value::obj(std::move(out));
}

Value node_value(const graph::Sym &sym, const std::string &freshness,
                 bool truncated, int depth,
                 const std::optional<std::string> &workspace) {
  Object out;
  out.emplace_back("id", Value::of(std::string("s:") + std::to_string(sym.id)));
  out.emplace_back("symbol_id", Value::of(sym.id));
  out.emplace_back("usr", Value::of(sym.usr));
  out.emplace_back("identity_key", Value::of(sym.identity_key));
  out.emplace_back("name", Value::of(sym.name.empty() ? sym.usr : sym.name));
  out.emplace_back("kind", Value::of(sym.kind));
  out.emplace_back("location", Value::of(location(sym, workspace)));
  out.emplace_back("file", sym.file
                               ? Value::of(redacted_path(*sym.file, workspace))
                               : Value::null());
  out.emplace_back("line", optional_int(sym.line));
  out.emplace_back("col", optional_int(sym.col));
  out.emplace_back("depth", Value::of(depth));
  out.emplace_back("status", status(sym, freshness, truncated));
  out.emplace_back("color", Value::of(symbol_color(sym)));
  out.emplace_back("border", Value::of(symbol_color(sym)));
  out.emplace_back("evidence",
                   Value::of(sym.file ? location(sym, workspace)
                                      : "No indexed source evidence"));
  return Value::obj(std::move(out));
}

Value site_value(const graph::Site &site,
                 const std::optional<std::string> &workspace) {
  Object out;
  out.emplace_back("file", site.file
                               ? Value::of(redacted_path(*site.file, workspace))
                               : Value::null());
  out.emplace_back("line", optional_int(site.line));
  out.emplace_back("col", optional_int(site.col));
  out.emplace_back("conditional", Value::of(site.conditional));
  out.emplace_back("args_sig", optional_string(site.args_sig));
  std::string loc =
      site.file ? redacted_path(*site.file, workspace) : "<no-location>";
  if (site.line) {
    loc += ":" + std::to_string(*site.line);
    if (site.col) {
      loc += ":" + std::to_string(*site.col);
    }
  }
  out.emplace_back("location", Value::of(std::move(loc)));
  return Value::obj(std::move(out));
}

Value edge_value(const graph::Edge &edge, const std::string &freshness,
                 bool truncated, const std::optional<std::string> &workspace) {
  Object out;
  out.emplace_back("id",
                   Value::of(std::string("e:") + std::to_string(edge.edge_id)));
  out.emplace_back("edge_id", Value::of(edge.edge_id));
  out.emplace_back("source",
                   Value::of(std::string("s:") + std::to_string(edge.src_id)));
  out.emplace_back("target",
                   Value::of(std::string("s:") + std::to_string(edge.dst_id)));
  out.emplace_back("kind", Value::of(edge.kind));
  out.emplace_back("count", Value::of(edge.count));
  out.emplace_back("status", [&] {
    Object s;
    s.emplace_back("completeness", Value::of(std::string("complete")));
    s.emplace_back("freshness", Value::of(freshness));
    s.emplace_back("truncated", Value::of(truncated));
    s.emplace_back("external", Value::of(edge.peer.external));
    return Value::obj(std::move(s));
  }());
  Array sites;
  for (const auto &site : edge.sites) {
    sites.push_back(site_value(site, workspace));
  }
  out.emplace_back("sites", Value::arr(std::move(sites)));
  out.emplace_back("color", Value::of(std::string(
                                edge.peer.external ? "#f37777" : "#7891aa")));
  return Value::obj(std::move(out));
}

std::optional<graph::Sym> select_root(graph::GraphQuery &graph,
                                      const std::optional<std::string> &root) {
  if (!root || root->empty()) {
    return std::nullopt;
  }
  if (std::ranges::all_of(
          *root, [](unsigned char c) { return std::isdigit(c) != 0; })) {
    try {
      return graph.get_by_id(std::stoll(*root));
    } catch (const std::exception &) {
      return std::nullopt;
    }
  }
  if (auto exact = graph.get_by_usr(*root)) {
    return exact;
  }
  auto matches = graph.find(*root, std::nullopt, 2);
  if (!matches.empty()) {
    return matches.front();
  }
  return std::nullopt;
}

} // namespace

Value build_graph_view(Storage &db, const GraphViewRequest &request) {
  graph::GraphQuery graph(db, "<ui>");
  const IndexIdentity identity = db.index_identity();
  const std::string freshness = identity.freshness;
  const int node_budget = std::max(1, request.node_budget);
  const int edge_budget = std::max(1, request.edge_budget);
  const auto root = select_root(graph, request.root);

  Array nodes;
  Array edges;
  bool truncated = false;
  Object metadata;
  metadata.emplace_back("contract",
                        Value::of(std::string("cidx.graph-view.v1")));
  metadata.emplace_back("version", Value::of(kGraphViewVersion));
  metadata.emplace_back("freshness", Value::of(freshness));
  metadata.emplace_back("graph_resolved", Value::of(db.graph_resolved()));
  metadata.emplace_back("node_budget", Value::of(node_budget));
  metadata.emplace_back("edge_budget", Value::of(edge_budget));
  metadata.emplace_back("depth", Value::of(request.depth));
  metadata.emplace_back("direction", Value::of(request.direction));
  metadata.emplace_back("query", optional_string(request.query));
  metadata.emplace_back("workspace",
                        request.workspace
                            ? Value::of(pathutil::basename(*request.workspace))
                            : Value::null());
  Object index;
  index.emplace_back("schema_version", Value::of(identity.schema_version));
  index.emplace_back("source_revision",
                     optional_string(identity.source_revision));
  index.emplace_back("source_fingerprint",
                     optional_string(identity.source_fingerprint));
  index.emplace_back("index_config", optional_string(identity.index_config));
  index.emplace_back("index_config_fingerprint",
                     optional_string(identity.index_config_fingerprint));
  index.emplace_back("freshness", Value::of(identity.freshness));
  metadata.emplace_back("index", Value::obj(std::move(index)));

  if (root) {
    const auto traversal =
        graph.walk(root->id, request.edge_kinds, request.direction,
                   std::clamp(request.depth, 0, 32), node_budget);
    const auto ordered_nodes = traversal.nodes();
    truncated = ordered_nodes.size() >= static_cast<std::size_t>(node_budget);
    for (const auto &sym : ordered_nodes) {
      const int depth = traversal.depth_by_id.contains(sym.id)
                            ? traversal.depth_by_id.at(sym.id)
                            : 0;
      nodes.push_back(
          node_value(sym, freshness, truncated, depth, request.workspace));
    }
    std::map<int64_t, graph::Edge> by_id;
    for (const auto &sym : ordered_nodes) {
      const auto adjacent =
          request.direction == "in"
              ? graph.edges_in(sym.id, request.edge_kinds, edge_budget + 1)
              : graph.edges_out(sym.id, request.edge_kinds, edge_budget + 1);
      for (const auto &edge : adjacent) {
        if (traversal.nodes_by_id.contains(edge.peer.id)) {
          by_id.emplace(edge.edge_id, edge);
        }
      }
    }
    if (by_id.size() > static_cast<std::size_t>(edge_budget)) {
      truncated = true;
    }
    int emitted = 0;
    for (const auto &[id, edge] : by_id) {
      if (emitted++ == edge_budget) {
        break;
      }
      edges.push_back(
          edge_value(edge, freshness, truncated, request.workspace));
    }
  } else {
    metadata.emplace_back(
        "empty_reason", Value::of(std::string("a bounded --root is required")));
  }
  metadata.emplace_back("truncated", Value::of(truncated));

  Object out;
  out.emplace_back("schema", Value::of(std::string("cidx.graph-view.v1")));
  out.emplace_back("request", [&] {
    Object r;
    r.emplace_back("root", optional_string(request.root));
    r.emplace_back("query", optional_string(request.query));
    r.emplace_back("direction", Value::of(request.direction));
    r.emplace_back("depth", Value::of(request.depth));
    return Value::obj(std::move(r));
  }());
  out.emplace_back("metadata", Value::obj(std::move(metadata)));
  out.emplace_back("nodes", Value::arr(std::move(nodes)));
  out.emplace_back("edges", Value::arr(std::move(edges)));
  out.emplace_back("view_state", Value::obj({}));
  return Value::obj(std::move(out));
}

} // namespace cidx::ui
