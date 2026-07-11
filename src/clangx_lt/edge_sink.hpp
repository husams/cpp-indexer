// Edge sink interface.
//
// The edge visitors emit through this interface and know nothing about
// storage. The real implementation (StorageEdgeSink) wraps cidx::Storage;
// tests can substitute a recorder. Method set mirrors exactly the Storage
// operations the libclang edge pass uses (ast_edges.cpp).
#pragma once

#include "clangx_lt/edge_records.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cidx::lt {

class EdgeSink {
public:
  virtual ~EdgeSink() = default;

  // symbol.id for an indexed USR (lookup_symbol).
  virtual std::optional<int64_t>
  lookup_symbol_id(const std::string &usr) = 0;

  // Upsert a USR-keyed stub, returning its stable id (mint_symbol_id).
  virtual int64_t mint_symbol(const MintRequest &req) = 0;

  virtual void add_edge(const EdgeRecord &edge) = 0;
  virtual void add_template_param(const TemplateParamRecord &param) = 0;
  virtual void add_template_arg(const TemplateArgRecord &arg) = 0;

  // file.id for a registered absolute path (get_file); nullopt when the path
  // is not part of any indexed component (system/stdlib headers).
  virtual std::optional<int64_t>
  file_id_for_path(const std::string &path) = 0;

  // Symbols matching `name` by qual_name (qualified=true) or spelling, for
  // template-arg reference resolution (lookup_symbols_by_[qual_]name).
  virtual std::vector<TypeArgCandidate>
  type_arg_candidates(const std::string &name, bool qualified) = 0;
};

} // namespace cidx::lt
