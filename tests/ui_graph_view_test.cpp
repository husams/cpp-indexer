#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <string>

#include "cli/json_out.hpp"
#include "graph/query.hpp"
#include "storage/records.hpp"
#include "storage/storage.hpp"
#include "ui/assets.hpp"
#include "ui/graph_view.hpp"
#include "util/errors.hpp"

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

std::string first_node_id(const std::string &json) {
  const std::string marker = "\"nodes\": [\n    {\n      \"id\": \"";
  const std::size_t start = json.find(marker);
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t value_start = start + marker.size();
  const std::size_t value_end = json.find('"', value_start);
  return value_end == std::string::npos
             ? std::string{}
             : json.substr(value_start, value_end - value_start);
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

  const std::string emitted_id = first_node_id(json);
  CHECK(emitted_id.starts_with("symbol:v1:"));

  request.root.reset();
  request.query = "ns::a";
  const std::string query_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(query_json.find("\"query_plan\": \"{\\n") != std::string::npos);
  CHECK(query_json.find("USR::b") != std::string::npos);

  request.query = emitted_id;
  const std::string roundtrip_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(roundtrip_json.find("\"status\": \"exact_portable\"") !=
        std::string::npos);
  CHECK(roundtrip_json.find("USR::b") != std::string::npos);

  request.query = "ns::a";
  request.edge_kinds = std::vector<std::string>{"calls"};
  request.byte_budget = 16384;
  const std::string bounded_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(bounded_json.size() <= 16384);
}

TEST_CASE("GraphView snapshot identity and export bytes are deterministic") {
  Storage db(":memory:");
  const int64_t component =
      db.add_component("test", "/tmp/cidx-ui-determinism");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "main.cpp");
  auto source = symbol("USR::det-source", "ns::det_source");
  source.file_id = file;
  const int64_t source_id = db.add_symbol(source);
  auto target = symbol("USR::det-target", "ns::det_target");
  target.file_id = file;
  const int64_t target_id = db.add_symbol(target);
  cidx::Edge edge;
  edge.src_id = source_id;
  edge.dst_id = target_id;
  edge.kind = cidx::graph::edge_kinds_map().at("calls");
  db.add_edge(edge);

  cidx::ui::GraphViewRequest request;
  request.root = "ns::det_source";
  request.edge_kinds = std::vector<std::string>{"calls"};
  request.depth = 1;
  const auto first = cidx::ui::build_graph_view(db, request);
  const auto second = cidx::ui::build_graph_view(db, request);
  const std::string first_json = cidx::json_out::dumps_indent2(first);
  CHECK(first_json == cidx::json_out::dumps_indent2(second));
  CHECK(first_json.find("\"query_identity\": \"") != std::string::npos);
  CHECK(first_json.find("\"result_id\": \"") != std::string::npos);
  CHECK(first_json.find("\"fact_sets\": [\n        \"symbols\"") !=
        std::string::npos);
  CHECK(first_json.find("\"status\": \"unknown\"") != std::string::npos);

  const std::string first_html = cidx::ui::render_html(first);
  CHECK(first_html == cidx::ui::render_html(second));
  CHECK(first_html.find("window.CIDX_OFFLINE = true") != std::string::npos);
  CHECK(first_html.find("cidx.offline-snapshot.v1") != std::string::npos);
  CHECK(first_html.find("https://") == std::string::npos);
}

TEST_CASE("GraphView live rendering keeps offline export transport disabled") {
  Storage db(":memory:");
  cidx::ui::GraphViewRequest request;
  const auto view = cidx::ui::build_graph_view(db, request);
  const std::string html =
      cidx::ui::render_html(view, cidx::ui::RenderMode::LoopbackLive);
  CHECK(html.find("window.CIDX_OFFLINE = false") != std::string::npos);
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

TEST_CASE("GraphView reports evidence beyond the site budget") {
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
  for (int line = 1; line <= 201; ++line) {
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
  request.site_budget = 200;
  const std::string json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(json.find("\"sites_used\": 200") != std::string::npos);
  CHECK(json.find("\"evidence_truncated\": true") != std::string::npos);
}

TEST_CASE("GraphView enforces the byte budget or fails explicitly") {
  Storage db(":memory:");
  cidx::ui::GraphViewRequest request;
  request.query = std::string(5000, 'q');
  request.byte_budget = 1024;
  CHECK_THROWS_AS(cidx::ui::build_graph_view(db, request), cidx::CidxError);
}

TEST_CASE(
    "GraphView keeps retained element status and site counters consistent") {
  Storage db(":memory:");
  const int64_t component = db.add_component("test", "/tmp/cidx-ui-trim");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "main.cpp");
  auto source = symbol("USR::trim-source", "ns::trim_source");
  source.file_id = file;
  auto target_a = symbol("USR::trim-target-a", "ns::trim_target_a");
  target_a.file_id = file;
  auto target_b = symbol("USR::trim-target-b", "ns::trim_target_b");
  target_b.file_id = file;
  const int64_t source_id = db.add_symbol(source);
  const int64_t target_a_id = db.add_symbol(target_a);
  const int64_t target_b_id = db.add_symbol(target_b);

  for (const int64_t target_id : {target_a_id, target_b_id}) {
    cidx::Edge edge;
    edge.src_id = source_id;
    edge.dst_id = target_id;
    edge.kind = cidx::graph::edge_kinds_map().at("calls");
    const int64_t edge_id = db.add_edge(edge);
    cidx::EdgeSite site;
    site.edge_id = edge_id;
    site.file_id = file;
    site.line = target_id == target_a_id ? 10 : 20;
    site.col = 1;
    site.args_sig = std::string(1000, '<');
    db.add_edge_site(site);
  }

  CHECK(db.graph_edges(
              source_id, "out",
              std::vector<int64_t>{cidx::graph::edge_kinds_map().at("calls")},
              false, 10)
            .size() == 2);

  cidx::ui::GraphViewRequest request;
  request.root = "ns::trim_source";
  request.edge_kinds = std::vector<std::string>{"calls"};
  request.depth = 1;
  request.node_budget = 3;
  request.edge_budget = 2;
  request.site_budget = 2;
  request.byte_budget = 16000;
  const std::string json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));

  CHECK(json.find("\"sites_used\": 1") != std::string::npos);
  CHECK(json.find("\"evidence_truncated\": true") != std::string::npos);
  const std::size_t edge_start = json.find("\"edges\": [");
  const std::size_t edge_status = json.find("\"status\": {", edge_start);
  CHECK(edge_start != std::string::npos);
  CHECK(edge_status != std::string::npos);
  CHECK(json.find("\"truncated\": true", edge_status) != std::string::npos);
  CHECK(json.find("\"evidence_truncated\": true", edge_status) !=
        std::string::npos);
  CHECK(json.find("\"edges\": [\n    {") != std::string::npos);
  CHECK(json.find("\"edges\": [\n    {",
                  json.find("\"edges\": [\n    {") + 1) == std::string::npos);
}

TEST_CASE("GraphView export enforces the byte budget after script escaping") {
  Storage db(":memory:");
  cidx::ui::GraphViewRequest request;
  request.query =
      std::string(150, '<') + std::string(150, '&') + std::string(150, '>');
  request.byte_budget = 8192;
  const auto view = cidx::ui::build_graph_view(db, request);
  const std::string html = cidx::ui::render_html(view);
  const std::string marker = "window.CIDX_GRAPH_VIEW = ";
  const std::size_t start = html.find(marker);
  const std::size_t end = html.find("</script>", start);

  REQUIRE(start != std::string::npos);
  REQUIRE(end != std::string::npos);
  CHECK(end - (start + marker.size()) <= request.byte_budget);
  CHECK(html.find("\\u003c", start) != std::string::npos);
  CHECK(html.find("\\u0026", start) != std::string::npos);
  CHECK(html.find("\\u003e", start) != std::string::npos);
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
