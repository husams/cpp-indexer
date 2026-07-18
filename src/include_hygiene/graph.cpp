#include "include_hygiene/graph.hpp"

#include "storage/storage.hpp"

#include <algorithm>
#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace cidx::hygiene {

namespace {

// Merge the per-configuration rows for one (src, dst) pair into a single
// displayed edge: occurrence counts add, configuration digests collect.
void merge_into(std::vector<GraphEdge> &list, GraphEdge e) {
  for (GraphEdge &x : list) {
    if (x.dst_path == e.dst_path && x.src_path == e.src_path) {
      x.count += e.count;
      for (std::string &d : e.configs) {
        if (std::find(x.configs.begin(), x.configs.end(), d) ==
            x.configs.end()) {
          x.configs.push_back(std::move(d));
        }
      }
      std::sort(x.configs.begin(), x.configs.end());
      // A target owned under one configuration and unresolved under another is
      // still that target: keep the id we know.
      if (!x.dst_file_id) {
        x.dst_file_id = e.dst_file_id;
      }
      return;
    }
  }
  list.push_back(std::move(e));
}

void sort_by_far_end(std::vector<GraphEdge> &list, bool by_dst) {
  std::sort(list.begin(), list.end(), [by_dst](const GraphEdge &a,
                                               const GraphEdge &b) {
    return by_dst ? a.dst_path < b.dst_path : a.src_path < b.src_path;
  });
}

// Breadth-first reachable set over one adjacency map.
std::vector<std::string>
reachable(const std::map<std::string, std::vector<GraphEdge>> &adj,
          const std::string &start, int max_depth, bool follow_dst) {
  std::unordered_set<std::string> seen{start};
  std::deque<std::pair<std::string, int>> queue{{start, 0}};
  std::set<std::string> out;
  while (!queue.empty()) {
    const auto [path, depth] = queue.front();
    queue.pop_front();
    if (max_depth > 0 && depth >= max_depth) {
      continue;
    }
    const auto it = adj.find(path);
    if (it == adj.end()) {
      continue;
    }
    for (const GraphEdge &e : it->second) {
      const std::string &next = follow_dst ? e.dst_path : e.src_path;
      if (!seen.insert(next).second) {
        continue;
      }
      out.insert(next);
      queue.emplace_back(next, depth + 1);
    }
  }
  return {out.begin(), out.end()};
}

} // namespace

IncludeGraph IncludeGraph::load(cidx::Storage &db, bool include_system) {
  IncludeGraph g;

  // One pass over `file` builds the id -> absolute path map. Reconstructing a
  // path per edge would re-resolve the component/clone chain thousands of
  // times; the edges only carry ids for the source side anyway.
  std::unordered_map<int64_t, std::string> paths;
  for (const auto &[row, abs] : db.list_files()) {
    paths.emplace(row.id, abs);
  }

  std::unordered_map<int64_t, std::string> digests;
  for (const IncludeEdge &e : db.all_include_edges(include_system)) {
    const auto src = paths.find(e.src_file_id);
    if (src == paths.end()) {
      continue; // source file row vanished: nothing to attribute the edge to
    }
    auto d = digests.find(e.config_id);
    if (d == digests.end()) {
      std::string digest;
      if (const std::optional<IncludeConfig> c =
              db.include_config_by_id(e.config_id)) {
        digest = c->digest;
      }
      d = digests.emplace(e.config_id, std::move(digest)).first;
    }

    GraphEdge ge;
    ge.src_path = src->second;
    ge.dst_path = e.dst_path;
    ge.dst_file_id = e.dst_file_id;
    ge.is_system = e.is_system;
    ge.count = e.count;
    ge.configs = {d->second};
    merge_into(g.out_[ge.src_path], ge);
    merge_into(g.in_[ge.dst_path], ge);
  }

  for (auto &[_, list] : g.out_) {
    sort_by_far_end(list, /*by_dst=*/true);
  }
  for (auto &[_, list] : g.in_) {
    sort_by_far_end(list, /*by_dst=*/false);
  }
  return g;
}

std::vector<GraphEdge> IncludeGraph::direct_from(const std::string &path) const {
  const auto it = out_.find(path);
  return it == out_.end() ? std::vector<GraphEdge>{} : it->second;
}

std::vector<GraphEdge> IncludeGraph::direct_to(const std::string &path) const {
  const auto it = in_.find(path);
  return it == in_.end() ? std::vector<GraphEdge>{} : it->second;
}

std::vector<std::string> IncludeGraph::transitive_from(const std::string &path,
                                                       int max_depth) const {
  return reachable(out_, path, max_depth, /*follow_dst=*/true);
}

std::vector<std::string> IncludeGraph::transitive_to(const std::string &path,
                                                     int max_depth) const {
  return reachable(in_, path, max_depth, /*follow_dst=*/false);
}

std::vector<std::string> IncludeGraph::shortest_path(const std::string &from,
                                                     const std::string &to) const {
  if (from == to) {
    return has_node(from) ? std::vector<std::string>{from}
                          : std::vector<std::string>{};
  }
  std::unordered_map<std::string, std::string> parent;
  std::unordered_set<std::string> seen{from};
  std::deque<std::string> queue{from};
  bool found = false;
  while (!queue.empty() && !found) {
    const std::string cur = queue.front();
    queue.pop_front();
    const auto it = out_.find(cur);
    if (it == out_.end()) {
      continue;
    }
    // Adjacency is path-sorted, so ties between equally short chains always
    // resolve the same way.
    for (const GraphEdge &e : it->second) {
      if (!seen.insert(e.dst_path).second) {
        continue;
      }
      parent[e.dst_path] = cur;
      if (e.dst_path == to) {
        found = true;
        break;
      }
      queue.push_back(e.dst_path);
    }
  }
  if (!found) {
    return {};
  }
  std::vector<std::string> chain{to};
  for (std::string cur = to; cur != from;) {
    cur = parent[cur];
    chain.push_back(cur);
  }
  std::reverse(chain.begin(), chain.end());
  return chain;
}

std::vector<std::vector<std::string>> IncludeGraph::cycles() const {
  // Tarjan's SCC, iterative: an include chain can be thousands deep and a
  // recursive walk would risk the native stack.
  struct Frame {
    std::string node;
    std::size_t next_child = 0;
  };
  std::unordered_map<std::string, int> index, lowlink;
  std::unordered_set<std::string> on_stack;
  std::vector<std::string> stack;
  std::vector<std::vector<std::string>> out;
  int counter = 0;

  for (const std::string &root : nodes()) {
    if (index.count(root) != 0) {
      continue;
    }
    std::vector<Frame> work{{root, 0}};
    index[root] = lowlink[root] = counter++;
    stack.push_back(root);
    on_stack.insert(root);

    while (!work.empty()) {
      Frame &f = work.back();
      const auto it = out_.find(f.node);
      const std::vector<GraphEdge> *kids =
          it == out_.end() ? nullptr : &it->second;

      if (kids != nullptr && f.next_child < kids->size()) {
        const std::string &child = (*kids)[f.next_child++].dst_path;
        if (index.count(child) == 0) {
          index[child] = lowlink[child] = counter++;
          stack.push_back(child);
          on_stack.insert(child);
          work.push_back({child, 0});
        } else if (on_stack.count(child) != 0) {
          lowlink[f.node] = std::min(lowlink[f.node], index[child]);
        }
        continue;
      }

      // f.node is fully explored: it roots an SCC when its lowlink never
      // reached above itself.
      if (lowlink[f.node] == index[f.node]) {
        std::vector<std::string> comp;
        while (true) {
          const std::string w = stack.back();
          stack.pop_back();
          on_stack.erase(w);
          comp.push_back(w);
          if (w == f.node) {
            break;
          }
        }
        // A single node is only a cycle when it includes itself.
        const bool self_loop =
            comp.size() == 1 && it != out_.end() &&
            std::any_of(it->second.begin(), it->second.end(),
                        [&](const GraphEdge &e) { return e.dst_path == f.node; });
        if (comp.size() > 1 || self_loop) {
          std::sort(comp.begin(), comp.end());
          out.push_back(std::move(comp));
        }
      }
      const std::string done = f.node;
      work.pop_back();
      if (!work.empty()) {
        lowlink[work.back().node] =
            std::min(lowlink[work.back().node], lowlink[done]);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<IncludeGraph::Hotspot> IncludeGraph::hotspots() const {
  std::vector<Hotspot> out;
  for (const std::string &p : nodes()) {
    Hotspot h;
    h.path = p;
    if (const auto i = in_.find(p); i != in_.end()) {
      h.fan_in = static_cast<int64_t>(i->second.size());
    }
    if (const auto o = out_.find(p); o != out_.end()) {
      h.fan_out = static_cast<int64_t>(o->second.size());
    }
    out.push_back(std::move(h));
  }
  return out;
}

std::vector<std::string> IncludeGraph::nodes() const {
  std::set<std::string> all;
  for (const auto &[p, _] : out_) {
    all.insert(p);
  }
  for (const auto &[p, _] : in_) {
    all.insert(p);
  }
  return {all.begin(), all.end()};
}

bool IncludeGraph::has_node(const std::string &path) const {
  return out_.count(path) != 0 || in_.count(path) != 0;
}

} // namespace cidx::hygiene
