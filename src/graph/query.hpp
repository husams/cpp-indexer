// graph/query.hpp -- read-only graph traversal engine over a cidx index.
//
// Mirrors Python indexer/query.py GraphQuery class (query.py:497-1393).
// Pure read path: no writes, no schema changes, no libclang at runtime.
// Opens the DB in read-write mode (via the existing SQLite service which
// is the same file; graph reads go through the same SqliteDb handle to avoid
// requiring a separate read-only open).
//
// ADR-007: C++ graph port (M6). The query engine is a 1:1 port of query.py
// with the same SQL, the same traversal bounds, and byte-identical output.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "catalogs/generated_catalog.hpp"
#include "graph/records.hpp"
#include "storage/ports.hpp"

namespace cidx::graph {

// Internal format helpers used in error message construction.
namespace format {
std::string py_repr_simple(const std::string &s);
} // namespace format

// ---- Error types ----------------------------------------------------------

class NoIndexError : public std::runtime_error {
public:
  explicit NoIndexError(const std::string &msg) : std::runtime_error(msg) {}
};

class NoEdgesError : public std::runtime_error {
public:
  explicit NoEdgesError(const std::string &msg) : std::runtime_error(msg) {}
};

// ---- EDGE_KINDS -----------------------------------------------------------
// edge_kind.id <-> name seeded identically by storage.py. Hard-coded to avoid
// a query and to validate any DB that disagrees.

inline const std::map<std::string, int64_t> &edge_kinds_map() {
  static const std::map<std::string, int64_t> m = [] {
    std::map<std::string, int64_t> result;
    for (const auto &relation : catalog::kRelations) {
      if (relation.layer == catalog::View::Symbol) {
        result.emplace(std::string(relation.name), relation.id);
      }
    }
    return result;
  }();
  return m;
}

inline const std::map<int64_t, std::string> &edge_names_map() {
  static const std::map<int64_t, std::string> m = [] {
    std::map<int64_t, std::string> result;
    for (const auto &relation : catalog::kRelations) {
      if (relation.layer == catalog::View::Symbol) {
        result.emplace(relation.id, std::string(relation.name));
      }
    }
    return result;
  }();
  return m;
}

// ---- GraphQuery -----------------------------------------------------------

class GraphQuery {
public:
  // Open or wrap an existing SQLite service. `db_path` is used only for error
  // messages.
  explicit GraphQuery(storage::GraphReadPort &db, std::string db_path = "");

  // Convenience: open from path (reserved for a service opened at path).
  // Throws NoIndexError when the DB file does not exist.
  static GraphQuery open(const std::string &db_path);

  // Total number of edges. 0 means the graph layer is empty.
  int64_t edge_count();

  // Raise NoEdgesError unless edge_count() > 0.
  void require_edges();

  // ---- Symbol lookup -------------------------------------------------------

  std::optional<Sym> get_by_id(int64_t id);
  std::optional<Sym> get_by_usr(const std::string &usr);

  // Fuzzy qualified-name lookup (COALESCE(qual_name,spelling) LIKE pattern).
  // Mirrors query.py:find() (R1: uses find_symbols accessor, NOT
  // search_symbols).
  std::vector<Sym> find(const std::string &pattern,
                        const std::optional<std::string> &kind = std::nullopt,
                        int limit = 50);

  // ---- Edge traversal ------------------------------------------------------

  // Incoming / outgoing edges of `kinds` (nullopt = all), up to `limit`.
  // with_sites=true: batch-load edge_site rows and attach them.
  std::vector<Edge> edges(int64_t sym_id, const std::string &direction,
                          const std::optional<std::vector<int64_t>> &kind_ids,
                          int limit, bool with_sites = true);

  std::vector<Edge>
  edges_in(int64_t sym_id, const std::optional<std::vector<std::string>> &kinds,
           int limit = 500);
  std::vector<Edge>
  edges_out(int64_t sym_id,
            const std::optional<std::vector<std::string>> &kinds,
            int limit = 500);

  // calls + uses inbound.
  std::vector<Edge> references(int64_t sym_id, int limit = 500);

  // Type aliases / typedefs whose underlying type directly names `sym_id`
  // (inverse of alias --alias_of--> target).
  std::vector<Sym> aliased_by(int64_t sym_id, int limit = 500);

  // Per-edge sites (A8, limit 200). Used by emitter for --json re-query (R8).
  std::vector<Site> sites(int64_t edge_id, int limit = 200);

  // Whether ANY of an edge's sites is config-conditional -- an indexed
  // EXISTS aggregate, exact regardless of how many sites the edge has and
  // never bounded by (or dependent on) a response's evidence budget.
  bool edge_conditional(int64_t edge_id);

  // ---- Navigation ----------------------------------------------------------

  // Internal: peer Syms with no site loading (BFS internal).
  std::vector<Sym> peers(int64_t sym_id,
                         const std::optional<std::vector<std::string>> &kinds,
                         const std::string &direction = "out", int limit = 500);

  // Parse kind spec string into kind_id vector. Throws std::invalid_argument on
  // unknown kind. Returns nullopt for null/empty (= all kinds).
  static std::optional<std::vector<int64_t>>
  kind_ids(const std::optional<std::vector<std::string>> &kinds);

  // Bounded BFS (walk). Mirrors query.py:walk() (query.py:967-1003).
  Traversal walk(int64_t start_id,
                 const std::optional<std::vector<std::string>> &kinds,
                 const std::string &direction = "out", int depth = 3,
                 int max_nodes = 500);

  // Shortest path from src to dst. Mirrors query.py:reaches()
  // (query.py:1005-1046). Returns nullopt when unreachable.
  std::optional<std::vector<Sym>>
  reaches(int64_t src_id, int64_t dst_id,
          const std::optional<std::vector<std::string>> &kinds,
          const std::string &direction = "out", int max_depth = 8);

  // ---- Hierarchy -----------------------------------------------------------

  std::vector<Sym> bases(int64_t sym_id, bool direct = true);
  std::vector<Sym> subclasses(int64_t sym_id, bool direct = true);
  std::vector<Sym>
  members(int64_t sym_id,
          const std::optional<std::string> &access = std::nullopt);

  // ---- Dispatch ------------------------------------------------------------

  std::vector<Sym> overrides_of(int64_t sym_id);  // outgoing overrides
  std::vector<Sym> overridden_by(int64_t sym_id); // incoming overrides
  bool is_virtual_method(int64_t sym_id);
  // query.py:dispatch_targets (R4: insertion-ordered BFS, self first if !pure).
  std::vector<Sym> dispatch_targets(int64_t sym_id);

  // ---- v27 multi-definition (query.py:GraphQuery.redefined/definitions/
  //      possible_callees)
  //      -----------------------------------------------------
  std::vector<Sym> redefined(int limit = 500);
  std::vector<Definition> definitions(int64_t sym_id);
  std::vector<Definition> possible_callees(int64_t sym_id);

  // ---- v30 signature/type tier ----------------------------------------------

  // Display info for one type_node (kind resolved to its name; canonical
  // spelling attached when the node is sugared).
  struct TypeInfo {
    int64_t id = -1;
    std::string spelling;
    std::string kind; // type_kind name ("builtin", "record", "alias", ...)
    std::optional<std::string> canonical; // canonical spelling when sugared
    std::optional<std::string> decl_usr;
    bool is_const = false;
    bool is_volatile = false;
    bool is_restrict = false;
    std::optional<std::string> extent;
  };
  struct ParamInfo {
    int64_t position = 0;
    std::optional<int64_t> pack_index;
    std::optional<std::string> name;
    std::optional<TypeInfo> type;
    std::optional<TypeInfo> declared_type;
    std::optional<TypeInfo> adjusted_type;
    std::string mode = "value";
    std::string value_kind = "other";
    std::optional<std::string> named_decl;
    std::optional<std::string> default_text;
    std::optional<std::string> default_origin;
    std::optional<std::string> reference_semantics;
  };
  struct SlotFacts {
    std::string mode = "value";
    std::string value_kind = "other";
    std::optional<std::string> named_decl;
  };
  struct TypeLayer {
    std::string path;
    std::string relation;
    int64_t position = 0;
    int depth = 0;
    std::string status = "complete";
    std::optional<std::string> element_type;
    TypeInfo type;
  };
  // Everything the signature/type tier knows about one symbol. Callables get
  // returns/params; variables/fields get of_type; typedef/alias symbols get
  // underlying. Absent facts stay nullopt/empty.
  struct SignatureInfo {
    std::optional<TypeInfo> returns;
    std::vector<ParamInfo> params;
    std::optional<TypeInfo> of_type;
    std::optional<TypeInfo> underlying;
    [[nodiscard]] bool empty() const {
      return !returns && params.empty() && !of_type && !underlying;
    }
  };
  SignatureInfo signature(int64_t sym_id);
  SlotFacts slot_facts_for_ids(std::optional<int64_t> declared_type_id,
                              std::optional<int64_t> adjusted_type_id);
  SlotFacts slot_facts(const std::optional<TypeInfo> &declared,
                       const std::optional<TypeInfo> &adjusted);
  std::vector<TypeLayer> type_layers(int64_t type_id);
  std::optional<TypeInfo> type_child(int64_t type_id, int64_t edge_kind,
                                     int64_t position = 0);

  // One symbol whose signature/type facts reach the queried type: a callable
  // parameter ("param", with position), a return type ("returns"), a
  // variable/field type ("of_type"), or an alias target ("underlying_type").
  struct TypeUser {
    Sym sym;
    std::string role;
    std::optional<int64_t> position; // param role only
  };
  // Users of the type named by `usr`, through any number of pointer/
  // reference/array/alias/template-argument layers. Ordered param rows first
  // then symbol_type rows (each by symbol id).
  std::vector<TypeUser> type_users(const std::string &usr, int limit = 500);

  // ---- Accessors -----------------------------------------------------------

  [[nodiscard]] const std::string &db_path() const { return db_path_; }

private:
  storage::GraphReadPort &db_;
  std::string db_path_;
  std::optional<bool> resolved_; // memoized _is_resolved
  std::optional<std::unordered_map<
      int64_t, std::pair<std::string, std::optional<std::string>>>>
      file_cache_; // {file_id -> (abs_path, component_name)}

  bool is_resolved();
  const std::unordered_map<int64_t,
                           std::pair<std::string, std::optional<std::string>>> &
  files();

  Sym make_sym_from_row(const GraphEdgeRow &row);
  Sym make_sym_from_symbol(const Symbol &sym);
  // v30: display info for a type_node id (nullopt when absent).
  std::optional<TypeInfo> type_info(int64_t type_id);
  std::optional<std::string> named_decl(TypeInfo type);

  // Batch-load sites for edge_ids.
  std::map<int64_t, std::vector<Site>>
  sites_for(const std::vector<int64_t> &edge_ids);

  // Resolve site file_id to abs path using the file cache.
  Site make_site(const EdgeSiteRow &row);
};

} // namespace cidx::graph
