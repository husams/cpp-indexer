#include "ast/storage_edge_sink.hpp"

#include <algorithm>

#include "storage/ports.hpp"

namespace cidx::ast {

StorageEdgeSink::StorageEdgeSink(cidx::storage::AstStoragePorts &ports)
    : ports_(ports) {}

void StorageEdgeSink::reset_fact_ids() {
  edge_ids_.clear();
  definition_ids_.clear();
}

std::optional<int64_t> StorageEdgeSink::lookup_symbol_id(
    const std::string &usr, const std::optional<std::string> &identity_source) {
  std::string cache_key = usr;
  cache_key.push_back('\x1f');
  if (identity_source) {
    cache_key += *identity_source;
  }
  cache_key.push_back('\x1f');
  if (identity_translation_unit_) {
    cache_key += *identity_translation_unit_;
  }
  if (const auto cached = lookup_cache_.find(cache_key);
      cached != lookup_cache_.end()) {
    return cached->second;
  }
  const std::optional<cidx::Symbol> sym = ports_.symbols_read.lookup_symbol(
      usr, current_universe_id_, identity_source, identity_translation_unit_);
  const std::optional<int64_t> result =
      sym ? std::optional<int64_t>(sym->id) : std::nullopt;
  lookup_cache_.emplace(std::move(cache_key), result);
  return result;
}

void StorageEdgeSink::set_current_file_id(int64_t file_id) {
  current_file_id_ = file_id;
  current_universe_id_ =
      file_id >= 0
          ? std::optional<int64_t>(
                ports_.workspace.semantic_universe_for_file_id(file_id))
          : std::nullopt;
  lookup_cache_.clear();
}

void StorageEdgeSink::set_identity_translation_unit_config_id(
    int64_t config_id, int64_t translation_unit_file_id) {
  identity_translation_unit_ =
      translation_unit_file_id >= 0
          ? ports_.workspace.portable_translation_unit_identity_for_config(
                config_id, translation_unit_file_id)
          : ports_.workspace.portable_translation_unit_identity_for_config(
                config_id);
  lookup_cache_.clear();
}

void StorageEdgeSink::set_identity_translation_unit_file_id(int64_t file_id) {
  identity_translation_unit_ =
      file_id >= 0
          ? std::optional<std::string>(
                ports_.workspace.portable_translation_unit_identity_for_file(
                    file_id))
          : std::nullopt;
  lookup_cache_.clear();
}

int64_t StorageEdgeSink::mint_symbol(const MintRequest &req) {
  cidx::SymbolIdentityRecord identity{
      .usr = req.usr,
      .spelling = req.spelling,
      .qual_name = req.qual_name,
      .display_name = req.display_name,
      .kind = req.kind_name,
      .decl_file_id = req.decl_file_id,
      .decl_line = req.decl_line,
      .decl_col = req.decl_col,
      .decl_path = req.decl_path,
      .is_instantiation = req.is_instantiation,
      .is_named_instance = req.is_named_instance,
      .type_info = req.type_info,
      .semantic_universe_id = current_universe_id_,
      .identity_source = req.identity_source,
      .linkage = req.linkage,
      .identity_translation_unit = identity_translation_unit_};
  const int64_t id = ports_.symbols_write.mint_symbol_id(identity);
  lookup_cache_.clear();
  return id;
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
  const int64_t id = ports_.facts_write.add_edge(e);
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
  const int64_t id = ports_.facts_write.ensure_edge(e);
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
  ports_.facts_write.add_edge_site(s);
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
  ports_.facts_write.add_call_arg(a);
}

void StorageEdgeSink::delete_edges_for_file(int64_t file_id) {
  ports_.definitions_write.delete_edges_for_file(file_id);
}

void StorageEdgeSink::delete_definitions_for_file(int64_t file_id) {
  ports_.definitions_write.delete_definitions_for_file(file_id);
}

int64_t StorageEdgeSink::get_or_create_definition(
    int64_t symbol_id, int64_t file_id, int64_t line, int64_t col,
    int64_t end_line, int64_t end_col,
    const std::optional<std::string> &init_text) {
  const int64_t id = ports_.definitions_write.get_or_create_definition(
      symbol_id, file_id, line, col, end_line, end_col, init_text);
  if (std::ranges::find(definition_ids_, id) == definition_ids_.end()) {
    definition_ids_.push_back(id);
  }
  return id;
}

void StorageEdgeSink::add_def_edge(int64_t def_id, int64_t dst_id,
                                   int64_t kind) {
  ports_.definitions_write.add_def_edge(def_id, dst_id, kind);
}

void StorageEdgeSink::copy_body_edges_to_def_edge(int64_t def_id,
                                                  int64_t src_id) {
  ports_.definitions_write.copy_body_edges_to_def_edge(def_id, src_id);
}

std::optional<std::string> StorageEdgeSink::lookup_display_name(int64_t id) {
  const std::optional<cidx::Symbol> sym =
      ports_.symbols_read.lookup_symbol_by_id(id);
  if (!sym) {
    return std::nullopt;
  }
  return sym->display_name;
}

void StorageEdgeSink::update_display_name(int64_t id,
                                          const std::string &display) {
  const std::optional<cidx::Symbol> sym =
      ports_.symbols_read.lookup_symbol_by_id(id);
  if (!sym) {
    return;
  }
  ports_.symbols_write.update_symbol_by_id(id, {{"display_name", display}});
}

std::vector<int64_t>
StorageEdgeSink::symbol_ids_by_qual_name_kind(const std::string &qual_name,
                                              const std::string &kind_name) {
  std::vector<int64_t> out;
  for (const cidx::Symbol &sym :
       ports_.symbols_read.lookup_symbols_by_qual_name(
           qual_name, kind_name,
           current_file_id_ >= 0
               ? std::optional<int64_t>(
                     ports_.workspace.semantic_universe_for_file_id(
                         current_file_id_))
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
  ports_.facts_write.add_template_param(p);
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
  ports_.facts_write.add_template_arg(a);
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
  return ports_.types_write.intern_type_node(n);
}

void StorageEdgeSink::add_type_edge(int64_t src_id, int64_t kind,
                                    int64_t position, int64_t dst_id) {
  ports_.types_write.add_type_edge(src_id, kind, position, dst_id);
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
  ports_.types_write.replace_parameters(owner_id, rows);
}

void StorageEdgeSink::add_symbol_type(int64_t symbol_id, int64_t kind,
                                      int64_t type_id) {
  ports_.types_write.add_symbol_type(symbol_id, kind, type_id);
}

std::optional<int64_t>
StorageEdgeSink::file_id_for_path(const std::string &path) {
  const auto row = ports_.source.get_file(path);
  if (!row) {
    return std::nullopt;
  }
  return row->id;
}

std::vector<TypeArgCandidate>
StorageEdgeSink::type_arg_candidates(const std::string &name, bool qualified) {
  const std::vector<cidx::Symbol> hits =
      qualified ? ports_.symbols_read.lookup_symbols_by_qual_name(name)
                : ports_.symbols_read.lookup_symbols_by_name(name);
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
