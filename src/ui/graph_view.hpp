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
  GraphViewError(GraphViewFailureKind kind, const std::string &message,
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

  // HSE-92 live-explorer filters. All optional/empty by default so existing
  // CLI/offline-export callers (HSE-90/HSE-91) are unaffected byte-for-byte.
  std::optional<std::vector<std::string>> node_kinds;   // symbol/entity/type kind
  std::optional<std::vector<std::string>> files;        // redacted file path membership
  std::optional<std::vector<std::string>> components;   // Sym::component membership
  std::optional<std::vector<std::string>> repositories; // resolved repository name
  std::optional<std::string> status_filter; // resolved|unresolved|external|internal|stub
  std::optional<std::string>
      applicability_filter; // universal|conditional (edge site aggregate)

  // Progressive continuation: an opaque token bounding this request to the
  // NEXT bounded slice of a strictly larger, previously-issued request. See
  // decode_continuation()/encode_continuation() below.
  std::optional<std::string> continuation;
};

// Build the portable, renderer-independent GraphView contract. The browser
// receives this value; it never receives a Storage or SQL handle.
json_out::Value build_graph_view(Storage &db, const GraphViewRequest &request);

// A bounded, ranked list of typed search candidates for a free-text query.
// Used by the live explorer's search operation to resolve a name to a
// portable root identity without exposing SQL or raw database rows.
json_out::Value search_candidates(Storage &db, const std::string &text,
                                  const std::optional<std::string> &node_kind,
                                  const std::optional<std::string> &workspace,
                                  int limit);

// Full bounded evidence (source sites) for one previously-emitted portable
// edge id, beyond whatever the originating GraphView response embedded. The
// edge id must have come from a prior build_graph_view() response.
json_out::Value load_edge_evidence(Storage &db, const std::string &edge_id,
                                   const std::optional<std::string> &workspace,
                                   int site_offset, int site_limit);

} // namespace cidx::ui
