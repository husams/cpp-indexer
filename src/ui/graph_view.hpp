#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cli/json_out.hpp"
#include "storage/storage.hpp"

namespace cidx::ui {

struct GraphViewRequest {
  std::optional<std::string> root;
  std::optional<std::string> query;
  std::optional<std::vector<std::string>> edge_kinds;
  std::string direction = "out";
  int depth = 2;
  int node_budget = 250;
  int edge_budget = 500;
  int site_budget = 200;
  int byte_budget = 4 * 1024 * 1024;
  std::optional<std::string> workspace;
};

// Build the portable, renderer-independent GraphView contract. The browser
// receives this value; it never receives a Storage or SQL handle.
json_out::Value build_graph_view(Storage &db, const GraphViewRequest &request);

} // namespace cidx::ui
