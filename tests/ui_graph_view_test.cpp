#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <string>

#include "cli/json_out.hpp"
#include "graph/query.hpp"
#include "storage/records.hpp"
#include "storage/storage.hpp"
#include "ui/assets.hpp"
#include "ui/graph_view.hpp"

using cidx::Storage;

namespace {

cidx::Symbol symbol(const char *usr, const char *name) {
  cidx::Symbol result;
  result.usr = usr;
  result.spelling = name;
  result.qual_name = name;
  result.kind = "function";
  result.is_definition = true;
  result.resolved = true;
  return result;
}

} // namespace

TEST_CASE("GraphView is bounded, portable, and carries evidence") {
  Storage db(":memory:");
  const int64_t component = db.add_component("test", "/tmp/cidx-ui-test");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "main.cpp");
  auto sym_a = symbol("USR::a", "ns::a");
  sym_a.file_id = file;
  sym_a.line = 1;
  auto sym_b = symbol("USR::b", "ns::b");
  sym_b.file_id = file;
  sym_b.line = 2;
  const int64_t a = db.add_symbol(sym_a);
  const int64_t b = db.add_symbol(sym_b);
  cidx::Edge edge;
  edge.src_id = a;
  edge.dst_id = b;
  edge.kind = cidx::graph::edge_kinds_map().at("calls");
  const int64_t edge_id = db.add_edge(edge);
  cidx::EdgeSite site;
  site.edge_id = edge_id;
  site.file_id = file;
  site.line = 12;
  site.col = 4;
  db.add_edge_site(site);

  cidx::ui::GraphViewRequest request;
  request.root = "ns::a";
  request.depth = 1;
  request.node_budget = 3;
  request.edge_budget = 1;
  const std::string json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));

  CHECK(json.find("cidx.graph-view.v1") != std::string::npos);
  CHECK(json.find("USR::a") != std::string::npos);
  CHECK(json.find("USR::b") != std::string::npos);
  CHECK(json.find("\"edge_kind\"") == std::string::npos);
  CHECK(json.find("\"symbol_id\"") == std::string::npos);
  CHECK(json.find("\"edge_id\"") == std::string::npos);
  CHECK(json.find("\"source\": \"s:") == std::string::npos);
  CHECK(json.find("\"line\": 12") != std::string::npos);
  CHECK(json.find("\"truncated\": false") != std::string::npos);

  request.root.reset();
  request.query = "ns::a";
  const std::string query_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(query_json.find("\"query_plan\": \"{\\n") != std::string::npos);
  CHECK(query_json.find("USR::b") != std::string::npos);
}

TEST_CASE("GraphView reports ambiguous roots instead of choosing one") {
  Storage db(":memory:");
  db.add_symbol(symbol("USR::one", "ns::ambiguous"));
  db.add_symbol(symbol("USR::two", "ns::ambiguous"));
  cidx::ui::GraphViewRequest request;
  request.root = "ns::ambiguous";
  const std::string json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(json.find("\"status\": \"ambiguous\"") != std::string::npos);
  CHECK(json.find("\"nodes\": []") != std::string::npos);
  CHECK(json.find("USR::one") != std::string::npos);
  CHECK(json.find("USR::two") != std::string::npos);
}

TEST_CASE("GraphView bounds evidence sites") {
  Storage db(":memory:");
  const int64_t component = db.add_component("test", "/tmp/cidx-ui-sites");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "main.cpp");
  auto source = symbol("USR::source", "ns::source");
  source.file_id = file;
  auto target = symbol("USR::target", "ns::target");
  target.file_id = file;
  const int64_t source_id = db.add_symbol(source);
  const int64_t target_id = db.add_symbol(target);
  cidx::Edge edge;
  edge.src_id = source_id;
  edge.dst_id = target_id;
  edge.kind = cidx::graph::edge_kinds_map().at("calls");
  const int64_t edge_id = db.add_edge(edge);
  for (int line = 1; line <= 3; ++line) {
    cidx::EdgeSite site;
    site.edge_id = edge_id;
    site.file_id = file;
    site.line = line;
    site.col = 1;
    db.add_edge_site(site);
  }
  cidx::ui::GraphViewRequest request;
  request.root = "ns::source";
  request.edge_kinds = std::vector<std::string>{"calls"};
  request.depth = 1;
  request.site_budget = 1;
  const std::string json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(json.find("\"sites_used\": 1") != std::string::npos);
  CHECK(json.find("\"evidence_truncated\": true") != std::string::npos);
}

TEST_CASE("GraphView refuses to enumerate without a bounded root") {
  Storage db(":memory:");
  cidx::ui::GraphViewRequest request;
  const std::string json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(json.find("a bounded --root is required") != std::string::npos);
  CHECK(json.find("\"nodes\": []") != std::string::npos);
}

TEST_CASE("GraphView export is self-contained and offline") {
  Storage db(":memory:");
  cidx::ui::GraphViewRequest request;
  const std::string html =
      cidx::ui::render_html(cidx::ui::build_graph_view(db, request));
  CHECK(html.find("Cytoscape") != std::string::npos);
  CHECK(html.find("window.CIDX_GRAPH_VIEW") != std::string::npos);
  CHECK(html.find("cytoscape") != std::string::npos);
  CHECK(html.find("https://") == std::string::npos);
}
