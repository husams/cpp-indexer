// The configuration-aware include graph (planning/cidx-include-hygiene M1).
//
// Nodes are PATHS, not file ids: a directive's target may be a system header or
// a file no component owns, which has no file row but is still a real
// dependency the graph must answer for. Sources always have a file row (the
// recorder only persists directives it could attribute to an owned file), so
// every edge has a real, editable origin.
//
// Every query here is read-only and deterministically ordered.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "storage/records.hpp"

namespace cidx {
class Storage;
}

namespace cidx::hygiene {

// One direct file -> file dependency, flattened for display. The same
// (src, dst) pair appears once per configuration that produced it; `configs`
// collects those digests so callers can tell a configuration-common edge from a
// configuration-specific one without re-querying.
struct GraphEdge {
  std::string src_path;
  std::string dst_path;
  std::optional<int64_t> dst_file_id; // absent: system, unowned, or unresolved
  bool is_system = false;
  int64_t count = 1; // directive occurrences, summed across configurations
  std::vector<std::string> configs; // sorted digests
};

// An in-memory adjacency view built once from the database.
class IncludeGraph {
public:
  // `include_system` keeps direct system targets as nodes (their internals are
  // never persisted, so they are always leaves).
  static IncludeGraph load(cidx::Storage &db, bool include_system);

  // Direct targets of `path` / direct includers of `path`, ordered by path.
  std::vector<GraphEdge> direct_from(const std::string &path) const;
  std::vector<GraphEdge> direct_to(const std::string &path) const;

  // Every path reachable from `path` within `max_depth` hops (0 = unbounded),
  // excluding `path` itself. Ordered by path. Cycle-safe.
  std::vector<std::string> transitive_from(const std::string &path,
                                           int max_depth) const;
  // Every path that can reach `path` within `max_depth` hops -- the impact set
  // of changing that header. Ordered by path. Cycle-safe.
  std::vector<std::string> transitive_to(const std::string &path,
                                         int max_depth) const;

  // Shortest include chain from `from` to `to` inclusive, or empty when `to` is
  // unreachable -- the "why is this header here?" answer. BFS over a
  // path-sorted adjacency, so the chain is stable across runs when several
  // shortest paths tie.
  std::vector<std::string> shortest_path(const std::string &from,
                                         const std::string &to) const;

  // Strongly connected components of size > 1, plus self-loops: the include
  // cycles. Each component is path-sorted; components are ordered by their
  // first path. Tarjan, iterative -- a deep include chain must not blow the
  // native stack.
  std::vector<std::vector<std::string>> cycles() const;

  // (path, fan_in, fan_out) for every node, ordered by path.
  struct Hotspot {
    std::string path;
    int64_t fan_in = 0;
    int64_t fan_out = 0;
  };
  std::vector<Hotspot> hotspots() const;

  // Every node, ordered by path.
  std::vector<std::string> nodes() const;
  bool has_node(const std::string &path) const;

private:
  // path -> outgoing/incoming edges, each sorted by the far endpoint's path.
  std::map<std::string, std::vector<GraphEdge>> out_;
  std::map<std::string, std::vector<GraphEdge>> in_;
};

} // namespace cidx::hygiene
