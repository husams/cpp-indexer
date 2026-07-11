// Storage-backed EdgeSink: writes through the real cidx::Storage, so the LT
// edge pass produces byte-identical `edge`/`template_param`/`template_arg`
// rows in an index.db. The only file in the LT layer that includes storage
// headers.
#pragma once

#include "clangx_lt/edge_sink.hpp"

namespace cidx {
class Storage;
}

namespace cidx::lt {

class StorageEdgeSink : public EdgeSink {
public:
  explicit StorageEdgeSink(cidx::Storage &db);

  std::optional<int64_t> lookup_symbol_id(const std::string &usr) override;
  int64_t mint_symbol(const MintRequest &req) override;
  void add_edge(const EdgeRecord &edge) override;
  void add_template_param(const TemplateParamRecord &param) override;
  void add_template_arg(const TemplateArgRecord &arg) override;
  std::optional<int64_t> file_id_for_path(const std::string &path) override;
  std::vector<TypeArgCandidate>
  type_arg_candidates(const std::string &name, bool qualified) override;

private:
  cidx::Storage &db_;
};

} // namespace cidx::lt
