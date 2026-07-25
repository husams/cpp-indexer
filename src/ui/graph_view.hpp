#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cli/json_out.hpp"
#include "storage/storage.hpp"
#include "util/errors.hpp"

namespace cidx::ui {

enum class GraphInputKind : std::uint8_t {
  Symbol,
  File,
  Entity,
  Type,
  Cxq,
  QueryPlan,
  Path,
  Analysis,
};

const char *graph_input_kind_name(GraphInputKind kind);

struct GraphViewInput {
  GraphInputKind kind = GraphInputKind::Symbol;
  std::string value;

  [[nodiscard]] std::string canonical() const;
};

enum class GraphViewFailureKind : std::uint8_t {
  InvalidInput,
  UnsupportedInput,
  UnknownIdentity,
  AmbiguousIdentity,
  Oversized,
};

class GraphViewError : public CidxError {
public:
  GraphViewError(GraphViewFailureKind kind, std::string message,
                 std::string next_action);

  [[nodiscard]] GraphViewFailureKind kind() const noexcept { return kind_; }
  [[nodiscard]] const std::string &next_action() const noexcept {
    return next_action_;
  }
  [[nodiscard]] const char *code() const noexcept;

private:
  GraphViewFailureKind kind_;
  std::string next_action_;
};

struct GraphViewRequest {
  std::optional<std::string> root;
  std::optional<std::string> query;
  std::optional<GraphViewInput> input;
  std::optional<std::vector<std::string>> edge_kinds;
  std::string direction = "out";
  int depth = 2;
  int node_budget = 250;
  int edge_budget = 500;
  int site_budget = 200;
  int byte_budget = 4 * 1024 * 1024;
  std::optional<std::string> workspace;
  bool strict = false;
};

// Build the portable, renderer-independent GraphView contract. The browser
// receives this value; it never receives a Storage or SQL handle.
json_out::Value build_graph_view(Storage &db, const GraphViewRequest &request);

} // namespace cidx::ui
