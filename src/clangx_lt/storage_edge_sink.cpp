#include "clangx_lt/storage_edge_sink.hpp"

#include "storage/storage.hpp"

namespace cidx::lt {

StorageEdgeSink::StorageEdgeSink(cidx::Storage &db) : db_(db) {}

std::optional<int64_t>
StorageEdgeSink::lookup_symbol_id(const std::string &usr) {
  const std::optional<cidx::Symbol> sym = db_.lookup_symbol(usr);
  if (!sym)
    return std::nullopt;
  return sym->id;
}

int64_t StorageEdgeSink::mint_symbol(const MintRequest &req) {
  return db_.mint_symbol_id(req.usr, req.spelling, req.qual_name,
                            req.display_name, req.kind_name, req.decl_file_id,
                            req.decl_line, req.decl_col, req.decl_path);
}

void StorageEdgeSink::add_edge(const EdgeRecord &edge) {
  cidx::Edge e;
  e.src_id = edge.src_id;
  e.dst_id = edge.dst_id;
  e.kind = edge.kind;
  e.count = edge.count;
  if (edge.base_access)
    e.base_access = *edge.base_access;
  if (edge.is_virtual)
    e.is_virtual = *edge.is_virtual;
  db_.add_edge(e);
}

void StorageEdgeSink::add_template_param(const TemplateParamRecord &param) {
  cidx::TemplateParam p;
  p.owner_id = param.owner_id;
  p.position = param.position;
  p.param_kind = param.param_kind;
  p.name = param.name;
  db_.add_template_param(p);
}

void StorageEdgeSink::add_template_arg(const TemplateArgRecord &arg) {
  cidx::TemplateArg a;
  a.owner_id = arg.owner_id;
  a.position = arg.position;
  a.arg_kind = arg.arg_kind;
  a.ref_id = arg.ref_id;
  a.literal = arg.literal;
  db_.add_template_arg(a);
}

std::optional<int64_t>
StorageEdgeSink::file_id_for_path(const std::string &path) {
  const auto row = db_.get_file(path);
  if (!row)
    return std::nullopt;
  return row->id;
}

std::vector<TypeArgCandidate>
StorageEdgeSink::type_arg_candidates(const std::string &name, bool qualified) {
  const std::vector<cidx::Symbol> hits =
      qualified ? db_.lookup_symbols_by_qual_name(name)
                : db_.lookup_symbols_by_name(name);
  std::vector<TypeArgCandidate> out;
  out.reserve(hits.size());
  for (const cidx::Symbol &sym : hits) {
    TypeArgCandidate c;
    c.id = sym.id;
    c.kind_name = sym.kind;
    c.is_instantiation = sym.is_instantiation;
    out.push_back(std::move(c));
  }
  return out;
}

} // namespace cidx::lt
