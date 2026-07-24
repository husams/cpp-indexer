#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <string>

#include "cli/json_out.hpp"
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
  edge.kind = 1;
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
  CHECK(json.find("\"line\": 12") != std::string::npos);
  CHECK(json.find("\"truncated\": false") != std::string::npos);
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
