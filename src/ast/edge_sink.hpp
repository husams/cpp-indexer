// Edge sink interface.
//
// The edge visitors emit through this interface and know nothing about
// storage. The real implementation (StorageEdgeSink) wraps cidx::Storage;
// tests can substitute a recorder. Method set mirrors exactly the Storage
// operations the libclang edge pass uses (ast_edges.cpp).
#pragma once

#include "ast/edge_records.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cidx::ast {

class EdgeSink {
public:
  virtual ~EdgeSink() = default;

  // symbol.id for an indexed USR (lookup_symbol).
  virtual std::optional<int64_t>
  lookup_symbol_id(const std::string &usr) = 0;

  // Upsert a USR-keyed stub, returning its stable id (mint_symbol_id).
  virtual int64_t mint_symbol(const MintRequest &req) = 0;

  // Upsert; returns the stable edge.id (ON CONFLICT increments count).
  virtual int64_t add_edge(const EdgeRecord &edge) = 0;

  // Idempotent structural upsert: a relationship already recorded (e.g. a
  // specializes/instantiates edge emitted at declaration time) is never
  // re-counted by later emissions. Returns the stable edge.id.
  virtual int64_t ensure_edge(const EdgeRecord &edge) = 0;
  virtual void add_edge_site(const EdgeSiteRecord &site) = 0;
  virtual void add_call_arg(const CallArgRecord &arg) = 0;
  virtual void add_template_param(const TemplateParamRecord &param) = 0;
  virtual void add_template_arg(const TemplateArgRecord &arg) = 0;

  // Pre-walk cleanup (index_edges/ast.cpp): drop this file's non-contains
  // edges (keyed by the SRC symbol's file_id) and its definition rows.
  virtual void delete_edges_for_file(int64_t file_id) = 0;
  virtual void delete_definitions_for_file(int64_t file_id) = 0;

  // v27 per-backend definition rows + their def_edge snapshots.
  virtual int64_t
  get_or_create_definition(int64_t symbol_id, int64_t file_id, int64_t line,
                           int64_t col, int64_t end_line, int64_t end_col,
                           const std::optional<std::string> &init_text) = 0;
  virtual void add_def_edge(int64_t def_id, int64_t dst_id, int64_t kind) = 0;
  virtual void copy_body_edges_to_def_edge(int64_t def_id,
                                           int64_t src_id) = 0;

  // file.id for a registered absolute path (get_file); nullopt when the path
  // is not part of any indexed component (system/stdlib headers).
  virtual std::optional<int64_t>
  file_id_for_path(const std::string &path) = 0;

  // Symbols matching `name` by qual_name (qualified=true) or spelling, for
  // template-arg reference resolution (lookup_symbols_by_[qual_]name).
  virtual std::vector<TypeArgCandidate>
  type_arg_candidates(const std::string &name, bool qualified) = 0;

  // Display-name rewrite for callable template specializations
  // (update_callable_template_display_name).
  virtual std::optional<std::string> lookup_display_name(int64_t id) = 0;
  virtual void update_display_name(int64_t id, const std::string &display) = 0;

  // symbol.ids matching (qual_name, kind name) — recovered/overloaded-call
  // fallback (lookup_symbols_by_qual_name(qn, sk)).
  virtual std::vector<int64_t>
  symbol_ids_by_qual_name_kind(const std::string &qual_name,
                               const std::string &kind_name) = 0;
};

} // namespace cidx::ast
