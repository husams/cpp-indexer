#include "ast/storage_edge_sink.hpp"

#include <algorithm>

#include "storage/storage.hpp"

namespace cidx::ast {

StorageEdgeSink::StorageEdgeSink(cidx::Storage &db) : db_(db) {}

void StorageEdgeSink::reset_fact_ids() {
  edge_ids_.clear();
  definition_ids_.clear();
}

std::optional<int64_t> StorageEdgeSink::lookup_symbol_id(
    const std::string &usr, const std::optional<std::string> &identity_source) {
  const std::optional<int64_t> scope =
      current_file_id_ >= 0
          ? std::optional<int64_t>(
                db_.semantic_universe_for_file_id(current_file_id_))
          : std::nullopt;
  const std::optional<cidx::Symbol> sym =
      db_.lookup_symbol(usr, scope, identity_source);
  if (!sym) {
    return std::nullopt;
  }
  return sym->id;
}

void StorageEdgeSink::set_current_file_id(int64_t file_id) {
  current_file_id_ = file_id;
}

int64_t StorageEdgeSink::mint_symbol(const MintRequest &req) {
  return db_.mint_symbol_id(
      req.usr, req.spelling, req.qual_name, req.display_name, req.kind_name,
      req.decl_file_id, req.decl_line, req.decl_col, req.decl_path,
      req.is_instantiation, req.is_named_instance, req.type_info,
      current_file_id_ >= 0
          ? std::optional<int64_t>(
                db_.semantic_universe_for_file_id(current_file_id_))
          : std::nullopt,
      req.identity_source, req.linkage);
}

int64_t StorageEdgeSink::add_edge(const EdgeRecord &edge) {
  cidx::Edge e;
  e.src_id = edge.src_id;
  e.dst_id = edge.dst_id;
  e.kind = edge.kind;
  e.count = edge.count;
  if (edge.base_access) {
    e.base_access = edge.base_access;
  }
  if (edge.is_virtual) {
    e.is_virtual = edge.is_virtual;
  }
  const int64_t id = db_.add_edge(e);
  if (std::ranges::find(edge_ids_, id) == edge_ids_.end()) {
    edge_ids_.push_back(id);
  }
  return id;
}

int64_t StorageEdgeSink::ensure_edge(const EdgeRecord &edge) {
  cidx::Edge e;
  e.src_id = edge.src_id;
  e.dst_id = edge.dst_id;
  e.kind = edge.kind;
  e.count = edge.count;
  if (edge.base_access) {
    e.base_access = edge.base_access;
  }
  if (edge.is_virtual) {
    e.is_virtual = edge.is_virtual;
  }
  const int64_t id = db_.ensure_edge(e);
  if (std::ranges::find(edge_ids_, id) == edge_ids_.end()) {
    edge_ids_.push_back(id);
  }
  return id;
}

void StorageEdgeSink::add_edge_site(const EdgeSiteRecord &site) {
  cidx::EdgeSite s;
  s.edge_id = site.edge_id;
  s.file_id = site.file_id;
  s.line = site.line;
  s.col = site.col;
  s.conditional = site.conditional;
  s.recv_src_kind = site.recv_src_kind;
  s.recv_type_usr = site.recv_type_usr;
  s.recv_decl_usr = site.recv_decl_usr;
  s.recv_param_pos = site.recv_param_pos;
  s.recv_type_is_value = site.recv_type_is_value;
  db_.add_edge_site(s);
}

void StorageEdgeSink::add_call_arg(const CallArgRecord &arg) {
  cidx::CallArg a;
  a.edge_id = arg.edge_id;
  a.file_id = arg.file_id;
  a.line = arg.line;
  a.col = arg.col;
  a.position = arg.position;
  a.src_kind = arg.src_kind;
  a.type_usr = arg.type_usr;
  a.decl_usr = arg.decl_usr;
  a.callee_usr = arg.callee_usr;
  a.type_is_value = arg.type_is_value;
  db_.add_call_arg(a);
}

void StorageEdgeSink::delete_edges_for_file(int64_t file_id) {
  db_.delete_edges_for_file(file_id);
}

void StorageEdgeSink::delete_definitions_for_file(int64_t file_id) {
  db_.delete_definitions_for_file(file_id);
}

int64_t StorageEdgeSink::get_or_create_definition(
    int64_t symbol_id, int64_t file_id, int64_t line, int64_t col,
    int64_t end_line, int64_t end_col,
    const std::optional<std::string> &init_text) {
  const int64_t id = db_.get_or_create_definition(symbol_id, file_id, line, col,
                                                  end_line, end_col, init_text);
  if (std::ranges::find(definition_ids_, id) == definition_ids_.end()) {
    definition_ids_.push_back(id);
  }
  return id;
}

void StorageEdgeSink::add_def_edge(int64_t def_id, int64_t dst_id,
                                   int64_t kind) {
  db_.add_def_edge(def_id, dst_id, kind);
}

void StorageEdgeSink::copy_body_edges_to_def_edge(int64_t def_id,
                                                  int64_t src_id) {
  db_.copy_body_edges_to_def_edge(def_id, src_id);
}

std::optional<std::string> StorageEdgeSink::lookup_display_name(int64_t id) {
  const std::optional<cidx::Symbol> sym = db_.lookup_symbol_by_id(id);
  if (!sym) {
    return std::nullopt;
  }
  return sym->display_name;
}

void StorageEdgeSink::update_display_name(int64_t id,
                                          const std::string &display) {
  const std::optional<cidx::Symbol> sym = db_.lookup_symbol_by_id(id);
  if (!sym) {
    return;
  }
  db_.update_symbol_by_id(id, {{"display_name", display}});
}

std::vector<int64_t>
StorageEdgeSink::symbol_ids_by_qual_name_kind(const std::string &qual_name,
                                              const std::string &kind_name) {
  std::vector<int64_t> out;
  for (const cidx::Symbol &sym : db_.lookup_symbols_by_qual_name(
           qual_name, kind_name,
           current_file_id_ >= 0
               ? std::optional<int64_t>(
                     db_.semantic_universe_for_file_id(current_file_id_))
               : std::nullopt)) {
    out.push_back(sym.id);
  }
  return out;
}

void StorageEdgeSink::add_template_param(const TemplateParamRecord &param) {
  cidx::TemplateParam p;
  p.owner_id = param.owner_id;
  p.position = param.position;
  p.param_kind = param.param_kind;
  p.name = param.name;
  p.default_txt = param.default_txt;
  p.type_id = param.type_id;
  p.default_type_id = param.default_type_id;
  p.default_ref_id = param.default_ref_id;
  db_.add_template_param(p);
}

void StorageEdgeSink::add_template_arg(const TemplateArgRecord &arg) {
  cidx::TemplateArg a;
  a.owner_id = arg.owner_id;
  a.position = arg.position;
  a.pack_index = arg.pack_index;
  a.arg_kind = arg.arg_kind;
  a.ref_id = arg.ref_id;
  a.literal = arg.literal;
  a.type_id = arg.type_id;
  db_.add_template_arg(a);
}

int64_t StorageEdgeSink::intern_type_node(const TypeNodeRecord &node) {
  cidx::TypeNode n;
  n.type_key = node.type_key;
  n.spelling = node.spelling;
  n.kind = node.kind;
  n.is_const = node.is_const;
  n.is_volatile = node.is_volatile;
  n.is_restrict = node.is_restrict;
  n.decl_usr = node.decl_usr;
  n.canonical_id = node.canonical_id;
  return db_.intern_type_node(n);
}

void StorageEdgeSink::add_type_edge(int64_t src_id, int64_t kind,
                                    int64_t position, int64_t dst_id) {
  db_.add_type_edge(src_id, kind, position, dst_id);
}

void StorageEdgeSink::replace_parameters(
    int64_t owner_id, const std::vector<ParameterRecord> &params) {
  std::vector<cidx::Parameter> rows;
  rows.reserve(params.size());
  for (const ParameterRecord &p : params) {
    cidx::Parameter row;
    row.owner_id = owner_id;
    row.position = p.position;
    row.pack_index = p.pack_index;
    row.name = p.name;
    row.type_id = p.type_id;
    row.declared_type_id = p.declared_type_id;
    row.adjusted_type_id = p.adjusted_type_id;
    row.default_text = p.default_text;
    row.default_origin = p.default_origin;
    row.reference_semantics = p.reference_semantics;
    row.file_id = p.file_id;
    row.line = p.line;
    row.col = p.col;
    rows.push_back(std::move(row));
  }
  db_.replace_parameters(owner_id, rows);
}

void StorageEdgeSink::add_symbol_type(int64_t symbol_id, int64_t kind,
                                      int64_t type_id) {
  db_.add_symbol_type(symbol_id, kind, type_id);
}

std::optional<int64_t>
StorageEdgeSink::file_id_for_path(const std::string &path) {
  const auto row = db_.get_file(path);
  if (!row) {
    return std::nullopt;
  }
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

} // namespace cidx::ast
