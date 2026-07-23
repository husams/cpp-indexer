// graph/records.hpp -- lightweight value types for the graph query layer.
//
// Sym/Edge/Site/Traversal mirror the Python query.py dataclasses (Sym/Edge/Site
// and Traversal). These are read-side only; writing uses storage/records.hpp.
//
// Key differences from storage::Symbol / storage::Edge / storage::EdgeSite:
//   - Sym carries the RESOLVED file path (not the raw file_id), component name,
//     and the `external` flag -- built from the file cache in GraphQuery.
//   - Edge carries the peer Sym (not just peer id) and a human kind string.
//   - Site carries the resolved file path, not the raw file_id.
//   - Traversal records BFS depth and parent for every reached node.
//
// to_dict() / loc() semantics are byte-identical to the Python counterparts
// (query.py:108-254, Traversal:1659-1697); key order is EXACT (R7).
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cli/json_out.hpp"
#include "util/pathutil.hpp"

namespace cidx::graph {

// ---- Sym ------------------------------------------------------------------

struct Sym {
  int64_t id = -1;
  std::string usr;
  std::string semantic_universe;     // portable universe key
  int64_t semantic_universe_id = -1; // v35: database-local scope row
  std::string identity_key; // v35: portable scope-keyed semantic identity
  std::string spelling;
  std::string name; // COALESCE(qual_name, spelling) -- displayed name
  std::string kind;
  std::optional<std::string> type_info;
  std::optional<std::string> const_value; // v33: evaluated constant initializer
                                          // (variable) or enumerator value
  bool is_definition = false;
  bool is_pure = false;
  bool is_static = false;
  bool is_instantiation = false;
  std::optional<std::string> access;
  std::optional<std::string> parent_usr;
  bool resolved = false;
  std::optional<std::string> component;
  std::optional<std::string>
      file; // abs path of best-known location, or nullopt
  std::optional<int64_t> line;
  std::optional<int64_t> col;
  std::optional<int64_t> end_line; // v25: end of the symbol's own extent at
  std::optional<int64_t> end_col;  // (line, col); nullopt for decl-only / stubs
  bool external = false;           // file is a raw path in an UNREGISTERED file
  int64_t multi_def = 0; // v27: number of definitions (bodies). >1 == redefined
                         // per backend. Deliberately NOT in to_dict() (parity).

  // Python Sym.is_redefined property (query.py)
  [[nodiscard]] bool is_redefined() const { return multi_def > 1; }

  // Python Sym.loc property (query.py:135-140)
  [[nodiscard]] std::string loc() const {
    if (!file) {
      return "<no-location>";
    }
    std::string base = pathutil::basename(*file);
    if (line && *line != 0) {
      return base + ":" + std::to_string(*line);
    }
    return base;
  }

  // Python Sym.span property: file:line-end_line, or nullopt when no end is
  // known -- the line range that slices the whole entity.
  [[nodiscard]] std::optional<std::string> span() const {
    if (!file || !line || *line == 0 || !end_line || *end_line == 0) {
      return std::nullopt;
    }
    return pathutil::basename(*file) + ":" + std::to_string(*line) + "-" +
           std::to_string(*end_line);
  }

  // Python Sym.is_stub property (query.py:143-153)
  [[nodiscard]] bool is_stub() const {
    return !resolved && (!file.has_value() || external);
  }

  // Python Sym.to_dict() -- key order EXACT (query.py:155-172, R7)
  [[nodiscard]] json_out::Value to_dict() const {
    using namespace json_out;
    Object o;
    o.emplace_back("id", Value::of(id));
    o.emplace_back("usr", Value::of(usr));
    o.emplace_back("semantic_universe", Value::of(semantic_universe));
    o.emplace_back("identity_key", Value::of(identity_key));
    o.emplace_back("spelling", Value::of(spelling));
    o.emplace_back("qual_name", Value::of(name)); // COALESCE result
    o.emplace_back("kind", Value::of(kind));
    if (type_info) {
      o.emplace_back("type_info", Value::of(*type_info));
    } else {
      o.emplace_back("type_info", Value::null());
    }
    if (const_value) {
      o.emplace_back("const_value", Value::of(*const_value));
    } else {
      o.emplace_back("const_value", Value::null());
    }
    if (file) {
      o.emplace_back("file", Value::of(*file));
    } else {
      o.emplace_back("file", Value::null());
    }
    if (line) {
      o.emplace_back("line", Value::of(*line));
    } else {
      o.emplace_back("line", Value::null());
    }
    if (col) {
      o.emplace_back("col", Value::of(*col));
    } else {
      o.emplace_back("col", Value::null());
    }
    if (end_line) {
      o.emplace_back("end_line", Value::of(*end_line));
    } else {
      o.emplace_back("end_line", Value::null());
    }
    if (end_col) {
      o.emplace_back("end_col", Value::of(*end_col));
    } else {
      o.emplace_back("end_col", Value::null());
    }
    o.emplace_back("is_definition", Value::of(is_definition));
    o.emplace_back("is_pure", Value::of(is_pure));
    o.emplace_back("is_static", Value::of(is_static));
    o.emplace_back("is_instantiation", Value::of(is_instantiation));
    o.emplace_back("is_stub", Value::of(is_stub()));
    return Value::obj(std::move(o));
  }
};

// ---- Definition (v27) -----------------------------------------------------
// One backend body of a (possibly redefined) symbol. Mirrors
// query.py:Definition
// -- to_dict()/loc() byte-identical.
struct Definition {
  Sym sym;
  std::optional<std::string> component;
  std::optional<std::string> file; // abs path (resolved from file cache)
  std::optional<int64_t> line, col, end_line, end_col;
  std::optional<std::string> init_text; // v28: (static member) var initializer

  [[nodiscard]] std::string loc() const {
    if (!file) {
      return "<no-location>";
    }
    std::string base = pathutil::basename(*file);
    if (line && *line != 0) {
      return base + ":" + std::to_string(*line);
    }
    return base;
  }

  // query.py:Definition.to_dict() -- key order EXACT.
  [[nodiscard]] json_out::Value to_dict() const {
    using namespace json_out;
    Object o;
    o.emplace_back("usr", Value::of(sym.usr));
    o.emplace_back("semantic_universe", Value::of(sym.semantic_universe));
    o.emplace_back("identity_key", Value::of(sym.identity_key));
    o.emplace_back("name", Value::of(sym.name));
    o.emplace_back("kind", Value::of(sym.kind));
    if (component) {
      o.emplace_back("component", Value::of(*component));
    } else {
      o.emplace_back("component", Value::null());
    }
    if (file) {
      o.emplace_back("file", Value::of(pathutil::basename(*file)));
    } else {
      o.emplace_back("file", Value::null());
    }
    if (line) {
      o.emplace_back("line", Value::of(*line));
    } else {
      o.emplace_back("line", Value::null());
    }
    if (col) {
      o.emplace_back("col", Value::of(*col));
    } else {
      o.emplace_back("col", Value::null());
    }
    if (end_line) {
      o.emplace_back("end_line", Value::of(*end_line));
    } else {
      o.emplace_back("end_line", Value::null());
    }
    if (end_col) {
      o.emplace_back("end_col", Value::of(*end_col));
    } else {
      o.emplace_back("end_col", Value::null());
    }
    if (init_text) {
      o.emplace_back("init_text", Value::of(*init_text));
    } else {
      o.emplace_back("init_text", Value::null());
    }
    return Value::obj(std::move(o));
  }
};

// ---- Site -----------------------------------------------------------------

struct Site {
  std::optional<std::string> file; // abs path (resolved from file cache)
  std::optional<int64_t> line;
  std::optional<int64_t> col;
  bool conditional = false;
  std::optional<std::string> args_sig;
  // Phase 2 provenance fields (present in DB, not serialized by graph output)
  std::optional<std::string> recv_src_kind;
  std::optional<std::string> recv_type_usr;
  std::optional<std::string> recv_decl_usr;
  std::optional<int64_t> recv_param_pos;
  std::optional<int64_t> recv_type_is_value;

  // Python Site.loc property (query.py:241-245)
  [[nodiscard]] std::string loc() const {
    if (!file) {
      return "<no-location>";
    }
    std::string base = pathutil::basename(*file);
    if (line && *line != 0) {
      return base + ":" + std::to_string(*line) + ":" +
             (col ? std::to_string(*col) : "");
    }
    return base;
  }

  // Python Site.to_dict() (query.py:247-254)
  [[nodiscard]] json_out::Value to_dict() const {
    using namespace json_out;
    Object o;
    if (file) {
      o.emplace_back("file", Value::of(*file));
    } else {
      o.emplace_back("file", Value::null());
    }
    if (line) {
      o.emplace_back("line", Value::of(*line));
    } else {
      o.emplace_back("line", Value::null());
    }
    if (col) {
      o.emplace_back("col", Value::of(*col));
    } else {
      o.emplace_back("col", Value::null());
    }
    o.emplace_back("conditional", Value::of(conditional));
    if (args_sig) {
      o.emplace_back("args_sig", Value::of(*args_sig));
    } else {
      o.emplace_back("args_sig", Value::null());
    }
    return Value::obj(std::move(o));
  }
};

// ---- Edge -----------------------------------------------------------------

struct Edge {
  int64_t edge_id = -1;
  std::string kind; // edge_kind name (e.g. "calls")
  int64_t src_id = -1;
  int64_t dst_id = -1;
  Sym peer; // the symbol at the other end
  int64_t count = 1;
  std::optional<int64_t> base_access; // inherits only
  std::optional<int64_t> is_virtual;  // inherits only (raw int)
  std::vector<Site> sites;            // eager-loaded reference sites

  // Python Edge.to_dict(sites) (query.py:196-216, R7)
  // `sites_override` is passed for --json re-query (R8).
  [[nodiscard]] json_out::Value
  to_dict(const std::vector<Site> &sites_override) const {
    using namespace json_out;
    // Start with the peer's dict then append edge fields.
    Value pv = peer.to_dict();
    // pv is already an Object; extend it.
    pv.o.emplace_back("edge_kind", Value::of(kind));
    pv.o.emplace_back("count", Value::of(count));
    // base_access / is_virtual: only when non-null (R7 -- calls/uses MUST be
    // absent)
    if (base_access) {
      pv.o.emplace_back("base_access", Value::of(*base_access));
    }
    if (is_virtual) {
      // is_virtual serialized as bool (R7)
      pv.o.emplace_back("is_virtual",
                        Value::of(static_cast<bool>(*is_virtual)));
    }
    Array sarr;
    for (const Site &s : sites_override) {
      sarr.push_back(s.to_dict());
    }
    pv.o.emplace_back("sites", Value::arr(std::move(sarr)));
    return pv;
  }
};

// ---- Traversal ------------------------------------------------------------

struct Traversal {
  std::unordered_map<int64_t, Sym> nodes_by_id;
  std::unordered_map<int64_t, int> depth_by_id;
  std::unordered_map<int64_t, std::optional<int64_t>> parent_by_id;
  // BFS insertion order: ids in the order they were first discovered.
  // Required so that stable_sort by (depth, name) breaks same-key ties by
  // BFS discovery order (mirrors Python dict insertion order + sorted()
  // stable). Populated by GraphQuery::walk(); callers that build Traversal
  // directly should also append to this vector whenever they insert into
  // nodes_by_id.
  std::vector<int64_t> insertion_order_;

  // Python Traversal.nodes property (query.py:1667-1673, R5):
  // stable_sort by (depth, name) where name = sym.name (COALESCE).
  // The initial vector is built in BFS insertion order so that stable_sort
  // preserves discovery order for same (depth, name) ties.
  [[nodiscard]] std::vector<Sym> nodes() const {
    std::vector<Sym> out;
    out.reserve(insertion_order_.size());
    // Build in insertion order to get a deterministic stable_sort input.
    for (int64_t id : insertion_order_) {
      auto it = nodes_by_id.find(id);
      if (it != nodes_by_id.end()) {
        out.push_back(it->second);
      }
    }
    std::ranges::stable_sort(out, [this](const Sym &a, const Sym &b) {
      const int da = depth_by_id.contains(a.id) ? depth_by_id.at(a.id) : 0;
      const int db = depth_by_id.contains(b.id) ? depth_by_id.at(b.id) : 0;
      if (da != db) {
        return da < db;
      }
      return a.name < b.name;
    });
    return out;
  }
};

} // namespace cidx::graph
