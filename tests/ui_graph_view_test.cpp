#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <string>
#include <string_view>

#include "cli/json_out.hpp"
#include "graph/query.hpp"
#include "query/plan.hpp"
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

std::string result_id(const std::string &json) {
  const std::string marker = "\"result_id\": \"";
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

std::string hex_encode(std::string_view value) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    encoded.push_back(digits[byte >> 4]);
    encoded.push_back(digits[byte & 0x0f]);
  }
  return encoded;
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
  CHECK(first_json.find("\"fact_sets\": [") != std::string::npos);
  CHECK(first_json.find("\"edges\"") != std::string::npos);
  CHECK(first_json.find("\"catalog_hash\"") != std::string::npos);
  CHECK(result_id(first_json).size() > 64);
  request.node_budget = 2;
  const std::string changed_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(result_id(changed_json) != result_id(first_json));
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
  request.strict = true;
  CHECK_THROWS_AS(cidx::ui::build_graph_view(db, request),
                  cidx::ui::GraphViewError);
}

TEST_CASE("GraphView normalizes typed file, path, and CXQ inputs") {
  Storage db(":memory:");
  const int64_t component = db.add_component("test", "/tmp/cidx-ui-typed");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "main.cpp");
  const int64_t header = db.add_file(directory, "header.hpp");
  auto source = symbol("USR::typed-source", "ns::typed_source");
  source.file_id = file;
  auto target = symbol("USR::typed-target", "ns::typed_target");
  target.file_id = file;
  const int64_t source_id = db.add_symbol(source);
  const int64_t target_id = db.add_symbol(target);
  const int64_t include_config = db.add_include_config(
      {.tu_file_id = file,
       .digest = "typed",
       .working_dir = "/secret/checkout",
       .arguments = {"-DSECRET_TOKEN=top-secret", "/secret/checkout/include"}});
  const int64_t include_edge_id =
      db.add_include_edge({.src_file_id = file,
                           .dst_file_id = header,
                           .dst_path = "/tmp/cidx-ui-typed/header.hpp",
                           .config_id = include_config});
  db.add_include_site({.edge_id = include_edge_id,
                       .line = 3,
                       .col = 1,
                       .begin_offset = 20,
                       .end_offset = 38,
                       .spelling = "header.hpp",
                       .is_angled = true});
  cidx::Edge edge{.src_id = source_id,
                  .dst_id = target_id,
                  .kind = cidx::graph::edge_kinds_map().at("calls")};
  db.add_edge(edge);

  cidx::ui::GraphViewRequest request;
  request.input =
      cidx::ui::GraphViewInput{.kind = cidx::ui::GraphInputKind::File,
                               .value = "/tmp/cidx-ui-typed/main.cpp"};
  const std::string file_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(file_json.find("\"input_kind\": \"file\"") != std::string::npos);
  CHECK(file_json.find("USR::typed-source") != std::string::npos);
  CHECK(file_json.find("\"files\"") != std::string::npos);
  CHECK(file_json.find("\"includes\"") != std::string::npos);
  CHECK(file_json.find("file:v2:sha256:") != std::string::npos);
  CHECK(file_json.find("include-edge:v1:") != std::string::npos);
  CHECK(file_json.find("\"configuration\"") != std::string::npos);
  CHECK(file_json.find("arguments_redacted") != std::string::npos);
  CHECK(file_json.find("SECRET_TOKEN") == std::string::npos);
  CHECK(file_json.find("top-secret") == std::string::npos);
  CHECK(file_json.find("/secret/checkout") == std::string::npos);
  CHECK(file_json.find("\"spelling\"") != std::string::npos);

  request.input =
      cidx::ui::GraphViewInput{.kind = cidx::ui::GraphInputKind::Path,
                               .value = "ns::typed_source->ns::typed_target"};
  const std::string path_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(path_json.find("\"input_kind\": \"path\"") != std::string::npos);
  CHECK(path_json.find("witness-path") != std::string::npos);

  request.input = cidx::ui::GraphViewInput{
      .kind = cidx::ui::GraphInputKind::Cxq,
      .value = "symbol('USR::typed-source') | out(calls)"};
  const std::string cxq_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(cxq_json.find("\"input_kind\": \"cxq\"") != std::string::npos);
  CHECK(cxq_json.find("\"query_plan\": \"{\\n") != std::string::npos);

  request.input = cidx::ui::GraphViewInput{
      .kind = cidx::ui::GraphInputKind::QueryPlan,
      .value = cidx::query::canonical_json(
          cidx::query::start(cidx::query::symbol("USR::typed-source")).plan())};
  const std::string plan_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(plan_json.find("\"input_kind\": \"plan\"") != std::string::npos);
  CHECK(plan_json.find("\"query_plan\": \"{\\n") != std::string::npos);

  request.input = cidx::ui::GraphViewInput{
      .kind = cidx::ui::GraphInputKind::Cxq,
      .value = "symbol('USR::typed-source') | count()"};
  CHECK_THROWS_AS(cidx::ui::build_graph_view(db, request),
                  cidx::ui::GraphViewError);

  auto entity_insert = db.raw_db().prepare(
      "INSERT INTO entity_node (id, kind) VALUES (?, 1), (?, 1)");
  entity_insert.bind(1, source_id);
  entity_insert.bind(2, target_id);
  entity_insert.step_done();
  db.add_entity_edge(source_id, target_id, 8);
  request.input = cidx::ui::GraphViewInput{
      .kind = cidx::ui::GraphInputKind::Entity, .value = "ns::typed_source"};
  const std::string entity_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(entity_json.find("\"input_kind\": \"entity\"") != std::string::npos);
  CHECK(entity_json.find("entity:v1:") != std::string::npos);
  CHECK(entity_json.find("entity-edge:v1:") != std::string::npos);
  CHECK(entity_json.find("\"entities\"") != std::string::npos);
  CHECK(entity_json.find("\"entity_edges\"") != std::string::npos);

  request.input = cidx::ui::GraphViewInput{
      .kind = cidx::ui::GraphInputKind::QueryPlan,
      .value = cidx::query::canonical_json(
          cidx::query::start(cidx::query::entity("ns::typed_source")).plan())};
  const std::string entity_plan_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(entity_plan_json.find("entity:v1:") != std::string::npos);
  CHECK(entity_plan_json.find("\"entities\"") != std::string::npos);
}

TEST_CASE("GraphView fixtures preserve type, stub, and stale markers") {
  Storage db(":memory:");
  const int64_t component = db.add_component("test", "/tmp/cidx-ui-fixtures");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file =
      db.add_file(directory, "fixture.cpp", 1.0, "fixture-md5");
  auto type_owner = symbol("USR::fixture-owner", "ns::fixture_owner");
  type_owner.file_id = file;
  const int64_t owner_id = db.add_symbol(type_owner);
  cidx::TypeNode type{.type_key = "record:Fixture",
                      .spelling = "Fixture",
                      .kind = cidx::kTypeKindRecord,
                      .decl_usr = "USR::fixture-owner"};
  const int64_t type_id = db.intern_type_node(type);
  const int64_t pointee_id =
      db.intern_type_node({.type_key = "builtin:int",
                           .spelling = "int",
                           .kind = cidx::kTypeKindBuiltin});
  db.add_type_edge(type_id, 1, 0, pointee_id);
  db.add_symbol_type(owner_id, cidx::kSymbolTypeOfType, type_id);
  auto stub = symbol("USR::fixture-stub", "std::fixture_stub");
  stub.resolved = false;
  stub.decl_path = "/usr/include/fixture.hpp";
  db.add_symbol(stub);

  cidx::ui::GraphViewRequest request;
  request.input = cidx::ui::GraphViewInput{
      .kind = cidx::ui::GraphInputKind::Type, .value = "USR::fixture-owner"};
  const std::string type_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(type_json.find("\"input_kind\": \"type\"") != std::string::npos);
  CHECK(type_json.find("\"types\"") != std::string::npos);
  CHECK(type_json.find("type:v1:") != std::string::npos);
  CHECK(type_json.find("\"kind\": \"type\"") != std::string::npos);
  CHECK(type_json.find("type-edge:v1:") != std::string::npos);

  request.input = cidx::ui::GraphViewInput{
      .kind = cidx::ui::GraphInputKind::Symbol, .value = "USR::fixture-stub"};
  const std::string stub_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(stub_json.find("\"external\": true") != std::string::npos);
  CHECK(stub_json.find("\"stub\": true") != std::string::npos);

  request.input =
      cidx::ui::GraphViewInput{.kind = cidx::ui::GraphInputKind::File,
                               .value = "/tmp/cidx-ui-fixtures/fixture.cpp"};
  db.mark_file_indexed(file, 1.0, "fixture-md5");
  db.stamp_index_identity();
  db.raw_db().exec(
      "UPDATE meta SET value='stale-fixture' WHERE key='source_fingerprint'");
  db.set_file_indexed(file, false);
  const std::string stale_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(stale_json.find("\"unknown\"") != std::string::npos);
  CHECK(stale_json.find("\"stale\"") != std::string::npos);
}

TEST_CASE("GraphView path export keeps the ordered witness and omits chords") {
  Storage db(":memory:");
  const int64_t a = db.add_symbol(symbol("USR::path-a", "path_a"));
  const int64_t b = db.add_symbol(symbol("USR::path-b", "path_b"));
  const int64_t c = db.add_symbol(symbol("USR::path-c", "path_c"));
  const int64_t d = db.add_symbol(symbol("USR::path-d", "path_d"));
  const int64_t calls = cidx::graph::edge_kinds_map().at("calls");
  const int64_t uses = cidx::graph::edge_kinds_map().at("uses");
  db.add_edge({.src_id = a, .dst_id = b, .kind = calls});
  db.add_edge({.src_id = b, .dst_id = d, .kind = calls});
  db.add_edge({.src_id = a, .dst_id = c, .kind = uses});
  db.add_edge({.src_id = c, .dst_id = d, .kind = uses});
  db.add_edge({.src_id = a, .dst_id = d, .kind = uses});

  cidx::ui::GraphViewRequest request;
  request.input = cidx::ui::GraphViewInput{
      .kind = cidx::ui::GraphInputKind::Path, .value = "path_a->path_d"};
  request.edge_kinds = std::vector<std::string>{"calls"};
  request.depth = 3;
  const std::string calls_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(calls_json.find("\"edges\": [\n    {\n") != std::string::npos);
  CHECK(calls_json.find("\"kind\": \"calls\"") != std::string::npos);
  CHECK(calls_json.find("\"kind\": \"uses\"") == std::string::npos);
  CHECK(calls_json.find("path:v1:") != std::string::npos);

  request.edge_kinds = std::vector<std::string>{"uses"};
  const std::string uses_json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(uses_json.find("\"kind\": \"uses\"") != std::string::npos);
  CHECK(result_id(calls_json) != result_id(uses_json));
  CHECK(calls_json.find("path_b") != std::string::npos);
  CHECK(calls_json.find("path_c") == std::string::npos);
}

TEST_CASE("GraphView file identities are non-reversible redacted hashes") {
  Storage db(":memory:");
  const int64_t component = db.add_component("test", "/secret/checkout");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "main.cpp");
  const int64_t config =
      db.add_include_config({.tu_file_id = file, .digest = "external-config"});
  db.add_include_edge({.src_file_id = file,
                       .dst_path = "/external/sys/header.hpp",
                       .config_id = config});

  cidx::ui::GraphViewRequest request;
  request.input =
      cidx::ui::GraphViewInput{.kind = cidx::ui::GraphInputKind::File,
                               .value = "/secret/checkout/main.cpp"};
  const std::string json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));

  CHECK(json.find("file:v2:sha256:") != std::string::npos);
  CHECK(json.find("/secret/checkout") == std::string::npos);
  CHECK(json.find("/external/sys/header.hpp") == std::string::npos);
  CHECK(json.find(hex_encode("/secret/checkout")) == std::string::npos);
  CHECK(json.find(hex_encode("/external/sys/header.hpp")) == std::string::npos);
}

TEST_CASE("GraphView resolves repository-relative files after checkout moves") {
  Storage db(":memory:");
  const int64_t component =
      db.add_component("test", "/tmp/cidx-ui-original-checkout");
  const int64_t directory = db.add_directory(component, "src/ui");
  db.add_file(directory, "main.cpp");

  cidx::ui::GraphViewRequest request;
  request.input = cidx::ui::GraphViewInput{
      .kind = cidx::ui::GraphInputKind::File, .value = "src/ui/main.cpp"};
  const std::string json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));

  CHECK(json.find("\"status\": \"exact_file\"") != std::string::npos);
  CHECK(json.find("\"input_kind\": \"file\"") != std::string::npos);
}

TEST_CASE("GraphView exposes partial entity-edge completeness and status") {
  Storage db(":memory:");
  const int64_t source =
      db.add_symbol(symbol("USR::partial-source", "partial_source"));
  const int64_t target =
      db.add_symbol(symbol("USR::partial-target", "partial_target"));
  auto entity_insert = db.raw_db().prepare(
      "INSERT INTO entity_node (id, kind) VALUES (?, 1), (?, 1)");
  entity_insert.bind(1, source);
  entity_insert.bind(2, target);
  entity_insert.step_done();
  db.add_entity_edge(source, target, 8, 1, std::nullopt, 1, 0, 0, std::nullopt,
                     1);
  db.stamp_graph_resolved();
  db.stamp_index_identity();

  cidx::ui::GraphViewRequest request;
  request.input = cidx::ui::GraphViewInput{
      .kind = cidx::ui::GraphInputKind::Entity, .value = "partial_source"};
  const std::string json =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));

  CHECK(json.find("entity-edge:v1:") != std::string::npos);
  CHECK(json.find("\"partial\": 1") != std::string::npos);
  CHECK(json.find("\"completeness\": \"partial\"") != std::string::npos);
  CHECK(json.find("\"status\": \"partial\"") != std::string::npos);
  CHECK(json.find("\"partial\"") != std::string::npos);
}

TEST_CASE("GraphView uses one site budget across include and semantic edges") {
  Storage db(":memory:");
  const int64_t component = db.add_component("test", "/tmp/cidx-ui-site-mix");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "main.cpp");
  const int64_t header = db.add_file(directory, "header.hpp");
  auto source = symbol("USR::mixed-source", "mixed_source");
  source.file_id = file;
  auto target = symbol("USR::mixed-target", "mixed_target");
  target.file_id = file;
  const int64_t source_id = db.add_symbol(source);
  const int64_t target_id = db.add_symbol(target);
  const int64_t semantic_edge =
      db.add_edge({.src_id = source_id,
                   .dst_id = target_id,
                   .kind = cidx::graph::edge_kinds_map().at("calls")});
  db.add_edge_site(
      {.edge_id = semantic_edge, .file_id = file, .line = 20, .col = 1});
  db.add_edge_site(
      {.edge_id = semantic_edge, .file_id = file, .line = 30, .col = 1});
  const int64_t config =
      db.add_include_config({.tu_file_id = file, .digest = "mixed-sites"});
  const int64_t include =
      db.add_include_edge({.src_file_id = file,
                           .dst_file_id = header,
                           .dst_path = "/tmp/cidx-ui-site-mix/header.hpp",
                           .config_id = config});
  db.add_include_site(
      {.edge_id = include, .line = 10, .col = 1, .begin_offset = 10});

  cidx::ui::GraphViewRequest request;
  request.input =
      cidx::ui::GraphViewInput{.kind = cidx::ui::GraphInputKind::File,
                               .value = "/tmp/cidx-ui-site-mix/main.cpp"};
  request.edge_budget = 2;
  const auto count = [](const std::string &value, std::string_view needle) {
    std::size_t total = 0;
    for (std::size_t pos = value.find(needle); pos != std::string::npos;
         pos = value.find(needle, pos + needle.size())) {
      ++total;
    }
    return total;
  };
  for (const auto &[budget, retained] :
       {std::pair{0, 0}, std::pair{1, 1}, std::pair{3, 3}}) {
    request.site_budget = budget;
    const std::string json =
        cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
    CHECK(count(json, "\"line\": 10") + count(json, "\"line\": 20") +
              count(json, "\"line\": 30") ==
          static_cast<std::size_t>(retained));
    if (budget < 3) {
      CHECK(json.find("\"evidence_truncated\": true") != std::string::npos);
      CHECK(json.find("\"truncated\": true") != std::string::npos);
    } else {
      CHECK(json.find("\"evidence_truncated\": false") != std::string::npos);
    }
  }
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

TEST_CASE(
    "GraphView shares edge budget and truncates include evidence exactly") {
  Storage db(":memory:");
  const int64_t component = db.add_component("test", "/secret/checkout");
  const int64_t directory = db.add_directory(component, "");
  const int64_t file = db.add_file(directory, "main.cpp");
  const int64_t header = db.add_file(directory, "header.hpp");
  auto source = symbol("USR::budget-source", "budget_source");
  source.file_id = file;
  auto target = symbol("USR::budget-target", "budget_target");
  target.file_id = file;
  const int64_t source_id = db.add_symbol(source);
  const int64_t target_id = db.add_symbol(target);
  db.add_edge({.src_id = source_id,
               .dst_id = target_id,
               .kind = cidx::graph::edge_kinds_map().at("calls")});
  const int64_t config =
      db.add_include_config({.tu_file_id = file,
                             .digest = "budget",
                             .working_dir = "/secret/checkout"});
  const int64_t include =
      db.add_include_edge({.src_file_id = file,
                           .dst_file_id = header,
                           .dst_path = "/secret/checkout/header.hpp",
                           .config_id = config});
  db.add_include_site(
      {.edge_id = include, .line = 10, .col = 1, .begin_offset = 10});
  db.add_include_site(
      {.edge_id = include, .line = 20, .col = 1, .begin_offset = 20});

  cidx::ui::GraphViewRequest request;
  request.input =
      cidx::ui::GraphViewInput{.kind = cidx::ui::GraphInputKind::File,
                               .value = "/secret/checkout/main.cpp"};
  request.edge_budget = 1;
  request.site_budget = 0;
  const auto count = [](const std::string &value, std::string_view needle) {
    std::size_t total = 0;
    for (std::size_t pos = value.find(needle); pos != std::string::npos;
         pos = value.find(needle, pos + needle.size())) {
      ++total;
    }
    return total;
  };
  const std::string zero_sites =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(count(zero_sites, "\"line\": 10") == 0);
  CHECK(zero_sites.find("\"sites_truncated\": true") != std::string::npos);
  CHECK(zero_sites.find("\"evidence_truncated\": true") != std::string::npos);
  CHECK(zero_sites.find("\"id\": \"edge:v1:") == std::string::npos);
  CHECK(zero_sites.find("/secret/checkout") == std::string::npos);

  request.site_budget = 1;
  const std::string one_site =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(count(one_site, "\"line\": 10") == 1);
  CHECK(one_site.find("\"sites_truncated\": true") != std::string::npos);

  request.site_budget = 2;
  const std::string all_sites =
      cidx::json_out::dumps_indent2(cidx::ui::build_graph_view(db, request));
  CHECK(count(all_sites, "\"line\": 10") == 1);
  CHECK(count(all_sites, "\"line\": 20") == 1);
  CHECK(all_sites.find("\"sites_truncated\": false") != std::string::npos);
  CHECK(all_sites.find("\"truncated\": true") != std::string::npos);
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
  request.byte_budget = 9000;
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
  CHECK(html.find(std::string(150, '<'), start) == std::string::npos);
  CHECK(html.find(std::string(150, '&'), start) == std::string::npos);
  CHECK(html.find(std::string(150, '>'), start) == std::string::npos);
  CHECK(html.find("\"input_redacted\": true", start) != std::string::npos);
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
